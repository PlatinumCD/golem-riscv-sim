#!/usr/bin/env python3

"""Render a deterministic, linearly time-scaled Mittens mesh movie."""

import argparse
import hashlib
import json
import math
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont
from imageio_ffmpeg import get_ffmpeg_exe


BASE_WIDTH = 1920
BASE_HEIGHT = 1080
RENDERER_VERSION = 3

# Deliberately restrained, print-like palette. There is no application chrome
# and no blue UI tint: the architecture is the entire frame.
COLORS = {
    "background": "#ffffff",
    "ink": "#171717",
    "muted": "#8b8b84",
    "grid": "#deded8",
    "tile_idle": "#ffffff",
    "tile_wait": "#f0ece4",
    "wait_pattern": "#aaa49a",
    "wait_outline": "#76736d",
    "digital": "#171717",
    "critical": "#000000",
    "packet": "#f2a900",
    "packet_hot": "#ef6c00",
    "contention": "#df2e21",
    "analog": "#c2185b",
    "analog_load": "#e83e8c",
    "analog_compute": "#9c1758",
    "analog_store": "#7b1fa2",
    "analog_set": "#d45b87",
    "dma": "#138a5b",
}

FONT_REGULAR = Path("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf")
FONT_BOLD = Path("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf")
FONT_MONO = Path("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf")
FONT_MONO_BOLD = Path("/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf")


def load_trace(path):
    trace = json.loads(path.read_text(encoding="utf-8"))
    if trace.get("schemaVersion") not in {1, 2}:
        raise ValueError("movie renderer supports visualization schema 1 or 2")
    meta = trace.get("meta", {})
    for field in ("width", "height", "durationTicks", "timebasePs"):
        if field not in meta:
            raise ValueError(f"trace is missing meta.{field}")
    if int(meta["width"]) <= 0 or int(meta["height"]) <= 0:
        raise ValueError("trace mesh dimensions must be positive")
    if float(meta["durationTicks"]) <= 0 or float(meta["timebasePs"]) <= 0:
        raise ValueError("trace duration and timebase must be positive")
    for collection in (
        "tasks",
        "routes",
        "packets",
        "dma",
        "analog",
        "waits",
        "blocked",
        "links",
    ):
        trace.setdefault(collection, [])
    if not trace["packets"]:
        trace["packets"] = [
            {
                "packet": route.get("route", index),
                "route": route.get("route", index),
                "execution": route.get("execution", 0),
                "kind": "logical-route",
                "source": route["source"],
                "destination": route["destination"],
                "words": route.get("words", 0),
                "payloadWords": route.get("payloadWords", 0),
                "protocolWords": (
                    route.get("words", 0) - route.get("payloadWords", 0)
                ),
                "hops": route.get("hops", 0),
                "wordHops": route.get("words", 0) * route.get("hops", 0),
                "ready": route.get("ready", route["start"]),
                "start": route["start"],
                "end": route["end"],
                "queueTicks": route.get("queueTicks", 0),
                "transitTicks": route["end"] - route["start"],
                "path": route.get(
                    "path",
                    [route["source"], route["destination"]],
                ),
            }
            for index, route in enumerate(trace["routes"])
        ]
    return trace


def active_at(events, tick):
    return [
        event
        for event in events
        if event.get("start", 0) <= tick <= event.get("end", 0)
    ]


def pending_at(events, tick):
    return [
        event
        for event in events
        if event.get("ready", event.get("start", 0))
        <= tick
        < event.get("start", 0)
    ]


def progress(event, tick):
    start = float(event.get("start", 0))
    end = float(event.get("end", start))
    if end <= start:
        return 1.0
    return min(1.0, max(0.0, (tick - start) / (end - start)))


def route_position(packet, tick):
    path = packet.get("path") or [packet["source"], packet["destination"]]
    if len(path) <= 1:
        return path[0], path[0], 0.0
    scaled = min(0.999999, progress(packet, tick)) * (len(path) - 1)
    segment = int(math.floor(scaled))
    return path[segment], path[segment + 1], scaled - segment


class MovieRenderer:
    """Full-frame architectural renderer with no dashboard UI."""

    def __init__(self, trace, width, height, movie_seconds, fps):
        self.trace = trace
        self.width = width
        self.height = height
        self.movie_seconds = float(movie_seconds)
        self.fps = float(fps)
        self.sx = width / BASE_WIDTH
        self.sy = height / BASE_HEIGHT
        self.ss = min(self.sx, self.sy)
        self.meta = trace["meta"]
        self.columns = int(self.meta["width"])
        self.rows = int(self.meta["height"])
        self.duration = float(self.meta["durationTicks"])
        self.timebase_ps = float(self.meta["timebasePs"])
        self.simulation_seconds = self.duration * self.timebase_ps * 1.0e-12
        self.slowdown = (
            self.movie_seconds / self.simulation_seconds
            if self.simulation_seconds > 0
            else 0
        )
        self.fonts = self._load_fonts()
        self.tile_points = self._tile_points()
        self.tile_half = self._tile_half_size()
        self.static = self._draw_static()

    def x(self, value):
        return int(round(value * self.sx))

    def y(self, value):
        return int(round(value * self.sy))

    def p(self, value):
        return max(1, int(round(value * self.ss)))

    def _font(self, path, size):
        if path.is_file():
            return ImageFont.truetype(str(path), self.p(size))
        return ImageFont.load_default()

    def _load_fonts(self):
        return {
            "tile": self._font(FONT_MONO_BOLD, 17),
            "tile_small": self._font(FONT_MONO, 10),
            "key": self._font(FONT_MONO_BOLD, 12),
        }

    def _tile_points(self):
        # The mesh owns nearly the entire 16:9 frame. Its physical topology is
        # allowed to stretch with the canvas instead of being trapped in a
        # square dashboard panel.
        left = self.x(115)
        right = self.x(1805)
        top = self.y(82)
        bottom = self.y(998)
        points = {}
        for tile in range(self.columns * self.rows):
            column = tile % self.columns
            row = tile // self.columns
            x = (
                (left + right) // 2
                if self.columns == 1
                else left + (right - left) * column / (self.columns - 1)
            )
            y = (
                (top + bottom) // 2
                if self.rows == 1
                else top + (bottom - top) * row / (self.rows - 1)
            )
            points[tile] = (int(round(x)), int(round(y)))
        return points

    def _tile_half_size(self):
        if self.columns > 1:
            spacing_x = abs(self.tile_points[1][0] - self.tile_points[0][0])
        else:
            spacing_x = self.x(220)
        if self.rows > 1:
            spacing_y = abs(
                self.tile_points[self.columns][1] - self.tile_points[0][1]
            )
        else:
            spacing_y = self.y(220)
        side = min(self.p(78), int(min(spacing_x, spacing_y) * 0.52))
        return max(self.p(24), side // 2)

    def _draw_static(self):
        image = Image.new("RGB", (self.width, self.height), COLORS["background"])
        draw = ImageDraw.Draw(image)

        # Links are laid down first and disappear cleanly beneath each tile.
        for tile, point in self.tile_points.items():
            column = tile % self.columns
            row = tile // self.columns
            if column + 1 < self.columns:
                draw.line(
                    [point, self.tile_points[tile + 1]],
                    fill=COLORS["grid"],
                    width=self.p(4),
                )
            if row + 1 < self.rows:
                draw.line(
                    [point, self.tile_points[tile + self.columns]],
                    fill=COLORS["grid"],
                    width=self.p(4),
                )

        half = self.tile_half
        for tile, (x, y) in self.tile_points.items():
            draw.rectangle(
                [x - half, y - half, x + half, y + half],
                fill=COLORS["tile_idle"],
                outline=COLORS["ink"],
                width=self.p(2),
            )
            draw.text(
                (x, y),
                f"{tile:02d}",
                font=self.fonts["tile"],
                fill=COLORS["ink"],
                anchor="mm",
            )
        self._draw_color_key(draw)
        return image

    def _draw_hatched_box(self, draw, box, fill, outline, pattern, width=2):
        left, top, right, bottom = box
        draw.rectangle(
            box,
            fill=fill,
            outline=outline,
            width=self.p(width),
        )
        box_width = right - left
        box_height = bottom - top
        gap = self.p(9)
        for offset in range(-box_height, box_width + gap, gap):
            start_x = left + max(0, offset)
            start_y = top + max(0, -offset)
            length = min(
                box_width - max(0, offset),
                box_height - max(0, -offset),
            )
            if length > 0:
                draw.line(
                    [
                        (start_x, start_y),
                        (start_x + length, start_y + length),
                    ],
                    fill=pattern,
                    width=self.p(3),
                )
        draw.rectangle(
            box,
            outline=outline,
            width=self.p(width),
        )

    def _draw_color_key(self, draw):
        # One quiet line of decoding, anchored outside the mesh. It is the only
        # explanatory overlay retained in the film.
        entries = [
            ("DIGITAL", "square", COLORS["digital"]),
            ("ANALOG", "square", COLORS["analog"]),
            ("TRANSFER", "circle", COLORS["packet"]),
            ("RECEIVE", "square", COLORS["dma"]),
            ("WAITING", "wait", COLORS["tile_wait"]),
            ("BLOCKED", "square", COLORS["contention"]),
        ]
        item_width = self.x(170)
        total_width = item_width * len(entries)
        start_x = (self.width - total_width) // 2
        center_y = self.y(1056)
        mark = self.p(14)
        for index, (label, shape, color) in enumerate(entries):
            x = start_x + index * item_width
            box = [
                x,
                center_y - mark // 2,
                x + mark,
                center_y + mark // 2,
            ]
            if shape == "circle":
                draw.ellipse(box, fill=color)
            elif shape == "wait":
                self._draw_hatched_box(
                    draw,
                    box,
                    COLORS["tile_wait"],
                    COLORS["wait_outline"],
                    COLORS["wait_pattern"],
                    width=1,
                )
            else:
                draw.rectangle(box, fill=color)
            draw.text(
                (x + mark + self.x(9), center_y),
                label,
                font=self.fonts["key"],
                fill=COLORS["ink"],
                anchor="lm",
            )

    @staticmethod
    def _event_map(events, field="tile"):
        result = defaultdict(list)
        for event in events:
            result[int(event[field])].append(event)
        return result

    def _draw_network(self, draw, tick, active_packets):
        segment_packets = defaultdict(list)
        positions = []
        for packet in active_packets:
            source, destination, fraction = route_position(packet, tick)
            if source not in self.tile_points or destination not in self.tile_points:
                continue
            segment_packets[(source, destination)].append(packet)
            positions.append((packet, source, destination, fraction))

        # Link temperature communicates load without text. Amber is occupied;
        # orange is shared; red is severe instantaneous contention.
        for (source, destination), packets in segment_packets.items():
            count = len(packets)
            if count >= 5:
                color = COLORS["contention"]
            elif count >= 2:
                color = COLORS["packet_hot"]
            else:
                color = COLORS["packet"]
            width = min(self.p(16), self.p(5) + self.p(math.log2(count + 1) * 2))
            draw.line(
                [self.tile_points[source], self.tile_points[destination]],
                fill=color,
                width=width,
            )

        # Every stream influences link color. Individual heads are capped only
        # to prevent one dense instant from turning into an unreadable blob.
        positions.sort(
            key=lambda item: (
                item[0].get("queueTicks", 0) == 0,
                -item[0].get("words", 0),
                item[0].get("packet", 0),
            )
        )
        for packet, source, destination, fraction in positions[:192]:
            start = self.tile_points[source]
            finish = self.tile_points[destination]
            x = int(round(start[0] + (finish[0] - start[0]) * fraction))
            y = int(round(start[1] + (finish[1] - start[1]) * fraction))
            count = len(segment_packets[(source, destination)])
            queued = packet.get("queueTicks", 0) > 0
            color = (
                COLORS["contention"]
                if queued or count >= 5
                else COLORS["packet_hot"]
                if count >= 2
                else COLORS["packet"]
            )
            radius = self.p(
                min(9.0, 4.0 + math.log2(max(1, packet.get("words", 1))) * 0.35)
            )
            draw.ellipse(
                [x - radius, y - radius, x + radius, y + radius],
                fill=color,
                outline=COLORS["background"],
                width=self.p(2),
            )

    def _tile_state(
        self,
        tile,
        tasks_by_tile,
        analog_by_tile,
        dma_by_tile,
        waits_by_tile,
        packets_by_tile,
        blocked_by_tile,
    ):
        if blocked_by_tile.get(tile):
            return "blocked", COLORS["contention"]
        if analog_by_tile.get(tile):
            operation = analog_by_tile[tile][0].get("operation", "analog")
            return "analog", COLORS.get(
                f"analog_{operation}",
                COLORS["analog"],
            )
        if tasks_by_tile.get(tile):
            critical = any(event.get("critical") for event in tasks_by_tile[tile])
            return "digital", COLORS["critical"] if critical else COLORS["digital"]
        if dma_by_tile.get(tile):
            return "dma", COLORS["dma"]
        if packets_by_tile.get(tile):
            return "network", COLORS["packet"]
        if waits_by_tile.get(tile):
            return "wait", COLORS["tile_wait"]
        return "idle", COLORS["tile_idle"]

    def _draw_array_marks(self, draw, x, y, events):
        # Array activity is encoded by compact marks inside the tile—never by a
        # label floating over the network.
        arrays = sorted({int(event.get("array", 0)) for event in events})
        if not arrays:
            return
        half = self.tile_half
        mark = self.p(7)
        gap = self.p(4)
        total = len(arrays) * mark + max(0, len(arrays) - 1) * gap
        start_x = x - total // 2
        mark_y = y + half - self.p(13)
        for index, array in enumerate(arrays):
            event = next(
                event for event in events if int(event.get("array", 0)) == array
            )
            color = COLORS.get(
                f"analog_{event.get('operation', 'analog')}",
                COLORS["analog"],
            )
            left = start_x + index * (mark + gap)
            draw.rectangle(
                [left, mark_y, left + mark, mark_y + mark],
                fill=color,
            )

    def _draw_tiles(
        self,
        draw,
        tick,
        active_tasks,
        active_analog,
        active_dma,
        active_waits,
        active_packets,
        active_blocked,
        pending_packets,
    ):
        tasks_by_tile = self._event_map(active_tasks)
        analog_by_tile = self._event_map(active_analog)
        dma_by_tile = self._event_map(active_dma)
        waits_by_tile = self._event_map(active_waits)
        blocked_by_tile = self._event_map(active_blocked)
        pending_by_tile = self._event_map(pending_packets, "source")
        packets_by_tile = defaultdict(list)
        for packet in active_packets:
            packets_by_tile[int(packet["source"])].append(packet)
            packets_by_tile[int(packet["destination"])].append(packet)

        half = self.tile_half
        for tile, (x, y) in self.tile_points.items():
            state, color = self._tile_state(
                tile,
                tasks_by_tile,
                analog_by_tile,
                dma_by_tile,
                waits_by_tile,
                packets_by_tile,
                blocked_by_tile,
            )
            fill = color
            outline = COLORS["ink"]
            text = COLORS["ink"]
            width = self.p(2)
            if state in {"digital", "analog", "dma", "blocked"}:
                text = COLORS["background"]
                outline = color
            elif state == "network":
                fill = COLORS["tile_idle"]
                outline = COLORS["packet"]
                width = self.p(4)
            elif state == "wait":
                outline = COLORS["wait_outline"]

            tile_box = [x - half, y - half, x + half, y + half]
            if state == "wait":
                self._draw_hatched_box(
                    draw,
                    tile_box,
                    COLORS["tile_wait"],
                    COLORS["wait_outline"],
                    COLORS["wait_pattern"],
                )
            else:
                draw.rectangle(
                    tile_box,
                    fill=fill,
                    outline=outline,
                    width=width,
                )
            draw.text(
                (x, y),
                f"{tile:02d}",
                font=self.fonts["tile"],
                fill=text,
                anchor="mm",
            )

            # Digital progress is a precise, silent line at the tile floor.
            if tasks_by_tile.get(tile):
                task = tasks_by_tile[tile][0]
                completed = int((half * 2 - self.p(12)) * progress(task, tick))
                draw.rectangle(
                    [
                        x - half + self.p(6),
                        y + half - self.p(9),
                        x - half + self.p(6) + completed,
                        y + half - self.p(5),
                    ],
                    fill=COLORS["background"],
                )

            if analog_by_tile.get(tile):
                self._draw_array_marks(draw, x, y, analog_by_tile[tile])

            # Queue depth is intentionally qualitative: short red ticks signal
            # pressure without turning the film into a telemetry dashboard.
            queued = len(pending_by_tile.get(tile, []))
            if queued:
                tick_count = min(4, queued)
                for index in range(tick_count):
                    offset = index * self.p(7)
                    draw.rectangle(
                        [
                            x + half + self.p(5),
                            y - half + offset,
                            x + half + self.p(10),
                            y - half + offset + self.p(5),
                        ],
                        fill=COLORS["contention"],
                    )

    def render_frame(self, tick, frame_index=0, frame_count=1):
        del frame_index, frame_count
        image = self.static.copy()
        draw = ImageDraw.Draw(image)

        active_tasks = active_at(self.trace["tasks"], tick)
        active_packets = active_at(self.trace["packets"], tick)
        pending_packets = pending_at(self.trace["packets"], tick)
        active_analog = active_at(self.trace["analog"], tick)
        active_dma = active_at(self.trace["dma"], tick)
        active_waits = active_at(self.trace["waits"], tick)
        active_blocked = active_at(self.trace["blocked"], tick)

        self._draw_network(draw, tick, active_packets)
        self._draw_tiles(
            draw,
            tick,
            active_tasks,
            active_analog,
            active_dma,
            active_waits,
            active_packets,
            active_blocked,
            pending_packets,
        )
        return image


def write_manifest(
    path,
    trace_path,
    movie_path,
    renderer,
    frame_count,
    fps,
    width,
    height,
    crf,
):
    trace_sha256 = hashlib.sha256(trace_path.read_bytes()).hexdigest()
    movie_sha256 = hashlib.sha256(movie_path.read_bytes()).hexdigest()
    manifest = {
        "schemaVersion": 1,
        "rendererVersion": RENDERER_VERSION,
        "trace": {
            "path": str(trace_path),
            "sha256": trace_sha256,
            "visualizationSchema": renderer.trace.get("schemaVersion"),
            "source": renderer.meta.get("source", ""),
        },
        "simulation": {
            "mesh": [
                int(renderer.meta["width"]),
                int(renderer.meta["height"]),
            ],
            "durationTicks": renderer.duration,
            "timebasePs": renderer.timebase_ps,
            "durationSeconds": renderer.simulation_seconds,
            "routing": renderer.meta.get("routing", "xy"),
        },
        "movie": {
            "path": str(movie_path),
            "sha256": movie_sha256,
            "bytes": movie_path.stat().st_size,
            "width": width,
            "height": height,
            "fps": fps,
            "frames": frame_count,
            "durationSeconds": renderer.movie_seconds,
            "codec": "H.264",
            "crf": crf,
            "linearSlowdown": renderer.slowdown,
            "frameMapping": (
                "simulation_tick = duration_ticks * frame_index / "
                "(frame_count - 1)"
            ),
            "composition": "full-frame mesh with no dashboard overlays",
        },
        "fidelity": {
            "measured": renderer.meta.get("timingFidelity", {}).get(
                "measured",
                [
                    "task intervals",
                    "logical route endpoints",
                    "DMA intervals",
                    "analog intervals",
                    "wait intervals",
                ],
            ),
            "reconstructed": renderer.meta.get("timingFidelity", {}).get(
                "reconstructed",
                [
                    "intermediate packet position along deterministic XY path",
                ],
            ),
            "timeScale": "continuous and linear over the complete trace",
        },
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def render_movie(arguments):
    trace_path = arguments.trace.resolve()
    output_path = arguments.output.resolve()
    trace = load_trace(trace_path)
    renderer = MovieRenderer(
        trace,
        arguments.width,
        arguments.height,
        arguments.seconds,
        arguments.fps,
    )
    frame_count = max(2, int(round(arguments.seconds * arguments.fps)))
    output_path.parent.mkdir(parents=True, exist_ok=True)

    if arguments.preview:
        preview_tick = renderer.duration * arguments.preview_fraction
        preview = renderer.render_frame(
            preview_tick,
            int(round((frame_count - 1) * arguments.preview_fraction)),
            frame_count,
        )
        arguments.preview.parent.mkdir(parents=True, exist_ok=True)
        preview.save(arguments.preview)
        print(f"preview frame: {arguments.preview}")

    if arguments.preview_only:
        return

    ffmpeg = get_ffmpeg_exe()
    command = [
        ffmpeg,
        "-y",
        "-hide_banner",
        "-loglevel",
        "warning",
        "-f",
        "rawvideo",
        "-pix_fmt",
        "rgb24",
        "-s:v",
        f"{arguments.width}x{arguments.height}",
        "-r",
        str(arguments.fps),
        "-i",
        "-",
        "-an",
        "-c:v",
        "libx264",
        "-preset",
        arguments.preset,
        "-crf",
        str(arguments.crf),
        "-pix_fmt",
        "yuv420p",
        "-movflags",
        "+faststart",
        str(output_path),
    ]
    process = subprocess.Popen(command, stdin=subprocess.PIPE)
    update_interval = max(1, frame_count // 100)
    try:
        for frame_index in range(frame_count):
            tick = renderer.duration * frame_index / (frame_count - 1)
            frame = renderer.render_frame(tick, frame_index, frame_count)
            process.stdin.write(frame.tobytes())
            if (
                frame_index % update_interval == 0
                or frame_index + 1 == frame_count
            ):
                percentage = 100 * (frame_index + 1) / frame_count
                print(
                    f"\rrendering {percentage:6.2f}% "
                    f"({frame_index + 1:,}/{frame_count:,} frames)",
                    end="",
                    flush=True,
                )
    except BrokenPipeError as error:
        raise RuntimeError("FFmpeg terminated while receiving frames") from error
    finally:
        if process.stdin:
            process.stdin.close()
    return_code = process.wait()
    print()
    if return_code != 0:
        raise RuntimeError(f"FFmpeg failed with exit code {return_code}")

    if not output_path.is_file() or output_path.stat().st_size == 0:
        raise RuntimeError("movie output is missing or empty")

    if arguments.manifest:
        write_manifest(
            arguments.manifest.resolve(),
            trace_path,
            output_path,
            renderer,
            frame_count,
            arguments.fps,
            arguments.width,
            arguments.height,
            arguments.crf,
        )
    print(
        f"movie: {output_path} "
        f"({arguments.width}x{arguments.height}, {arguments.fps} fps, "
        f"{arguments.seconds:.3f} s, {renderer.slowdown:,.1f}x slow motion)"
    )


def parse_arguments():
    parser = argparse.ArgumentParser(
        description=(
            "Render one continuous, deterministic, linearly time-scaled MP4 "
            "from a Mittens visualization trace"
        )
    )
    parser.add_argument("trace", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--seconds", type=float, default=90.0)
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--width", type=int, default=1920)
    parser.add_argument("--height", type=int, default=1080)
    parser.add_argument("--crf", type=int, default=17)
    parser.add_argument("--preset", default="medium")
    parser.add_argument("--preview", type=Path)
    parser.add_argument("--preview-fraction", type=float, default=0.5)
    parser.add_argument("--preview-only", action="store_true")
    parser.add_argument("--manifest", type=Path)
    arguments = parser.parse_args()
    if arguments.seconds <= 0:
        parser.error("--seconds must be positive")
    if arguments.fps <= 0:
        parser.error("--fps must be positive")
    if arguments.width <= 0 or arguments.height <= 0:
        parser.error("--width and --height must be positive")
    if arguments.width % 2 != 0 or arguments.height % 2 != 0:
        parser.error("--width and --height must be even for yuv420p")
    if not 0 <= arguments.crf <= 51:
        parser.error("--crf must be between 0 and 51")
    if not 0 <= arguments.preview_fraction <= 1:
        parser.error("--preview-fraction must be between 0 and 1")
    return arguments


def main():
    try:
        render_movie(parse_arguments())
    except Exception as error:
        print(f"trace movie failed: {error}", file=sys.stderr)
        raise


if __name__ == "__main__":
    main()
