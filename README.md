# ARIB Player

A video player for Windows, made for Japanese digital-TV recordings and broadcast streams, with ARIB captions shown as on a TV.

## Features

- Plays local files and network streams: MPEG-TS, MP4, MKV, HLS and more.
- **ARIB captions (字幕) and superimposed text (文字スーパー)** at the broadcaster's layout, colours and ruby.
- **Film mode (inverse telecine) and deinterlacing** for 1080i broadcasts.
- Hardware decoding on any GPU.
- Playlists (M3U/M3U8) with resume.
- Frame stepping, frame copy, picture and sound filters, and a mini player.
- Fluent-style interface in Korean or English, light and dark.

## Requirements

- 64-bit Windows.

## Install

1. Download the MSI from the **Releases** page.
   - Tagged releases (`vX.Y.Z`) are stable builds: `aribplayer-X.Y.Z-x64.msi`.
   - The **main** pre-release is rebuilt from the latest code: `aribplayer-<commit>-x64.msi`, where `<commit>` is the
     first 8 characters of the commit id.
2. Run it. The installer is per-user:
   - it needs no administrator rights;
   - it installs to `%LOCALAPPDATA%\Programs\aribplayer`;
   - it adds **ARIB Player** to the Start menu.

Installing a newer version upgrades the existing one in place.

The installer is not code-signed yet, so Windows SmartScreen may ask for confirmation.

## Uninstall

Remove **ARIB Player** in *Settings → Apps → Installed apps*.

Your settings, playlists, resume history and logs live in `%LOCALAPPDATA%\aribplayer`, and uninstalling keeps them. Delete that folder to remove them too.

## License

ARIB Player bundles FFmpeg built with GPL components and is distributed under the GNU General Public License. Third-party components and the bundled caption font are listed in [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt).
