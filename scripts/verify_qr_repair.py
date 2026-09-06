#!/usr/bin/env python3
"""Verify QR repair correctness and latency against the sample manifest."""

import argparse
import math
import os
import pathlib
import subprocess
import sys
import time


SAMPLES = {
    **{
        f"version1/123{suffix}.png": "123"
        for suffix in [
            "",
            "_destroy1",
            "_destroy2",
            "_destroy3",
            "_destroy4",
            "_destroy5",
            "_destroy6",
            "_destroy7",
            "_destroy8",
        ]
    },
    **{f"version5/{index}.png": "version5" for index in range(1, 6)},
    **{f"version10/{index}.png": "version10" for index in range(1, 5)},
    "print/1.png": "333",
    **{f"print/{index}.png": "123" for index in range(2, 13)},
    **{f"Rotated/123_rotated{index}.png": "123" for index in range(1, 3)},
    "Rotated/rotated5.png": "version5",
    "Rotated/rotated6.png": "version5",
    "Rotated/rotated7.png": "version10",
    "Rotated/rotated8.png": "version10",
}


def decoded_text(stdout):
    """Return the final non-empty line emitted by the decoder."""
    lines = [line.strip() for line in stdout.splitlines() if line.strip()]
    return lines[-1] if lines else ""


def concise_stderr(stderr, limit=240):
    """Collapse decoder stderr to a bounded single-line diagnostic."""
    message = " ".join(stderr.split())
    return message if len(message) <= limit else message[: limit - 3] + "..."


def percentile95(values):
    """Return the nearest-rank 95th percentile."""
    return sorted(values)[max(0, math.ceil(len(values) * 0.95) - 1)]


def resolve_binary(root, binary_argument):
    """Resolve a binary relative to the repository unless it is absolute."""
    binary = pathlib.Path(binary_argument).expanduser()
    return binary if binary.is_absolute() else root / binary


def validate_args(parser, args, root, selected):
    """Reject invalid limits, names, files, and binaries before decoding."""
    for option, value, allow_zero in [
        ("--average", args.average, True),
        ("--p95", args.p95, True),
        ("--timeout", args.timeout, False),
    ]:
        valid = math.isfinite(value) and (value >= 0 if allow_zero else value > 0)
        if not valid:
            comparison = "non-negative" if allow_zero else "positive"
            parser.error(f"{option} must be a finite {comparison} number")

    unknown = [relative for relative in selected if relative not in SAMPLES]
    if unknown:
        parser.error(
            "unknown sample path(s): "
            + ", ".join(unknown)
            + "; paths must be relative to qr_fig and present in the manifest"
        )

    missing = [
        relative for relative in selected if not (root / "qr_fig" / relative).is_file()
    ]
    if missing:
        parser.error("missing sample file(s): " + ", ".join(missing))

    binary = resolve_binary(root, args.binary)
    if not binary.is_file():
        parser.error(f"decoder binary does not exist: {binary}")
    if not os.access(binary, os.X_OK):
        parser.error(f"decoder binary is not executable: {binary}")
    return binary


def main():
    parser = argparse.ArgumentParser(
        description="Verify exact QR payloads and decoder latency."
    )
    parser.add_argument("--binary", default="build/zxing")
    parser.add_argument("--average", type=float, default=0.5)
    parser.add_argument("--p95", type=float, default=0.8)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument(
        "samples",
        nargs="*",
        help="optional qr_fig-relative paths from the built-in manifest",
    )
    args = parser.parse_args()

    root = pathlib.Path(__file__).resolve().parents[1]
    selected = args.samples or list(SAMPLES)
    binary = validate_args(parser, args, root, selected)

    durations = []
    sample_failures = []
    failed_samples = 0
    for relative in selected:
        failures = []
        started = time.perf_counter()
        try:
            run = subprocess.run(
                [str(binary), str(root / "qr_fig" / relative)],
                text=True,
                capture_output=True,
                timeout=args.timeout,
                check=False,
            )
            actual = decoded_text(run.stdout)
            if run.returncode != 0:
                diagnostic = f"decoder exited {run.returncode}"
                stderr = concise_stderr(run.stderr)
                if stderr:
                    diagnostic += f": {stderr}"
                failures.append(diagnostic)
        except subprocess.TimeoutExpired:
            actual = "<timeout>"
            failures.append(f"decoder timed out after {args.timeout:.4f}s")
        elapsed = time.perf_counter() - started
        durations.append(elapsed)

        expected = SAMPLES[relative]
        print(f"{relative}: {actual or '<empty>'} ({elapsed:.4f}s)", flush=True)
        if actual != expected:
            failures.append(f"expected {expected}, got {actual or '<empty>'}")
        if failures:
            failed_samples += 1
            sample_failures.extend(f"{relative}: {failure}" for failure in failures)

    average = sum(durations) / len(durations)
    p95 = percentile95(durations)
    passed = len(selected) - failed_samples
    print(
        f"summary: {passed}/{len(selected)}, "
        f"average={average:.4f}s, p95={p95:.4f}s",
        flush=True,
    )

    failures = list(sample_failures)
    if average > args.average:
        failures.append(f"average {average:.4f}s > {args.average:.4f}s")
    if p95 > args.p95:
        failures.append(f"p95 {p95:.4f}s > {args.p95:.4f}s")
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
