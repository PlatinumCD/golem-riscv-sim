#!/usr/bin/env python3

import csv
import json
import math
import re
from collections import defaultdict
from html import escape
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "tests"
FANOUT = BUILD / "transmit-fanout" / "results"
GPT = BUILD / "sculptor-gpt2-8x8"
OUTPUT = (
    ROOT
    / "docs"
    / "research-results"
    / "transmit-synchronization"
    / "figures"
)

BLUE = "#2563eb"
ORANGE = "#ea580c"
GREEN = "#16a34a"
PURPLE = "#7c3aed"
RED = "#dc2626"
TEAL = "#0891b2"
GRAY = "#64748b"
LIGHT = "#e2e8f0"
DARK = "#0f172a"
PALETTE = [BLUE, ORANGE, GREEN, PURPLE, TEAL]


class Svg:
    def __init__(self, width, height, title):
        self.width = width
        self.height = height
        self.items = [
            (
                f'<svg xmlns="http://www.w3.org/2000/svg" '
                f'width="{width}" height="{height}" '
                f'viewBox="0 0 {width} {height}" role="img" '
                f'aria-label="{escape(title)}">'
            ),
            "<style>"
            "text{font-family:Inter,ui-sans-serif,system-ui,sans-serif;"
            "fill:" + DARK + "}"
            ".title{font-size:22px;font-weight:700}"
            ".subtitle{font-size:13px;fill:#475569}"
            ".axis{font-size:11px;fill:#475569}"
            ".label{font-size:12px;font-weight:600}"
            ".value{font-size:11px;font-weight:600}"
            "</style>",
            '<rect width="100%" height="100%" fill="white"/>',
        ]

    def line(self, x1, y1, x2, y2, stroke=LIGHT, width=1, dash=None):
        dashed = f' stroke-dasharray="{dash}"' if dash else ""
        self.items.append(
            f'<line x1="{x1:.2f}" y1="{y1:.2f}" '
            f'x2="{x2:.2f}" y2="{y2:.2f}" '
            f'stroke="{stroke}" stroke-width="{width}"{dashed}/>'
        )

    def rect(self, x, y, width, height, fill, radius=0,
             stroke="none", stroke_width=0):
        self.items.append(
            f'<rect x="{x:.2f}" y="{y:.2f}" '
            f'width="{max(width, 0):.2f}" '
            f'height="{max(height, 0):.2f}" rx="{radius}" '
            f'fill="{fill}" stroke="{stroke}" '
            f'stroke-width="{stroke_width}"/>'
        )

    def text(self, x, y, value, css="axis", anchor="start",
             fill=None, rotate=None):
        color = f' fill="{fill}"' if fill else ""
        transform = (
            f' transform="rotate({rotate} {x:.2f} {y:.2f})"'
            if rotate is not None
            else ""
        )
        self.items.append(
            f'<text x="{x:.2f}" y="{y:.2f}" '
            f'class="{css}" text-anchor="{anchor}"'
            f'{color}{transform}>{escape(str(value))}</text>'
        )

    def circle(self, x, y, radius, fill, stroke="white"):
        self.items.append(
            f'<circle cx="{x:.2f}" cy="{y:.2f}" r="{radius}" '
            f'fill="{fill}" stroke="{stroke}" stroke-width="1.5"/>'
        )

    def polyline(self, points, stroke, width=2):
        encoded = " ".join(f"{x:.2f},{y:.2f}" for x, y in points)
        self.items.append(
            f'<polyline points="{encoded}" fill="none" '
            f'stroke="{stroke}" stroke-width="{width}" '
            f'stroke-linejoin="round" stroke-linecap="round"/>'
        )

    def save(self, path):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("\n".join(self.items + ["</svg>\n"]),
                        encoding="utf-8")


def rows(path):
    with path.open("r", encoding="utf-8", newline="") as source:
        return list(csv.DictReader(source))


def load_json(path):
    with path.open("r", encoding="utf-8") as source:
        return json.load(source)


def number(value):
    return int(value)


def title(canvas, heading, subtitle):
    canvas.text(40, 35, heading, "title")
    canvas.text(40, 57, subtitle, "subtitle")


def nice_ticks(maximum, count=5):
    if maximum <= 0:
        return [0]
    raw = maximum / count
    power = 10 ** math.floor(math.log10(raw))
    step = min((1, 2, 5, 10), key=lambda value: abs(value * power - raw))
    step *= power
    return [index * step for index in range(int(maximum / step) + 2)]


def format_us(value):
    if value >= 1000:
        return f"{value / 1000:.2f} ms"
    return f"{value:.1f} μs"


def fanout_figure():
    datasets = {
        "Polling, Q=1M": ("polling-results.csv", 1_000_000, RED),
        "Polling, Q=100K": ("polling-results.csv", 100_000, ORANGE),
        "Polling, Q=10K": ("polling-results.csv", 10_000, PURPLE),
        "Timestamped doorbell": ("blocking-results.csv", 1_000_000, BLUE),
    }
    canvas = Svg(1000, 620, "Fanout synchronization validation")
    title(
        canvas,
        "Fanout exposes synchronization-amplified polling",
        "Two tiles, 512 payload words per route, 32-bit 1 GHz mesh link",
    )
    left, top, width, height = 90, 105, 840, 410
    fanouts = [1, 2, 4, 8, 16, 24]
    y_min, y_max = 10.0, 4000.0

    def sx(value):
        return left + fanouts.index(value) * width / (len(fanouts) - 1)

    def sy(value):
        ratio = (
            math.log10(value) - math.log10(y_min)
        ) / (math.log10(y_max) - math.log10(y_min))
        return top + height * (1 - ratio)

    for tick in (10, 30, 100, 300, 1000, 3000):
        y = sy(tick)
        canvas.line(left, y, left + width, y)
        canvas.text(left - 12, y + 4, format_us(tick), anchor="end")
    for fanout in fanouts:
        x = sx(fanout)
        canvas.line(x, top, x, top + height, "#f1f5f9")
        canvas.text(x, top + height + 22, fanout, anchor="middle")
    canvas.text(left + width / 2, 562, "Outgoing routes", "label",
                anchor="middle")
    canvas.text(20, top + height / 2, "Simulated completion time",
                "label", anchor="middle", rotate=-90)

    legend_x = 580
    for index, (label, (_, _, color)) in enumerate(datasets.items()):
        y = 78 + index * 19
        canvas.line(legend_x, y, legend_x + 24, y, color, 3)
        canvas.text(legend_x + 31, y + 4, label)

    for label, (filename, quantum, color) in datasets.items():
        selected = {
            number(row["fanout"]):
                number(row["simulated_time_ticks"]) / 1e6
            for row in rows(FANOUT / filename)
            if number(row["sync_quantum"]) == quantum
        }
        points = [(sx(fanout), sy(selected[fanout]))
                  for fanout in fanouts]
        canvas.polyline(points, color, 2.5)
        for (x, y), fanout in zip(points, fanouts):
            canvas.circle(x, y, 4, color)
        if label == "Polling, Q=1M":
            canvas.text(
                points[-1][0] - 8,
                points[-1][1] - 10,
                format_us(selected[24]),
                "value",
                "end",
                color,
            )
        if label == "Timestamped doorbell":
            canvas.text(
                points[-1][0] - 8,
                points[-1][1] + 18,
                format_us(selected[24]),
                "value",
                "end",
                color,
            )

    analytical = [(fanout, fanout * 517 / 1000) for fanout in fanouts]
    canvas.polyline(
        [(sx(x), sy(y)) for x, y in analytical], GRAY, 1.5)
    canvas.text(
        sx(24) - 8,
        sy(analytical[-1][1]) - 8,
        "link serialization lower bound",
        "axis",
        "end",
        GRAY,
    )
    canvas.save(OUTPUT / "01-fanout-validation.svg")


def critical_breakdown(directory):
    critical = rows(directory / "critical-path.csv")
    routes_by_key = {
        (number(row["execution_id"]), number(row["route_id"])): row
        for row in rows(directory / "routes.csv")
    }
    tasks = {
        (number(row["execution_id"]), number(row["task_id"])): row
        for row in critical
    }
    result = {
        "Task execution": sum(number(row["duration_ticks"])
                              for row in critical),
        "Same-tile gap": sum(
            number(row["wait_ticks"])
            for row in critical
            if row["predecessor_type"] == "core"
        ),
        "Source dispatch": 0,
        "Network + RX DMA": 0,
        "Destination wait": sum(
            number(row["wait_ticks"])
            for row in critical
            if row["predecessor_type"] == "route"
        ),
    }
    for row in critical:
        if row["predecessor_type"] != "route":
            continue
        key = (number(row["execution_id"]), number(row["route_id"]))
        route = routes_by_key[key]
        source = tasks[
            (number(row["execution_id"]),
             number(row["predecessor_task"]))
        ]
        result["Source dispatch"] += (
            number(route["injection_start_tick"])
            - number(source["finish_tick"])
        )
        result["Network + RX DMA"] += (
            number(route["ready_tick"])
            - number(route["injection_start_tick"])
        )
    return {name: value / 1e9 for name, value in result.items()}


POLLING_REPORT = GPT / "deployment" / "performance-profile"
NOTIFY_REPORT = (
    GPT / "deployment-blocking-trace" / "performance-profile"
)
DOORBELL_REPORT = (
    GPT
    / "deployment-doorbell-blocking-trace"
    / "performance-profile"
)
ASYNC_REPORT = (
    GPT / "deployment-doorbell-async-trace" / "performance-profile"
)


def stacked_bars(canvas, datasets, left, top, width, height,
                 maximum, show_values=True):
    categories = list(next(iter(datasets.values())))
    colors = dict(zip(categories, PALETTE))
    count = len(datasets)
    bar_width = width / count * 0.58
    for tick in nice_ticks(maximum):
        if tick > maximum:
            continue
        y = top + height * (1 - tick / maximum)
        canvas.line(left, y, left + width, y)
        canvas.text(left - 10, y + 4, f"{tick:.0f} ms", anchor="end")
    for index, (label, values) in enumerate(datasets.items()):
        x = left + (index + 0.5) * width / count - bar_width / 2
        baseline = top + height
        total = sum(values.values())
        for category in categories:
            segment = values[category]
            segment_height = height * segment / maximum
            baseline -= segment_height
            canvas.rect(
                x, baseline, bar_width, segment_height,
                colors[category], 1,
            )
            if show_values and segment / maximum > 0.055:
                canvas.text(
                    x + bar_width / 2,
                    baseline + segment_height / 2 + 4,
                    f"{segment:.1f}",
                    "value",
                    "middle",
                    "white",
                )
        canvas.text(x + bar_width / 2, baseline - 9,
                    f"{total:.3f} ms", "value", "middle")
        canvas.text(
            x + bar_width / 2,
            top + height + 20,
            label,
            "axis",
            "middle",
        )
    legend_x = left
    for index, category in enumerate(categories):
        x = legend_x + index * 168
        canvas.rect(x, 75, 12, 12, colors[category], 2)
        canvas.text(x + 18, 85, category)


def runtime_design_figure():
    datasets = {
        "Polling": critical_breakdown(POLLING_REPORT),
        "Blocking notify": critical_breakdown(NOTIFY_REPORT),
        "Doorbell": critical_breakdown(DOORBELL_REPORT),
        "Async queue": critical_breakdown(ASYNC_REPORT),
    }
    canvas = Svg(1120, 620, "Runtime synchronization design comparison")
    title(
        canvas,
        "Runtime synchronization design comparison",
        "GPT-2, 4 tokens, 60 active tiles, 8×8 mesh, dual issue",
    )
    stacked_bars(canvas, datasets, 90, 120, 960, 400, 80)
    canvas.text(
        570,
        575,
        "Critical-path time (stacked categories are non-overlapping)",
        "label",
        "middle",
    )
    canvas.save(OUTPUT / "02-runtime-design-comparison.svg")


def critical_path_figure():
    before = critical_breakdown(POLLING_REPORT)
    after = critical_breakdown(DOORBELL_REPORT)
    canvas = Svg(1050, 600, "GPT-2 critical path before and after")
    title(
        canvas,
        "The synchronization fix removes dispatch bubbles—not traffic",
        "Exact predecessor-chain decomposition; task graph and payloads unchanged",
    )
    stacked_bars(
        canvas,
        {"Before: polling": before, "After: doorbell": after},
        105,
        125,
        560,
        360,
        80,
    )
    before_total = sum(before.values())
    after_total = sum(after.values())
    x = 720
    canvas.text(x, 150, "Measured change", "title")
    comparisons = [
        ("Critical path", before_total, after_total),
        ("Source dispatch", before["Source dispatch"],
         after["Source dispatch"]),
        ("Same-tile gaps", before["Same-tile gap"],
         after["Same-tile gap"]),
        ("Network + DMA", before["Network + RX DMA"],
         after["Network + RX DMA"]),
    ]
    for index, (label, old, new) in enumerate(comparisons):
        y = 195 + index * 70
        canvas.text(x, y, label, "label")
        canvas.text(x, y + 22, f"{old:.3f} → {new:.3f} ms")
        delta = (new / old - 1) * 100 if old else 0
        canvas.text(
            970, y + 22, f"{delta:+.1f}%", "value", "end",
            GREEN if delta < 0 else RED,
        )
    canvas.text(
        x,
        500,
        f"Overall speedup: {before_total / after_total:.2f}×",
        "title",
        fill=BLUE,
    )
    canvas.save(OUTPUT / "03-gpt2-critical-path.svg")


def vertical_summary(path):
    return {
        row["metric"]: number(row["value"])
        for row in rows(path)
    }


def instruction_figure():
    summary = load_json(DOORBELL_REPORT / "summary.json")
    task_rows = rows(DOORBELL_REPORT / "tasks.csv")
    task_by_tile = defaultdict(int)
    for row in task_rows:
        task_by_tile[number(row["tile_id"])] += number(
            row["retired_instructions"])
    raw = (
        GPT
        / "deployment-doorbell-blocking-trace"
        / "performance-profile-raw"
    )
    totals = {}
    for path in raw.glob("tile-*-summary.csv"):
        data = vertical_summary(path)
        totals[data["tile_id"]] = data["instructions"]
    ordered = sorted(totals, key=totals.get, reverse=True)[:12]
    total_instructions = summary["tile_totals"]["instructions"]
    task_instructions = summary["tasks"]["retired_instructions"]
    outside = summary["tasks"]["outside_task_instructions"]

    canvas = Svg(1100, 650, "GPT-2 retired instruction breakdown")
    title(
        canvas,
        "More than half of retired instructions are outside task bodies",
        "Doorbell design, GPT-2 4 tokens; task intervals measured at fd-41 markers",
    )
    left, top, width = 90, 120, 900
    canvas.text(left, top - 15, "All 60 active tiles", "label")
    task_width = width * task_instructions / total_instructions
    canvas.rect(left, top, task_width, 52, BLUE, 3)
    canvas.rect(left + task_width, top, width - task_width, 52, ORANGE, 3)
    canvas.text(left + task_width / 2, top + 31,
                f"Tasks {task_instructions / 1e6:.2f}M", "value",
                "middle", "white")
    canvas.text(
        left + task_width + (width - task_width) / 2,
        top + 31,
        f"Runtime / boot / communication {outside / 1e6:.2f}M",
        "value",
        "middle",
        "white",
    )
    canvas.text(
        left + width,
        top + 75,
        f"{total_instructions / 1e6:.2f}M retired instructions total",
        "label",
        "end",
    )

    chart_top, chart_height = 245, 300
    max_total = max(totals[tile] for tile in ordered)
    bar_width = width / len(ordered) * 0.62
    for tick in nice_ticks(max_total, 4):
        if tick > max_total:
            continue
        y = chart_top + chart_height * (1 - tick / max_total)
        canvas.line(left, y, left + width, y)
        canvas.text(left - 10, y + 4, f"{tick / 1e6:.1f}M",
                    anchor="end")
    for index, tile in enumerate(ordered):
        x = left + (index + 0.5) * width / len(ordered) - bar_width / 2
        task_value = task_by_tile[tile]
        outside_value = max(totals[tile] - task_value, 0)
        task_height = chart_height * task_value / max_total
        outside_height = chart_height * outside_value / max_total
        baseline = chart_top + chart_height
        canvas.rect(x, baseline - task_height, bar_width,
                    task_height, BLUE, 1)
        canvas.rect(x, baseline - task_height - outside_height,
                    bar_width, outside_height, ORANGE, 1)
        canvas.text(x + bar_width / 2, baseline + 19,
                    f"T{tile}", "axis", "middle")
    canvas.text(
        left + width / 2,
        600,
        "Twelve tiles with the most retired instructions",
        "label",
        "middle",
    )
    canvas.save(OUTPUT / "04-instruction-breakdown.svg")


def timeline_panel(canvas, report, panel_x, panel_y, panel_width,
                   panel_height, heading):
    route_rows = sorted(rows(report / "routes.csv"),
                        key=lambda row: number(row["route_id"]))
    critical = rows(report / "critical-path.csv")
    source_finish = min(
        number(row["finish_tick"])
        for row in critical
        if number(row["task_id"]) == 11
    )
    maximum = max(number(row["ready_tick"]) for row in route_rows)
    maximum -= source_finish

    def sx(value):
        return panel_x + panel_width * value / maximum

    canvas.text(panel_x, panel_y - 18, heading, "label")
    for fraction in (0, 0.25, 0.5, 0.75, 1):
        x = panel_x + panel_width * fraction
        canvas.line(x, panel_y, x, panel_y + panel_height)
        canvas.text(
            x,
            panel_y + panel_height + 18,
            f"{maximum * fraction / 1e6:.0f} μs",
            "axis",
            "middle",
        )
    lane = panel_height / len(route_rows)
    for index, row in enumerate(route_rows):
        y = panel_y + index * lane + lane * 0.18
        h = max(lane * 0.64, 2)
        injection = number(row["injection_start_tick"]) - source_finish
        arrival = number(row["arrival_finish_tick"]) - source_finish
        ready = number(row["ready_tick"]) - source_finish
        canvas.rect(panel_x, y, sx(injection) - panel_x, h,
                    ORANGE)
        canvas.rect(sx(injection), y, sx(arrival) - sx(injection),
                    h, BLUE)
        canvas.rect(sx(arrival), y, sx(ready) - sx(arrival),
                    h, GREEN)
        if index % 4 == 0:
            canvas.text(panel_x - 8, y + h, row["route_id"],
                        "axis", "end")


def route_timeline_figure():
    canvas = Svg(1150, 780, "Twenty-four route source timeline")
    title(
        canvas,
        "Twenty-four-route source timeline",
        "Orange: dispatch delay · blue: mesh traversal · green: receive DMA",
    )
    timeline_panel(
        canvas,
        FANOUT / "polling" / "fanout-24" / "quantum-1000000"
        / "report",
        105,
        125,
        965,
        250,
        "Polling, Q=1M",
    )
    timeline_panel(
        canvas,
        FANOUT / "blocking" / "fanout-24" / "quantum-1000000"
        / "report",
        105,
        465,
        965,
        250,
        "Timestamped doorbell",
    )
    canvas.save(OUTPUT / "05-route-fanout-timeline.svg")


def color_scale(value, maximum):
    if maximum <= 0:
        return "#eff6ff"
    ratio = math.sqrt(value / maximum)
    low = (239, 246, 255)
    high = (30, 64, 175)
    rgb = tuple(
        round(low[index] + ratio * (high[index] - low[index]))
        for index in range(3)
    )
    return f"#{rgb[0]:02x}{rgb[1]:02x}{rgb[2]:02x}"


def heatmap(canvas, values, x, y, size, heading, unit):
    maximum = max(values.values(), default=0)
    cell = size / 8
    canvas.text(x, y - 18, heading, "label")
    for tile_y in range(8):
        for tile_x in range(8):
            tile = tile_y * 8 + tile_x
            value = values.get(tile, 0)
            px = x + tile_x * cell
            py = y + tile_y * cell
            fill = color_scale(value, maximum)
            canvas.rect(px, py, cell - 2, cell - 2, fill, 2)
            canvas.text(px + cell / 2, py + cell / 2 - 2,
                        tile, "value", "middle",
                        "white" if value > maximum * 0.35 else DARK)
            canvas.text(
                px + cell / 2,
                py + cell / 2 + 12,
                f"{value / 1e6:.1f}",
                "axis",
                "middle",
                "white" if value > maximum * 0.35 else DARK,
            )
    canvas.text(x + size / 2, y + size + 22,
                f"tile ID; cell value in {unit}", "axis", "middle")


def mesh_heatmap_figure():
    traffic = defaultdict(int)
    for row in rows(DOORBELL_REPORT / "network-packets.csv"):
        traffic[number(row["source"])] += number(row["word_hops"])
    stalls_by_router = defaultdict(int)
    pattern = re.compile(r"router_(\d+)_(\d+)$")
    for row in rows(DOORBELL_REPORT / "link-statistics.csv"):
        match = pattern.fullmatch(row["component"])
        if match is None:
            continue
        x, y = map(int, match.groups())
        stalls_by_router[y * 8 + x] += number(row["stalls"])

    canvas = Svg(1120, 650, "GPT-2 mesh traffic and stalls")
    title(
        canvas,
        "Traffic is spatially uneven; congestion is a placement property",
        "GPT-2 doorbell trace on the physical 8×8 XY-routed mesh",
    )
    heatmap(
        canvas, traffic, 80, 125, 400,
        "Directional word-hops sourced per tile", "million word-hops",
    )
    heatmap(
        canvas, stalls_by_router, 640, 125, 400,
        "Router stall events per coordinate", "million stalls",
    )
    canvas.text(
        560,
        600,
        "Inactive tiles remain visible because they still occupy physical mesh coordinates.",
        "subtitle",
        "middle",
    )
    canvas.save(OUTPUT / "06-mesh-traffic-stall-heatmaps.svg")


def write_summary():
    before = load_json(POLLING_REPORT / "summary.json")
    after = load_json(DOORBELL_REPORT / "summary.json")
    asynchronous = load_json(ASYNC_REPORT / "summary.json")
    summary = {
        "gpt2": {
            "polling_finish_ticks": before["finish_tick"],
            "doorbell_finish_ticks": after["finish_tick"],
            "async_finish_ticks": asynchronous["finish_tick"],
            "speedup": before["finish_tick"] / after["finish_tick"],
            "tasks": after["tasks"]["count"],
            "routes": after["network"]["routes"],
            "injected_words": after["network"]["injected_words"],
            "word_hops": after["network"]["directional_word_hops"],
            "retired_instructions":
                after["tile_totals"]["instructions"],
            "task_instructions":
                after["tasks"]["retired_instructions"],
            "outside_task_instructions":
                after["tasks"]["outside_task_instructions"],
        },
        "critical_path_ms": {
            "polling": critical_breakdown(POLLING_REPORT),
            "doorbell": critical_breakdown(DOORBELL_REPORT),
        },
    }
    path = OUTPUT.parent / "study-summary.json"
    path.write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def main():
    required = [
        FANOUT / "polling-results.csv",
        FANOUT / "blocking-results.csv",
        POLLING_REPORT / "summary.json",
        NOTIFY_REPORT / "summary.json",
        DOORBELL_REPORT / "summary.json",
        ASYNC_REPORT / "summary.json",
        DOORBELL_REPORT / "tasks.csv",
    ]
    missing = [path for path in required if not path.is_file()]
    if missing:
        raise SystemExit(
            "missing required study inputs:\n"
            + "\n".join(str(path) for path in missing)
        )
    fanout_figure()
    runtime_design_figure()
    critical_path_figure()
    instruction_figure()
    route_timeline_figure()
    mesh_heatmap_figure()
    write_summary()
    print(OUTPUT)


if __name__ == "__main__":
    main()
