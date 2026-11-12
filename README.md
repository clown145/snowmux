# SnowMux

**English** | [简体中文](./README.zh-CN.md)

**SnowMux** is community reading firmware for the **Xuesu 3 (雪酥·3)**, board ID `xuesu-300-std`, based on [CrossMux](https://github.com/0x1abin/crossmux). It supports the reader's 300×400 e-ink panel, three physical buttons, and factory partition layout.

[Xuesu web flasher](https://clown145.github.io/snowmux/) · [SnowMux releases](https://github.com/clown145/snowmux/releases) · [Device and build guide (Chinese)](./docs/engineering/xuesu-300-port.md)

## Installation and updates

1. Back up the books and data on your SD card. Open the [Xuesu web flasher](https://clown145.github.io/snowmux/) in Chrome or Edge on a computer, or Chrome on Android. Connect the reader with a USB data cable and press OK to wake it. On Android, use a phone with USB OTG support and grant USB access when prompted.
2. Follow the prompts to connect and check the device. The flasher offers an optional 16 MiB Flash backup, recommended before the first installation.
3. If SnowMux already has the update menu, use **Settings → System → Check for Updates** to update over Wi-Fi. Use the web flasher for an older build without this menu.

The first web installation preserves the factory app slot. On builds that support on-device updates, an update may overwrite that slot; check the confirmation before proceeding. If the update menu offers **Restore factory firmware**, it can download and install the factory app. Use your original Flash backup for a full device restore. See the [web flasher and release notes (Chinese)](./docs/engineering/xuesu-300-port.md#社区刷机站与发版).

## Buttons and shortcuts

| Gesture | Action |
| --- | --- |
| Up or Down | Move through a list or turn pages. |
| Press OK | Open the selected item, open the reader menu, or wake the reader. |
| Hold OK | Usually go back or cancel. On a main tab, return focus to the tab bar and use Up or Down to switch tabs. |
| Press OK twice | Open the shortcut panel on a main tab or reading page. Use Up or Down to move, press OK to select, and hold OK to close. |

The panel offers Sleep, Night Mode, Refresh Screen, and Reading Orientation. Files, Library, bookmarks, clippings, paginated OPDS catalogs, and Standby also offer actions for the current item.

On first use with a fresh SD card, SnowMux adds its user guide to the home screen. It uses the Chinese edition for Simplified Chinese and the English edition for other UI languages. An upgrade does not replace an older copy on an existing card.

## Reading and file transfer

- **Reading and library**: EPUB, TXT, XTC/XTCH, and unencrypted MOBI/AZW3 files up to 4 MiB. DOCX text and supported inline images are reflowed. The reader offers bookmarks, clippings, offline dictionaries, SD-card fonts, and KOReader progress sync. Use **Settings → System → Import factory reading history** to import recent books from the factory firmware.
- **USB and wireless transfer**: Open **Apps → File Transfer**. Use **USB Drive** to copy files to the SD card from a computer; safely eject the drive when finished. Wi-Fi browser transfer and Calibre wireless are also available. If a new book appears in Files but not Library, use **Library Actions → Refresh library** from the shortcut panel.
- **Wi-Fi and phone setup**: Open **Settings → Connectivity → Wi-Fi Setup**. Scan the QR code with your phone to join the temporary hotspot, then choose and enter the password for your 2.4 GHz Wi-Fi network. OPDS and KOReader configuration, catalog search, and other text entry also use phone forms.
- **Page turners, statistics, and clock faces**: Pair BLE HID page turners in **Settings → Connectivity → Bluetooth Page Turner**. SnowMux also offers reading statistics and heatmaps. Preview clock faces in **Apps → Standby** and press OK twice to set the current face as the sleep screen.

Back up your SD card, including `/.crosspoint/`. This folder holds reading progress and settings; clearing the cache for one book does not require deleting the whole folder.

## Apps and visibility

These are the defaults for a fresh configuration in the current source. Existing visibility choices are kept.

| Status | Xuesu 3 apps |
| --- | --- |
| Shown by default | Library, File Transfer, WeRead, AirPage, Reading Stats, Standby. |
| Shown after setup | OPDS Browser requires a server under **Settings → Connectivity → OPDS Servers**. |
| Hidden by default, can be shown | Ugly Avatar, Buddy, Electronic Woodfish. |
| Unavailable in the Xuesu launcher | Sudoku, Gomoku, Sokoban, Chinese Chess, Minesweeper, 2048, Pixel Switch, and Calculator. These implementations require touch or separate Left and Right controls and have no visibility toggle on Xuesu. |

To show or hide a supported app, open **Settings → System → App Visibility**, select the app, and press OK to toggle it. **On** means it appears in Apps. Hold OK to leave and save. WeRead visibility is independent of UI language; see the [WeRead guide (Chinese)](./src/activities/apps/weread/README.md).

## Documentation and building

- [Xuesu device guide (Chinese)](./docs/engineering/xuesu-300-port.md): build setup, device limits, release workflow, and hardware validation.
- [Apps development](./src/activities/apps/README.md) · [Web transfer](./docs/webserver.md) · [Web API](./docs/webserver-endpoints.md) · [SD-card fonts](./docs/sd-card-fonts.md)
- [Bundled user guide sources](./docs/user-guide/xuesu/) · [Engineering reference](./docs/engineering/index.md) · [Contributor guide](./docs/contributing/README.md)

Clone the [SnowMux repository](https://github.com/clown145/snowmux) recursively, install pioarduino Core and Python, then follow the [source setup steps (Chinese)](./docs/engineering/xuesu-300-port.md#从源码构建) to apply the FreeInk SDK patch and generate the panel waveform header. After setup, run this in Bash:

```bash
PYTHONUTF8=1 pio run -e xuesu_300
```

The application image is `.pio/build/xuesu_300/firmware.bin`. Follow the Xuesu installation procedure; the generated `firmware.factory.bin` is unsuitable for first installation on Xuesu.

For other devices, see [upstream CrossMux](https://github.com/0x1abin/crossmux), the [general user guide](./USER_GUIDE.md), and [device variants](./docs/engineering/device-variants.md). CrossMux Stable and Nightly images do not target the Xuesu 3.

## Credits

Thanks to [CrossMux](https://github.com/0x1abin/crossmux), [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader), [Inx](https://github.com/obijuankenobiii/inx), [cpr-vcodex](https://github.com/franssjz/cpr-vcodex), and their contributors, and to [diy-esp32-epub-reader](https://github.com/atomic14/diy-esp32-epub-reader) for the original inspiration.

SnowMux is an independent community project. See [LICENSE](./LICENSE) and the [firmware licensing notes](./licenses/README.md).
