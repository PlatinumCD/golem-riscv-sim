#!/usr/bin/env python3

import importlib.util
import json
import subprocess
import tempfile
from pathlib import Path


MOVIE_DIR = Path(__file__).resolve().parents[1]
PROJECT_ROOT = MOVIE_DIR.parents[1]
RENDERER_PATH = MOVIE_DIR / "render-movie.py"
EXPORTER = PROJECT_ROOT / "visualizer" / "exporter" / "export-profile.py"
PROFILE = PROJECT_ROOT / "visualizer" / "tests" / "fixtures" / "profile"


def load_renderer():
    specification = importlib.util.spec_from_file_location(
        "mittens_trace_movie",
        RENDERER_PATH,
    )
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def main():
    renderer_module = load_renderer()
    with tempfile.TemporaryDirectory(prefix="mittens-movie-") as directory:
        temporary = Path(directory)
        trace_path = temporary / "trace.json"
        preview_path = temporary / "preview.png"
        subprocess.run(
            [
                "python3",
                str(EXPORTER),
                str(PROFILE),
                str(trace_path),
                "--title",
                "Movie renderer test",
                "--source",
                "fixture",
            ],
            check=True,
        )
        trace = renderer_module.load_trace(trace_path)
        renderer = renderer_module.MovieRenderer(
            trace,
            width=640,
            height=360,
            movie_seconds=4,
            fps=5,
        )

        assert renderer.duration == 990000000
        assert renderer.simulation_seconds == 0.00099
        assert round(renderer.slowdown, 3) == round(4 / 0.00099, 3)

        beginning = renderer.render_frame(0, 0, 20)
        middle = renderer.render_frame(renderer.duration / 2, 10, 20)
        finish = renderer.render_frame(renderer.duration, 19, 20)
        assert beginning.size == (640, 360)
        assert middle.size == (640, 360)
        assert finish.size == (640, 360)
        assert beginning.getpixel((0, 0)) == (255, 255, 255)
        assert middle.getpixel((0, 0)) == (255, 255, 255)
        assert beginning.tobytes() != middle.tobytes()
        assert middle.tobytes() != finish.tobytes()

        middle.save(preview_path)
        assert preview_path.stat().st_size > 1_000

        output_path = temporary / "five-frame.mp4"
        manifest_path = temporary / "manifest.json"
        arguments = type(
            "Arguments",
            (),
            {
                "trace": trace_path,
                "output": output_path,
                "seconds": 1.0,
                "fps": 5,
                "width": 640,
                "height": 360,
                "crf": 28,
                "preset": "ultrafast",
                "preview": None,
                "preview_fraction": 0.5,
                "preview_only": False,
                "manifest": manifest_path,
            },
        )()
        renderer_module.render_movie(arguments)
        assert output_path.stat().st_size > 1_000
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        assert manifest["movie"]["frames"] == 5
        assert manifest["movie"]["frameMapping"].startswith(
            "simulation_tick = duration_ticks"
        )
        assert renderer_module.COLORS["tile_wait"] != (
            renderer_module.COLORS["contention"]
        )
        assert manifest["rendererVersion"] == 3
        assert manifest["movie"]["composition"] == (
            "full-frame mesh with no dashboard overlays"
        )
        assert manifest["fidelity"]["timeScale"] == (
            "continuous and linear over the complete trace"
        )

    print("Mittens deterministic trace movie renderer: PASS")


if __name__ == "__main__":
    main()
