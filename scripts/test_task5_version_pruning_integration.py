#!/usr/bin/env python3
"""Reproduce the tuning case that exposed QR-version pruning."""

import sys

sys.dont_write_bytecode = True

import os
import pathlib
import subprocess
import tempfile

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import verify_qr_generalization as generator  # noqa: E402


FIXTURE_INDEX = 1
TUNING_CASE_COUNT = 100
FIXTURE_SOURCE = "Rotated/rotated7.png"
EXPECTED_PAYLOAD = "version10"


def run_contract(root, temporary_root):
    """Compile and run the internal Version 1-10 coverage contract."""
    output = temporary_root / "task5_version_pruning_contract_test"
    subprocess.run(
        [
            os.environ.get("CXX", "c++"),
            "-std=c++11",
            f"-I{root / 'core/src'}",
            str(root / "scripts/task5_version_pruning_contract_test.cpp"),
            str(root / "build/libzxing.a"),
            "-o",
            str(output),
        ],
        check=True,
    )
    subprocess.run([str(output)], check=True)


def generate_fixture(root, binary, temporary_root):
    """Generate tuning case 1 while preserving the corpus RNG stream."""
    prepared = generator.prepare_bases(root, binary, 5.0, temporary_root)
    rng = np.random.default_rng(generator.TUNING_SEED)
    source_indices = generator.balanced_sources(
        TUNING_CASE_COUNT, generator.TUNING_SEED, rng
    )
    selected = None
    fixture = None
    for source_index in source_indices[: FIXTURE_INDEX + 1]:
        selected = generator.POSITIVE_SOURCES[int(source_index)]
        fixture = generator.make_positive(prepared[selected], rng)
    if selected.path != FIXTURE_SOURCE or selected.payload != EXPECTED_PAYLOAD:
        raise AssertionError(
            f"tuning fixture drifted to {selected.path}:{selected.payload}"
        )
    path = temporary_root / "task5_version_pruning.png"
    generator.write_image(path, fixture)
    return path


def main():
    root = pathlib.Path(__file__).resolve().parents[1]
    binary = root / "build" / "zxing"
    if not binary.is_file():
        raise RuntimeError(f"decoder binary does not exist: {binary}")
    with tempfile.TemporaryDirectory(prefix="task5-version-pruning-") as directory:
        temporary_root = pathlib.Path(directory)
        run_contract(root, temporary_root)
        fixture = generate_fixture(root, binary, temporary_root)
        result = generator.decode(binary, fixture, 5.0)
    if result.error or result.text != EXPECTED_PAYLOAD:
        actual = result.error or result.text or "<empty>"
        raise AssertionError(
            f"expected {EXPECTED_PAYLOAD}, got {actual}"
        )
    print(
        "task5 version-pruning integration passed: "
        f"seed={generator.TUNING_SEED} index={FIXTURE_INDEX} "
        f"payload={result.text} elapsed={result.elapsed:.3f}s"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
