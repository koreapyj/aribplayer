# Main window UI notes

The main window is a Fluent (WinUI-style) layout. Visual spec: `.ref/prototype-unpacked/main-window.html`
(1280x760 DIP). All widgets are `pui` widgets drawn with Direct2D into DirectComposition
layers; the video is a child window that the D3D11 presenter draws into (its own swap chain window, disabled, so the
input reaches the video child). Pure layout/logic lives in headers covered by `--logictest`
(`src/App/ShellLogic.h`, `BarLogic.h`, `WindowSizing.h`, `Commands.cpp`, `shell/PlaylistPanelLogic.h`, `features/*Logic.*`).

## 1. Layout math

All numbers are DIP; rects are snapped to whole device pixels (`px / scale`). Single source: `ComputeShellLayout`.

| Constant | Value | Where |
|---|---|---|
| `kStripDip` | 44 | title strip |
| `kDockDip` | 86 | seek row (10 pad + 20) + transport row (6 pad + 40 + 10 pad) |
| `kBodyGutterDip` / `kBodyGapDip` | 8 / 8 | around / between video and panel |
| `kPanelDefaultDip` / `kPanelMinDip` | 320 / 240 | panel width default (`AppSettings::playlistPanelWidthDip`, persisted) / minimum |
| `kVideoMinDockDip` | 240 | minimum video column width for the panel to stay docked (row >= 8 + 240 + 240 = 488, else overlay) |
| `kMinWindowWidthDip` x `Height` | 500 x 374 | `WM_GETMINMAXINFO` and the 1-5 keys |

Body row: x = gutter .. W - gutter, y = 44 .. H - 86. With the panel docked the panel has the user width (clamped to kPanelMinDip ..
row - gap - kVideoMinDockDip) and the video takes the rest (936 / 320 at 1280); window resizing changes only the video. The docked panel is a full-height right
column (y = 44 .. H - 8) and the dock spans only the video column (x = video.l .. video.r, y = H - 86 .. H); without a docked panel
the dock spans the whole width.

Splitter: the 8 DIP gap between the video column and the docked panel (`ShellLayout::splitterPx`, panel column height) is the only
resize handle (inner side only). Hover shows the horizontal-resize cursor; dragging changes the panel width live
(relayout per move, video child resized), release persists `playlistPanelWidthDip`; double click resets to 320. It is client area
(`NcHitTest` HTCLIENT), ignored by the video-drag and panel-active logic, and absent in full screen, mini, overlay mode and
with the panel hidden. The overlay panel and the full-screen edge panel use the same stored width (min(width, W - 16)), without a handle.

| Mode | Title | Video rect | Bar | Panel |
|---|---|---|---|---|
| Player (NowPlaying) | strip 44 | body row (8 gutters), above the dock | docked, 86 (video column only while the panel is docked) | docked (full height) when wanted and the video column would be >= 240 (row >= 568); otherwise `panelOverlay` (toggle opens a light-dismiss popup over the video's right side) |
| Full screen | overlay, edge-revealed (cursor in the top 44 DIP; own slide/fade) | whole client | overlay, edge-revealed (cursor in the bottom 86 DIP; hides at once when the cursor leaves it) | overlay column, full height at the right edge, revealed by the right 12 DIP band (closes at once when the cursor leaves it; toggle disabled) |
| Mini player | strip 44 (no slide) | below the strip | overlay, auto-hides | hidden; toggle disabled |

Window size keys 1-5 (`WindowSizing.h`): the window is sized so the VIDEO rect gets 0.5x / 1x / 1.5x / 2x of the display size
(SAR applied) or the work area for "Max". `ClientWidthForVideo` / `ClientHeightForVideo` solve client size from the
video size against `ComputeShellLayout` itself (gutters, strip, dock, and the panel when it would dock at that width);
the non-client frame is added from `GetWindowRect - GetClientRect`. Each axis is clamped to the work area independently.

## 2. Title bar and caption technique (variant B2)

The window has no `WS_CAPTION`; it keeps `WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX` (`kCaptionlessStyle`) so
Aero Snap, resize borders and the system shadow still work. Mica stays (`DwmExtendFrameIntoClientArea(-1)`).

- `WM_NCCALCSIZE`: the system keeps left/right/bottom resize borders as non-client and only the caption band is dropped
  (client top = window top). Maximized: client = monitor work area. Full screen: client = window.
- `WM_GETMINMAXINFO`: clamps the maximized size/position to the work area and sets the minimum size.
- `WM_NCHITTEST` (`App::NcHitTest`, rects from `TitleLayout`): `HTMAXBUTTON` over the Max button (Windows 11 snap layouts flyout),
  `HTCLIENT` over main-menu / Pin / Min / Close, `HTCAPTION` on the rest of the strip (drag, double-click maximizes), resize
  codes on the outer 6 px.
- NC mouse: `WM_NCMOUSEMOVE/LEAVE`, `WM_NCLBUTTONDOWN/UP` and `WM_NCPOINTERDOWN/UP` (touch/pen) feed hover/press/invoke of the Max button
  into the widget tree, because the system owns those messages over `HTMAXBUTTON`.
- `WM_NCACTIVATE` → `DefWindowProc(..., lParam = -1)`: without `WS_CAPTION` the default handler repaints the top sizing frame as a
  light band over the strip whenever focus changes; -1 skips that non-client repaint. Test it with a window that was really active,
  then deactivated — a window launched from a background process is never activated and hides the problem.
- Strip layout `[☰ main menu] file name ... [Pin][Min][Max/Restore][Close]`, buttons (incl. the hamburger) 46 x 44, file name 8 DIP after the hamburger. The Max slot
  swaps to Restore on `WM_SIZE`; full screen drops Pin/Max and shows "exit full screen".
- The video child is `HTTRANSPARENT` near the resize borders, under the docked/visible bar and while a popup is open, so those
  clicks reach the shell.

## 3. Dock bar, chips, tiers

Seek row: elapsed (click toggles elapsed / remaining, persisted as `timeRemaining`) | seek slider (4 track, accent fill, 20 thumb
with a 10 accent dot) | total. Transport row: Play/Pause (accent fill) - Stop - Prev - Next - Open | chips | Mute + volume slider
(96) - Subtitle - Control panel - Playlist panel - Full screen.

Chips (`features/BadgeLogic`): "H/W" or "S/W" (shows the actual decoding state; clickable: toggles `ToggleHwDecoding` between the configured decoder - `hwDecoderLastOn`, "auto" by default, updated whenever Settings picks a non-off decoder - and "off", reopening at the same position; tooltip says on/off), video codec as the FourCC from libffplay (container tag, else the RIFF tag: MPG2, H264, HEVC ...; codec name when none), "HDR" before it when the selected video's transfer is PQ or HLG (`smpte2084`, `arib-std-b67`), audio codec, audio channels ("5.1"). Display-only except H/W. Refreshed on open,
shortly after prepare, then every 2 s while playing.

Tiers (`BarLayout::Compute`, widest that fits wins): all chips -> fewer chips (last first) -> volume slider 96 -> 64 -> Open, Subtitle
and Control buttons hidden -> volume slider hidden. 1 px strokes use `1 / scale` (`Theme::hairline`).

Windowed: no bar fill (the window backdrop shows). Full screen / mini: `barFill` + stroke, auto-hide with slide + fade.

## 4. Control behaviour

| Control | Left click | Right click | Notes |
|---|---|---|---|
| Play/Pause | toggle | - | one slot, glyph swaps |
| Stop | close media (idle view) | - | mini player: returns to the player first |
| Prev / Next | seek -5 s / +5 s | previous / next file | tooltips "L: back, R: previous file" |
| Open | open file dialog | - | |
| Mute | toggle | - | wheel over the slider = volume |
| Subtitle | toggle captions / subtitles | subtitle track menu | |
| Control panel (F7) | filters panel, pressed while open | - | |
| Playlist toggle (F6) | show/hide panel, pressed while visible | - | narrow window: popup mode |
| Pin (title) | cycle always-on-top: Off -> On -> While playing -> While fullscreen | - | same as Ctrl+T, persisted |
| Elapsed label | elapsed / remaining | - | |
| H/W or S/W chip | toggle hardware decoding: configured decoder (auto by default) <-> off, reopens at the same position | - | no key binding; same setting as Settings > Playback > Hardware decoding |
| Hamburger button / video | main menu | main menu on the video | the main menu is the context menu |
| Video | double click = play/pause (not fullscreen); left-drag moves the window (not in fullscreen) | main menu | wheel = volume |
| Playlist toggle, windowed | window grows/shrinks by panel width + gap so the video keeps its size | - | maximized/fullscreen/narrow: video resizes instead |

Main menu (`features/Menus`): Open file / folder / URL, Close; Play submenu (play/pause, prev/next, seek
steps, speed, repeat, shuffle, mini player); Subtitle / Video / Sound submenus; Always on top submenu; Equalizer, Copy frame, Properties;
Playlist panel (F6), Control panel (F7), Settings (F5: opens the separate Settings window, see §9); Exit.

Menus are NATIVE Win32 popup menus (`features/NativeMenu`): the `pui::MenuItemDesc` trees from the builders (`menus::BuildMenu`,
`BuildSubtitleItems`, playlist panel) are converted to an `HMENU` and tracked with `TrackPopupMenuEx(TPM_RETURNCMD)` on the owner
window (the main window; the Settings window while it dispatches its own input, `nativemenu::OwnerScope`), so they are real top-level
windows and never clipped by a small main window (Windows 11 styling, mnemonics / arrows / Esc come
from the OS). Anchors: a screen point (right click on the video, mini player, list rows) or the rect of a button / chip (`TPMPARAMS.rcExclude`,
below it, or above for the bar and panel footer; the OS flips at screen edges). Dark / light: uxtheme ordinals 135 `SetPreferredAppMode`
(System -> AllowDark, Dark -> ForceDark, Light -> ForceLight) + 136 `FlushMenuThemes` on every `App::ApplyTheme`; when unavailable the
menus use the light system style. The anchor's pressed look (`SetMenuOpen`) is set through `Callbacks.onOpen/onClose`; while a menu is
open `App::NonPanelPopups` counts it, so the full screen bar / title / edge panel stay visible. Item actions run after the menu closed.
Icons (glyph) of the descriptors are not shown. `--logictest` / `--headless-check` never show a menu (`OsDialogsBlocked`): the call only
records the descriptors (`nativemenu::LastShown`) and tests run actions with `nativemenu::InvokeItem`.

Playlist panel. Chips pick the viewed playlist. The player view is the only view (no playlist pages, no Back button, no back history): all playlist management is native menus. "+" menu: New playlist… (Ctrl+N), Import playlist file… (editable copy via `ImportPlaylist`), Import from URL… (prompt, then a read-only reference playlist through `AddPlaylistUrl` = `AddOrUpdateReference`; HLS / non-playlist URLs are refused), Open playlists folder (Explorer on `PlaylistStore::Dir()`). Two context menus. Playlist menu (right-click on a chip, which first switches the view to that playlist, or on empty list space): Play playlist (resumes it), Add ▸ (Files… Ctrl+I, Folder… Ctrl+O, URL… Ctrl+U; disabled on read-only playlists), Add to current playlist (not offered on the current one), then Rename… (Ctrl+R) / Delete playlist (Ctrl+D, in-app confirm; both also work on references), for the default chip instead Save as playlist… (names the default playlist; disabled while empty), and for reference playlists Refresh (`RefreshPlaylistReference`, offline fallback keeps the cached items with a warning bar) and Make editable copy (`ImportReferenceAsEditableCopy`). Item menu (right-click on a row, which selects it first when it is not selected), flat with separators between the groups: Play (Enter) | Copy ▸ (every other editable playlist, or a disabled "(No playlists)"; then a separator and New playlist… which creates it from the selected items) | Move to top (Ctrl+Home) / Move to bottom (Ctrl+End) (need a selection; Ctrl+Up/Down still move by keyboard) | Delete (Del = selected), Delete others (Ctrl+Del = unselected), Delete missing files (Ctrl+T), Delete items above (everything above the topmost selected item; disabled when that is row 0 or nothing is selected; no shortcut). Move and delete items are disabled on read-only playlists. A Sort icon button right of the search box opens the Sort menu (pressed look while open; same items and Ctrl+1..0 shortcuts). The footer holds only the item count text; there are no Add / Delete / Sort / move buttons. Reference playlists are read-only: their chip carries a Link glyph (U+E71B) and every edit control stays disabled. There is no export (Save as file) command. An overflowing chip strip scrolls with the wheel, by dragging it, or with hover-only ‹ › edge buttons, and keeps the viewed chip in view.
Rows: click select, double-click / Enter / P play, Del removes, drag reorders, dropped files insert at the row, current row highlighted and
scrolled into view. Search box filters. Footer: Add (files Ctrl+I, folder Ctrl+O, URL Ctrl+U), Delete (selected Del, others Ctrl+Del, missing files Ctrl+T; "Delete items above" has no shortcut),
Sort (Ctrl+1/2 sort/reverse, 3 title, 4 name, 5 extension, 6 size, 7 duration, 8 date, 9 shuffle, 0 folder), move top/up/down/bottom
(Ctrl+Home/Up/Down/End), Select all Ctrl+A, invert Ctrl+J, item count + total time. The player views (Now playing, full screen, mini player) have NO widget keyboard focus (`App::AllowsFocus` is the
`UiHost::focusFilter`). Only the playlist search box takes focus, while the user types in it (click, or
Ctrl+S from anywhere, which also takes the Win32 focus back from the video child); Esc on an empty box, Enter or a press anywhere else
releases it. Panel keys apply while the panel is ACTIVE (`PlaylistPanel::Active`): a press inside the panel activates it, a press on
the video / bar / title or hiding the panel deactivates it. Keys go to the active panel first (`PlaylistPanel::OnKeyDown`: accelerators,
then Up/Down/Home/End/PageUp/PageDown/Enter/Del on the list), everything unhandled falls through to the player shortcuts, so Ctrl+O/U/T/S,
Enter and Del mean the panel's action while it is active and the player's otherwise. Tab toggles the debug overlay (Shift+Tab does nothing).
Mouse input on the video is the video child's own (`App::VideoProc` -> `App::OnVideoInput`: move, button down / up, double click;
keys and the wheel are forwarded to the main window). A left press on the video drags the window past the drag threshold
(`App::VideoPressDrag`): the native move loop is started with the press point.

Edge snapping (`WindowSnap.h`, `App::SnapWindowMessage` on WM_MOVING / WM_SIZING; `SnapTracker` lives for one move / size loop):
- Threshold `kSnapThresholdDip` = 12 DIP. Targets: monitor work areas and whole monitor bounds (taskbar edge and screen edge) and other `aribplayer.Main` windows (outer
  edges, to sit adjacent or align); a target only counts when it overlaps the window on the other axis. Measured on the visible frame.
- Interactive only: the tracker is active strictly between WM_ENTERSIZEMOVE and WM_EXITSIZEMOVE and inert otherwise. Programmatic
  resizes (keys 1-5 / `SizeWindowToVideo`, auto-size on open, playlist toggle; all `SetWindowPos`) never snap; nothing snaps in
  WM_WINDOWPOSCHANGING.
- Works on the cursor-implied raw position (anchored on the first proposal), not on Windows' already snapped proposal.
- Hysteresis: a snap holds until the raw position has travelled the threshold from where it engaged (either direction); the released
  target cannot re-grab until the raw position has left its reach, so unsnapping takes exactly as much travel as snapping.
- Moving snaps only the leading edge per axis (the side being moved toward; the last non-zero direction, either edge when still
  unmoved), so a window slightly narrower than a gap cannot flip between its left- and right-snapped positions. Resizing snaps only
  the dragged edges, never below the minimum size.

## 5. Keyboard changes (versus the previous design)

| Key | Now | Before |
|---|---|---|
| Left / Right | seek 5 s | 10 s |
| Ctrl+Shift+Left / Right | previous / next keyframe | Alt+Left / Alt+Right |
| Ctrl+F1 | properties | Ctrl+I |
| Shift+E | equalizer | Ctrl+Shift+E |
| F2 / F4 / F5 / F6 / F7 | open folder / stop / Settings window / playlist panel / control panel | - |
| Ctrl+T | cycle always on top | - |
| Ctrl+S | focus playlist search | - |

Mini player: Esc leaves it; Stop leaves it then stops; the control panel command is a no-op; the playlist panel toggle is disabled.

## 6. Localisation

`src/App/Localize.h`: `app::Tr(en, ko)` returns the Korean text when the Windows UI language is Korean, English otherwise. Every
user-visible string in the app goes through it.

## 7. Glyph map

Segoe Fluent Icons (fallback Segoe MDL2 Assets), constants in `src/Ui/include/pui/Glyphs.h`, checked by `GlyphCoverageTests`.

| Use | Code point |
|---|---|
| Play / Pause | E768 / E769 (solid F5B0 / E62E) |
| Stop | E71A |
| Previous / Next | E622 / E623 |
| Open | E8E5 (folder E838, URL E71B) |
| Volume 0 / 1-33 / 34-66 / 67-100, Mute | E992 / E993 / E994 / E995, E74F |
| Subtitles (CC) | E7F0 (menu entries ED1E) |
| Control panel (sliders) | E9E9 |
| Playlist panel | E8FD |
| Full screen / exit | E740 / E73F |
| Mini player / exit | EE49 / EE47 |
| Pin / pinned | E718 / E840 |
| Minimize / maximize / restore / close | E921 / E922 / E923 / E8BB |
| Chevron (menu button) | E70D, small E96E / E96D |
| Search | E721 |
| Sort | E8CB |
| Move top / up / down / bottom | E96D / E74A / E74B / E96E |
| Add / Delete | E710 / E74D |

## 8. High contrast, DPI, device loss

- High contrast: the translucent panel fill and 8 % borders are replaced by `COLOR_WINDOW` / `COLOR_WINDOWTEXT` (opaque) in
  `App::ApplyTheme`; Acrylic falls back to the system window colour.
- DPI 100-200 %: layout rects are pixel-snapped; hairlines are `Theme::hairline = 1 / scale`; `pui::SnapDip` snaps panel dividers.
  A DPI change re-applies the theme.
- Device loss: `RebindUi` recreates every layer in `kLayerStack` (Content, Panel, DebugOverlay, Title, Bar, Popup), resets all paint
  hashes and repaints. An open Settings window releases its target / visuals / surfaces before the devices go and gets a new
  target for its HWND and a forced repaint before the rebind's final commit.
- Acrylic: popups get blurred regions only when every popup surface lies inside the video rect and clear of the visible bar/title;
  otherwise all popups fall back to the opaque flyout surface together (no mixed looks).

## 9. Settings window

`shell/SettingsWindow` is a separate, modeless, single-instance top-level window hosting the Settings
content (`pages/SettingsPage`, unchanged rows; every change saves and applies live through `OnSettingsChanged`). F5 and the main
menu's Settings run `CommandId::OpenSettings` -> `App::ShowSettings`: create it, or restore + bring the open one to the front.
The player keeps running and keeps its own keyboard / view state meanwhile.

- Window: unowned by the player (so the player can cover it) but owned by one invisible app-wide top-level (`aribplayer.SettingsOwner`,
  never shown, no `WS_EX_TOOLWINDOW`), which gives it no taskbar button; standard system frame
  (`WS_OVERLAPPEDWINDOW`, title "Settings" / "환경 설정", app icon; caption buttons, snap layouts, system menu and resize borders are
  all native, none of the main window's custom-caption handling). Mica (`DWMWA_SYSTEMBACKDROP_TYPE` + frame extended into the
  client) and the immersive dark caption follow `App::ApplyTheme`. Default 900 x 700 DIP centred over the main window, minimum
  480 x 360 DIP client; position / size / maximized are remembered in `AppSettings::settingsWindow` when it closes.
- Z-order: Settings sits directly above the player, in the player's always-on-top band, until the user presses a mouse button on
  the player; the player then stays in front until the next focus cycle. Nothing here activates Settings.
  - Focus cycle (`SettingsWindow::StartFocusCycle`): Settings opened, or the app comes back from another application
    (`WM_ACTIVATE` of the player or Settings whose previous window belongs to another process, `SettingsWindow::FromOtherApp`).
    The click that brought the app back is not a press on the player (`App::skipActivationPress_`). Focus moving between the
    app's own windows changes nothing.
  - Press (`App::NotePlayerPressed` -> `OnPlayerPressed`): any `WM_NC*BUTTONDOWN` / `WM_*BUTTONDOWN` (incl. double clicks) in
    `App::Proc` or the video child's `VideoProc`, whether or not the player is active; never consumed. Settings leaves the topmost
    band and sits below a non-topmost player.
  - Every place the player re-asserts its z-order (`App::ApplyWindowTopmost`, `WM_ACTIVATEAPP` deactivation,
    `WM_WINDOWPOSCHANGED`) calls `KeepAbove`, so losing focus or a topmost flip never lifts the player over Settings.
  - Settings is hidden while the player is minimized and shown again (`SW_SHOWNOACTIVATE`, raised above the player) on restore.
- Rendering: the App's UI device (`Gfx`) with its own DComp target for this HWND (content visual = page tree, popup visual =
  ComboBox flyouts + tooltip), its own `D2dRenderer` / `LayerRenderer` at its own DPI (WM_DPICHANGED) and hash-gated repaints.
  Draw / commit failures go through `Gfx::onFailure`, so a loss in either window runs the one rebind.
- Input: its own `pui::UiHost` with normal focus (Tab traversal; the player-view "no focus" rule does not apply), mouse / wheel /
  capture / keys / WM_CHAR / tooltips and its own UIA bridge. Esc closes an open flyout or the licenses dialog first, otherwise the window. Deactivating
  it closes its flyouts (not for its own dialogs, e.g. the caption font picker, which it owns).
- Edit filters... closes the window and opens the filters panel in the main window (Settings could cover it). Toasts from
  the page (caption font) still appear in the main window.
- Sections: Appearance, Playback, Captions, About. About = player version, "Open source licenses" (View opens an in-app modal
  `pages/LicensesDialog`: scrollable read-only text of `THIRD-PARTY-NOTICES.txt`, embedded in the exe as RCDATA `IDR_THIRD_PARTY_NOTICES`
  via `app.rc`; Close button / Esc), then the Debug logging toggle. The libffplay version row is gone.
- Close: close button / Esc / Alt+F4 destroy it (placement saved); closing the main window closes it first.

## Full screen edge reveal

Nothing shows while the cursor moves over the middle of the screen (the cursor itself still reappears / auto-hides after 2 s).
Three independent overlays: title strip (cursor in the top kStripDip), control bar (bottom kDockDip), playlist panel (right
kEdgeRightDip band; the panel is the existing overlay popup, the stored panel width wide and full height, shown regardless of the
windowed panel setting). Each shows and hides instantly (no slide / fade) and stays only while the cursor is over its own rect
(once shown, the rect keeps it, not just the thin edge zone), with no idle timeout; it also stays while a popup/menu is open, during
a seek drag and while the panel search box has focus, then hides at once if the cursor is outside. `AutoHide` edge mode: only
`Enter` shows; pointer `Activity`, `Reveal` (paused/stopped) and keys never do; `EdgeTick` hides; Show/Hide return `Snapped`.
The windowed and mini auto-hide keep the 3 s idle + 500 ms slide/fade. Leaving the window hides all at once.
