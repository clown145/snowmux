"""Generate the private panel tables from the verified stock app read from this unit."""
import argparse
import hashlib
from pathlib import Path
import struct

APP_SHA256 = "8974d6bb10c5a3f768812799219ec7b8f8af638867ca27732518feb27c8ea9df"
# Register order VCOM, WW, KW, WK, KK (0x20–0x24). The loader at 0x42094D30
# reads WK from caller SP+4 and KK from SP+12, in the opposite order to the
# literal loads at the fast call site.
FULL_LUT_ADDRESSES = (0x3C2735D0, 0x3C2735A6, 0x3C27357C, 0x3C273552, 0x3C273528)
FAST_LUT_ADDRESSES = (0x3C2734FE, 0x3C2734D4, 0x3C2734AA, 0x3C273480, 0x3C273456)
LUT_SIZE = 42

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("app", type=Path, help="Validated ota0-app.bin from the reference Snow 3")
args = parser.parse_args()
image = args.app.read_bytes()
if hashlib.sha256(image).hexdigest() != APP_SHA256:
    parser.error("stock app hash differs from the verified reference")

base, size = struct.unpack_from("<II", image, 24)
drom = image[32:32 + size]


def render_lut(name, addresses):
    rows = []
    for address in addresses:
        offset = address - base
        if not 0 <= offset <= len(drom) - LUT_SIZE:
            parser.error("waveform offset is outside the stock data segment")
        row = ", ".join(f"0x{byte:02x}" for byte in drom[offset:offset + LUT_SIZE])
        rows.append(f"    {{{row}}},")
    return f"inline constexpr uint8_t {name}[{len(addresses)}][{LUT_SIZE}] = {{\n" + "\n".join(rows) + "\n};\n"


output = Path(".pio/xuesu-local-include/Xuesu300StockLut.generated.h")
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(
    "#pragma once\n#include <stdint.h>\n\n"
    + render_lut("stockFullLut", FULL_LUT_ADDRESSES)
    + "\n"
    + render_lut("stockFastLut", FAST_LUT_ADDRESSES),
    encoding="ascii",
)
