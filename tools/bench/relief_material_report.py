"""Prepare local relief inputs or score exported Height PNGs against displacement maps."""

import argparse
from pathlib import Path

import numpy as np
from PIL import Image


def load_map(path):
    values = np.asarray(Image.open(path))
    if values.ndim == 3:
        values = values[:, :, 0]
    divisor = 255.0 if values.dtype == np.uint8 else 65535.0
    return values.astype(np.float32) / divisor


def srgb_to_linear(rgb):
    return np.where(rgb <= 0.04045, rgb / 12.92, ((rgb + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(rgb):
    return np.where(rgb <= 0.0031308, rgb * 12.92, 1.055 * rgb ** (1.0 / 2.4) - 0.055)


def ranks(values):
    _, inverse, counts = np.unique(values, return_inverse=True, return_counts=True)
    return (np.cumsum(counts) - (counts + 1) / 2)[inverse]


def band_pass(height):
    radius = 9
    offsets = np.arange(-radius, radius + 1)
    weights = np.exp(-offsets ** 2 / 18.0)
    weights /= weights.sum()
    blurred = height
    for axis in (0, 1):
        padding = [(0, 0), (0, 0)]
        padding[axis] = (radius, radius)
        padded = np.pad(blurred, padding, mode='edge')
        windows = np.lib.stride_tricks.sliding_window_view(padded, len(weights), axis=axis)
        blurred = np.einsum('ijk,k->ij', windows, weights)
    return height - blurred


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('sources', type=Path)
parser.add_argument('inputs', type=Path)
parser.add_argument('exports', nargs='?', type=Path)
args = parser.parse_args()
args.inputs.mkdir(parents=True, exist_ok=True)
diffuse_maps = sorted(args.sources.glob('*_Diffuse.png'))
if not diffuse_maps:
    parser.error(f'no *_Diffuse.png files in {args.sources}')
for diffuse in diffuse_maps:
    name = diffuse.name.removesuffix('_Diffuse.png')
    rgb = np.asarray(Image.open(diffuse).convert('RGB'), dtype=np.float32) / 255.0
    height = load_map(args.sources / f'{name}_Displacement.png')
    ambient = load_map(args.sources / f'{name}_AO.png')
    if rgb.shape[:2] != height.shape or rgb.shape[:2] != ambient.shape:
        raise ValueError(f'{name}: map dimensions differ')
    for target, image in {
        'diffuse': rgb,
        'ao': linear_to_srgb(srgb_to_linear(rgb) * (0.6 + 0.4 * ambient[:, :, None])),
    }.items():
        stem = f'{name}_{target}'
        path = args.inputs / f'{stem}.png'
        if not args.exports:
            Image.fromarray(np.uint8(np.rint(np.clip(image, 0.0, 1.0) * 255))).save(path)
            continue
        # --relief-image exports the final pan window (horizontal offset 32 - 12 + 23).
        # Ignore an additional eight pixels at its border to avoid filter boundary effects.
        estimate = load_map(args.exports / f'{stem}_s050_height.png')[8:-8, 8:-8] * 2 - 1
        truth = band_pass(height)[40:-40, 51:-29]
        if estimate.shape != truth.shape:
            raise ValueError(f'{name}: estimate and reference windows differ')
        a, b = estimate.ravel(), truth.ravel()
        significant = (np.abs(a) > 0.02) & (np.abs(b) > 0.05)
        score = np.corrcoef(ranks(a), ranks(b))[0, 1]
        sign = np.mean((a[significant] > 0) == (b[significant] > 0)) if np.any(significant) else float('nan')
        print(f'{name:24s} {target:7s} Spearman {score:+.4f}  sign agreement {sign:.4f}')
