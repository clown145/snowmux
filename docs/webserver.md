# Web Server Guide

This guide explains how to use CrossMux's built-in web server for file
transfer, device settings, Wi-Fi/OPDS management, and SD-card font management.

## Overview

The web server is available while the device is in **File Transfer** or
**Calibre Wireless** mode. It can:

- Upload, download, rename, move, and delete files on the SD card
- Create folders
- Edit many device settings from a browser
- Manage saved Wi-Fi networks and OPDS servers
- Upload and delete SD-card font families
- Accept WebDAV clients and Calibre wireless uploads

The server does not require authentication. Use it only on trusted private
networks or in hotspot mode when you control who is connected.

## Starting File Transfer

1. From Home, select **File Transfer**. On Xuesu 3, it is also under **Apps → File Transfer**.
2. Choose one of the available modes:

| Mode | Use when |
|------|----------|
| **Join Network** | You want the reader to join an existing Wi-Fi network. |
| **Calibre Wireless** | You want to receive books from the CrossPoint Calibre plugin workflow. |
| **Create Hotspot** | You want the reader to create its own open Wi-Fi network. |
| **USB Drive** (supported devices, including Xuesu 3) | You want to copy files directly to the SD card over USB. This mode does not start the web server; safely eject the drive on your computer when finished. |

On Xuesu 3, **Settings → Connectivity → Wi-Fi Setup** is a separate temporary
hotspot for configuring the reader's router connection from a phone. Use
**File Transfer** when you want to upload books.

## Join Network Mode

1. Select **Join Network**.
2. If you have saved Wi-Fi credentials, CrossPoint first tries the last
   connected network, then other visible saved networks in signal-strength
   order. Press **Back** to cancel or **Confirm** to stop auto-connect and show
   the network list.
3. If the network list is shown, pick a 2.4 GHz Wi-Fi network from the scan
   results.
4. Enter the password if prompted.
5. Save credentials if you want the reader to reconnect automatically next time.

After connection, the reader shows:

- The connected SSID
- A QR code for the web URL
- The direct IP URL, for example `http://192.168.1.102/`
- The mDNS fallback URL, usually `http://crosspoint.local/`

Use either URL from a phone, tablet, or computer on the same network.

## Create Hotspot Mode

1. Select **Create Hotspot**.
2. Connect your phone or computer to the open Wi-Fi network:

```text
CrossPoint-Reader
```

3. Open the URL shown on the reader. `http://crosspoint.local/` is preferred
   when supported; the fallback IP is typically `http://192.168.4.1/`. Some
   phones and computers open the reader's home page automatically after
   joining.

The reader displays one QR code for joining the hotspot and another QR code for
opening the web interface.

## Calibre Wireless Mode

Calibre Wireless starts the same web server in station mode, then displays setup
instructions and upload progress on the reader. Use this mode with the
CrossPoint Calibre plugin or other clients that speak the documented WebSocket
upload protocol.

For Calibre OPDS browsing, add `/opds` to the catalog URL when configuring an
OPDS server.

## Web Interface

The browser UI has five primary pages. It serves Simplified-Chinese HTML when
the current device UI language is `ZH_CN`; every other language uses English.

### Home

The Home page shows firmware status, network mode, IP address, device type,
uptime, and free heap.

### File Manager

The File Manager page can:

- Browse SD-card folders
- Upload files, using WebSocket upload when available and HTTP upload as a fallback
- Create folders
- Download files
- Rename files
- Move files into existing folders
- Delete one or more selected files or empty folders. Hidden dotfiles and
  credential stores remain protected, even when **Show Hidden Files** is on.

Uploads do not replace a file that has the same name: the upload fails with
"File already exists". API clients can replace a file with
`POST /upload?overwrite=1` (see `webserver-endpoints.md`). When EPUB files are
replaced, moved, renamed, or deleted through the web server, the matching book
cache is cleared so stale metadata is not reused.

### Settings

The Settings page exposes many firmware settings in the browser. It also has
cards for:

- Saved Wi-Fi networks
- OPDS servers

Passwords are accepted when adding or editing entries, but saved passwords are
not returned by the API.

### Fonts

In the file manager, click **Upload Fonts** to open the font uploader. Choose
individual files or use **Choose Folder** to select a font family folder.
The uploader groups files by name. For example, `Bookerly-Regular.ttf` and
`Bookerly-Bold.ttf` go in the `Bookerly` family folder. The reader creates that
folder in its SD-card font directory.

All devices accept `.cpfont` files named like `Bookerly_14.cpfont`. Devices with
external RAM and vector font support also accept `.ttf`, `.otf`, and `.ttc`.
The server checks the names and file headers before installing the uploads.

Select an installed font in **Settings > Reader > Text Settings > Font**.
You can also copy fonts into `/fonts/` or `/.fonts/` on the SD card. See the
[SD card font guide](./sd-card-fonts.md) for the folder layout.
To remove a loose direct font, delete its file from the SD card. The Fonts page
can delete a family stored in a subfolder.

### Plugins

The Plugins page shows the web settings cards of installed SD-card plugins.
Its navigation link appears only while **Settings > Connectivity > Plugins** is
enabled on the device. See [SD-card plugins](./sd-plugins.md).

## Command Line Use

Power users can use `curl`, WebDAV clients, or WebSocket clients while the web
server is running.

Endpoint details are documented in [webserver-endpoints.md](./webserver-endpoints.md).

## Security Notes

- The HTTP server runs on port 80.
- The WebSocket upload server runs on port 81.
- There is no authentication.
- Anyone on the same network can access the web interface while it is running.
- The server stops when you exit File Transfer or Calibre Wireless mode.
- Hotspot mode creates an open network for connectivity fallback; disconnect when done.

## Tips

1. Use **Create Hotspot** when no trusted network is available.
2. Prefer `crosspoint.local` when available, but keep the displayed IP address as a fallback.
3. Move closer to the router if upload progress stalls in Join Network mode.
4. Upload custom fonts through the Fonts page or copy them to `/.fonts/` or `/fonts/` on the SD card.
5. Exit File Transfer mode when finished to conserve battery.

## Related Documentation

- [User Guide](../USER_GUIDE.md)
- [Webserver Endpoints](./webserver-endpoints.md)
- [SD Card Fonts](./sd-card-fonts.md)
- [Troubleshooting](./troubleshooting.md)
