# ARIB Player

A video player for Windows, made for Japanese digital-TV recordings and broadcast streams, with ARIB captions shown as on a TV.

## Features

- Plays local files and network streams: MPEG-TS, MP4, MKV, HLS and more.
- **ARIB captions (字幕) and superimposed text (文字スーパー)**, drawn at the broadcaster's layout, colours and ruby, and kept sharp at any window size. Caption style follows the Windows caption settings.
- **Film mode (inverse telecine) and deinterlacing** for 1080i broadcasts, on the GPU when available.
- Hardware decoding through Direct3D 11, with an NVIDIA CUDA path. Video frames stay on the GPU.
- Playlists: local and remote M3U/M3U8, an automatic playlist for anything you open, and resume where you left off.
- Frame-accurate stepping, and copying the current frame to the clipboard, with or without captions.
- A Windows 11 (Fluent) interface with light and dark themes: a title strip with a main menu, a docked seek and transport bar with decoder and codec chips, and a playlist panel beside the video (F6). Always on top cycles through its four modes (Ctrl+T). Playback keys: Left / Right seek 5 s, F2 open folder, F4 stop, F5 settings, F7 control panel. The interface language follows Windows (Korean or English).

## Requirements

- Windows 11, 64-bit.
- A GPU with Direct3D 11 support. An NVIDIA GPU enables the CUDA zero-copy path.

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
