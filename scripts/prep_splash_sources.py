#!/usr/bin/env python3
"""Prepare on-black, cropped splash source PNGs from the raw brand assets.

The opening splash + screensaver render the SeedSigner wordmark and the HRF
partner logo on a solid black background.  This script turns the raw brand
artwork into clean, opaque, tightly-cropped sources that png_to_lvgl.py can then
bake at any height with no surprises.

ORDER MATTERS — flatten transparency BEFORE any crop/resample:
  * LANCZOS resampling of an RGBA image blends un-premultiplied RGB from fully
    transparent pixels into opaque edges -> dark fringing.
  * A crop of a still-transparent image leaves a hard transparent border that
    fringes the same way on the next resample.
  * png_to_lvgl.py itself resizes-then-composites, so feeding it a pre-flattened
    OPAQUE source makes its internal composite a no-op and its resize clean.
Flattening on black first eliminates all transparency, so every downstream
resample happens on pure RGB and edges fade correctly to black.

Pipeline:
  seedsigner_logo_transparent.png
      -> composite on black -> crop to content bbox -> images/src/seedsigner_logo_on_black.png
  HRF-Logo.webp  (HRF's on-white version: pink "H" + BLACK text)
      -> recolor dark ink -> white (keep the pink, keep alpha)
      -> composite on black -> crop to content bbox -> images/src/hrf_logo_on_black.png
The HRF recolor MUST precede flatten: its ink is ~(1,1,1); flattening first would
merge it into the black background and make it unrecoverable.
"""

import os

import numpy as np
from PIL import Image, ImageChops
from scipy import ndimage

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIR = os.path.join(REPO, "components", "seedsigner", "images", "src")

# Pixels darker than this (max channel) are treated as background for cropping.
# Low, so anti-aliased edges that fade toward black are preserved.
CROP_THRESHOLD = 16
# Below this RGB chroma (max-min) a pixel is "neutral/black ink" -> recolor white.
# The pink "H" has chroma ~184 so it is always kept.
CHROMA_THRESHOLD = 40


def flatten_on_black(img: Image.Image) -> Image.Image:
    """Alpha-composite onto black; return an opaque RGB image."""
    rgba = img.convert("RGBA")
    bg = Image.new("RGB", rgba.size, (0, 0, 0))
    bg.paste(rgba, mask=rgba.split()[3])
    return bg


def crop_to_content(img: Image.Image, threshold: int = CROP_THRESHOLD) -> Image.Image:
    """Crop away the pure-black margins (bbox of pixels whose max channel > threshold)."""
    r, g, b = img.convert("RGB").split()
    max_channel = ImageChops.lighter(ImageChops.lighter(r, g), b)
    mask = max_channel.point(lambda p: 255 if p > threshold else 0)
    bbox = mask.getbbox()
    return img.crop(bbox) if bbox else img


def remove_exterior_background(img: Image.Image) -> Image.Image:
    """Strip the wordmark source's leftover white background to transparency.

    The original artwork was drawn on a WHITE background and the supplied
    transparency is an imperfect after-the-fact extraction: a thin ring of white
    (and near-white peachy) background remains around the orange pill — opaque at
    the straight top/bottom edges (a full 1px white row) and partial along the
    rounded-corner stair-steps. Composited on black it reads as a light fringe and
    bleeds into a lighter edge when downscaled. The correct fix is to finish the
    extraction: the leftover white belongs to the background, so make it
    TRANSPARENT (it then composites cleanly to black, orange -> black edges).

    Interior white is real design (the SEED box, the SIGNER text) and must be
    kept, so background is identified by CONNECTIVITY: flood from the image border
    through "background-like" pixels (transparent OR whitish) and only clear what
    is reachable. The solid orange ring (g<150) is not background-like, so it
    walls off the interior white — which is therefore never reached and stays.
    """
    rgba = np.array(img.convert("RGBA"))
    r, g, b, a = rgba[..., 0], rgba[..., 1], rgba[..., 2], rgba[..., 3]

    # Background-like = transparent, or bright+desaturated (white/peachy remnant).
    # Orange (255,115,0) has g=115 (<150), so it is never background-like.
    bg_like = (a < 40) | ((g >= 150) & (b >= 100))

    # Connected-component label (8-connectivity); clear components touching the border.
    labels, _ = ndimage.label(bg_like, structure=np.ones((3, 3), dtype=bool))
    border = np.concatenate([labels[0, :], labels[-1, :], labels[:, 0], labels[:, -1]])
    border_labels = np.unique(border)
    border_labels = border_labels[border_labels != 0]
    exterior = np.isin(labels, border_labels)

    rgba[..., 3][exterior] = 0
    return Image.fromarray(rgba, "RGBA")


# HRF partner logo reference framing, measured from Python's hrf_logo.png: a
# 200x61 file whose visible content is 190x53, centered with ~5px horizontal /
# ~4px vertical padding. The opening splash positions that FILE with
# COMPONENT_PADDING margin, so the visible logo renders a touch smaller and higher
# than its raw extent. We bake from a tightly-cropped high-res source, so without
# reproducing this framing our HRF renders noticeably wider and lower than Python.
# Re-padding our content to the same content-to-canvas ratios makes the baked
# asset a faithful analog, and the splash's Python-mirroring layout then lands
# identically — no positioning constants in the screen code.
HRF_REF_CONTENT_WIDTH_FRACTION  = 190 / 200   # 0.950
HRF_REF_CONTENT_HEIGHT_FRACTION = 53 / 61     # 0.869
HRF_REF_FILE_ASPECT             = 200 / 61     # 3.279


def pad_to_hrf_reference_framing(content: Image.Image) -> Image.Image:
    """Pad opaque content onto a black canvas matching Python hrf_logo.png's framing."""
    w, h = content.size
    # Width-limited (our artwork is proportionally wider than the reference content):
    # the content spans the reference width fraction; the canvas takes the file aspect.
    canvas_w = round(w / HRF_REF_CONTENT_WIDTH_FRACTION)
    canvas_h = round(canvas_w / HRF_REF_FILE_ASPECT)
    if canvas_h < h:  # safety: content too tall for that canvas -> height-limit instead
        canvas_h = round(h / HRF_REF_CONTENT_HEIGHT_FRACTION)
        canvas_w = round(canvas_h * HRF_REF_FILE_ASPECT)
    canvas = Image.new("RGB", (canvas_w, canvas_h), (0, 0, 0))
    canvas.paste(content, ((canvas_w - w) // 2, (canvas_h - h) // 2))
    return canvas


def recolor_dark_ink_to_white(img: Image.Image, chroma_threshold: int = CHROMA_THRESHOLD) -> Image.Image:
    """Map low-chroma (neutral/black) ink to white, preserve colored pixels + alpha."""
    rgba = img.convert("RGBA")
    px = rgba.load()
    w, h = rgba.size
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a == 0:
                continue
            if (max(r, g, b) - min(r, g, b)) < chroma_threshold:
                px[x, y] = (255, 255, 255, a)
    return rgba


def main() -> None:
    os.makedirs(SRC_DIR, exist_ok=True)

    # SeedSigner wordmark (finish the alpha extraction: exterior white -> transparent).
    logo = Image.open(os.path.join(SRC_DIR, "seedsigner_logo_transparent.png"))
    logo_out = crop_to_content(flatten_on_black(remove_exterior_background(logo)))
    logo_path = os.path.join(SRC_DIR, "seedsigner_logo_on_black.png")
    logo_out.save(logo_path)
    print(f"Written: {logo_path}  ({logo_out.width}x{logo_out.height}, aspect {logo_out.width/logo_out.height:.3f})")

    # HRF partner logo (recolor BEFORE flatten, then pad to the reference framing).
    hrf = Image.open(os.path.join(SRC_DIR, "HRF-Logo.webp"))
    hrf_out = pad_to_hrf_reference_framing(crop_to_content(flatten_on_black(recolor_dark_ink_to_white(hrf))))
    hrf_path = os.path.join(SRC_DIR, "hrf_logo_on_black.png")
    hrf_out.save(hrf_path)
    print(f"Written: {hrf_path}  ({hrf_out.width}x{hrf_out.height}, aspect {hrf_out.width/hrf_out.height:.3f})")


if __name__ == "__main__":
    main()
