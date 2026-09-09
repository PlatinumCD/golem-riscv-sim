#!/usr/bin/env python3
"""Fail-closed integration checks for the Phase 9 compiler/ABI contract."""

from __future__ import annotations

import argparse
import re
import subprocess
from pathlib import Path


CERTIFICATE_PREFIX = (
    "sculptor.materialization.exact_ram_readiness_certificate = {"
)


def certificate_span(module: str) -> tuple[int, int]:
    begin = module.find(CERTIFICATE_PREFIX)
    if begin < 0:
        raise AssertionError("exact-readiness certificate is missing")
    body_begin = begin + len(CERTIFICATE_PREFIX) - 1
    depth = 0
    for index in range(body_begin, len(module)):
        if module[index] == "{":
            depth += 1
        elif module[index] == "}":
            depth -= 1
            if depth == 0:
                return begin, index + 1
    raise AssertionError("exact-readiness certificate is unbalanced")


def replace_certificate(module: str, certificate: str) -> str:
    begin, end = certificate_span(module)
    return module[:begin] + certificate + module[end:]


def get_certificate(module: str) -> str:
    begin, end = certificate_span(module)
    return module[begin:end]


def mutate_nth_integer(text: str, field: str, occurrence: int, delta: int) -> str:
    matches = list(
        re.finditer(rf"\b{re.escape(field)} = (-?\d+) : i64", text)
    )
    if occurrence < 0:
        occurrence += len(matches)
    if occurrence < 0 or occurrence >= len(matches):
        raise AssertionError(f"certificate field {field!r} occurrence is missing")
    match = matches[occurrence]
    value = int(match.group(1)) + delta
    return text[: match.start(1)] + str(value) + text[match.end(1) :]


def mutate_descriptor_flags(module: str, epoch_zero: bool) -> str:
    pattern = re.compile(r"#sculptor\.materialized_dma_descriptor<([^<>]*)>")
    for match in pattern.finditer(module):
        body = match.group(1)
        direction = re.search(r"\bdirection = (\d+) : i64", body)
        flags = re.search(r"\bflags = (\d+) : i64", body)
        if not direction or not flags or int(direction.group(1)) != 0:
            continue
        old_flags = int(flags.group(1))
        if bool(old_flags & 1) != epoch_zero:
            continue
        new_flags = old_flags ^ 1
        rewritten = (
            body[: flags.start(1)] + str(new_flags) + body[flags.end(1) :]
        )
        return module[: match.start(1)] + rewritten + module[match.end(1) :]
    raise AssertionError(
        f"no {'epoch-zero' if epoch_zero else 'producer-dependent'} input descriptor"
    )


def mutate_descriptor_step(module: str) -> str:
    pattern = re.compile(r"#sculptor\.materialized_dma_descriptor<([^<>]*)>")
    match = pattern.search(module)
    if not match:
        raise AssertionError("no materialized DMA descriptor")
    body = match.group(1)
    rewritten, count = re.subn(
        r"\biterationStep = \d+ : i64",
        "iterationStep = 0 : i64",
        body,
        count=1,
    )
    if count != 1:
        raise AssertionError("descriptor iteration step is missing")
    return module[: match.start(1)] + rewritten + module[match.end(1) :]


def run_opt(opt: Path, module: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            str(opt),
            "--mlir-disable-threading",
            "--mlir-print-op-on-diagnostic=false",
            "--sculptor-emit-golem-tile-abi",
            "-o",
            "/dev/null",
        ],
        input=module,
        text=True,
        capture_output=True,
        check=False,
    )


def require_pass(opt: Path, name: str, module: str) -> None:
    result = run_opt(opt, module)
    if result.returncode != 0:
        raise AssertionError(f"{name} unexpectedly failed:\n{result.stderr[:2000]}")


def require_rejection(
    opt: Path, name: str, module: str, diagnostic: str
) -> None:
    result = run_opt(opt, module)
    if result.returncode == 0:
        raise AssertionError(f"{name} was accepted")
    if not re.search(diagnostic, result.stderr, re.IGNORECASE):
        first_line = result.stderr.splitlines()[0] if result.stderr else "<none>"
        raise AssertionError(
            f"{name} produced the wrong diagnostic: {first_line!r}; "
            f"expected /{diagnostic}/"
        )


def certificate_mutations(module: str) -> list[tuple[str, str, str]]:
    certificate = get_certificate(module)
    begin, end = certificate_span(module)
    without_certificate_end = end
    if module[without_certificate_end :].startswith(", "):
        without_certificate_end += 2
    without_certificate = module[:begin] + module[without_certificate_end:]

    wrong_mode = certificate.replace(
        'mode = "exact_dependencies"', 'mode = "bulk_barrier"', 1
    )
    if wrong_mode == certificate:
        raise AssertionError("certificate mode is missing")

    wrong_top_bytes = mutate_nth_integer(certificate, "write_bytes", -1, 1)
    wrong_tile_digest = mutate_nth_integer(certificate, "interval_digest", 1, 1)
    wrong_tile_count = mutate_nth_integer(certificate, "tile_count", 0, 1)

    resources = re.search(r"\bglobal_resource_ids = \[([^]]+)\]", certificate)
    if not resources:
        raise AssertionError("certificate resource list is missing")
    resource_values = [value.strip() for value in resources.group(1).split(",")]
    if len(resource_values) < 2:
        raise AssertionError("certificate fixture needs at least two resources")
    duplicated_resources = (
        certificate[: resources.start(1)]
        + ", ".join([resource_values[0]] * len(resource_values))
        + certificate[resources.end(1) :]
    )

    disabled = module.replace(
        "sculptor.materialization.exact_ram_readiness_enabled = true",
        "sculptor.materialization.exact_ram_readiness_enabled = false",
        1,
    )
    if disabled == module:
        raise AssertionError("exact-readiness policy marker is missing")

    missing_policy = module.replace(
        ", sculptor.materialization.exact_ram_readiness_enabled = true", "", 1
    )
    if missing_policy == module:
        missing_policy = module.replace(
            "sculptor.materialization.exact_ram_readiness_enabled = true, ",
            "",
            1,
        )
    if missing_policy == module:
        raise AssertionError("could not remove exact-readiness policy marker")

    return [
        (
            "missing certificate",
            without_certificate,
            r"malformed exact-readiness certificate",
        ),
        (
            "wrong dependency mode",
            replace_certificate(module, wrong_mode),
            r"malformed exact-readiness certificate",
        ),
        (
            "top byte total",
            replace_certificate(module, wrong_top_bytes),
            r"deployment certificate does not reconcile",
        ),
        (
            "tile interval digest",
            replace_certificate(module, wrong_tile_digest),
            r"deployment certificate does not reconcile",
        ),
        (
            "tile count",
            replace_certificate(module, wrong_tile_count),
            r"deployment certificate is incomplete",
        ),
        (
            "duplicate resources",
            replace_certificate(module, duplicated_resources),
            r"not strictly increasing",
        ),
        (
            "bulk mode with exact certificate",
            disabled,
            r"bulk materialized deployment carries an exact-readiness certificate",
        ),
        (
            "missing policy marker",
            missing_policy,
            r"no explicit exact-readiness policy",
        ),
    ]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--opt", required=True, type=Path)
    parser.add_argument("--certificate-module", required=True, type=Path)
    parser.add_argument("--dependency-module", required=True, type=Path)
    args = parser.parse_args()

    certificate_module = args.certificate_module.read_text(encoding="utf-8")
    dependency_module = args.dependency_module.read_text(encoding="utf-8")
    require_pass(args.opt, "certificate baseline", certificate_module)
    require_pass(args.opt, "dependency baseline", dependency_module)

    cases = certificate_mutations(certificate_module)
    cases.extend(
        [
            (
                "initial input marked producer-dependent",
                mutate_descriptor_flags(dependency_module, True),
                r"producer-dependent read references an initial-data buffer",
            ),
            (
                "internal input marked epoch-zero",
                mutate_descriptor_flags(dependency_module, False),
                r"epoch-zero read references a produced buffer",
            ),
            (
                "malformed affine interval",
                mutate_descriptor_step(dependency_module),
                r"iteration phase requires.*positive step",
            ),
        ]
    )
    for name, corrupted, diagnostic in cases:
        require_rejection(args.opt, name, corrupted, diagnostic)

    print(f"exact RAM readiness ABI corruption: PASS ({len(cases)} cases)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
