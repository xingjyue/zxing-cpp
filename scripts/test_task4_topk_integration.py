#!/usr/bin/env python3
"""Exercise real bounded Top-K decoding on one baseline polluted image."""

import pathlib
import subprocess
import tempfile

import numpy as np

import verify_qr_generalization as generator


FIXTURE_INDEX = 51
BASELINE_CASE_COUNT = 100
FIXTURE_SOURCE = "version1/123_destroy1.png"
EXPECTED_PAYLOAD = "123"


def prepare_bases(root):
    """Build deterministic guarded source images without running a corpus."""
    if generator.DEPENDENCY_ERROR is not None:
        raise RuntimeError(
            "test dependencies unavailable: "
            f"{generator.DEPENDENCY_ERROR}"
        )
    bases = {}
    for source in generator.POSITIVE_SOURCES:
        image = generator.cv2.imread(
            str(root / "qr_fig" / source.path),
            generator.cv2.IMREAD_GRAYSCALE,
        )
        if image is None:
            raise RuntimeError(f"could not load fixture source: {source.path}")
        bases[source] = generator.guarded_base(image, source)
    return bases


def generate_fixture(root, output):
    """Reproduce baseline case 51 while preserving the generator RNG stream."""
    rng = np.random.default_rng(generator.BASELINE_SEED)
    source_indices = generator.balanced_sources(
        BASELINE_CASE_COUNT, generator.BASELINE_SEED, rng
    )
    bases = prepare_bases(root)
    selected = None
    fixture = None
    for index, source_index in enumerate(source_indices[: FIXTURE_INDEX + 1]):
        selected = generator.POSITIVE_SOURCES[int(source_index)]
        fixture = generator.make_positive(bases[selected], rng)
    if selected.path != FIXTURE_SOURCE or selected.payload != EXPECTED_PAYLOAD:
        raise AssertionError(
            f"baseline fixture drifted to {selected.path}:{selected.payload}"
        )
    generator.write_image(output, fixture)


def decode_statuses(stdout):
    """Extract the ordered hybrid/global result for each source attempt."""
    prefixes = ("Hybrid binarizer ", "Global binarizer ")
    return [
        line.strip()
        for line in stdout.splitlines()
        if line.strip().startswith(prefixes)
    ]


def main():
    root = pathlib.Path(__file__).resolve().parents[1]
    binary = root / "build" / "zxing"
    if not binary.is_file():
        raise RuntimeError(f"decoder binary does not exist: {binary}")
    with tempfile.TemporaryDirectory(prefix="task4-topk-") as directory:
        fixture = pathlib.Path(directory) / "baseline_topk.png"
        generate_fixture(root, fixture)
        run = subprocess.run(
            [str(binary), "--verbose", str(fixture)],
            text=True,
            capture_output=True,
            timeout=5,
            check=False,
        )
    statuses = decode_statuses(run.stdout)
    expected = [
        "Hybrid binarizer failed: zxing::ReaderException: No code detected",
        "Global binarizer failed: zxing::ReaderException: No code detected",
        "Hybrid binarizer failed: zxing::ReaderException: No code detected",
        "Global binarizer failed: zxing::ReaderException: No code detected",
        "Hybrid binarizer succeeded:",
        "Global binarizer succeeded:",
    ]
    payloads = [
        line.strip()
        for line in run.stdout.splitlines()
        if line.strip() == EXPECTED_PAYLOAD
    ]
    if run.returncode != 0 or statuses != expected or payloads != ["123", "123"]:
        raise AssertionError(
            "unexpected bounded decode sequence\n"
            f"returncode={run.returncode}\nstatuses={statuses}\n"
            f"payloads={payloads}\nstderr={run.stderr.strip()}"
        )
    print(
        "task4 Top-K integration passed: "
        f"seed={generator.BASELINE_SEED} index={FIXTURE_INDEX} "
        "original=fail candidate1=fail candidate2=123 attempts=3"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
