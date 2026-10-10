"""Deterministic atlas crop/resize/alpha-composite; no image generation.
Run with the bundled Python. The original atlas is never modified.
"""
from pathlib import Path
import json
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
ATLAS = ROOT / 'Assets/未处理UI/sactx-0-512x256-DXT5_BC3-NotepadAtlas-a8efa592.png'
OUT = ROOT / 'Assets/processed/center-hud'
OUT.mkdir(parents=True, exist_ok=True)
image = Image.open(ATLAS).convert('RGBA')
# Rectangles are (x, y, width, height), matching the existing Qt atlas crops.
# N15 is painted first. N16B/N16A overlap its outer wings, facing inward.
parts = [
    ('N15', (7, 156, 400, 94), (0, 14, 400, 94)),
    ('N16B', (421, 194, 82, 62), (18, 22, 98, 74)),
    ('N16A', (421, 133, 82, 61), (284, 22, 98, 74)),
]
canvas = Image.new('RGBA', (400, 118), (0, 0, 0, 0))
for name, source, destination in parts:
    x, y, w, h = source
    dx, dy, dw, dh = destination
    crop = image.crop((x, y, x+w, y+h))
    crop = crop.resize((dw, dh), Image.Resampling.LANCZOS)
    canvas.alpha_composite(crop, (dx, dy))
asset = OUT / 'center_hud_combined.png'
canvas.save(asset)
# Enlarged display preview only. The reusable file above stays transparent.
preview = Image.new('RGBA', canvas.size, '#101416')
preview.alpha_composite(canvas)
preview = preview.convert('RGB').resize((800, 236), Image.Resampling.LANCZOS)
preview.save(OUT / 'center_hud_combined_preview.png')
metadata = {
    'sourceAtlas': str(ATLAS.relative_to(ROOT)).replace('\\', '/'),
    'size': list(canvas.size),
    'composition': 'N15 below N16B and N16A, static artwork only',
    'parts': [{'id': name, 'sourceRect': list(src), 'destinationRect': list(dst)} for name, src, dst in parts],
    'textBakedIn': False,
}
(OUT / 'center_hud_combined.json').write_text(json.dumps(metadata, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
print(asset)
print(OUT / 'center_hud_combined_preview.png')
