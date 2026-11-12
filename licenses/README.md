# Firmware licenses

CrossPoint's original code uses the MIT license in the root `LICENSE` file.
Third-party code and assets keep their own licenses.
The combined firmware executable uses GPLv3 because it includes wolfSSL.

The release `LICENSE` file includes the full license texts and third-party notices.
Short references in the source point to the notices in this directory.
These notices do not change the original licenses or grant extra permissions.

## Source for a release

Corresponding Source means the source and scripts needed to rebuild the firmware.
GitHub provides a source archive for each release tag.
That archive contains CrossPoint's code and patches, but it omits submodule contents and downloaded libraries.

The release `LICENSE` identifies the source revision and the SDK revision.
It also lists the source packages for the libraries that PlatformIO downloads.
Keep these sources and this license file available with the firmware downloads.

To build the same source revision:

1. Clone the source repository named in the release `LICENSE` using `git clone --recursive <source-repository>`.
2. Select the release tag with `git checkout <release-tag>`.
3. Download the matching SDK with `git submodule update --init --recursive`.
4. Follow `docs/contributing/getting-started.md` for the build tools.
5. Build the required device with `pio run -e <environment>`.

`platformio.ini` selects the platform, dependency versions, build flags, and patch scripts.
The source packages listed in the release file include third-party code that the GitHub source archive omits.
The `scripts/` directory contains CrossPoint's changes to those dependencies.

## Third-party material

| Material | License and notices |
| --- | --- |
| wolfSSL and wolfCrypt | GPL-2.0-or-later. CrossPoint selects GPLv3 for the combined firmware. |
| Arduino-ESP32 and WebSockets | LGPL-2.1-or-later, with separate terms for some framework files. |
| NimBLE-Arduino, PNGdec, and JPEGDEC | Apache-2.0 and the applicable upstream notices. |
| ArduinoJson, SdFat, QRCode, Expat, and miniz | MIT and the original copyright notices. |
| uzlib | Zlib-style terms in the source files. |
| FreeInk SDK | MIT, with separate licenses for third-party code and assets. |
| libmobi (`xuesu_300` only) | LGPL-3.0-or-later. The pinned source supplies `COPYING` and `AUTHORS`. |
| FreeType | FreeType License in `freeink-sdk/libs/font/FreeInkFont/third_party/freetype/FTL.TXT`. |
| stb_truetype | MIT or its public-domain alternative. |
| ESP-IDF | Apache-2.0 and separate terms for some components. |
| GCC runtime libraries | Their source grants include the GCC Runtime Library Exception. |
| Copied utilities, browser code, and Unicode data | `borrowed-code.txt`. |
| Built-in fonts and SD card fonts | `fonts.txt`. |
| Hyphenation data | `hyphenation.txt`. |
| Lucide and Feather icons | `freeink-sdk/libs/assets/Icons/lucide/LICENSE`. |

The release file includes the notices from the installed dependency versions.
Keep original notices when you update or copy third-party code.
If you change third-party code, identify the changes under its license terms.

The Xuesu build uses a pinned [libmobi source revision](https://github.com/bfabiszewski/libmobi/tree/85dcfe803fc2a21020ddcf15c3eb66b93d388add). The [build script](../scripts/configure_xuesu_libmobi.py) renames the private `memory.h`, uses CrossMux's configured miniz, and sets the allocation and compilation flags. The [device guide](../docs/engineering/xuesu-300-port.md) describes the SDK patch and local generation of the panel tables from the vendor app. The vendor app and generated tables are outside this repository. Redistribution terms for the vendor waveform data have not been established here.

## Fonts and data

Font licenses apply to converted fonts and subsets as well as the source fonts.
Keep `fonts.txt` with font copies that you distribute.
The font release workflow attaches the same notices as a release asset named `LICENSE`.
If you update the catalog or source fonts, update `fonts.txt` from the original font notices.

A Reserved Font Name is a name that the font owner protects.
Format conversion can require a new font name or permission from the owner.
The notices do not grant permission to use a reserved name.

Russian, Swedish, and Ukrainian hyphenation patterns use LPPL terms.
Their compatibility with the combined GPL firmware remains an open question.
The source requirements for some platform binary libraries also remain open.
Keep these questions separate from the notice changes.

## Release files

From the same revision used to build the firmware, run `python3 scripts/build_release_license.py --env <environment> --output <path>`.
For a local build of a fork, pass `--repository <owner>/<repo>` so the notice links to the matching source.
The script reads the installed dependency notices and writes one release `LICENSE` file.
It does not download packages or create another source archive.

For the final release, the workflow attaches the license file beside the firmware files.
For a release candidate, each artifact includes its own copy.
The web footer links to the license information in this repository.
