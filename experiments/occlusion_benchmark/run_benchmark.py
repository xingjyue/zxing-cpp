#!/usr/bin/env python3
import argparse
import json
import random
import string
import subprocess
from pathlib import Path

import qrcode
from PIL import Image, ImageDraw


def decode(zxing_bin: str, image_path: Path) -> str:
    p = subprocess.run([zxing_bin, '--try-harder', str(image_path)], capture_output=True, text=True)
    out = (p.stdout + p.stderr).strip().splitlines()
    return out[-1].strip() if out else ''


def apply_case(img: Image.Image, case: str, frac: float, box: int, border: int, finder_modules: int) -> Image.Image:
    img = img.copy()
    draw = ImageDraw.Draw(img)
    start = border * box
    end = (border + finder_modules) * box
    size = end - start

    if case == 'clean':
        return img
    if case == 'finder_white':
        cs = start + (size * (1 - frac)) / 2
        ce = end - (size * (1 - frac)) / 2
        draw.rectangle([cs, cs, ce, ce], fill='white')
    elif case == 'finder_black':
        cs = start + (size * (1 - frac)) / 2
        ce = end - (size * (1 - frac)) / 2
        draw.rectangle([cs, cs, ce, ce], fill='black')
    elif case == 'left_edge_white':
        w = int(img.width * frac)
        draw.rectangle([0, 0, w, img.height], fill='white')
    elif case == 'top_edge_white':
        h = int(img.height * frac)
        draw.rectangle([0, 0, img.width, h], fill='white')
    elif case == 'finder_gray':
        cs = start + (size * (1 - frac)) / 2
        ce = end - (size * (1 - frac)) / 2
        draw.rectangle([cs, cs, ce, ce], fill=(190, 190, 190))
    elif case == 'finder_checker':
        cs = int(start + (size * (1 - frac)) / 2)
        ce = int(end - (size * (1 - frac)) / 2)
        for y in range(cs, ce, 4):
            for x in range(cs, ce, 4):
                if ((x + y) // 4) % 2 == 0:
                    draw.rectangle([x, y, x + 3, y + 3], fill='white')
    elif case == 'left_edge_gray':
        w = int(img.width * frac)
        draw.rectangle([0, 0, w, img.height], fill=(190, 190, 190))
    elif case == 'top_edge_gray':
        h = int(img.height * frac)
        draw.rectangle([0, 0, img.width, h], fill=(190, 190, 190))
    else:
        raise ValueError(f'unknown case: {case}')

    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--output', required=True)
    ap.add_argument('--zxing-bin', default='/workspace/build/zxing')
    ap.add_argument('--seed', type=int, default=1234)
    ap.add_argument('--count', type=int, default=20)
    args = ap.parse_args()

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)

    box = 10
    border = 4
    finder_modules = 7

    rng = random.Random(args.seed)
    payloads = [f'OCC_BM_{i}_' + ''.join(rng.choice(string.ascii_uppercase + string.digits) for _ in range(8)) for i in range(args.count)]

    cases = [
        ('clean', None),
        ('finder_white_20', ('finder_white', 0.20)),
        ('finder_white_35', ('finder_white', 0.35)),
        ('finder_white_50', ('finder_white', 0.50)),
        ('finder_white_70', ('finder_white', 0.70)),
        ('finder_black_20', ('finder_black', 0.20)),
        ('finder_black_35', ('finder_black', 0.35)),
        ('finder_black_50', ('finder_black', 0.50)),
        ('finder_black_70', ('finder_black', 0.70)),
        ('left_edge_white_20', ('left_edge_white', 0.20)),
        ('left_edge_white_35', ('left_edge_white', 0.35)),
        ('left_edge_white_50', ('left_edge_white', 0.50)),
        ('top_edge_white_20', ('top_edge_white', 0.20)),
        ('top_edge_white_35', ('top_edge_white', 0.35)),
        ('top_edge_white_50', ('top_edge_white', 0.50)),
    ]

    records = []
    summary = {name: {'total': 0, 'ok': 0} for name, _ in cases}

    for payload in payloads:
        qr = qrcode.QRCode(version=3, error_correction=qrcode.constants.ERROR_CORRECT_H, box_size=box, border=border)
        qr.add_data(payload)
        qr.make(fit=False)
        base = qr.make_image(fill_color='black', back_color='white').convert('RGB')

        for case_name, spec in cases:
            if spec is None:
                img = base
            else:
                img = apply_case(base, spec[0], spec[1], box, border, finder_modules)

            tmp = output.parent / f'__tmp_{case_name}.png'
            img.save(tmp)
            got = decode(args.zxing_bin, tmp)
            tmp.unlink(missing_ok=True)
            ok = got == payload

            records.append({'payload': payload, 'case': case_name, 'decoded': got, 'ok': ok})
            summary[case_name]['total'] += 1
            summary[case_name]['ok'] += int(ok)

    for k in summary:
        t = summary[k]['total']
        o = summary[k]['ok']
        summary[k]['rate'] = o / t if t else 0.0

    output.write_text(json.dumps({'summary': summary, 'records': records}, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
