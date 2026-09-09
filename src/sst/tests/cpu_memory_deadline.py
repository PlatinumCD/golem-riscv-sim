#!/usr/bin/env python3
"""R5b cacheless correctness oracle; reference failures remain failures."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from global_dma_clock import digest, rows, save, execute, ROOT, HERE

def analyze(trial, mode, factor, stores):
    assert "CPU_MEMORY_DEADLINE_PAYLOAD_PASS" in (trial / "serial/tile-0.log").read_text()
    tasks = rows(trial / "tasks/tile-0.csv")
    assert [r["event"] for r in tasks] == ["start", "finish"], tasks
    start, finish = [int(r["sim_time_ticks"]) for r in tasks]
    memory = [r for r in rows(trial / "profile/tile-0-memory.csv")
              if int(r["address"]) == 0x80800000 and int(r["event_tick"]) >= start
              and int(r["event_tick"]) <= finish]
    assert [r["event"] for r in memory] == ["issue", "response"], memory
    issue, response = [int(r["event_tick"]) for r in memory]
    waits = [r for r in rows(trial / "profile/tile-0-waits.csv")
             if start <= int(r["start_tick"]) and int(r["finish_tick"]) <= finish]
    observations = []
    if mode == "delivery":
        # Fences have no waits.csv row. The task-finish marker follows the
        # fence and must not precede even the register-only instruction budget.
        deadline = issue + 4096 * factor
        actual = finish
        if stores > 1:
            assert issue < response < deadline, "no response during captured instruction delay"
        observations.append({"kind": "instruction_delivery", "earliest_tick": deadline,
                             "actual_tick": actual, "pass": actual >= deadline})
    else:
        accesses = [r for r in waits if r["reason"] == "memory-access"]
        assert len(accesses) == 3, accesses
        for access in accesses[1:]:
            begin, end = int(access["start_tick"]), int(access["finish_tick"])
            deadline = begin + 10000 * factor
            observations.append({"kind": "spm_service", "start_tick": begin,
                                 "earliest_tick": deadline, "actual_tick": end,
                                 "pass": end >= deadline})
        if stores > 1:
            assert observations[0]["start_tick"] < response < observations[0]["earliest_tick"], "no SPM interruption"
    return {"oracle": "PASS" if all(o["pass"] for o in observations) else "FAIL",
            "payload": "PASS", "issue_tick": issue, "response_tick": response,
            "region_finish_tick": finish, "observations": observations}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference-install", type=Path, help="Optional explicit historical hardware installation")
    parser.add_argument("--reference-only", action="store_true")
    parser.add_argument("--cpu", action="append", choices=["500MHz", "1GHz", "2GHz"])
    args = parser.parse_args()
    if args.reference_only and args.reference_install is None:
        parser.error('--reference-only requires --reference-install')
    if args.reference_install is not None and not args.reference_install.is_dir():
        parser.error('reference installation does not exist')
    evidence = Path(os.environ.get('GOLEM_BUILD_ROOT', ROOT / 'build/src'))
    evidence.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="cpu-memory-deadline-", dir=evidence))
    print(f"CPU memory deadline evidence: {output}", flush=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith(("MITTENS_", "CPU_DEADLINE_"))}
    with (output / "guest-build.log").open("w") as log:
        subprocess.run(["bash", str(HERE / "cpu_memory_deadline_build.sh"), str(output / "guest")],
                       env=dict(env, GOLEM_HARDWARE_TREE="src"), cwd=ROOT,
                       stdout=log, stderr=subprocess.STDOUT, check=True, timeout=30)
    installs = {"reference": args.reference_install.resolve()} if args.reference_install else {}
    if not args.reference_only:
        installs["src"] = Path(os.environ.get('GOLEM_INSTALL_ROOT', ROOT / 'install/src'))
    binaries = {t: {"qemu": i / "qemu/bin/qemu-system-riscv64",
                    "element": i / "sst-elements/lib/sst-elements-library/libmittens.so"}
                for t, i in installs.items()}
    def hashes():
        return {t: {k: digest(p) for k, p in paths.items()} for t, paths in binaries.items()}
    report = {"binary_paths": {t: {k: str(p) for k, p in paths.items()} for t, paths in binaries.items()},
              "binary_sha256_before": hashes(), "cases": [],
              "guest_sha256": {m: digest(output / f"guest/{m}.elf") for m in ("delivery", "spm")},
              "fixture_sha256": {p.name: digest(p) for p in HERE.glob("cpu_memory_deadline*") if p.is_file()}}
    for tree, binary in binaries.items():
        for cpu in args.cpu or ["500MHz", "1GHz", "2GHz"]:
            for mode in ("delivery", "spm"):
                for stores in (1, 2):
                    trial = output / tree / f"{cpu}-{mode}-stores{stores}"
                    trial.mkdir(parents=True)
                    for directory in ("serial", "tasks", "profile"):
                        (trial / directory).mkdir()
                    command = [str(ROOT / "install/sst-core/bin/sst"), str(HERE / "cpu_memory_deadline_simulation.py")]
                    trial_env = dict(env, SST_LIB_PATH=str(binary["element"].parent),
                                     CPU_DEADLINE_TRIAL=str(trial), CPU_DEADLINE_QEMU=str(binary["qemu"]),
                                     CPU_DEADLINE_ELF=str(output / f"guest/{mode}.elf"),
                                     CPU_DEADLINE_CPU=cpu, CPU_DEADLINE_STORES=str(stores))
                    code = execute(command, trial_env, trial, 20)
                    case = {"tree": tree, "cpu": cpu, "mode": mode, "stores": stores,
                            "trial": str(trial), "command": command, "exit_code": code}
                    try:
                        assert code == 0, f"simulation exit {code}"
                        case.update(analyze(trial, mode, {"500MHz": 2000, "1GHz": 1000, "2GHz": 500}[cpu], stores))
                    except (AssertionError, OSError, ValueError, KeyError) as error:
                        case.update(oracle="INVALID_EVIDENCE", error=str(error))
                    report["cases"].append(case)
                    save(trial / "oracle.json", case)
                    save(output / "results.json", report)
                    print(f"{tree} {trial.name}: {case['oracle']}", flush=True)
    report["binary_sha256_after"] = hashes()
    report["binaries_unchanged"] = report["binary_sha256_before"] == report["binary_sha256_after"]
    required = "reference" if args.reference_only else "src"
    invalid = not report["binaries_unchanged"] or any(c["oracle"] == "INVALID_EVIDENCE" for c in report["cases"])
    failed = any(c["tree"] == required and c["oracle"] != "PASS" for c in report["cases"])
    report["status"] = "INVALID_EVIDENCE" if invalid else "FAIL" if failed else "PASS"
    save(output / "results.json", report)
    print(f"{report['status']}: {output / 'results.json'}", flush=True)
    return 2 if invalid else 1 if failed else 0

if __name__ == "__main__":
    raise SystemExit(main())
