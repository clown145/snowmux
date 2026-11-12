"""Generate the Xuesu panel tables from a verified factory app image."""
import argparse
import hashlib
from pathlib import Path
import struct

# The public 1.3.3 image stores these same tables 0x6a8 bytes earlier in DROM.
LUT_ADDRESS_DELTAS = {
    "7e2df1bb773037576aa2ee4b8e2bf309922db9bc227636d465c132e2ac01ea14": -0x6A8,
    "8974d6bb10c5a3f768812799219ec7b8f8af638867ca27732518feb27c8ea9df": 0,
}
# Register order VCOM, WW, KW, WK, KK (0x20–0x24). The loader at 0x42094D30
# reads WK from caller SP+4 and KK from SP+12, in the opposite order to the
# literal loads at the fast call site.
FULL_LUT_ADDRESSES = (0x3C2735D0, 0x3C2735A6, 0x3C27357C, 0x3C273552, 0x3C273528)
FAST_LUT_ADDRESSES = (0x3C2734FE, 0x3C2734D4, 0x3C2734AA, 0x3C273480, 0x3C273456)
GRAY_VCOM_ADDRESS = 0x3C27342C
LUT_SIZE = 42
DEFAULT_OUTPUT = Path(__file__).resolve().parents[1] / ".pio/xuesu-local-include/Xuesu300StockLut.generated.h"


def render_lut(name, rows):
    declarations = ["    {" + ", ".join(f"0x{byte:02x}" for byte in row) + "}," for row in rows]
    return f"inline constexpr uint8_t {name}[{len(rows)}][{LUT_SIZE}] = {{\n" + "\n".join(declarations) + "\n};\n"


def gray_rows(vcom):
    # Register loader at 0x42094C1C and table generator at 0x4215C8BC.
    # Each row has seven 6-byte groups. Only the first and the first byte of
    # the second group are set by the stock generator.
    def row(drive, last_drive=0, last_length=0):
        return bytes([1, drive, 1, last_drive, last_length, 1, 1]).ljust(LUT_SIZE, b"\0")

    neutral, wk, kw = row(0x14), row(0x92), row(0x48)
    return (vcom, neutral, wk, kw, row(0x8C, 0x42, 1)), (vcom, neutral, neutral, neutral, row(0x8C, 0x43, 1))


def render_header(image):
    digest = hashlib.sha256(image).hexdigest()
    delta = LUT_ADDRESS_DELTAS.get(digest)
    if delta is None:
        raise ValueError(f"Unrecognized factory app SHA-256: {digest}. Use the Xuesu 1.3.3 image in the build guide.")

    base, size = struct.unpack_from("<II", image, 24)
    drom = image[32:32 + size]

    def read_lut(address):
        offset = address + delta - base
        if not 0 <= offset <= len(drom) - LUT_SIZE:
            raise ValueError("Waveform offset is outside the factory data segment")
        return drom[offset:offset + LUT_SIZE]

    gray_first, gray_second = gray_rows(read_lut(GRAY_VCOM_ADDRESS))
    return "\n".join((
        "#pragma once\n#include <stdint.h>\n",
        render_lut("stockFullLut", [read_lut(address) for address in FULL_LUT_ADDRESSES]),
        render_lut("stockFastLut", [read_lut(address) for address in FAST_LUT_ADDRESSES]),
        render_lut("stockGrayFirstLut", gray_first),
        render_lut("stockGraySecondLut", gray_second),
    ))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app", type=Path, help="Official Xuesu firmware10033.bin or the verified reference OTA0 app")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT, help="Generated header path")
    args = parser.parse_args()
    try:
        header = render_header(args.app.read_bytes())
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(header, encoding="ascii")
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
