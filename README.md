# ARIB Player

A video player for Android that plays Japanese digital-TV recordings
(MPEG-TS) the way a real TV does — with proper broadcast captions,
film-mode playback, and dual-audio support.

## What it does

- **Plays broadcast recordings** (`.ts` / `.m2ts` / `.mts`) from any file
  manager or network storage app. There is no app icon: open a recording
  from your file manager (e.g. **Files**, or an SMB/NAS client) and choose
  **ARIB Player**.
- **ARIB captions (字幕), rendered like a TV.** Captions appear and
  disappear exactly when the broadcast says so, in the correct broadcast
  colors, with ruby (furigana) positioned properly. Emergency ticker
  overlays (文字スーパー) are shown independently of the caption toggle.
- **Film mode (IVTC).** Anime and film content broadcast at 30i can be
  restored to its original 24p using your device's GPU — smoother pans,
  no combing. A deinterlace mode is available for sports/news/studio
  content, and **Auto** picks a safe mode by itself.
- **Dual audio.** Programs with two audio tracks, or with 主/副
  (main/sub) dual-mono audio, show an audio selector — switch mid-play
  without interrupting the video.
- **Resume.** Reopening a recording you've watched offers to continue
  where you left off — playback starts directly at that point.
- **Remembers your choices per recording**: film/deinterlace mode, audio
  track, and caption on/off are restored the next time you open the same
  file.

## How to use

1. Install the APK (see Releases), or build it yourself.
2. Open your file manager, navigate to a recording, tap it (or use
   *Open with…*) and pick **ARIB Player**.
3. During playback:
   - **Tap** the screen to show/hide the controls. They hide by
     themselves after a few seconds; the status bar hides with them.
   - **Center buttons**: skip back / play–pause / skip forward
     (10 or 30 seconds — configurable).
   - **Drag the bar** at the bottom to seek. The time preview follows
     your finger; the jump happens when you let go.
   - **CC button**: captions on/off.
   - **Gear button**: video filter (Auto / IVTC / Deinterlace / Off),
     audio track, and app settings.
   - **Info button**: playback diagnostics (decoder, filter, fps).
   - **Back** or leaving the app ends playback.
4. TV / D-pad: all controls are reachable with a remote — press any
   direction key to reveal the controls, navigate with the D-pad.

### Caption appearance

Captions follow the broadcast styling. If you want to remove the black
box behind captions or force outlined text, use Android's system caption
settings (**Settings → Accessibility → Caption preferences**):
a transparent background and the outline edge style are respected.
Text color, size, and font always follow the broadcast, as on a TV.

## Good to know

- Hardware film mode (IVTC) uses your device's OpenCL driver. On devices
  without one, the same processing runs on the CPU automatically — the
  gear menu shows which is active.
- MPEG-2 recordings decode in software on most phones and tablets
  (hardware MPEG-2 decoders are rare outside TVs); modern devices handle
  this comfortably.
- Recordings on network storage work through your SMB/NAS app as long as
  it provides files to other apps ("Open with…").
- Seeking within a recording is fast but not instant — broadcast streams
  have no index, so the player decodes forward from the nearest keyframe.

## Building

Requires Linux or WSL for the native build, and Android Studio / the
Android SDK for the app:

```
# 1. Native libraries (FFmpeg + patches), inside WSL/Linux:
cd third_party/scripts && bash build-all.sh

# 2. App:
./gradlew :app:assembleDebug
```

See `third_party/README.md` for details on the native build.

## License

GPL-2.0-or-later. See [LICENSE](LICENSE),
[LICENSE.addendum](LICENSE.addendum) and [NOTICE](NOTICE).
Player UI strings are derived from AndroidX Media3 (Apache-2.0).
