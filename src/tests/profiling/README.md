# Optional cycle profiling regression

Run `bash tests/run-all.sh --case platform/profiling`.

The existing four-tile CPU/RVV/SPM/array/Mordred fixture runs with profiling
unset, explicitly disabled, enabled, and enabled with instruction budget 1.
All runs use freshly compiled guests and the ordinary component build.

Checks cover numerical outputs, exact memory service, unchanged architectural
timing and traffic, byte-identical legacy event traces when toggling profiling,
and no profile directory when disabled—even if an output path was supplied.
The enabled runs check all 40 component/port profiles, CPU trace availability,
every network flit hop, one-cycle links, and one flit per port per cycle.
No dashboard, studies, saved measurements, or visualization tools are required.
