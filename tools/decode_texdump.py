# Decodes the raw BC4 slices that the D3D12 renderer writes to <user>/texdump/ (create <user>/dump_textures.txt while the
# game runs, see D3D12Renderer::DumpArrayTexturesIfRequested) into grayscale PNGs next to them.
# usage: python decode_texdump.py <texdump folder>
import re
import struct
import sys
from pathlib import Path

from PIL import Image

DXGI_BC4_UNORM = 80
DXGI_BC4_TYPELESS = 79


def decode_bc4_block(block):
    r0, r1 = block[0], block[1]
    if r0 > r1:
        palette = [r0, r1] + [((7 - i) * r0 + i * r1) // 7 for i in range(1, 7)]
    else:
        palette = [r0, r1] + [((5 - i) * r0 + i * r1) // 5 for i in range(1, 5)] + [0, 255]
    bits = int.from_bytes(block[2:8], "little")
    return [palette[(bits >> (3 * i)) & 7] for i in range(16)]


def decode(path):
    m = re.search(r"_(\d+)x(\d+)_dxgi(\d+)_pitch(\d+)_rows(\d+)\.bin$", path.name)
    if not m:
        return
    width, height, dxgi, pitch, rows = map(int, m.groups())
    if dxgi not in (DXGI_BC4_UNORM, DXGI_BC4_TYPELESS):
        print(f"{path.name}: format {dxgi} not supported")
        return
    data = path.read_bytes()
    image = Image.new("L", (width, height))
    pixels = image.load()
    for by in range(min(rows, height // 4)):
        row = data[by * pitch:(by + 1) * pitch]
        for bx in range(width // 4):
            texels = decode_bc4_block(row[bx * 8:bx * 8 + 8])
            for i, v in enumerate(texels):
                pixels[bx * 4 + i % 4, by * 4 + i // 4] = v
    out = path.with_suffix(".png")
    image.save(out)
    print(out.name)


if __name__ == "__main__":
    for file in sorted(Path(sys.argv[1]).glob("*.bin")):
        decode(file)
