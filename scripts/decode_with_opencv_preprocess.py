#!/usr/bin/env python3
"""Decode QR images with an OpenCV preprocessing fallback.

The script keeps the existing zxing CLI as the decoder. For each input image it
first runs zxing directly. If that fails, it writes OpenCV-generated candidates
with adaptive thresholding, morphology, and perspective correction, then retries
zxing on those candidates.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import cv2
import numpy as np


def run_zxing(zxing_bin: Path, image: Path, try_harder: bool) -> str | None:
    cmd = [str(zxing_bin)]
    if try_harder:
        cmd.append("--try-harder")
    cmd.append(str(image))
    completed = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    lines = [line.strip() for line in completed.stdout.splitlines() if line.strip()]
    payload = "\n".join(line for line in lines if line != "decoding failed")
    return payload or None


def order_points(points: np.ndarray) -> np.ndarray:
    pts = points.reshape(4, 2).astype("float32")
    sums = pts.sum(axis=1)
    diffs = np.diff(pts, axis=1).reshape(4)
    ordered = np.zeros((4, 2), dtype="float32")
    ordered[0] = pts[np.argmin(sums)]
    ordered[2] = pts[np.argmax(sums)]
    ordered[1] = pts[np.argmin(diffs)]
    ordered[3] = pts[np.argmax(diffs)]
    return ordered


def warp_quad(image: np.ndarray, quad: np.ndarray, margin_ratio: float = 0.08) -> np.ndarray:
    rect = order_points(quad)
    width_a = np.linalg.norm(rect[2] - rect[3])
    width_b = np.linalg.norm(rect[1] - rect[0])
    height_a = np.linalg.norm(rect[1] - rect[2])
    height_b = np.linalg.norm(rect[0] - rect[3])
    content_size = int(max(width_a, width_b, height_a, height_b))
    content_size = max(content_size, 64)
    margin = max(8, int(content_size * margin_ratio))
    side = content_size + margin * 2
    dst = np.array(
        [
            [margin, margin],
            [side - margin - 1, margin],
            [side - margin - 1, side - margin - 1],
            [margin, side - margin - 1],
        ],
        dtype="float32",
    )
    transform = cv2.getPerspectiveTransform(rect, dst)
    return cv2.warpPerspective(image, transform, (side, side), borderValue=255)


def adaptive_binary(gray: np.ndarray) -> np.ndarray:
    clahe = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8))
    enhanced = clahe.apply(gray)
    blurred = cv2.GaussianBlur(enhanced, (3, 3), 0)
    block = max(31, (min(gray.shape[:2]) // 20) | 1)
    if block % 2 == 0:
        block += 1
    binary = cv2.adaptiveThreshold(
        blurred,
        255,
        cv2.ADAPTIVE_THRESH_GAUSSIAN_C,
        cv2.THRESH_BINARY,
        block,
        5,
    )
    kernel = cv2.getStructuringElement(cv2.MORPH_RECT, (3, 3))
    return cv2.morphologyEx(binary, cv2.MORPH_CLOSE, kernel)


def detector_quad(gray: np.ndarray) -> np.ndarray | None:
    detector = cv2.QRCodeDetector()
    ok, points = detector.detect(gray)
    if ok and points is not None and len(points.reshape(-1, 2)) == 4:
        return points.reshape(4, 2)
    return None


def contour_quad(binary: np.ndarray) -> np.ndarray | None:
    foreground = 255 - binary
    kernel = cv2.getStructuringElement(cv2.MORPH_RECT, (5, 5))
    foreground = cv2.morphologyEx(foreground, cv2.MORPH_CLOSE, kernel)
    points = cv2.findNonZero(foreground)
    if points is None or len(points) < 16:
        return None
    rect = cv2.minAreaRect(points)
    box = cv2.boxPoints(rect)
    width, height = rect[1]
    if width < 32 or height < 32:
        return None
    return box.astype("float32")


def foreground_bbox(binary: np.ndarray) -> tuple[int, int, int, int] | None:
    foreground = 255 - binary
    points = cv2.findNonZero(foreground)
    if points is None or len(points) < 16:
        return None
    x, y, w, h = cv2.boundingRect(points)
    if w < 32 or h < 32:
        return None
    return x, y, w, h


def square_crop_with_quiet_zone(image: np.ndarray, bbox: tuple[int, int, int, int]) -> np.ndarray:
    x, y, w, h = bbox
    side = max(w, h)
    margin = max(16, side // 8)
    center_x = x + w / 2.0
    center_y = y + h / 2.0
    left = int(center_x - side / 2.0 - margin)
    top = int(center_y - side / 2.0 - margin)
    right = int(center_x + side / 2.0 + margin)
    bottom = int(center_y + side / 2.0 + margin)

    pad_left = max(0, -left)
    pad_top = max(0, -top)
    pad_right = max(0, right - image.shape[1])
    pad_bottom = max(0, bottom - image.shape[0])
    padded = cv2.copyMakeBorder(
        image,
        pad_top,
        pad_bottom,
        pad_left,
        pad_right,
        borderType=cv2.BORDER_CONSTANT,
        value=255,
    )
    left += pad_left
    right += pad_left
    top += pad_top
    bottom += pad_top
    return padded[top:bottom, left:right]


def write_candidates(image_path: Path, out_dir: Path) -> list[Path]:
    source = cv2.imread(str(image_path), cv2.IMREAD_COLOR)
    if source is None:
        raise ValueError(f"OpenCV could not read {image_path}")

    gray = cv2.cvtColor(source, cv2.COLOR_BGR2GRAY)
    binary = adaptive_binary(gray)
    candidates: list[Path] = []

    adaptive_path = out_dir / f"{image_path.stem}.adaptive.png"
    cv2.imwrite(str(adaptive_path), binary)
    candidates.append(adaptive_path)

    bbox = foreground_bbox(binary)
    if bbox is not None:
        crop_gray = square_crop_with_quiet_zone(gray, bbox)
        crop_binary = adaptive_binary(crop_gray)
        crop_gray_path = out_dir / f"{image_path.stem}.bbox-crop-gray.png"
        crop_binary_path = out_dir / f"{image_path.stem}.bbox-crop-binary.png"
        cv2.imwrite(str(crop_gray_path), crop_gray)
        cv2.imwrite(str(crop_binary_path), crop_binary)
        candidates.extend([crop_gray_path, crop_binary_path])

    quads: list[tuple[str, np.ndarray]] = []
    detected = detector_quad(gray)
    if detected is not None:
        quads.append(("detector", detected))
    contoured = contour_quad(binary)
    if contoured is not None:
        quads.append(("contour", contoured))

    for name, quad in quads:
        warped_gray = warp_quad(gray, quad)
        warped_binary = adaptive_binary(warped_gray)
        gray_path = out_dir / f"{image_path.stem}.{name}.warp-gray.png"
        binary_path = out_dir / f"{image_path.stem}.{name}.warp-binary.png"
        cv2.imwrite(str(gray_path), warped_gray)
        cv2.imwrite(str(binary_path), warped_binary)
        candidates.extend([gray_path, binary_path])

    return candidates


def decode_preprocessed(zxing_bin: Path, image_path: Path, out_dir: Path, try_harder: bool) -> tuple[str | None, str]:
    candidates = write_candidates(image_path, out_dir)
    for candidate in candidates:
        decoded = run_zxing(zxing_bin, candidate, try_harder)
        if decoded:
            return decoded, candidate.name
    return None, "failed"


def decode_one(
    zxing_bin: Path,
    image_path: Path,
    out_dir: Path,
    try_harder: bool,
    preprocess_first: bool,
) -> tuple[str | None, str]:
    if preprocess_first:
        decoded, source = decode_preprocessed(zxing_bin, image_path, out_dir, try_harder)
        if decoded:
            return decoded, source

    direct = run_zxing(zxing_bin, image_path, try_harder)
    if direct:
        return direct, "direct"

    if not preprocess_first:
        return decode_preprocessed(zxing_bin, image_path, out_dir, try_harder)
    return None, "failed"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("images", nargs="+", type=Path)
    parser.add_argument("--zxing-bin", type=Path, default=Path("build/zxing"))
    parser.add_argument("--out-dir", type=Path)
    parser.add_argument("--keep", action="store_true", help="keep temporary OpenCV candidate images")
    parser.add_argument("--no-try-harder", action="store_true", help="do not pass --try-harder to zxing")
    parser.add_argument(
        "--preprocess-first",
        action="store_true",
        help="try OpenCV candidates before direct zxing decode",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    zxing_bin = args.zxing_bin
    if not zxing_bin.exists():
        print(f"Missing zxing executable: {zxing_bin}", file=sys.stderr)
        return 2

    temp_dir = None
    if args.out_dir:
        out_dir = args.out_dir
        out_dir.mkdir(parents=True, exist_ok=True)
    else:
        temp_dir = Path(tempfile.mkdtemp(prefix="zxing-opencv-"))
        out_dir = temp_dir

    try:
        ok = True
        for image in args.images:
            decoded, source = decode_one(
                zxing_bin,
                image,
                out_dir,
                not args.no_try_harder,
                args.preprocess_first,
            )
            if decoded:
                print(f"{image}: {decoded} [{source}]")
            else:
                print(f"{image}: decoding failed", file=sys.stderr)
                ok = False
        return 0 if ok else 1
    finally:
        if temp_dir and not args.keep:
            shutil.rmtree(temp_dir, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
