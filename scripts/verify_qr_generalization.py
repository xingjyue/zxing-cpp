#!/usr/bin/env python3
"""Verify QR decoding against deterministic polluted and no-QR images."""

import argparse
import dataclasses
import hashlib
import json
import math
import os
import pathlib
import platform
import subprocess
import sys
import tempfile
import time

try:
    import cv2
    import numpy as np
except (ImportError, OSError, ValueError) as import_error:
    cv2 = None
    np = None
    DEPENDENCY_ERROR = import_error
else:
    DEPENDENCY_ERROR = None
CV2_ERROR = cv2.error if cv2 is not None else RuntimeError


@dataclasses.dataclass(frozen=True)
class Source:
    path: str
    payload: str
    target_side: int
    category: str


POSITIVE_SOURCES = (
    Source("version1/123.png", "123", 400, "synthetic-v1"),
    Source("version1/123_destroy1.png", "123", 400, "damaged-v1"),
    Source("print/1.png", "333", 400, "print-333"),
    Source("print/4.png", "123", 400, "print-123"),
    Source("print/8.png", "123", 400, "print-123-alt"),
    Source("Rotated/123_rotated1.png", "123", 400, "rotated-123"),
    Source("version5/1.png", "version5", 400, "version-5"),
    Source("Rotated/rotated7.png", "version10", 400, "rotated-version-10"),
    Source("version10/1.png", "version10", 400, "version-10"),
)

BASELINE_SEED = 20260728
TUNING_SEED = 20260729
HOLDOUT_SEED = 20260730
CORPUS_SEEDS = {
    "baseline": BASELINE_SEED,
    "tuning": TUNING_SEED,
    "holdout": HOLDOUT_SEED,
}
DEFAULT_HOLDOUT_COUNT = 100
DEFAULT_NEGATIVE_COUNT = 50
GENERATOR_VERSION = "2"
GUARD_RATIO = 0.20
GUARD_VALUE = 255
HOLDOUT_CONFIRMATION_ENV = "ZXING_QR_HOLDOUT_CONFIRMED"


@dataclasses.dataclass(frozen=True)
class DecodeResult:
    text: str
    elapsed: float
    error: str


@dataclasses.dataclass(frozen=True)
class RunSummary:
    positive_passes: int
    false_positives: int
    durations: tuple
    errors: tuple


@dataclasses.dataclass(frozen=True)
class ReportContext:
    config_hash: str
    versions: dict


def decoded_text(stdout):
    """Return the final payload line, excluding the failure sentinel."""
    lines = [
        line.strip()
        for line in stdout.splitlines()
        if line.strip() and line.strip() != "decoding failed"
    ]
    return lines[-1] if lines else ""


def concise_stderr(stderr, limit=240):
    """Collapse decoder stderr to a bounded single-line diagnostic."""
    message = " ".join(stderr.split())
    return message if len(message) <= limit else message[: limit - 3] + "..."


def decode(binary, image_path, timeout):
    """Run the decoder once and capture payload, wall time, and process errors."""
    started = time.perf_counter()
    try:
        run = subprocess.run(
            [str(binary), str(image_path)],
            text=True,
            capture_output=True,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired:
        elapsed = time.perf_counter() - started
        return DecodeResult("", elapsed, f"timed out after {timeout:.6f}s")
    elapsed = time.perf_counter() - started
    if run.returncode != 0:
        error = f"exited {run.returncode}"
        diagnostic = concise_stderr(run.stderr)
        if diagnostic:
            error += f": {diagnostic}"
        return DecodeResult(decoded_text(run.stdout), elapsed, error)
    return DecodeResult(decoded_text(run.stdout), elapsed, "")


def percentile95(values):
    """Return the nearest-rank 95th percentile."""
    return sorted(values)[max(0, math.ceil(len(values) * 0.95) - 1)]


def resolve_binary(root, binary_argument):
    """Resolve a binary relative to the repository unless it is absolute."""
    binary = pathlib.Path(binary_argument).expanduser()
    return binary if binary.is_absolute() else root / binary


def runtime_versions():
    """Return dependency versions that affect generated pixels."""
    return {
        "python": platform.python_version(),
        "opencv": cv2.__version__,
        "numpy": np.__version__,
    }


def resolve_corpus(parser, args):
    """Resolve a named corpus seed and guard holdout access."""
    if args.corpus == "custom":
        if args.seed is None:
            parser.error("--seed is required when --corpus=custom")
    elif args.seed is not None:
        parser.error("--seed may only be used when --corpus=custom")
    else:
        args.seed = CORPUS_SEEDS[args.corpus]
    holdout_selected = args.corpus == "holdout" or args.seed == HOLDOUT_SEED
    confirmed = args.confirm_holdout or os.environ.get(HOLDOUT_CONFIRMATION_ENV) == "1"
    if holdout_selected and not confirmed:
        parser.error(
            "holdout requires --confirm-holdout or "
            f"{HOLDOUT_CONFIRMATION_ENV}=1"
        )
    if holdout_selected:
        args.corpus = "holdout"


def validate_args(parser, args, root):
    """Reject invalid dependencies, limits, files, and binaries."""
    if DEPENDENCY_ERROR is not None:
        parser.error(
            "test dependencies unavailable; install OpenCV and NumPy: "
            f"{DEPENDENCY_ERROR}"
        )
    if args.positive_count <= 0:
        parser.error("--positive-count must be a positive integer")
    if args.negative_count < 0:
        parser.error("--negative-count must be a non-negative integer")
    if not math.isfinite(args.minimum_positive_rate) or not (
        0 <= args.minimum_positive_rate <= 1
    ):
        parser.error("--minimum-positive-rate must be between 0 and 1")
    for option, value, allow_zero in (
        ("--average", args.average, True),
        ("--p95", args.p95, True),
        ("--timeout", args.timeout, False),
    ):
        valid = math.isfinite(value) and (value >= 0 if allow_zero else value > 0)
        if not valid:
            comparison = "non-negative" if allow_zero else "positive"
            parser.error(f"{option} must be a finite {comparison} number")
    if args.timeout > 5:
        parser.error("--timeout must be at most 5 seconds")
    missing = [
        source.path
        for source in POSITIVE_SOURCES
        if not (root / "qr_fig" / source.path).is_file()
    ]
    if missing:
        parser.error("missing positive source file(s): " + ", ".join(missing))
    binary = resolve_binary(root, args.binary)
    if not binary.is_file():
        parser.error(f"decoder binary does not exist: {binary}")
    if not os.access(binary, os.X_OK):
        parser.error(f"decoder binary is not executable: {binary}")
    return binary


def file_sha256(path):
    """Return an exact source-file SHA-256."""
    digest = hashlib.sha256()
    with path.open("rb") as source_file:
        for chunk in iter(lambda: source_file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def configuration_hash(args, root, versions):
    """Hash every stable input that defines generated corpus pixels."""
    sources = []
    for source in POSITIVE_SOURCES:
        metadata = dataclasses.asdict(source)
        metadata["sha256"] = file_sha256(root / "qr_fig" / source.path)
        sources.append(metadata)
    configuration = {
        "generator_version": GENERATOR_VERSION,
        "corpus": args.corpus,
        "seed": args.seed,
        "positive_count": args.positive_count,
        "negative_count": args.negative_count,
        "sources": sources,
        "fixed_transform": {
            "resize": "longest-side-round-area-or-cubic",
            "guard_ratio_per_side": GUARD_RATIO,
            "guard_value": GUARD_VALUE,
        },
        "versions": versions,
    }
    serialized = json.dumps(configuration, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(serialized.encode("utf-8")).hexdigest()


def generation_error_message(args, error):
    """Hide individual paths whenever protected holdout data is selected."""
    if args.corpus == "holdout":
        return (
            f"generation failed: {type(error).__name__} "
            "(details and paths suppressed for holdout)"
        )
    return f"generation failed: {error}"


def write_image(path, image):
    """Write a generated PNG or raise a clear error."""
    if not cv2.imwrite(str(path), image):
        raise RuntimeError(f"could not write generated image: {path}")


def resized_patch(image, target_side):
    """Resize an image so its longest side equals the requested bound."""
    height, width = image.shape[:2]
    scale = target_side / max(width, height)
    dimensions = (
        max(1, round(width * scale)),
        max(1, round(height * scale)),
    )
    interpolation = cv2.INTER_AREA if scale < 1 else cv2.INTER_CUBIC
    return cv2.resize(image, dimensions, interpolation=interpolation)


def guarded_base(image, source):
    """Apply one fixed resize and a white guard around the whole source bitmap."""
    patch = resized_patch(image, source.target_side)
    guard = math.ceil(max(patch.shape) * GUARD_RATIO)
    return cv2.copyMakeBorder(
        patch,
        guard,
        guard,
        guard,
        guard,
        cv2.BORDER_CONSTANT,
        value=GUARD_VALUE,
    )


def prepare_bases(root, binary, timeout, temporary_root):
    """Build and verify each fixed guarded base exactly once."""
    prepared = {}
    for source_index, source in enumerate(POSITIVE_SOURCES):
        source_path = root / "qr_fig" / source.path
        image = cv2.imread(str(source_path), cv2.IMREAD_GRAYSCALE)
        if image is None:
            raise RuntimeError(f"OpenCV could not read positive source: {source_path}")
        base = guarded_base(image, source)
        path = temporary_root / f"base_{source_index:02d}.png"
        write_image(path, base)
        result = decode(binary, path, timeout)
        if result.error or result.text != source.payload:
            actual = result.error or result.text or "<empty>"
            raise RuntimeError(
                f"fixed guarded base failed for {source.path}: "
                f"expected {source.payload}, got {actual}"
            )
        prepared[source] = base
    return prepared


def outside_zones(width, height, protected):
    """Return non-empty rectangles wholly outside a protected rectangle."""
    left, top, right, bottom = protected
    zones = [
        (0, 0, width, top),
        (0, bottom, width, height),
        (0, top, left, bottom),
        (right, top, width, bottom),
    ]
    return [zone for zone in zones if zone[2] - zone[0] >= 12 and zone[3] - zone[1] >= 12]


def random_box(rng, zones, minimum, maximum, elongated=False):
    """Choose a rectangle contained in one unprotected zone."""
    viable = [
        zone
        for zone in zones
        if zone[2] - zone[0] >= minimum and zone[3] - zone[1] >= minimum
    ]
    zone = viable[int(rng.integers(len(viable)))]
    zone_width, zone_height = zone[2] - zone[0], zone[3] - zone[1]
    if elongated:
        horizontal = bool(rng.integers(2))
        box_width = rng.integers(minimum, min(maximum, zone_width) + 1)
        box_height = rng.integers(5, min(24, zone_height) + 1)
        if not horizontal:
            box_width, box_height = box_height, box_width
            box_width = min(box_width, zone_width)
            box_height = min(box_height, zone_height)
    else:
        side = int(rng.integers(minimum, min(maximum, zone_width, zone_height) + 1))
        box_width = box_height = side
    x = int(rng.integers(zone[0], zone[2] - box_width + 1))
    y = int(rng.integers(zone[1], zone[3] - box_height + 1))
    return x, y, x + box_width, y + box_height


def copy_outside(canvas, polluted, protected):
    """Copy changed pixels while preserving every pixel in the protected area."""
    outside = np.ones(canvas.shape, dtype=bool)
    outside[protected[1] : protected[3], protected[0] : protected[2]] = False
    canvas[outside] = polluted[outside]


def draw_positive_pollution(canvas, protected, rng):
    """Draw varied high-contrast objects without entering the source patch."""
    height, width = canvas.shape[:2]
    zones = outside_zones(width, height, protected)
    polluted = canvas.copy()
    for _ in range(7):
        x1, y1, x2, y2 = random_box(rng, zones, 35, 180, elongated=True)
        cv2.rectangle(polluted, (x1, y1), (x2, y2), int(rng.integers(0, 130)), -1)
    for _ in range(5):
        x1, y1, x2, y2 = random_box(rng, zones, 20, 100)
        color = int(rng.integers(0, 150))
        thickness = -1 if rng.random() < 0.5 else int(rng.integers(3, 12))
        cv2.rectangle(polluted, (x1, y1), (x2, y2), color, thickness)
    for _ in range(5):
        x1, y1, x2, y2 = random_box(rng, zones, 18, 85)
        center = ((x1 + x2) // 2, (y1 + y2) // 2)
        cv2.circle(
            polluted, center, (x2 - x1) // 2, int(rng.integers(0, 150)), -1
        )
    for _ in range(18):
        zone = zones[int(rng.integers(len(zones)))]
        point1 = (
            int(rng.integers(zone[0], zone[2])),
            int(rng.integers(zone[1], zone[3])),
        )
        point2 = (
            int(rng.integers(zone[0], zone[2])),
            int(rng.integers(zone[1], zone[3])),
        )
        cv2.line(
            polluted,
            point1,
            point2,
            int(rng.integers(0, 150)),
            int(rng.integers(2, 8)),
        )
    copy_outside(canvas, polluted, protected)


def add_outside_imaging_effects(canvas, protected, rng):
    """Apply mild gradient, sensor noise, and blur outside the patch."""
    height, width = canvas.shape[:2]
    x_gradient = np.linspace(-1, 1, width, dtype=np.float32)
    y_gradient = np.linspace(-1, 1, height, dtype=np.float32)[:, None]
    gradient = (
        x_gradient * rng.uniform(-12, 12) + y_gradient * rng.uniform(-12, 12)
    )
    noise = rng.normal(0, rng.uniform(1.0, 4.0), (height, width))
    adjusted = canvas.astype(np.float32) + gradient + noise
    polluted = np.clip(adjusted, 0, 255).astype(np.uint8)
    if rng.random() < 0.7:
        kernel = 3 if rng.random() < 0.8 else 5
        polluted = cv2.GaussianBlur(polluted, (kernel, kernel), 0)
    copy_outside(canvas, polluted, protected)


def make_positive(base, rng):
    """Embed a fixed guarded base while keeping all pollution outside it."""
    patch_height, patch_width = base.shape
    margins = [int(rng.integers(140, 361)) for _ in range(4)]
    width = patch_width + margins[0] + margins[2]
    height = patch_height + margins[1] + margins[3]
    canvas = np.full((height, width), int(rng.integers(220, 251)), np.uint8)
    x, y = margins[0], margins[1]
    protected = (x, y, x + patch_width, y + patch_height)
    # The >=20% guard surrounds the entire source bitmap, so no QR localization
    # is needed and pollution cannot approach source pixels or their quiet zone.
    canvas[y : y + patch_height, x : x + patch_width] = base
    draw_positive_pollution(canvas, protected, rng)
    add_outside_imaging_effects(canvas, protected, rng)
    if not np.array_equal(
        canvas[y : y + patch_height, x : x + patch_width], base
    ):
        raise RuntimeError("positive generator modified the protected guarded base")
    return canvas


def make_negative(rng):
    """Generate a high-contrast no-QR image with several distractor families."""
    height = int(rng.integers(420, 721))
    width = int(rng.integers(520, 901))
    canvas = np.full((height, width), int(rng.integers(215, 251)), np.uint8)
    center = (int(rng.integers(90, width - 90)), int(rng.integers(90, height - 90)))
    for side, color in ((160, 20), (125, 235), (90, 30), (55, 225)):
        half = side // 2
        cv2.rectangle(
            canvas,
            (center[0] - half, center[1] - half),
            (center[0] + half, center[1] + half),
            color,
            -1,
        )
    for _ in range(10):
        x1 = int(rng.integers(0, width - 40))
        y1 = int(rng.integers(0, height - 12))
        horizontal = bool(rng.integers(2))
        x2 = min(width - 1, x1 + int(rng.integers(40, 220)))
        y2 = min(height - 1, y1 + int(rng.integers(8, 26)))
        if not horizontal:
            x2 = min(width - 1, x1 + int(rng.integers(8, 26)))
            y2 = min(height - 1, y1 + int(rng.integers(40, 220)))
        cv2.rectangle(canvas, (x1, y1), (x2, y2), int(rng.integers(0, 120)), -1)
    cell = int(rng.integers(10, 23))
    checker_x = int(rng.integers(0, max(1, width - 8 * cell)))
    checker_y = int(rng.integers(0, max(1, height - 8 * cell)))
    for row in range(8):
        for column in range(8):
            if (row + column) % 2 == 0:
                cv2.rectangle(
                    canvas,
                    (checker_x + column * cell, checker_y + row * cell),
                    (checker_x + (column + 1) * cell, checker_y + (row + 1) * cell),
                    25,
                    -1,
                )
    for _ in range(30):
        point1 = (int(rng.integers(width)), int(rng.integers(height)))
        point2 = (int(rng.integers(width)), int(rng.integers(height)))
        cv2.line(canvas, point1, point2, int(rng.integers(0, 160)), int(rng.integers(1, 6)))
    return canvas


def balanced_sources(count, seed, rng):
    """Balance sources and rotate surplus ownership from the corpus seed."""
    source_count = len(POSITIVE_SOURCES)
    quotient, remainder = divmod(count, source_count)
    indices = [index for index in range(source_count) for _ in range(quotient)]
    seed_digest = hashlib.sha256(f"surplus:{seed}".encode("ascii")).digest()
    start = int.from_bytes(seed_digest[:8], "big") % source_count
    indices.extend((start + offset) % source_count for offset in range(remainder))
    indices = np.asarray(indices, dtype=np.int64)
    rng.shuffle(indices)
    return indices


def run_cases(args, root, binary, temporary_root):
    """Generate, decode, and aggregate all deterministic corpus cases."""
    rng = np.random.default_rng(args.seed)
    prepared = prepare_bases(root, binary, args.timeout, temporary_root)
    durations = []
    positive_passes = 0
    negative_false_positives = 0
    process_errors = []
    source_indices = balanced_sources(args.positive_count, args.seed, rng)
    for case_index, source_index in enumerate(source_indices):
        source = POSITIVE_SOURCES[int(source_index)]
        base = prepared[source]
        path = temporary_root / f"positive_{case_index:04d}.png"
        write_image(path, make_positive(base, rng))
        result = decode(binary, path, args.timeout)
        durations.append(result.elapsed)
        if result.error:
            process_errors.append(f"{path.name}: {result.error}")
        elif result.text == source.payload:
            positive_passes += 1
    for case_index in range(args.negative_count):
        path = temporary_root / f"negative_{case_index:04d}.png"
        write_image(path, make_negative(rng))
        result = decode(binary, path, args.timeout)
        durations.append(result.elapsed)
        if result.text:
            negative_false_positives += 1
        if result.error:
            process_errors.append(f"{path.name}: {result.error}")
    return RunSummary(
        positive_passes,
        negative_false_positives,
        tuple(durations),
        tuple(process_errors),
    )


def report_and_gate(args, summary, context):
    """Print reproducible aggregate metrics and return gate status."""
    positive_rate = summary.positive_passes / args.positive_count
    average = sum(summary.durations) / len(summary.durations)
    p95 = percentile95(summary.durations)
    print(f"corpus: {args.corpus}", flush=True)
    print(f"seed: {args.seed}", flush=True)
    print(f"configuration sha256: {context.config_hash}", flush=True)
    print(f"Python: {context.versions['python']}", flush=True)
    print(f"OpenCV: {context.versions['opencv']}", flush=True)
    print(f"NumPy: {context.versions['numpy']}", flush=True)
    print(
        f"positive: {summary.positive_passes}/{args.positive_count} "
        f"({positive_rate:.6%}, minimum {args.minimum_positive_rate:.6%})",
        flush=True,
    )
    print(
        f"negative false positives: {summary.false_positives}/{args.negative_count} "
        "(limit 0)",
        flush=True,
    )
    print(
        f"average: {average:.6f}s (limit {args.average:.6f}s)",
        flush=True,
    )
    print(f"p95: {p95:.6f}s (limit {args.p95:.6f}s)", flush=True)
    print(f"timeout: {args.timeout:.6f}s", flush=True)
    failures = []
    if args.corpus == "holdout" and summary.errors:
        failures.append(
            f"decoder process errors: {len(summary.errors)} (paths suppressed)"
        )
    else:
        failures.extend(summary.errors)
    if positive_rate < args.minimum_positive_rate:
        failures.append(
            f"positive rate {positive_rate:.6%} < "
            f"{args.minimum_positive_rate:.6%}"
        )
    if summary.false_positives:
        failures.append(
            f"negative false positives: {summary.false_positives}"
        )
    if average > args.average:
        failures.append(f"average {average:.6f}s > {args.average:.6f}s")
    if p95 > args.p95:
        failures.append(f"p95 {p95:.6f}s > {args.p95:.6f}s")
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    return 0


def build_parser():
    """Build the command-line interface without running a corpus."""
    parser = argparse.ArgumentParser(
        description="Verify deterministic QR background-pollution generalization.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--binary", default="build/zxing")
    parser.add_argument(
        "--corpus",
        choices=("baseline", "tuning", "holdout", "custom"),
        default="tuning",
    )
    parser.add_argument(
        "--seed",
        type=int,
        help="required for --corpus=custom; invalid for named corpora",
    )
    parser.add_argument(
        "--confirm-holdout",
        action="store_true",
        help="explicitly authorize the protected holdout corpus",
    )
    parser.add_argument(
        "--positive-count", type=int, default=DEFAULT_HOLDOUT_COUNT
    )
    parser.add_argument(
        "--negative-count", type=int, default=DEFAULT_NEGATIVE_COUNT
    )
    parser.add_argument("--minimum-positive-rate", type=float, default=0.95)
    parser.add_argument("--average", type=float, default=0.5)
    parser.add_argument("--p95", type=float, default=0.8)
    parser.add_argument("--timeout", type=float, default=5.0)
    return parser


def main():
    parser = build_parser()
    args = parser.parse_args()
    try:
        root = pathlib.Path(__file__).resolve().parents[1]
        resolve_corpus(parser, args)
        binary = validate_args(parser, args, root)
        versions = runtime_versions()
        config_hash = configuration_hash(args, root, versions)
        with tempfile.TemporaryDirectory(prefix="qr-generalization-") as directory:
            summary = run_cases(args, root, binary, pathlib.Path(directory))
        context = ReportContext(config_hash, versions)
        return report_and_gate(args, summary, context)
    except (
        OSError,
        ValueError,
        RuntimeError,
        subprocess.SubprocessError,
        CV2_ERROR,
    ) as error:
        print(generation_error_message(args, error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
