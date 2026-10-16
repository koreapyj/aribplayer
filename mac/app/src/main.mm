// ARIB Player for macOS: the shell widgets and pages of src/App over a Metal video view, played by the
// engine (wvp::FfPlayer over libffplay, the Mac VideoPlatform's Metal presenter from mac/platform + mac/presenter),
// in an AppKit window with an integrated title bar (traffic lights on the pui strip), a native menu bar, the Settings
// window, Finder / Dock "open" (application:openURLs: of the one running instance), drag and drop, full screen and the
// mini player. Settings, playlists and the playback history live in ~/Library/Application Support/aribplayer.
//
//   ARIB Player [FILE|URL ...]               opens the inputs like the Windows command line (OpenPaths)
//   --data-dir DIR                           settings / playlists / history in DIR instead of Application Support
//                                            (--selftest uses a fresh temporary one by default)
//   --media DIR                              queues the test clips of DIR (required for --selftest and --persist-phase 1)
//   --caption-clip PATH                      (--selftest) a TS with an ARIB caption stream (mac/app/make-arib-caption-clip.py):
//                                            the caption on / off checks run on it
//   --subs-dir DIR                           (--selftest) clip.ts + its four sidecars and extra.srt
//   --clip NAME                              only that clip of the media directory (repeat one)
//   --stats-every S                          prints a STATS line every S seconds (position, decoder, vf, A-V, presenter)
//   --deinterlace off|on|ivtc                the initial deinterlace choice
//   --audio-driver NAME                      libffplay's SDL audio driver (default: SDL's default, CoreAudio)
//   --lang ko|en                             force the UI language (default: the system's preferred language)
//   --selftest                               synthesised events through the real app (see mac/README.md "Test"); exit 0 = pass
//   --persist-phase 1|2                      two-launch persistence test (with --data-dir): 1 changes settings and plays to
//                                            12 s, then quits; 2 checks the settings, the restored playlist and the resume
//   --expect-open N                          waits for N Finder / `open -a` deliveries; exits 0 when the last one plays
//   --console                                log lines (features/Logging's format) on stderr as well
//   --log-level LEVEL                        libffplay / FFmpeg log level (quiet|error|warning|info|verbose|debug|trace|N;
//                                            default warning, info while a log sink is active), as on Windows
//   --log-file PATH                          append the log (libffplay + app events) to PATH (also --log-file=PATH)
//   Settings > About > Debug logging writes ~/Library/Logs/aribplayer/aribplayer-YYYYMMDD.log (LoggingMac.h).
#import <AppKit/AppKit.h>
#import <VideoToolbox/VideoToolbox.h>
#include <IOKit/pwr_mgt/IOPMLib.h>

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cwctype>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

#include "CaptionStyleMac.h"
#include "DebugOverlayMac.h"
#include "Localize.h"
#include "LoggingMac.h"
#include "MainView.h"
#include "MenuWindowsMac.h"
#include "PopupDismissMac.h"
#include "Engine.h"
#include "NativeMenuMac.h"
#include "OpenFlowsMac.h"
#include "SettingsWindowMac.h"
#include "ShellView.h"
#include "VideoView.h"
#include "WindowSizing.h"
#include "features/FiltersPanel.h"
#include "features/Logging.h"
#include "features/MiniLogic.h"
#include "features/OpenFlows.h"
#include "features/OpenLogic.h"
#include "features/PlaybackExtras.h"
#include "features/SettingsLogic.h"
#include "features/SettingsService.h"
#include "pui/ComboBox.h"
#include "pui/ScrollViewer.h"
#include "pui/TextBox.h"
#include "pui/Toggles.h"
#include "puimac/CGRenderer.h"
#include "puimac/MacTheme.h"
#include "puimac/MenuPopup.h"
#include "puimac/SymbolMap.h"
#include "wvp/DebugInfo.h"
#include "wvp/FfPlayer.h"
#include "wvp/JsonLite.h"
#include "wvp/MediaItem.h"
#include "wvp/PlaybackController.h"
#include "wvp/Platform.h"
#include "wvp/mac/MacVideoPlatform.h"
#include "wvp/win32/Utf.h"

namespace {
static char kAppearanceKVO;
constexpr CGFloat kStripHeight = app::kStripDip;   // 44: the traffic lights are centred on the real app's strip height
const char* kClips[] = {"tc_1080i_mpeg2.ts", "tc_1080p_hevc10.mp4", "hlg_tagged.mp4", "av_1080p_h264_aac.ts"};

long long JsonInt(const std::string& json, const char* key) {
    auto v = wvp::json::Parse(json);
    if (!v || !v->IsObject()) return -1;
    const wvp::json::Value* m = v->Find(key);
    return m && m->kind == wvp::json::Value::Kind::Number ? (long long)m->n : -1;
}

double JsonNum(const std::string& json, const char* key) {
    auto v = wvp::json::Parse(json);
    if (!v || !v->IsObject()) return NAN;
    const wvp::json::Value* m = v->Find(key);
    return m && m->kind == wvp::json::Value::Kind::Number ? m->n : NAN;
}

/// The Metal presenter's stats_json "overlay" counter `key` (rects_drawn: caption / superimpose quads drawn), -1 if absent.
long long OverlayStat(const std::string& json, const char* key) {
    auto v = wvp::json::Parse(json);
    const wvp::json::Value* o = v && v->IsObject() ? v->Find("overlay") : nullptr;
    const wvp::json::Value* m = o && o->IsObject() ? o->Find(key) : nullptr;
    return m && m->kind == wvp::json::Value::Kind::Number ? (long long)m->n : -1;
}

std::string OptionValue(const wvp::OpenOptions& o, const std::string& key) {
    for (const auto& [k, v] : o) if (k == key) return v;
    return {};
}

/// ARIBPLAYER_SELFTEST_EXPECT_SW_DECODE=1, for a machine whose VideoToolbox has no hardware H.264 / HEVC decoder (the
/// macOS 14 CI VM): the H.264 and HEVC clips are checked on the planner's software decode path (no hwaccel, frame_path
/// software, the S/W chip, system frames uploaded by the presenter) instead of VideoToolbox zero-copy. The startup step
/// fails when VideoToolbox does report a hardware decoder.
bool ExpectSoftwareDecode() {
    return [NSProcessInfo.processInfo.environment[@"ARIBPLAYER_SELFTEST_EXPECT_SW_DECODE"] isEqualToString:@"1"];
}

/// The debug info shows software decoding: no hwaccel device and the software frame path.
bool SoftwareDecoding(const wvp::DebugInfo& d) {
    return (d.Video.HwaccelDevice.empty() || d.Video.HwaccelDevice == "none") && d.Video.FramePath == "software";
}

std::wstring ToW(NSString* s) { return s ? wvp::ToWide(s.UTF8String) : std::wstring(); }
NSString* ToNS(const std::wstring& w) { return [NSString stringWithUTF8String:wvp::ToUtf8(w).c_str()]; }

NSArray<NSMenuItem*>* MenuItems(NSMenu* menu) {
    NSMutableArray<NSMenuItem*>* items = [NSMutableArray array];
    for (NSMenuItem* item in menu.itemArray) {
        [items addObject:item];
        if (item.submenu) [items addObjectsFromArray:MenuItems(item.submenu)];
    }
    return items;
}

pui::ScrollViewer* FindScrollViewer(pui::Widget* w) {
    if (!w) return nullptr;
    if (auto* sv = dynamic_cast<pui::ScrollViewer*>(w)) return sv;
    for (auto& c : w->Children())
        if (pui::ScrollViewer* f = FindScrollViewer(c.get())) return f;
    return nullptr;
}

pui::Widget* FindById(pui::Widget* w, const std::wstring& id) {
    if (!w) return nullptr;
    if (w->AutomationId() == id) return w;
    for (auto& c : w->Children())
        if (pui::Widget* f = FindById(c.get(), id)) return f;
    return nullptr;
}
}

/// A drag in the self-test: the NSDraggingInfo AppKit would hand ShellView, over a private pasteboard.
@interface TestDragInfo : NSObject <NSDraggingInfo>
@property(nonatomic, strong) NSPasteboard* pasteboard;
@property(nonatomic, assign) NSPoint location;
@property(nonatomic, weak) NSWindow* window;
@end

@implementation TestDragInfo
@synthesize draggingFormation, animatesToDestination, numberOfValidItemsForDrop, springLoadingHighlight;
- (NSWindow*)draggingDestinationWindow { return _window; }
- (NSDragOperation)draggingSourceOperationMask { return NSDragOperationCopy; }
- (NSPoint)draggingLocation { return _location; }
- (NSPoint)draggedImageLocation { return _location; }
- (NSImage*)draggedImage { return nil; }
- (NSPasteboard*)draggingPasteboard { return _pasteboard; }
- (id)draggingSource { return nil; }
- (NSInteger)draggingSequenceNumber { return 1; }
- (void)slideDraggedImageTo:(NSPoint)p { (void)p; }
- (void)enumerateDraggingItemsWithOptions:(NSDraggingItemEnumerationOptions)o forView:(NSView*)v classes:(NSArray<Class>*)c
                            searchOptions:(NSDictionary*)s usingBlock:(void (^)(NSDraggingItem*, NSInteger, BOOL*))b {
    (void)o; (void)v; (void)c; (void)s; (void)b;
}
- (void)resetSpringLoading {}
- (NSArray<NSFilePromiseReceiver*>*)namesOfPromisedFilesDroppedAtDestination:(NSURL*)u { (void)u; return @[]; }
@end

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate, NSMenuItemValidation>
@end

@implementation AppDelegate {
    NSWindow* _window;
    VideoView* _video;
    ShellView* _shell;
    std::unique_ptr<puimac::CGRenderer> _renderer;
    std::unique_ptr<mainview::MainView> _mv;
    std::unique_ptr<mainview::SettingsWindow> _settings;
    std::unique_ptr<puimac::ThemeObserver> _themeObserver;
    NSTimer* _modelTick;
    NSString* _media;
    NSString* _captionClip;   // --caption-clip: a TS with an ARIB caption stream (make-arib-caption-clip.py) for the self-test
    NSString* _subsDir;       // --subs-dir: clip.ts and its external subtitle fixtures
    std::string _dataDir;
    bool _tempDataDir;
    std::string _clip;
    std::string _audioDriver;
    std::string _deinterlace;
    double _statsEvery;
    NSTimer* _statsTimer;
    bool _launched;
    std::vector<std::wstring> _pendingOpen;   // Finder / Dock / command-line inputs that arrived before the player existed
    int _opensReceived;
    int _expectOpen;
    int _persistPhase;
    NSHashTable<NSMenuItem*>* _appMenuItems; // distinguish app-owned items from AppKit's Services/window additions
    id _pressMonitor;                         // mouse buttons pressed on the player (the Settings z-order)
    bool _skipActivationPress;                // App::skipActivationPress_: the click that activated the app
}

// --- menu bar (native, Mac conventions; the app's own menu is the pui main menu on the hamburger / video) -----------

/// SF Symbol image of a menu item. Every item of the menu bar has one: on macOS 26 a menu where only some items have
/// images (AppKit gives standard actions such as performClose: or toggleFullScreen: one by itself) indents the others.
static NSImage* MenuSymbol(NSString* name) {
    // The pui menus' table: its fallback replaces a symbol the running macOS lacks (gauge.with.needle needs macOS 14),
    // and its rotation turns some symbols (the pin leans like Fluent's), the same here.
    const puimac::SymbolEntry* entry = nullptr;
    size_t n = 0;
    const puimac::SymbolEntry* table = puimac::SymbolTable(&n);
    for (size_t i = 0; i < n; i++) if (strcmp(table[i].symbol, name.UTF8String) == 0) { entry = &table[i]; break; }
    NSImage* img = [NSImage imageWithSystemSymbolName:name accessibilityDescription:nil];
    if (!img && entry && entry->fallback)
        img = [NSImage imageWithSystemSymbolName:@(entry->fallback) accessibilityDescription:nil];
    if (!img) fprintf(stderr, "menu bar: no SF Symbol '%s'\n", name.UTF8String);
    const float degrees = entry ? entry->rotation : 0;
    if (!img || degrees == 0) return img;
    const NSSize natural = img.size;
    const double rad = degrees * M_PI / 180.0, c = std::fabs(std::cos(rad)), sn = std::fabs(std::sin(rad));
    const NSSize box = NSMakeSize(natural.width * c + natural.height * sn, natural.width * sn + natural.height * c);
    NSImage* turned = [NSImage imageWithSize:box flipped:NO drawingHandler:^BOOL(NSRect) {
        NSAffineTransform* t = [NSAffineTransform transform];
        [t translateXBy:box.width * 0.5 yBy:box.height * 0.5];
        [t rotateByDegrees:-degrees];   // clockwise on screen
        [t concat];
        [img drawInRect:NSMakeRect(-natural.width * 0.5, -natural.height * 0.5, natural.width, natural.height)];
        return YES;
    }];
    [turned setTemplate:YES];
    return turned;
}

static NSEventModifierFlags MacMods(unsigned m) {
    NSEventModifierFlags f = 0;
    if (m & app::nativemenu::kMacCmd) f |= NSEventModifierFlagCommand;
    if (m & app::nativemenu::kMacShift) f |= NSEventModifierFlagShift;
    if (m & app::nativemenu::kMacOpt) f |= NSEventModifierFlagOption;
    if (m & app::nativemenu::kMacCtrl) f |= NSEventModifierFlagControl;
    return f;
}

- (NSMenuItem*)add:(NSMenu*)menu title:(const wchar_t*)title action:(SEL)action key:(NSString*)key mods:(NSEventModifierFlags)mods
               tag:(NSInteger)tag symbol:(NSString*)symbol {
    NSMenuItem* it = [menu addItemWithTitle:ToNS(title) action:action keyEquivalent:key ?: @""];
    it.keyEquivalentModifierMask = mods;
    it.tag = tag;
    it.image = MenuSymbol(symbol);
    [_appMenuItems addObject:it];
    return it;
}

/// A menu item that runs a key-table command (CommandId in the tag) through MainView::Execute; its key equivalent is
/// the command's Mac shortcut (app::nativemenu::MacShortcuts, also what the pui menus show), none if it has none.
- (NSMenuItem*)add:(NSMenu*)menu title:(const wchar_t*)title command:(app::CommandId)c symbol:(NSString*)symbol {
    NSString* key = @"";
    NSEventModifierFlags mods = 0;
    if (const app::nativemenu::MacShortcut* sc = app::nativemenu::MacShortcutFor(c)) {
        const unichar ch = (unichar)sc->key;
        key = [NSString stringWithCharacters:&ch length:1];
        mods = MacMods(sc->mods);
    }
    return [self add:menu title:title action:@selector(runCommand:) key:key mods:mods tag:(NSInteger)c symbol:symbol];
}

- (void)buildMenuBar {
    using app::Tr;
    using C = app::CommandId;
    const NSEventModifierFlags cmd = NSEventModifierFlagCommand, opt = NSEventModifierFlagOption;
    NSMenu* bar = [[NSMenu alloc] init];
    _appMenuItems = [NSHashTable weakObjectsHashTable];
    NSString* appName = @"ARIB Player";
    auto quit = app::nativemenu::MacShortcutFor(C::Exit);
    auto settings = app::nativemenu::MacShortcutFor(C::OpenSettings);
    auto keyOf = [](const app::nativemenu::MacShortcut* sc) { const unichar ch = (unichar)sc->key; return [NSString stringWithCharacters:&ch length:1]; };

    NSMenuItem* appItem = [bar addItemWithTitle:appName action:nil keyEquivalent:@""];
    NSMenu* app = [[NSMenu alloc] initWithTitle:appName];
    [self add:app title:app::TrW(L"About ARIB Player", L"ARIB Player 정보").c_str() action:@selector(orderFrontStandardAboutPanel:) key:@"" mods:0 tag:0 symbol:@"info.circle"];
    [app addItem:[NSMenuItem separatorItem]];
    [self add:app title:Tr(L"Settings…", L"설정…") action:@selector(runCommand:) key:keyOf(settings) mods:MacMods(settings->mods) tag:(NSInteger)C::OpenSettings symbol:@"gearshape"];
    [app addItem:[NSMenuItem separatorItem]];
    NSMenuItem* services = [self add:app title:Tr(L"Services", L"서비스") action:nil key:@"" mods:0 tag:0 symbol:@"square.grid.2x2"];
    services.submenu = [[NSMenu alloc] init];
    NSApp.servicesMenu = services.submenu;
    [app addItem:[NSMenuItem separatorItem]];
    [self add:app title:Tr(L"Hide ARIB Player", L"ARIB Player 가리기") action:@selector(hide:) key:@"h" mods:cmd tag:0 symbol:@"eye.slash"];
    [self add:app title:Tr(L"Hide Others", L"기타 가리기") action:@selector(hideOtherApplications:) key:@"h" mods:cmd | opt tag:0 symbol:@"rectangle.stack"];
    [self add:app title:Tr(L"Show All", L"모두 보기") action:@selector(unhideAllApplications:) key:@"" mods:0 tag:0 symbol:@"eye"];
    [app addItem:[NSMenuItem separatorItem]];
    [self add:app title:Tr(L"Quit ARIB Player", L"ARIB Player 종료") action:@selector(terminate:) key:keyOf(quit) mods:MacMods(quit->mods) tag:0 symbol:@"power"];
    appItem.submenu = app;

    // File: no Close Window (Commands.cpp binds no Ctrl+W; the window's close button quits, as on Windows), so ⌘W is
    // unbound.
    NSMenuItem* fileItem = [bar addItemWithTitle:ToNS(Tr(L"File", L"파일")) action:nil keyEquivalent:@""];
    NSMenu* file = [[NSMenu alloc] initWithTitle:ToNS(Tr(L"File", L"파일"))];
    [self add:file title:Tr(L"Open File…", L"파일 열기…") command:C::OpenFiles symbol:@"doc"];
    [self add:file title:Tr(L"Open Folder…", L"폴더 열기…") command:C::OpenFolder symbol:@"folder"];
    [self add:file title:Tr(L"Open URL…", L"주소 열기…") command:C::OpenUrl symbol:@"link"];
    [file addItem:[NSMenuItem separatorItem]];
    [self add:file title:Tr(L"Close Media", L"미디어 닫기") command:C::Stop symbol:@"stop.circle"];
    [file addItem:[NSMenuItem separatorItem]];
    [self add:file title:Tr(L"Properties", L"속성") command:C::ShowProperties symbol:@"info.circle"];
    fileItem.submenu = file;

    NSMenuItem* editItem = [bar addItemWithTitle:ToNS(Tr(L"Edit", L"편집")) action:nil keyEquivalent:@""];
    NSMenu* edit = [[NSMenu alloc] initWithTitle:ToNS(Tr(L"Edit", L"편집"))];
    [self add:edit title:Tr(L"Cut", L"오려두기") action:@selector(cut:) key:@"x" mods:cmd tag:0 symbol:@"scissors"];
    [self add:edit title:Tr(L"Copy", L"복사하기") action:@selector(copy:) key:@"c" mods:cmd tag:0 symbol:@"doc.on.doc"];   // no text focus: copy the frame
    [self add:edit title:Tr(L"Paste", L"붙여넣기") action:@selector(paste:) key:@"v" mods:cmd tag:0 symbol:@"doc.on.clipboard"];
    [self add:edit title:Tr(L"Select All", L"전체 선택") action:@selector(selectAll:) key:@"a" mods:cmd tag:0 symbol:@"checkmark.circle"];
    [edit addItem:[NSMenuItem separatorItem]];
    [self add:edit title:Tr(L"Copy Frame", L"프레임 복사") command:C::CaptureFrame symbol:@"photo"];
    [self add:edit title:Tr(L"Find in Playlist", L"재생목록에서 찾기") command:C::FocusPlaylistSearch symbol:@"magnifyingglass"];
    editItem.submenu = edit;

    NSMenuItem* playItem = [bar addItemWithTitle:ToNS(Tr(L"Playback", L"재생")) action:nil keyEquivalent:@""];
    NSMenu* play = [[NSMenu alloc] initWithTitle:ToNS(Tr(L"Playback", L"재생"))];
    [self add:play title:Tr(L"Play / Pause  (Space)", L"재생 / 일시 정지  (Space)") command:C::PlayPause symbol:@"playpause"];
    [self add:play title:Tr(L"Previous File", L"이전 파일") command:C::Previous symbol:@"backward.end"];
    [self add:play title:Tr(L"Next File", L"다음 파일") command:C::Next symbol:@"forward.end"];
    [play addItem:[NSMenuItem separatorItem]];
    [self add:play title:Tr(L"Back 30 Seconds", L"30초 뒤로") command:C::SeekBack30 symbol:@"gobackward.30"];
    [self add:play title:Tr(L"Forward 30 Seconds", L"30초 앞으로") command:C::SkipForward30 symbol:@"goforward.30"];
    [self add:play title:Tr(L"Back 5 Minutes", L"5분 뒤로") command:C::SeekBack300 symbol:@"gobackward"];
    [self add:play title:Tr(L"Forward 5 Minutes", L"5분 앞으로") command:C::SeekForward300 symbol:@"goforward"];
    [play addItem:[NSMenuItem separatorItem]];
    [self add:play title:Tr(L"Faster  (C)", L"빠르게  (C)") command:C::SpeedUp symbol:@"hare"];
    [self add:play title:Tr(L"Slower  (X)", L"느리게  (X)") command:C::SpeedDown symbol:@"tortoise"];
    [self add:play title:Tr(L"Normal Speed  (Z)", L"보통 속도  (Z)") command:C::SpeedReset symbol:@"gauge.with.needle"];
    [play addItem:[NSMenuItem separatorItem]];
    [self add:play title:Tr(L"Subtitles On / Off", L"자막 켜기 / 끄기") command:C::ToggleSubtitles symbol:@"captions.bubble"];
    [self add:play title:Tr(L"Next Subtitle Track", L"다음 자막 트랙") command:C::CycleSubtitleTrack symbol:@"arrow.triangle.2.circlepath"];
    [self add:play title:Tr(L"Load Subtitle File…", L"자막 파일 불러오기…") command:C::LoadSubtitleFile symbol:@"doc.badge.plus"];
    NSMenuItem* syncItem = [self add:play title:Tr(L"Subtitle Sync", L"자막 싱크") action:nil key:@"" mods:0 tag:0 symbol:@"clock.arrow.2.circlepath"];
    NSMenu* sync = [[NSMenu alloc] initWithTitle:syncItem.title];
    [self add:sync title:Tr(L"0.5 Seconds Earlier", L"0.5초 빠르게") command:C::SubtitleDelayEarlier symbol:@"minus.circle"];
    [self add:sync title:Tr(L"0.5 Seconds Later", L"0.5초 느리게") command:C::SubtitleDelayLater symbol:@"plus.circle"];
    [self add:sync title:Tr(L"Reset Sync", L"싱크 초기화") command:C::SubtitleDelayReset symbol:@"arrow.counterclockwise"];
    syncItem.submenu = sync;
    [play addItem:[NSMenuItem separatorItem]];
    [self add:play title:Tr(L"Mute  (M)", L"음소거  (M)") command:C::ToggleMute symbol:@"speaker.slash"];
    playItem.submenu = play;

    NSMenuItem* viewItem = [bar addItemWithTitle:ToNS(Tr(L"View", L"보기")) action:nil keyEquivalent:@""];
    NSMenu* view = [[NSMenu alloc] initWithTitle:ToNS(Tr(L"View", L"보기"))];
    [self add:view title:Tr(L"Playlist Panel", L"재생목록 패널") command:C::TogglePlaylistPanel symbol:@"music.note.list"];
    [self add:view title:Tr(L"Control Panel", L"제어창") command:C::ToggleControlPanel symbol:@"slider.horizontal.3"];
    [view addItem:[NSMenuItem separatorItem]];
    // Mini player: pip.enter / pip.exit like the pui menu's MiniPlayer / ExitMiniPlayer glyphs (validateMenuItem:)
    [self add:view title:Tr(L"Mini Player", L"미니 플레이어") command:C::ToggleMiniPlayer symbol:@"pip.enter"];
    // Full screen: AppKit's toggleFullScreen: with ⌃⌘F (MacShortcuts). On a keyboard with a Globe key macOS shows it
    // as Globe-F (both work: Globe-F is the system's full-screen key).
    auto fsKey = app::nativemenu::MacShortcutFor(C::ToggleFullscreen);
    [self add:view title:Tr(L"Enter Full Screen", L"전체 화면 시작") action:@selector(toggleFullScreen:) key:keyOf(fsKey) mods:MacMods(fsKey->mods) tag:0
       symbol:@"arrow.up.left.and.arrow.down.right"];
    [view addItem:[NSMenuItem separatorItem]];
    [self add:view title:Tr(L"Half Size  (1)", L"50%  (1)") command:C::WindowSize50 symbol:@"1.square"];
    [self add:view title:Tr(L"Actual Size  (2)", L"100%  (2)") command:C::WindowSize100 symbol:@"2.square"];
    [self add:view title:Tr(L"150%  (3)", L"150%  (3)") command:C::WindowSize150 symbol:@"3.square"];
    [self add:view title:Tr(L"Double Size  (4)", L"200%  (4)") command:C::WindowSize200 symbol:@"4.square"];
    [self add:view title:Tr(L"Fit to Screen  (5)", L"화면에 맞춤  (5)") command:C::WindowSizeMax symbol:@"5.square"];
    [view addItem:[NSMenuItem separatorItem]];
    [self add:view title:Tr(L"Always on Top", L"항상 위에") command:C::CycleAlwaysOnTop symbol:@"pin"];
    [self add:view title:Tr(L"Zoom to Fill", L"화면 채우기") command:C::ZoomToFill symbol:@"arrow.up.and.down.and.arrow.left.and.right"];
    viewItem.submenu = view;

    NSMenuItem* windowItem = [bar addItemWithTitle:ToNS(Tr(L"Window", L"윈도우")) action:nil keyEquivalent:@""];
    NSMenu* window = [[NSMenu alloc] initWithTitle:ToNS(Tr(L"Window", L"윈도우"))];
    [self add:window title:Tr(L"Minimize", L"최소화") action:@selector(performMiniaturize:) key:@"m" mods:cmd tag:0 symbol:@"minus.rectangle"];
    [self add:window title:Tr(L"Zoom", L"확대/축소") action:@selector(performZoom:) key:@"" mods:0 tag:0 symbol:@"plus.rectangle"];
    [window addItem:[NSMenuItem separatorItem]];
    [self add:window title:Tr(L"Bring All to Front", L"모두 앞으로 가져오기") action:@selector(arrangeInFront:) key:@"" mods:0 tag:0 symbol:@"macwindow.on.rectangle"];
    windowItem.submenu = window;
    NSApp.windowsMenu = window;
    NSApp.mainMenu = bar;
}

- (void)runCommand:(NSMenuItem*)sender {
    if (!_mv) return;
    _mv->Execute((app::CommandId)sender.tag);
    [_shell afterEvent];
}

- (BOOL)validateMenuItem:(NSMenuItem*)item {
    if (item.action == @selector(runCommand:)) {
        if (!_mv) return NO;
        using C = app::CommandId;
        const auto c = (C)item.tag;
        const bool loaded = _mv->Ctl().Current() != nullptr;
        switch (c) {
        case C::ToggleMiniPlayer:
            item.image = MenuSymbol(_mv->IsMiniPlayer() ? @"pip.exit" : @"pip.enter");
            item.title = ToNS(_mv->IsMiniPlayer() ? app::Tr(L"Exit Mini Player", L"미니 플레이어 종료") : app::Tr(L"Mini Player", L"미니 플레이어"));
            return loaded || _mv->IsMiniPlayer();
        case C::Stop: case C::ShowProperties: case C::CaptureFrame:
        case C::LoadSubtitleFile: case C::CycleSubtitleTrack:
        case C::SubtitleDelayEarlier: case C::SubtitleDelayLater: case C::SubtitleDelayReset:
        case C::WindowSize50: case C::WindowSize100: case C::WindowSize150: case C::WindowSize200: case C::WindowSizeMax:
        case C::SeekBack30: case C::SkipForward30: case C::SeekBack300: case C::SeekForward300:
            return loaded;
        case C::TogglePlaylistPanel:
            item.state = _mv->PanelOpen() ? NSControlStateValueOn : NSControlStateValueOff;
            return !_mv->IsFullscreen() && !_mv->IsMiniPlayer();
        case C::ToggleMute:
            item.state = _mv->Ctl().IsMuted() ? NSControlStateValueOn : NSControlStateValueOff;
            return YES;
        case C::ToggleSubtitles:
            item.state = _mv->Svc().settings.showCaptions ? NSControlStateValueOn : NSControlStateValueOff;
            return YES;
        case C::CycleAlwaysOnTop:
            item.state = _mv->Svc().settings.alwaysOnTop != wvp::AlwaysOnTop::Disable ? NSControlStateValueOn : NSControlStateValueOff;
            return YES;
        default: break;
        }
        if (c == C::Previous) return _mv->Ctl().HasPrevious();
        if (c == C::Next) return _mv->Ctl().HasNext();
        return YES;
    }
    if (item.action == @selector(toggleFullScreen:)) {
        item.title = ToNS(_mv && _mv->IsFullscreen() ? app::Tr(L"Exit Full Screen", L"전체 화면 종료") : app::Tr(L"Enter Full Screen", L"전체 화면 시작"));
        item.image = MenuSymbol(_mv && _mv->IsFullscreen() ? @"arrow.down.right.and.arrow.up.left" : @"arrow.up.left.and.arrow.down.right");
        return _mv && !_mv->IsMiniPlayer();
    }
    return YES;
}

- (void)toggleFullScreen:(id)sender { (void)sender; if (_mv) _mv->Execute(app::CommandId::ToggleFullscreen); }

// --- window -------------------------------------------------------------------------------------------------------------

- (void)createWindow {
    NSRect frame = NSMakeRect(0, 0, 1280, 760);
    NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
                              NSWindowStyleMaskResizable | NSWindowStyleMaskFullSizeContentView;
    _window = [[NSWindow alloc] initWithContentRect:frame styleMask:style backing:NSBackingStoreBuffered defer:NO];
    _window.title = @"ARIB Player";
    _window.titlebarAppearsTransparent = YES;
    _window.titleVisibility = NSWindowTitleHidden;
    _window.titlebarSeparatorStyle = NSTitlebarSeparatorStyleNone;
    // Managed: always on top (the floating level) keeps the window in its Space and in Mission Control, as a topmost
    // window stays on its virtual desktop and in Task View on Windows; it can still enter full screen.
    _window.collectionBehavior |= NSWindowCollectionBehaviorFullScreenPrimary | NSWindowCollectionBehaviorManaged;
    _window.tabbingMode = NSWindowTabbingModeDisallowed;
    _window.minSize = NSMakeSize(500, 374);
    _window.delegate = self;
    _window.releasedWhenClosed = NO;
    [_window center];

    NSVisualEffectView* fx = [[NSVisualEffectView alloc] initWithFrame:frame];   // the Mica counterpart
    fx.material = NSVisualEffectMaterialUnderWindowBackground;
    fx.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    fx.state = NSVisualEffectStateFollowsWindowActiveState;
    _window.contentView = fx;

    _video = [[VideoView alloc] initWithFrame:NSMakeRect(0, 0, 16, 9)];
    [fx addSubview:_video];
    __weak AppDelegate* weakSelf = self;
    // resizes / backing changes -> FfPlayer::CurrentPresenter() -> ffp_metal_presenter_view_changed (main thread)
    _video.onViewChanged = [weakSelf] { AppDelegate* s = weakSelf; if (s && s->_mv) s->_mv->Eng().ViewChanged(); };

    _renderer = std::make_unique<puimac::CGRenderer>();
    _renderer->SetScale((float)_window.backingScaleFactor);
    try {
        mainview::MainView::Options opts;
        opts.audioDriver = _audioDriver;
        _mv = std::make_unique<mainview::MainView>(_renderer.get(), (__bridge void*)_video, _dataDir, opts);
    } catch (const std::exception& e) {
        fprintf(stderr, "player: %s\n", e.what());
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = @"ARIB Player cannot start";
        alert.informativeText = [NSString stringWithUTF8String:e.what()];
        [alert runModal];
        exit(3);
    }
    _mv->hooks.window = [weakSelf]() -> HWND { AppDelegate* s = weakSelf; return s ? (HWND)(__bridge void*)s->_window : nullptr; };
    _mv->hooks.videoRect = [weakSelf](const app::RectPx& r) { [weakSelf placeVideo:r]; };
    _mv->hooks.toggleFullScreen = [weakSelf] { AppDelegate* s = weakSelf; if (s) [s->_window toggleFullScreen:nil]; };
    _mv->hooks.resizeForPanel = [weakSelf](int videoW, bool wanted) { [weakSelf resizeForPanel:videoW wanted:wanted]; };
    _mv->hooks.sizeWindowToVideo = [weakSelf](const wvp::MediaPreparedInfo& p, double scale) { return (bool)[weakSelf sizeWindowToVideo:p scale:scale]; };
    _mv->hooks.applyTopmost = [weakSelf](bool on) {
        AppDelegate* s = weakSelf;
        if (!s || s->_mv->IsMiniPlayer()) return;
        s->_window.level = on ? NSFloatingWindowLevel : NSNormalWindowLevel;
        if (s->_settings) s->_settings->KeepAbove();   // App::NoteTopmostReasserted
    };
    _mv->hooks.cursorAt = [weakSelf]() -> std::optional<pui::PointF> {
        AppDelegate* s = weakSelf;
        if (!s || !NSApp.isActive || !s->_window.visible || s->_window.miniaturized) return std::nullopt;
        const NSPoint screen = NSEvent.mouseLocation;
        if ([NSWindow windowNumberAtPoint:screen belowWindowWithWindowNumber:0] != s->_window.windowNumber) return std::nullopt;
        const NSPoint p = [s->_shell convertPoint:[s->_window convertPointFromScreen:screen] fromView:nil];
        return pui::PointF{(float)p.x, (float)p.y};
    };
    _mv->hooks.applyTheme = [weakSelf](pui::ThemeMode m, std::uint32_t) { [weakSelf setAppearance:m]; };
    _mv->hooks.miniPlayer = [weakSelf](bool on) { AppDelegate* s = weakSelf; if (s) { [s->_shell relayout]; if (!on) [s relayoutTrafficLightsSoon]; } };
    _mv->hooks.openSettings = [weakSelf] { [weakSelf openSettings]; };
    _mv->hooks.copyFrame = [](const wvp::BgraFrame& f) { [AppDelegate copyFrameToPasteboard:f]; };
    _mv->hooks.quit = [] { [NSApp terminate:nil]; };
    _mv->hooks.layoutChanged = [weakSelf] { AppDelegate* s = weakSelf; if (s && s->_shell) [s->_window invalidateCursorRectsForView:s->_shell]; };

    _shell = [[ShellView alloc] initWithFrame:fx.bounds mainView:_mv.get() renderer:_renderer.get()];
    _shell.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [fx addSubview:_shell];
    menuwindows::Attach(_mv->host, (__bridge void*)_shell);   // the shell's menus in their own windows
    popupdismiss::Watch(_mv->host, (__bridge void*)_window, {}, [weakSelf] { AppDelegate* s = weakSelf; if (s) [s->_shell afterEvent]; });   // App WM_ACTIVATE / SIZE_MINIMIZED
    _themeObserver = std::make_unique<puimac::ThemeObserver>([weakSelf] {
        AppDelegate* s = weakSelf;
        if (!s) return;
        [s->_shell applyTheme];
        if (s->_settings) s->_settings->ApplyTheme();
    });

    // App::Create: settings (theme, placement), the restored playlist, then the window shows.
    _mv->Start();
    if (_deinterlace == "on") _mv->SetDeinterlace(wvp::DeinterlaceMode::On);
    else if (_deinterlace == "ivtc") _mv->SetDeinterlace(wvp::DeinterlaceMode::Ivtc);
    else if (_deinterlace == "off") _mv->SetDeinterlace(wvp::DeinterlaceMode::Off);

    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:_shell];
    [_shell relayout];
    [self layoutTrafficLights];
    // system light/dark switches don't go through setAppearance:
    [NSApp addObserver:self forKeyPath:@"effectiveAppearance" options:0 context:&kAppearanceKVO];

    // App::NotePlayerPressed: any mouse button pressed on the player (title strip, traffic lights, video, bar), whether or
    // not it is key; never consumed.
    _pressMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskLeftMouseDown | NSEventMaskRightMouseDown | NSEventMaskOtherMouseDown
                                                          handler:^NSEvent*(NSEvent* e) {
        AppDelegate* s = weakSelf;
        if (s && e.window == s->_window) [s notePlayerPressed];
        return e;
    }];

    _modelTick = [NSTimer scheduledTimerWithTimeInterval:0.1 repeats:YES block:^(NSTimer*) {
        AppDelegate* s = weakSelf;
        if (!s) return;
        s->_mv->Tick();
        [s->_shell afterEvent];
    }];
}

+ (bool)screenLocked {
    NSDictionary* d = CFBridgingRelease(CGSessionCopyCurrentDictionary());
    return [d[@"CGSSessionScreenIsLocked"] boolValue];
}

+ (void)copyFrameToPasteboard:(const wvp::BgraFrame&)f {
    if (f.Width <= 0 || f.Height <= 0 || f.Data.empty()) return;
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(const_cast<uint8_t*>(f.Data.data()), f.Width, f.Height, 8, f.Stride, cs,
                                             (CGBitmapInfo)kCGBitmapByteOrder32Little | (CGBitmapInfo)kCGImageAlphaNoneSkipFirst);
    CGImageRef img = ctx ? CGBitmapContextCreateImage(ctx) : nullptr;
    if (img) {
        NSImage* image = [[NSImage alloc] initWithCGImage:img size:NSMakeSize(f.Width, f.Height)];
        [NSPasteboard.generalPasteboard clearContents];
        [NSPasteboard.generalPasteboard writeObjects:@[image]];
        CGImageRelease(img);
    }
    if (ctx) CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
}

- (void)openSettings {
    if (!_settings) _settings = std::make_unique<mainview::SettingsWindow>(*_mv);
    _settings->Open();
}

- (void)notePlayerPressed {
    if (_skipActivationPress) {
        // The activating click: AppKit brings the clicked player to the front when it dispatches the press, after the
        // focus cycle put Settings above it; Settings goes back above it once the press is handled (Windows' order).
        _skipActivationPress = false;
        __weak AppDelegate* weakSelf = self;
        dispatch_async(dispatch_get_main_queue(), ^{ AppDelegate* s = weakSelf; if (s && s->_settings) s->_settings->KeepAbove(); });
        return;
    }
    if (_settings) _settings->OnPlayerPressed();
}

// The Settings window's focus cycle (App's WM_ACTIVATE from another process / WM_ACTIVATEAPP): the app came back from
// another application, so Settings goes above the player again; the click that did it is not a press on the player.
- (void)applicationDidBecomeActive:(NSNotification*)n {
    (void)n;
    _skipActivationPress = NSEvent.pressedMouseButtons != 0;
    if (_settings) _settings->StartFocusCycle();
}

- (void)applicationDidResignActive:(NSNotification*)n {
    (void)n;
    if (_mv) _mv->ApplyWindowTopmost();   // re-asserts the level; Settings stays above unless the player is in front
}

- (void)setAppearance:(pui::ThemeMode)m {
    NSApp.appearance = m == pui::ThemeMode::Dark ? [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua]
                     : m == pui::ThemeMode::Light ? [NSAppearance appearanceNamed:NSAppearanceNameAqua] : nil;
    [_shell applyTheme];
    if (_settings) _settings->ApplyTheme();
    [self relayoutTrafficLightsSoon];
}

/// AppKit re-lays out the title bar (back to its default height) after appearance changes and some window state
/// changes, which moves the traffic lights up; put them back once that pass has run.
- (void)relayoutTrafficLightsSoon {
    [self layoutTrafficLights];
    __weak AppDelegate* weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf layoutTrafficLights]; });
}

- (void)observeValueForKeyPath:(NSString*)keyPath ofObject:(id)object change:(NSDictionary*)change context:(void*)context {
    if (context == &kAppearanceKVO && [keyPath isEqualToString:@"effectiveAppearance"]) { [self relayoutTrafficLightsSoon]; return; }
    [super observeValueForKeyPath:keyPath ofObject:object change:change context:context];
}

/// Shell layout (device px, top-left origin) -> the video view's frame in the (bottom-left) content view.
- (void)placeVideo:(const app::RectPx&)r {
    if (r.Empty()) { _video.hidden = YES; return; }
    const CGFloat s = _window.backingScaleFactor, H = NSHeight(_window.contentView.bounds);
    NSRect f = NSMakeRect(r.l / s, H - r.b / s, r.w() / s, r.h() / s);
    _video.hidden = NO;
    if (!NSEqualRects(_video.frame, f)) _video.frame = f;
}

/// App::ResizeWindowForPlaylistPanel: the right edge moves by the panel width + gap so the video keeps its size, never
/// below the window's minimum width (kMinWindowWidthDip). A zoomed (maximized) or minimized window keeps its frame and
/// the video resizes instead (App::TogglePlaylistPanel's !IsZoomed / !IsIconic).
- (void)resizeForPanel:(int)videoW wanted:(bool)wanted {
    (void)videoW;
    if ((_window.styleMask & NSWindowStyleMaskFullScreen) || _window.zoomed || _window.miniaturized) { [_shell relayout]; return; }
    const CGFloat delta = (_mv->Svc().settings.playlistPanelWidthDip + app::kBodyGapDip) * (wanted ? 1 : -1);
    NSRect f = _window.frame;
    NSRect work = _window.screen ? _window.screen.visibleFrame : NSZeroRect;
    f.size.width = std::max(f.size.width + delta, _window.minSize.width);
    if (!NSIsEmptyRect(work)) {
        if (NSMaxX(f) > NSMaxX(work)) f.origin.x = std::max(NSMinX(work), NSMaxX(work) - NSWidth(f));
        if (NSWidth(f) > NSWidth(work)) f.size.width = NSWidth(work);
    }
    [_window setFrame:f display:YES animate:NO];
    [_shell relayout];
}

/// App::SizeWindowToVideo: the Windows sizing math (WindowSizing.h), in screen-local device pixels with a top-left
/// origin; scale = video pixels per device pixel (1 = 100 %), -1 = as large as the work area allows.
- (BOOL)sizeWindowToVideo:(const wvp::MediaPreparedInfo&)p scale:(double)scale {
    if ((_window.styleMask & NSWindowStyleMaskFullScreen) || !_window.visible || _window.miniaturized || _mv->IsMiniPlayer())
        return NO;
    NSScreen* screen = _window.screen;
    const auto size = app::VideoDisplaySize(p.Width, p.Height, p.SarNum, p.SarDen);
    if (!screen || !size) return NO;
    [_shell relayout];
    const app::RectPx& video = _mv->Layout().video;
    if (video.Empty()) return NO;
    const CGFloat s = _window.backingScaleFactor;
    const NSRect screenFrame = screen.frame, frame = _window.frame;
    auto px = [s](CGFloat pt) { return (int)std::lround(pt * s); };
    auto rect = [&](NSRect r) -> app::PxRect {
        return {px(NSMinX(r) - NSMinX(screenFrame)), px(NSMaxY(screenFrame) - NSMaxY(r)),
                px(NSMaxX(r) - NSMinX(screenFrame)), px(NSMaxY(screenFrame) - NSMinY(r))};
    };
    app::VideoWindowIn in;
    in.video = *size;
    in.scale = scale;
    in.window = rect(frame);
    in.work = rect(screen.visibleFrame);
    in.extraW = px(NSWidth(frame)) - video.w();
    in.extraH = px(NSHeight(frame)) - video.h();
    in.minW = px(_window.minSize.width);
    in.minH = px(_window.minSize.height);
    app::PxRect r = app::VideoWindowRect(in);
    // The shared function centres; seed a rect of the computed size at the old top-left, then let it clamp that anchor.
    in.window = {in.window.l, in.window.t, in.window.l + r.w(), in.window.t + r.h()};
    r = app::VideoWindowRect(in);
    if (_window.zoomed) [_window zoom:nil];
    NSRect f = NSMakeRect(NSMinX(screenFrame) + r.l / s, NSMaxY(screenFrame) - r.b / s, r.w() / s, r.h() / s);
    [_window setFrame:f display:YES animate:NO];
    [_shell relayout];
    return YES;
}

/// Centres the traffic lights on the pui title strip (strip height = kStripDip).
- (void)layoutTrafficLights {
    if ((_window.styleMask & NSWindowStyleMaskFullScreen) || (_mv && _mv->IsMiniPlayer())) return;
    NSButton* close = [_window standardWindowButton:NSWindowCloseButton];
    NSButton* mini = [_window standardWindowButton:NSWindowMiniaturizeButton];
    NSButton* zoom = [_window standardWindowButton:NSWindowZoomButton];
    if (!close || !mini || !zoom) return;
    NSView* titlebar = close.superview;
    NSView* container = titlebar.superview;
    NSView* frameView = _window.contentView.superview;
    CGFloat h = kStripHeight;
    if (container && frameView) {
        NSRect cf = container.frame;
        cf.size.height = h;
        cf.origin.y = NSHeight(frameView.bounds) - h;
        container.frame = cf;
        NSRect tf = titlebar.frame;
        tf.size.height = h;
        tf.origin.y = 0;
        titlebar.frame = tf;
    }
    CGFloat x = 12;
    for (NSButton* b in @[close, mini, zoom]) {
        NSRect f = b.frame;
        f.origin.x = x;
        f.origin.y = std::round((h - NSHeight(f)) / 2);
        b.frame = f;
        x += NSWidth(f) + 6;
    }
}

- (void)windowDidResize:(NSNotification*)n { (void)n; [self layoutTrafficLights]; }
- (void)windowDidChangeScreen:(NSNotification*)n { (void)n; if (_mv) _mv->Eng().DisplayChanged(); }
- (void)windowDidEndLiveResize:(NSNotification*)n { (void)n; [self layoutTrafficLights]; }
- (void)windowDidBecomeKey:(NSNotification*)n { (void)n; [self layoutTrafficLights]; [_shell setNeedsDisplay:YES]; }
- (void)windowDidMiniaturize:(NSNotification*)n { (void)n; if (_settings) _settings->OnPlayerMinimized(true); }
- (void)windowDidDeminiaturize:(NSNotification*)n { (void)n; if (_settings) _settings->OnPlayerMinimized(false); }
- (void)windowDidResignKey:(NSNotification*)n { (void)n; [self layoutTrafficLights]; [_shell setNeedsDisplay:YES]; }
- (void)windowDidEnterFullScreen:(NSNotification*)n { (void)n; _mv->SetFullscreenState(true); [_shell afterEvent]; }
- (void)windowDidExitFullScreen:(NSNotification*)n { (void)n; _mv->SetFullscreenState(false); [self relayoutTrafficLightsSoon]; [_shell afterEvent]; }
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)a { (void)a; return YES; }
- (BOOL)windowShouldClose:(NSWindow*)w { (void)w; [NSApp terminate:nil]; return NO; }

- (void)applicationWillTerminate:(NSNotification*)n {
    (void)n;
    [_modelTick invalidate];
    [_statsTimer invalidate];
    if (_pressMonitor) { [NSEvent removeMonitor:_pressMonitor]; _pressMonitor = nil; }
    if (_settings) _settings->Close();
    if (_mv) _mv->Shutdown();   // settings.json (window placement, current playlist), the resume position
    _settings.reset();
    menuwindows::Detach();
    if (_mv) popupdismiss::Unwatch(_mv->host);
    _mv.reset();                // controller, then FfPlayer (ffp_destroy) and its presenter
    if (_tempDataDir) {
        std::error_code ec;
        std::filesystem::remove_all(_dataDir, ec);
    }
}

// --- opening files: Finder (double click, Open With), drops on the Dock icon, `open -a`, the command line ----------------

- (void)application:(NSApplication*)app openURLs:(NSArray<NSURL*>*)urls {
    (void)app;
    std::vector<std::wstring> paths;
    for (NSURL* u in urls) paths.push_back(ToW(u.isFileURL ? u.path : u.absoluteString));
    _opensReceived++;
    printf("OPEN %s\n", wvp::ToUtf8(paths.empty() ? std::wstring() : paths.front()).c_str());
    fflush(stdout);
    if (!_launched || !_mv) { _pendingOpen.insert(_pendingOpen.end(), paths.begin(), paths.end()); return; }
    app::OpenPaths(*_mv, paths);
    [_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (BOOL)applicationShouldHandleReopen:(NSApplication*)app hasVisibleWindows:(BOOL)visible {
    (void)app;
    if (!visible) [_window makeKeyAndOrderFront:nil];
    return YES;
}

// --- playback ---------------------------------------------------------------------------------------------------------

- (void)queueClips {
    std::vector<wvp::MediaItem> items;
    std::vector<std::string> names;
    if (!_clip.empty()) names.push_back(_clip);
    else for (const char* c : kClips) names.emplace_back(c);
    for (const std::string& c : names) {
        NSString* p = [_media stringByAppendingPathComponent:[NSString stringWithUTF8String:c.c_str()]];
        if ([[NSFileManager defaultManager] fileExistsAtPath:p]) items.emplace_back(std::string(p.UTF8String));
    }
    if (items.empty()) { fprintf(stderr, "no test clips in %s\n", _media.UTF8String); return; }
    _mv->Ctl().SetRepeat(_clip.empty() ? wvp::RepeatMode::All : wvp::RepeatMode::One);
    _mv->ReplaceUnnamedPlaylistAtPosition(items, -1);
}

/// One line of engine diagnostics: position, decoder / hwaccel, vf, A-V sync, presenter stats.
- (std::string)statsLine {
    wvp::FfPlayer* pl = _mv->Player();
    std::string out;
    char head[160];
    snprintf(head, sizeof head, "pos %.2f/%.2f state %d", pl->Position(), pl->Duration(), (int)pl->State());
    out = head;
    if (auto j = pl->GetDebugInfoJson()) {
        const wvp::DebugInfo d = wvp::ParseDebugInfo(*j);
        char b[512];
        snprintf(b, sizeof b, " | video %s decoder %s hwaccel %s fmt %s | vf %s | audio %s %s out %s | master %s A-V %.1f ms | fps %.2f drops %lld/%lld",
                 d.Video.Codec.c_str(), d.Video.Decoder.c_str(), d.Video.HwaccelDevice.c_str(), d.Video.PixFmt.c_str(),
                 d.Filters.VfGraph.empty() ? "-" : d.Filters.VfGraph.c_str(), d.HasAudio ? d.Audio.Codec.c_str() : "-",
                 d.HasAudio ? d.Audio.ChannelLayout.c_str() : "", d.Audio.OutDevice.empty() ? "-" : d.Audio.OutDevice.c_str(),
                 d.Sync.Master.c_str(), d.Sync.AvDiffMs.value_or(NAN), d.Fps.Displayed.value_or(NAN),
                 (long long)d.Fps.DropsEarly.value_or(0), (long long)d.Fps.DropsLate.value_or(0));
        out += b;
    }
    out += " | presenter " + _mv->Eng().PresenterStats();
    return out;
}

// --- self test: synthesised NSEvents through ShellView (the shell widgets drive the real controller / player) ----------

- (NSEvent*)mouseEvent:(NSEventType)type at:(pui::PointF)p clicks:(NSInteger)n {
    NSPoint inWindow = [_shell convertPoint:NSMakePoint(p.x, p.y) toView:nil];
    return [NSEvent mouseEventWithType:type location:inWindow modifierFlags:0 timestamp:0 windowNumber:_window.windowNumber
                               context:nil eventNumber:0 clickCount:n pressure:1];
}

- (void)click:(pui::PointF)p {
    [_shell mouseDown:[self mouseEvent:NSEventTypeLeftMouseDown at:p clicks:1]];
    [_shell mouseUp:[self mouseEvent:NSEventTypeLeftMouseUp at:p clicks:1]];
}

- (void)key:(unsigned short)code chars:(NSString*)chars {
    NSEvent* down = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:_window.windowNumber
                                      context:nil characters:chars charactersIgnoringModifiers:chars isARepeat:NO keyCode:code];
    NSEvent* up = [NSEvent keyEventWithType:NSEventTypeKeyUp location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:_window.windowNumber
                                    context:nil characters:chars charactersIgnoringModifiers:chars isARepeat:NO keyCode:code];
    [_shell keyDown:down];
    [_shell keyUp:up];
}

/// A mouse event at a shell-view point for -[NSWindow sendEvent:] (AppKit's dispatch: its hit test, mouse-down view).
- (NSEvent*)windowEvent:(NSEventType)type at:(pui::PointF)p {
    NSPoint inWindow = [_shell convertPoint:NSMakePoint(p.x, p.y) toView:nil];
    return [NSEvent mouseEventWithType:type location:inWindow modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime
                          windowNumber:_window.windowNumber context:nil eventNumber:0 clickCount:1 pressure:1];
}

- (void)moveTo:(pui::PointF)p {
    [_shell mouseMoved:[self mouseEvent:NSEventTypeMouseMoved at:p clicks:0]];
}

- (void)after:(double)seconds run:(void (^)(void))block {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * NSEC_PER_SEC)), dispatch_get_main_queue(), block);
}

typedef void (^SelfTestStep)(void (^next)(void));

- (void)runSteps:(NSMutableArray<SelfTestStep>*)steps {
    if (steps.count == 0) return;
    SelfTestStep step = steps.firstObject;
    [steps removeObjectAtIndex:0];
    step(^{ [self runSteps:steps]; });
}

/// Polls cond every 50 ms until it holds or `seconds` passed, then runs next (the checks decide).
- (void)waitFor:(bool (^)(void))cond seconds:(double)seconds then:(void (^)(void))next {
    [self poll:cond deadline:CFAbsoluteTimeGetCurrent() + seconds then:next];
}

- (void)poll:(bool (^)(void))cond deadline:(CFAbsoluteTime)deadline then:(void (^)(void))next {
    if (cond() || CFAbsoluteTimeGetCurrent() > deadline) { next(); return; }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.05 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        [self poll:cond deadline:deadline then:next];
    });
}

- (wvp::DebugInfo)debugInfo {
    auto j = _mv->Player()->GetDebugInfoJson();
    return j ? wvp::ParseDebugInfo(*j) : wvp::DebugInfo{};
}

/// The engine serial of the picture on screen when nothing newer is decoded (frame trace), else -1.
- (std::int64_t)shownSerial {
    const wvp::DebugFrameTrace t = [self debugInfo].FrameTrace;
    return t.Presented.Serial && t.Decoded.Serial && *t.Presented.Serial == *t.Decoded.Serial ? *t.Presented.Serial : -1;
}

/// Seeks to `seconds` while playing, pauses once a picture of that seek is on screen and playback reached `seconds`,
/// then waits for Paused; `next` gets whether the paused picture is from the seek. A pause that lands between the seek
/// and its first picture keeps the pre-seek picture on screen, and the display thread still drops the queued pictures
/// of the old serial while paused, so the next paused redraw (a subtitle style change, for one) shows the last dropped
/// picture: the paused video pixels change. The engine's frame trace tells the cases apart. The serial before the seek
/// is read once a picture is on screen and nothing newer is decoded: right after an open neither is reported yet, and
/// the open's first pictures would then pass for the seek's. A picture of the seek is on screen once the presented
/// serial is newer than that and equal to the decoded serial.
- (void)seek:(double)seconds thenPause:(void (^)(bool landed))next {
    wvp::FfPlayer* player = _mv->Player();
    auto shown = ^std::int64_t { return [self shownSerial]; };
    [self waitFor:^bool { return player->State() == wvp::PlayerState::Playing && shown() >= 0; } seconds:10 then:^{
        const std::int64_t serial = shown();
        player->Seek(seconds);
        [self waitFor:^bool { return player->State() == wvp::PlayerState::Playing && shown() > serial && player->Position() >= seconds; }
               seconds:10 then:^{
            player->Pause();
            [self waitFor:^bool { return player->State() == wvp::PlayerState::Paused; } seconds:5 then:^{
                const std::int64_t now = shown();
                printf("     seek to %.1f s: shown serial %lld -> %lld, paused at %.3f s\n", seconds, (long long)serial, (long long)now, player->Position());
                next(player->State() == wvp::PlayerState::Paused && serial >= 0 && now > serial);
            }];
        }];
    }];
}

// One sample of the engine's sync state: A-V (audio clock - video clock, ms; NaN when the clip is not playing with the
// audio master), both clocks (s), the sampling time (s) and the late frame drops so far.
struct AvSample { double avMs, audio, video, at; long long dropsLate; };

- (void)sampleAv:(int)count interval:(double)interval into:(std::shared_ptr<std::vector<AvSample>>)samples then:(void (^)(void))done {
    if ((int)samples->size() >= count) { done(); return; }
    const wvp::DebugInfo d = [self debugInfo];
    const bool playing = _mv->Player()->State() == wvp::PlayerState::Playing && d.Sync.Master == "audio";
    samples->push_back({playing && d.Sync.AvDiffMs ? *d.Sync.AvDiffMs : NAN, d.Sync.AudioClock.value_or(NAN),
                        d.Sync.VideoClock.value_or(NAN), CACurrentMediaTime(), d.Sync.FrameDropsLate.value_or(0)});
    [self after:interval run:^{ [self sampleAv:count interval:interval into:samples then:done]; }];
}

- (bool)playingIndex:(int)index past:(double)seconds {
    wvp::FfPlayer* p = _mv->Player();
    const double pos = p->Position();
    return _mv->Ctl().CurrentIndex() == index && p->State() == wvp::PlayerState::Playing && !std::isnan(pos) && pos > seconds;
}

- (bool)playingUrlEnding:(const char*)suffix past:(double)seconds {
    wvp::FfPlayer* p = _mv->Player();
    const wvp::MediaItem* it = _mv->Ctl().Current();
    const double pos = p->Position();
    if (!it || p->State() != wvp::PlayerState::Playing || std::isnan(pos) || pos <= seconds) return false;
    const std::string& u = it->url;
    const size_t n = strlen(suffix);
    return u.size() >= n && u.compare(u.size() - n, n, suffix) == 0;
}

/// Logs the presenter's stats of the running clip and checks its zero-copy counters. The presenter's counters are
/// cumulative over its lifetime (every open), so the check uses the difference to the snapshot taken at PREPARED.
- (void)presenterCheck:(const char*)clip expectZeroCopy:(bool)zeroCopy check:(void (^)(bool, const char*))check {
    const std::string stats = _mv->Eng().PresenterStats(), base = _mv->Eng().PresenterStatsAtPrepared();
    printf("PRESENTER %s %s\n", clip, stats.c_str());
    auto delta = [&](const char* key) { const long long a = JsonInt(stats, key), b = JsonInt(base, key); return a < 0 ? -1 : a - std::max(0LL, b); };
    const long long presented = delta("frames_presented"), copies = delta("copies"), direct = delta("frames_direct"),
                    uploaded = delta("frames_uploaded"), verified = delta("zero_copy_verified"), mismatch = delta("zero_copy_mismatch");
    char what[240];
    snprintf(what, sizeof what, "%s: since PREPARED presented %lld, direct %lld, uploaded %lld, copies %lld, zero_copy_verified %lld, mismatch %lld",
             clip, presented, direct, uploaded, copies, verified, mismatch);
    printf("     %s\n", what);
    if (zeroCopy) {
        snprintf(what, sizeof what, "%s: presenter zero-copy (copies %lld of %lld frames, IOSurface verified %lld)", clip, copies, presented, verified);
        check(presented > 0 && copies == 0 && uploaded == 0 && mismatch == 0, what);
    } else {
        snprintf(what, sizeof what, "%s: presenter presents system frames (copies %lld of %lld frames)", clip, copies, presented);
        check(presented > 0, what);
    }
}

/// A drop of `paths` at a client DIP point through ShellView's NSDraggingDestination methods (what AppKit calls).
- (BOOL)dropFiles:(NSArray<NSString*>*)paths at:(pui::PointF)p {
    NSPasteboard* pb = [NSPasteboard pasteboardWithUniqueName];
    [pb clearContents];
    NSMutableArray* urls = [NSMutableArray array];
    for (NSString* s in paths) [urls addObject:[NSURL fileURLWithPath:s]];
    [pb writeObjects:urls];
    TestDragInfo* info = [[TestDragInfo alloc] init];
    info.pasteboard = pb;
    info.window = _window;
    info.location = [_shell convertPoint:NSMakePoint(p.x, p.y) toView:nil];
    const NSDragOperation op = [_shell draggingEntered:info];
    const BOOL ok = op != NSDragOperationNone && [_shell performDragOperation:info];
    [pb releaseGlobally];
    return ok;
}

- (std::string)settingsFileText {
    NSString* p = [NSString stringWithUTF8String:_mv->Svc().settingsStore.Path().c_str()];
    NSString* s = [NSString stringWithContentsOfFile:p encoding:NSUTF8StringEncoding error:nil];
    return s ? std::string(s.UTF8String) : std::string();
}

/// Alpha (0..255) of the top-left device pixel of `w` painted alone (PaintTree into a transparent bitmap with the
/// flipped point -> pixel CTM ShellView's drawRect: gets): a square-cornered surface paints it, a rounded one leaves it clear.
- (int)topLeftAlpha:(pui::Widget*)w {
    const CGFloat s = _window.backingScaleFactor;
    const pui::RectF b = w->Bounds();
    const size_t px = (size_t)std::lround(8 * s);
    std::vector<uint8_t> buf(px * px * 4, 0);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(buf.data(), px, px, 8, px * 4, cs, kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(cs);
    if (!ctx) return -1;
    CGContextTranslateCTM(ctx, 0, (CGFloat)px);
    CGContextScaleCTM(ctx, s, -s);
    CGContextTranslateCTM(ctx, -b.l, -b.t);
    _renderer->Begin(ctx, (float)s);
    w->PaintTree(*_renderer, _mv->host.GetTheme());
    _renderer->End();
    CGContextRelease(ctx);
    return buf[3];   // row 0 of the bitmap is its top
}

/// Full screen: the playlist panel at the right edge (docs/main-window-ui-notes.md, layout table row "Full screen" and
/// "Full screen edge reveal"): revealed by the right kEdgeRightDip band as the overlay popup (full height, the stored
/// width, square corners), kept while the pointer is over it, a menu from it is open or its search box has focus, closed
/// at once otherwise; the toggle stays disabled. Pointer moves are synthesised NSEvents through ShellView.
- (void)edgePanelTests:(void (^)(bool, const char*))check next:(void (^)(void))done {
    mainview::MainView* mv = _mv.get();
    app::ShellRoot* shell = mv->shell;
    auto centre = [](pui::Widget* w) { return pui::PointF{w->Bounds().cx(), w->Bounds().cy()}; };
    const pui::SizeF S = mv->host.Size();
    const pui::PointF mid{S.w / 2, S.h / 2}, edge{S.w - 2, S.h / 2};
    const bool wanted0 = mv->PanelOpen();
    mv->host.CloseAllPopups();
    [self moveTo:mid];
    const bool noneMid = mv->PanelPopup() == nullptr;
    [self moveTo:{S.w - app::kEdgeRightDip - 1, S.h / 2}];
    check(noneMid && !mv->PanelPopup(), "full screen: no playlist panel over the video or just left of the right 12 DIP band");
    mv->Svc().settings.playlistPanel = false;   // shown regardless of the windowed panel setting
    [self moveTo:edge];
    app::PlaylistPanel* p = mv->PanelPopup();
    const float w = std::min((float)mv->Svc().settings.playlistPanelWidthDip, S.w - 16);
    const pui::RectF b = p ? p->Bounds() : pui::RectF{};
    char what[240];
    snprintf(what, sizeof what, "the right 12 DIP band opens the overlay panel: full height at the right edge, the stored width "
             "(%.0f..%.0f x %.0f..%.0f in %.0f x %.0f, stored %d DIP), with the panel setting off", b.l, b.r, b.t, b.b, S.w, S.h,
             mv->Svc().settings.playlistPanelWidthDip);
    check(p && mv->host.IsPopupOpen(p) && std::fabs(b.r - S.w) < 0.5f && std::fabs(b.t) < 0.5f && std::fabs(b.b - S.h) < 0.5f &&
          std::fabs(b.w() - w) < 0.5f, what);
    mv->Svc().settings.playlistPanel = wanted0;
    const int alpha = p ? [self topLeftAlpha:p] : -1;
    snprintf(what, sizeof what, "its corners are square (top-left pixel alpha %d)", alpha);
    check(alpha >= 200, what);
    check(p && !p->Active() && !shell->Bar()->Visible() && !shell->Title()->Visible(),
          "it is not the key target until clicked into; the bar and the title strip stay hidden");
    pui::Widget* toggle = shell->Bar()->ButtonWidget(app::B_Panel);
    [self key:0x61 chars:@""];   // F6
    check(!toggle->Enabled() && !shell->Bar()->Model().panelAvailable && mv->PanelOpen() == wanted0 && mv->PanelPopup() == p,
          "the playlist toggle stays disabled (F6 changes nothing)");
    [self moveTo:{b.l + 24, S.h / 3}];
    check(mv->PanelPopup() == p, "it stays while the pointer is over its rect, off the edge band");
    [self moveTo:mid];
    check(!mv->PanelPopup(), "moving off it closes it at once");
    // a menu from it (the + button) keeps it
    [self moveTo:edge];
    p = mv->PanelPopup();
    if (!p) { check(false, "the edge band reopens it"); done(); return; }
    [self click:centre(p->PlusButton())];
    const bool menu = dynamic_cast<puimac::MenuPopup*>(mv->host.TopPopup()) != nullptr && app::nativemenu::IsOpen();
    [self moveTo:mid];
    check(menu && mv->PanelPopup() == p && mv->host.IsPopupOpen(p), "the panel's + menu is open: the panel stays with the pointer away");
    [self key:53 chars:@"\x1b"];   // Escape closes the menu
    [self waitFor:^bool { return !mv->PanelPopup(); } seconds:1 then:^{
        check(!app::nativemenu::IsOpen() && !mv->PanelPopup(), "the menu closed with the pointer away: the panel hides at once");
        // the search box keeps it
        [self moveTo:edge];
        app::PlaylistPanel* q = mv->PanelPopup();
        if (!q) { check(false, "the edge band reopens it"); done(); return; }
        [self click:centre(q->Search())];
        check(pui::TextBox::IsTextInputFocused(mv->host) && mv->host.Focused() == q->Search() && q->Active(),
              "a click into its search box focuses the box and makes the panel the key target");
        [self moveTo:mid];
        check(mv->PanelPopup() == q, "it stays while the search box has focus, with the pointer away");
        [self key:53 chars:@"\x1b"];   // Esc on the empty box hands the keyboard back
        [self waitFor:^bool { return !mv->PanelPopup(); } seconds:1 then:^{
            check(!pui::TextBox::IsTextInputFocused(mv->host) && !mv->PanelPopup(), "Esc releases the search box: the panel hides at once");
            [self moveTo:edge];
            const bool again = mv->PanelPopup() != nullptr;
            [self moveTo:{S.w / 2, S.h - 10}];
            check(again && !mv->PanelPopup() && shell->Bar()->Visible(), "the bottom edge shows the bar and closes the panel");
            // another menu (the video's main menu) open: the band opens no panel over it, then does once it closed
            [self moveTo:mid];
            [self->_shell rightMouseDown:[self mouseEvent:NSEventTypeRightMouseDown at:mid clicks:1]];
            [self->_shell rightMouseUp:[self mouseEvent:NSEventTypeRightMouseUp at:mid clicks:1]];
            const bool mainMenu = app::nativemenu::IsOpen();
            [self moveTo:edge];
            check(mainMenu && !mv->PanelPopup() && dynamic_cast<puimac::MenuPopup*>(mv->host.TopPopup()) != nullptr,
                  "with the video's main menu open the right band opens no panel over it");
            [self key:53 chars:@"\x1b"];   // Escape closes the menu
            [self waitFor:^bool { return mv->PanelPopup() != nullptr; } seconds:1 then:^{
                check(!app::nativemenu::IsOpen() && mv->PanelPopup() != nullptr, "once the menu closed, the pointer in the band opens it");
                [self moveTo:mid];
                check(!mv->PanelPopup(), "and moving off closes it");
                done();
            }];
        }];
    }];
}

/// The second half of the caption on / off step: caption visibility while paused (the engine redraws the held picture without / with
/// the caption layer) and the main menu's Subtitles entries (Off / the caption track: track cycling).
- (void)captionsPausedAndMenu:(void (^)(bool, const char*))check drawn:(long long (^)(void))drawn next:(void (^)(void))next {
    mainview::MainView* mv = _mv.get();
    app::ShellRoot* shell = mv->shell;
    wvp::FfPlayer* player = mv->Player();
    mainview::Engine& eng = mv->Eng();
    auto centre = [](pui::Widget* w) { return pui::PointF{w->Bounds().cx(), w->Bounds().cy()}; };
    auto presented = ^long long { return JsonInt(eng.PresenterStats(), "frames_presented"); };
    player->Pause();
    [self after:0.6 run:^{
        const auto shown = player->Snapshot(true);
        const long long f0 = presented(), r0 = drawn();
        [self click:centre(shell->Bar()->ButtonWidget(app::B_Subs))];
        [self after:0.6 run:^{
            const long long f1 = presented(), r1 = drawn();
            char what[200];
            snprintf(what, sizeof what, "paused: turning captions off redraws the held picture without them (presented %lld -> %lld, rects_drawn %lld -> %lld)", f0, f1, r0, r1);
            check(player->State() == wvp::PlayerState::Paused && !mv->Svc().settings.showCaptions && f1 > f0 && r1 == r0, what);
            const auto hidden = player->Snapshot(true);
            check(shown && hidden, "paused caption visibility snapshots are available");
            // A real broadcast can be paused between cues. Re-enable always redraws and restores the same pixels;
            // caption quads are required only when hiding the captions actually changes the held picture.
            const bool hadCaption = shown && hidden && shown->Data != hidden->Data;
            const long long f2 = presented(), r2 = drawn();
            [self click:centre(shell->Bar()->ButtonWidget(app::B_Subs))];
            [self after:0.6 run:^{
                const auto restored = player->Snapshot(true);
                char what2[256];
                snprintf(what2, sizeof what2, "paused: re-enable redraws and restores caption pixels (%s; presented %lld -> %lld, rects_drawn %lld -> %lld)",
                         hadCaption ? "cue present" : "cue gap", f2, presented(), r2, drawn());
                check(player->State() == wvp::PlayerState::Paused && mv->Svc().settings.showCaptions && presented() > f2 &&
                      (!hadCaption || drawn() > r2) && shown && restored && shown->Data == restored->Data, what2);
                player->Play();
                // the main menu's Subtitles > Off, then > the caption track
                auto pick = ^(int row) {
                    const pui::PointF v{mv->Layout().video.l / mv->Scale() + 60, mv->Layout().video.t / mv->Scale() + 40};
                    [self->_shell rightMouseDown:[self mouseEvent:NSEventTypeRightMouseDown at:v clicks:1]];
                    [self->_shell rightMouseUp:[self mouseEvent:NSEventTypeRightMouseUp at:v clicks:1]];
                    auto* root = dynamic_cast<puimac::MenuPopup*>(mv->host.TopPopup());
                    int subs = -1;
                    for (int k = 0; root && k < root->ItemCount(); k++) if (root->Items()[(size_t)k].text == app::Tr(L"Subtitles", L"자막")) subs = k;
                    if (!root || subs < 0) return false;
                    const pui::RectF r = root->RowRect(subs);
                    [self click:{r.cx(), r.cy()}];
                    puimac::MenuPopup* sm = root->SubMenu();
                    if (!sm || row >= sm->ItemCount()) { mv->host.CloseAllPopups(); return false; }
                    const bool checked = sm->Items()[(size_t)row].checked;
                    const pui::RectF rr = sm->RowRect(row);
                    [self click:{rr.cx(), rr.cy()}];
                    return !checked;
                };
                auto selected = ^bool {
                    auto mi = player->GetMediaInfo();
                    if (mi) for (const auto& st : mi->Streams) if (st.Type == wvp::StreamType::Subtitle && st.AribType == "caption") return st.Selected;
                    return false;
                };
                [self after:0.6 run:^{
                    const bool wasOn = selected();
                    if (auto mi = player->GetMediaInfo())
                        for (const auto& st : mi->Streams)
                            if (st.Type == wvp::StreamType::Subtitle)
                                printf("     subtitle stream %d arib '%s' tag %d selected %d\n", st.Index, st.AribType.c_str(), st.ComponentTag, st.Selected);
                    const bool clicked = pick(0);   // Off
                    [self after:1.0 run:^{
                        const long long m0 = drawn();
                        [self after:1.0 run:^{
                            char what3[320];
                            snprintf(what3, sizeof what3, "the main menu's Subtitles > Off deselects the caption track and nothing is drawn (rects_drawn %lld -> %lld; was on %d, clicked %d, selected %d, popups %d)",
                                     m0, drawn(), wasOn, clicked, selected(), (int)mv->host.PopupCount());
                            check(wasOn && clicked && !selected() && drawn() == m0 && mv->host.PopupCount() == 0, what3);
                            const long long t0 = drawn();
                            const bool clicked2 = pick(1);   // sample before selection: real broadcasts have gaps between cues
                            [self waitFor:^bool { return selected() && drawn() > t0; } seconds:5 then:^{
                                char what4[320];
                                snprintf(what4, sizeof what4, "Subtitles > the caption track selects it again and it is drawn (rects_drawn %lld -> %lld; clicked %d, selected %d)", t0, drawn(), clicked2, selected());
                                check(clicked2 && selected() && drawn() > t0, what4);
                                next();
                            }];
                        }];
                    }];
                }];
            }];
        }];
    }];
}

/// A mouse event at a host point (shell-view coordinates) for a menu window, in that window's coordinates.
- (NSEvent*)menuWindowEvent:(NSEventType)type at:(pui::PointF)p window:(NSWindow*)w {
    const NSPoint screen = [_window convertPointToScreen:[_shell convertPoint:NSMakePoint(p.x, p.y) toView:nil]];
    return [NSEvent mouseEventWithType:type location:[w convertPointFromScreen:screen] modifierFlags:0
                             timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:w.windowNumber context:nil eventNumber:0
                            clickCount:type == NSEventTypeMouseMoved ? 0 : 1 pressure:1];
}

/// Menus in their own windows (MenuWindowsMac): in the mini player and in a small main window at the screen's
/// bottom-right corner, the main menu (taller than either window) opens fully inside the screen's visible frame, beyond
/// the owner window, flipping left / above at the screen edges; submenus open beside it; hover, clicks and keys reach
/// the menus through their windows; Escape, a press outside, the owner's deactivation / move and a press in another
/// window close them.
- (void)runMenuWindowTests:(void (^)(bool, const char*))check next:(void (^)(void))next {
    mainview::MainView* mv = _mv.get();
    __block NSRect savedFrame = _window.frame;
    auto screenFrame = ^NSRect(NSRect r) {
        NSScreen* screen = self->_window.screen ?: NSScreen.mainScreen;
        for (NSScreen* s in NSScreen.screens)
            if (NSPointInRect(NSMakePoint(NSMidX(r), NSMidY(r)), s.frame)) { screen = s; break; }
        return screen.visibleFrame;
    };
    auto toScreen = ^NSPoint(pui::PointF p) { return [self->_window convertPointToScreen:[self->_shell convertPoint:NSMakePoint(p.x, p.y) toView:nil]]; };
    auto rowCentre = [](puimac::MenuPopup* m, int i) { const pui::RectF r = m->RowRect(i); return pui::PointF{r.cx(), r.cy()}; };
    auto indexOf = [](puimac::MenuPopup* m, const wchar_t* text) {
        for (int i = 0; m && i < m->ItemCount(); i++) if (m->Items()[(size_t)i].text == text) return i;
        return -1;
    };
    auto windowOf = [](puimac::MenuPopup* m) { return (__bridge NSPanel*)menuwindows::WindowOf(m); };
    auto openMenu = ^puimac::MenuPopup*(pui::PointF at) {
        [self->_shell rightMouseDown:[self mouseEvent:NSEventTypeRightMouseDown at:at clicks:1]];
        [self->_shell rightMouseUp:[self mouseEvent:NSEventTypeRightMouseUp at:at clicks:1]];
        return dynamic_cast<puimac::MenuPopup*>(mv->host.TopPopup());
    };
    auto closed = ^bool { return mv->host.PopupCount() == 0 && menuwindows::OpenWindows() == 0; };
    // the menu at `at` fits the visible frame, is taller than the owner and leaves it; flipped: opened left / above
    auto checkFits = ^(puimac::MenuPopup* root, pui::PointF at, const char* where, bool expectFlip) {
        NSPanel* w = windowOf(root);
        const NSRect f = w ? w.frame : NSZeroRect, owner = self->_window.frame, vf = screenFrame(f);
        const NSPoint click = toScreen(at);
        char what[480];
        snprintf(what, sizeof what, "%s: the main menu has its own window (non-activating panel, never key, a child of the player window at "
                 "level %ld above its level %ld, painted %d times; the host does not paint it)", where, (long)w.level, (long)self->_window.level,
                 menuwindows::PaintCount(root));
        check(root && w && w.visible && w.parentWindow == self->_window && (w.styleMask & NSWindowStyleMaskNonactivatingPanel) && !w.canBecomeKeyWindow &&
              w.level > self->_window.level && menuwindows::PaintCount(root) > 0 && root->OwnWindow(), what);
        snprintf(what, sizeof what, "%s: taller than the window it opens fully on screen (menu %.0f,%.0f %.0fx%.0f; window %.0f,%.0f %.0fx%.0f; visible frame %.0f,%.0f %.0fx%.0f)",
                 where, NSMinX(f), NSMinY(f), NSWidth(f), NSHeight(f), NSMinX(owner), NSMinY(owner), NSWidth(owner), NSHeight(owner),
                 NSMinX(vf), NSMinY(vf), NSWidth(vf), NSHeight(vf));
        check(w && NSHeight(f) > NSHeight(owner) && NSContainsRect(vf, f) && !NSContainsRect(owner, f), what);
        const NSRect expect = root ? [self->_window convertRectToScreen:[self->_shell convertRect:NSMakeRect(root->Bounds().l, root->Bounds().t, root->Bounds().w(), root->Bounds().h()) toView:nil]] : NSZeroRect;
        const bool left = std::fabs(NSMaxX(f) - click.x) < 1, above = std::fabs(NSMinY(f) - click.y) < 1;
        const bool atPoint = (left || std::fabs(NSMinX(f) - click.x) < 1) && (above || std::fabs(NSMaxY(f) - click.y) < 1);
        snprintf(what, sizeof what, "%s: the window shows the menu where it is in the host, a corner at the click (%.0f,%.0f)%s",
                 where, click.x, click.y, expectFlip ? ", opened to the left and above at the screen's bottom-right edge" : "");
        check(w && NSEqualRects(NSIntegralRect(expect), NSIntegralRect(f)) && atPoint && (!expectFlip || (left && above)), what);
    };
    // a submenu opened by hovering its row in the menu window: in its own window, fully inside the visible frame, beside
    //     the menu (to its right, or to its left where the right side leaves the screen) and level with the row
    auto checkSub = ^(puimac::MenuPopup* root, int row, const char* where, void (^then)(puimac::MenuPopup*)) {
        NSPanel* w = windowOf(root);
        if (w) [w.contentView mouseMoved:[self menuWindowEvent:NSEventTypeMouseMoved at:rowCentre(root, row) window:w]];
        [self after:0.45 run:^{
            puimac::MenuPopup* sub = root->SubMenu();
            NSPanel* sw = windowOf(sub);
            const NSRect f = sw ? sw.frame : NSZeroRect, rf = w.frame, vf = screenFrame(rf);
            // the submenu overlaps its menu by 4 pt (MenuPopup::OpenSubMenu)
            const bool right = std::fabs(NSMinX(f) - (NSMaxX(rf) - 4)) <= 2, left = std::fabs(NSMaxX(f) - (NSMinX(rf) + 4)) <= 2;
            const NSPoint rowAt = toScreen(rowCentre(root, row));
            char what[480];
            snprintf(what, sizeof what, "%s: hovering a submenu row in the menu window opens the submenu in its own window beside the menu (%s), level "
                     "with the row, inside the visible frame (submenu %.0f,%.0f %.0fx%.0f; menu %.0f,%.0f %.0fx%.0f; visible frame %.0f,%.0f %.0fx%.0f)",
                     where, right ? "to its right" : left ? "to its left" : "not adjacent",
                     NSMinX(f), NSMinY(f), NSWidth(f), NSHeight(f), NSMinX(rf), NSMinY(rf), NSWidth(rf), NSHeight(rf),
                     NSMinX(vf), NSMinY(vf), NSWidth(vf), NSHeight(vf));
            check(sub && sw && sw.visible && root->SubMenuIndex() == row && NSContainsRect(vf, f) && (left || right) &&
                  rowAt.y >= NSMinY(f) && rowAt.y <= NSMaxY(f) && sw.level == w.level && menuwindows::OpenWindows() == 2, what);
            then(sub);
        }];
    };

    NSMutableArray<SelfTestStep>* steps = [NSMutableArray array];
    // the mini player: open, a submenu, keys, Escape
    [steps addObject:^(void (^done)(void)) {
        mv->Execute(app::CommandId::ToggleMiniPlayer);
        [self after:0.6 run:^{
            const pui::PointF at{mv->Layout().video.l / mv->Scale() + 40, mv->Layout().video.t / mv->Scale() + 30};
            const bool wasKey = self->_window.keyWindow;
            puimac::MenuPopup* root = openMenu(at);
            const int video = indexOf(root, app::Tr(L"Video", L"영상"));
            [self after:0.2 run:^{
                checkFits(root, at, "mini player", false);
                check(self->_window.keyWindow == wasKey, "mini player: the player window keeps the keyboard (stays key) while the menu shows");
                if (!root || video < 0) { mv->host.CloseAllPopups(); done(); return; }
                checkSub(root, video, "mini player", ^(puimac::MenuPopup* sub) {
                    const int painted = menuwindows::PaintCount(sub);
                    [self key:124 chars:@""];   // Right: into the submenu, first item highlighted
                    [self after:0.15 run:^{
                        check(sub && root->SubMenu() == sub && sub->Highlight() == 0 && mv->host.Focused() == sub && menuwindows::PaintCount(sub) > painted,
                              "mini player: Right moves into the submenu and its window repaints the highlight");
                        [self key:53 chars:@"\x1b"];
                        check(mv->host.PopupCount() == 1 && menuwindows::OpenWindows() == 1 && !windowOf(root->SubMenu()),
                              "mini player: Escape closes the submenu and its window");
                        [self key:53 chars:@"\x1b"];
                        check(closed(), "mini player: Escape again closes the menu and its window");
                        done();
                    }];
                });
            }];
        }];
    }];
    // the mini player: clicks through the menu windows run items
    [steps addObject:^(void (^done)(void)) {
        const pui::PointF at{mv->Layout().video.l / mv->Scale() + 40, mv->Layout().video.t / mv->Scale() + 30};
        auto clickZoom = ^bool {
            puimac::MenuPopup* root = openMenu(at);
            const int video = indexOf(root, app::Tr(L"Video", L"영상"));
            NSPanel* w = windowOf(root);
            if (!root || video < 0 || !w) { mv->host.CloseAllPopups(); return false; }
            [w sendEvent:[self menuWindowEvent:NSEventTypeLeftMouseDown at:rowCentre(root, video) window:w]];
            [w sendEvent:[self menuWindowEvent:NSEventTypeLeftMouseUp at:rowCentre(root, video) window:w]];
            puimac::MenuPopup* sub = root->SubMenu();
            const int zoom = indexOf(sub, app::Tr(L"Zoom to fill", L"화면 채우기"));
            NSPanel* sw = windowOf(sub);
            if (!sub || zoom < 0 || !sw) { mv->host.CloseAllPopups(); return false; }
            [sw sendEvent:[self menuWindowEvent:NSEventTypeLeftMouseDown at:rowCentre(sub, zoom) window:sw]];
            [sw sendEvent:[self menuWindowEvent:NSEventTypeLeftMouseUp at:rowCentre(sub, zoom) window:sw]];
            return true;
        };
        auto zoomShown = ^int {   // Zoom to fill's check in a freshly opened menu
            puimac::MenuPopup* root = openMenu(at);
            const pui::MenuItemDesc* d = app::nativemenu::FindItem(app::nativemenu::LastShown(), app::Tr(L"Zoom to fill", L"화면 채우기"));
            const int v = root && d ? (d->checked ? 1 : 0) : -1;
            mv->host.CloseAllPopups();
            return v;
        };
        const bool before = app::IsZoomToFill();
        const bool clicked = clickZoom();
        const bool closedByItem = closed();
        // the item's action runs after the menu closed (posted, as Windows runs it after TrackPopupMenuEx returns)
        [self waitFor:^bool { return app::IsZoomToFill() != before; } seconds:2 then:^{
            const bool flipped = app::IsZoomToFill() != before && zoomShown() == (before ? 0 : 1);
            const bool clickedBack = clicked && clickZoom();
            [self waitFor:^bool { return app::IsZoomToFill() == before; } seconds:2 then:^{
                check(clicked && closedByItem && flipped && clickedBack && app::IsZoomToFill() == before,
                      "mini player: clicks in the menu windows open Video's submenu and run Zoom to fill (menus and windows closed, the check flips; clicked again it flips back)");
                done();
            }];
        }];
    }];
    // the mini player: a press outside, the app deactivating, the window starting to move and a press in another
    //     window close the menus
    [steps addObject:^(void (^done)(void)) {
        const pui::PointF at{mv->Layout().video.l / mv->Scale() + 40, mv->Layout().video.t / mv->Scale() + 30};
        // a press in the player window outside the menu
        puimac::MenuPopup* root = openMenu(at);
        pui::PointF outside{-1, -1};
        const pui::SizeF client = mv->host.Size();
        for (float y = 16; root && outside.x < 0 && y < client.h; y += 16)   // empty video, not the menu, not a MiniBar control
            for (float x = 16; x < client.w; x += 16)
                if (!root->Bounds().Contains({x, y}) && mv->IsVideoPoint({x, y}) && mv->shell->HitTest({x, y}) == mv->shell) { outside = {x, y}; break; }
        const bool opened = root && menuwindows::OpenWindows() == 1;
        if (outside.x >= 0) [self click:outside];
        check(opened && outside.x >= 0 && closed(), "mini player: a press in the player window outside the menu closes it and its window");
        auto closesOn = ^bool(NSNotificationName name, id object) {
            puimac::MenuPopup* m = openMenu(at);
            const bool shown = m && menuwindows::OpenWindows() == 1;
            [NSNotificationCenter.defaultCenter postNotificationName:name object:object];
            return shown && closed();
        };
        check(closesOn(NSApplicationDidResignActiveNotification, NSApp), "mini player: the app deactivating closes the menu");
        check(closesOn(NSWindowWillMoveNotification, self->_window), "mini player: the player window starting to move closes the menu");
        // a press in another window of the app, through the event queue (AppKit's local event monitors see it there)
        NSWindow* other = [[NSWindow alloc] initWithContentRect:NSMakeRect(NSMinX(screenFrame(self->_window.frame)) + 4, NSMinY(screenFrame(self->_window.frame)) + 4, 40, 40)
                                                      styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
        other.releasedWhenClosed = NO;
        [other orderFront:nil];
        root = openMenu(at);
        const bool shown = root && menuwindows::OpenWindows() == 1;
        for (NSEventType t : {NSEventTypeLeftMouseDown, NSEventTypeLeftMouseUp})
            [NSApp postEvent:[NSEvent mouseEventWithType:t location:NSMakePoint(20, 20) modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime
                                            windowNumber:other.windowNumber context:nil eventNumber:0 clickCount:1 pressure:1] atStart:NO];
        [self waitFor:^bool { return closed(); } seconds:2 then:^{
            check(shown && closed(), "mini player: a press in another window of the app closes the menu");
            [other orderOut:nil];
            mv->host.CloseAllPopups();
            mv->Execute(app::CommandId::ToggleMiniPlayer);
            [self after:0.6 run:done];
        }];
    }];
    // a small main window at the bottom-right corner of the visible frame: the menu flips left and above the click,
    //     its submenu opens beside it on screen
    [steps addObject:^(void (^done)(void)) {
        savedFrame = self->_window.frame;
        const NSRect vf = screenFrame(savedFrame);
        [self->_window setFrame:NSMakeRect(NSMaxX(vf) - 420, NSMinY(vf), 420, 300) display:YES];
        [self after:0.5 run:^{
            const pui::PointF at{mv->Layout().video.r / mv->Scale() - 16, mv->Layout().video.b / mv->Scale() - 16};
            puimac::MenuPopup* root = openMenu(at);
            const int video = indexOf(root, app::Tr(L"Video", L"영상"));
            [self after:0.2 run:^{
                checkFits(root, at, "small main window", true);
                if (!root || video < 0) { mv->host.CloseAllPopups(); done(); return; }
                checkSub(root, video, "small main window", ^(puimac::MenuPopup* sub) {
                    (void)sub;
                    [self key:53 chars:@"\x1b"];
                    [self key:53 chars:@"\x1b"];
                    check(closed(), "small main window: Escape twice closes the submenu and the menu with their windows");
                    [self->_window setFrame:savedFrame display:YES];
                    [self after:0.5 run:done];
                });
            }];
        }];
    }];
    [steps addObject:^(void (^done)(void)) { (void)done; next(); }];
    [self runSteps:steps];
}

/// Popups close where the Windows app closes them (PopupDismissMac; App / SettingsWindow WM_ACTIVATE WA_INACTIVE, App
/// SIZE_MINIMIZED): the player window losing key closes all its popups (a menu and the notice), not while one of the
/// app's own modal dialogs has the keyboard; miniaturising closes them; the Settings window losing key closes its
/// flyouts and keeps the caption style editor (Windows' owned font dialog); closing it closes its popups.
- (void)runPopupDismissTests:(void (^)(bool, const char*))check next:(void (^)(void))next {
    mainview::MainView* mv = _mv.get();
    const pui::PointF at{mv->Layout().video.l / mv->Scale() + 40, mv->Layout().video.t / mv->Scale() + 30};
    auto openPopups = ^bool {   // the app-owned notice and the video context menu beside it
        mv->ShowInfoBar(app::InfoSeverity::Informational, L"Self-test notice", L"Popup dismissal");
        [self->_shell rightMouseDown:[self mouseEvent:NSEventTypeRightMouseDown at:at clicks:1]];
        [self->_shell rightMouseUp:[self mouseEvent:NSEventTypeRightMouseUp at:at clicks:1]];
        return mv->host.PopupCount() == 2 && dynamic_cast<puimac::MenuPopup*>(mv->host.TopPopup()) && menuwindows::OpenWindows() == 1;
    };
    // `w` stops being the key window: another window becomes key (AppKit), or, where `w` is not key (an inactive
    // session), AppKit's resign-key notification is posted; `then` runs once the deferred decision ran, then `w` is key again
    auto resignKey = ^(NSWindow* w, void (^then)(const char* how)) {
        if (!w.keyWindow) {
            [NSNotificationCenter.defaultCenter postNotificationName:NSWindowDidResignKeyNotification object:w];
            [self after:0.2 run:^{ then("not key in this session: its resign-key notification posted"); }];
            return;
        }
        NSWindow* other = [[NSWindow alloc] initWithContentRect:NSMakeRect(NSMinX(w.frame) + 20, NSMinY(w.frame) + 20, 160, 90)
                                                      styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
        other.releasedWhenClosed = NO;
        [other makeKeyAndOrderFront:nil];
        [self waitFor:^bool { return !w.keyWindow; } seconds:2 then:^{
            [self after:0.2 run:^{
                then(w.keyWindow ? "another window did not become key" : "another window became key");
                [other orderOut:nil];
                [w makeKeyAndOrderFront:nil];
            }];
        }];
    };
    NSMutableArray<SelfTestStep>* steps = [NSMutableArray array];
    [steps addObject:^(void (^done)(void)) {
        const bool opened = openPopups();
        resignKey(self->_window, ^(const char* how) {
            char what[240];
            snprintf(what, sizeof what, "the player window losing key closes all its popups, the menu with its window and the notice (%s)", how);
            check(opened && mv->host.PopupCount() == 0 && menuwindows::OpenWindows() == 0, what);
            done();
        });
    }];
    [steps addObject:^(void (^done)(void)) {
        mv->ShowInfoBar(app::InfoSeverity::Informational, L"Self-test notice", L"Popup dismissal");
        const bool wasKey = self->_window.keyWindow;
        // The alert runs from a timer, not from this main-queue block: during the modal session the main queue runs, so
        // the resign-key decision is taken while the alert has the keyboard (NSApp.modalWindow), as for a dialog opened
        // from a key equivalent. A modal-mode timer ends it.
        NSTimer* show = [NSTimer timerWithTimeInterval:0.05 repeats:NO block:^(NSTimer* t) {
            (void)t;
            __block size_t during = 0;
            __block bool alertKey = false;
            NSAlert* alert = [[NSAlert alloc] init];
            alert.messageText = @"Self-test";
            NSTimer* end = [NSTimer timerWithTimeInterval:0.6 repeats:NO block:^(NSTimer* t2) {
                (void)t2;
                during = mv->host.PopupCount();
                alertKey = NSApp.keyWindow == alert.window;
                [NSApp abortModal];   // stopModal waits for another event
            }];
            [NSRunLoop.currentRunLoop addTimer:end forMode:NSModalPanelRunLoopMode];
            [alert runModal];
            char what[240];
            snprintf(what, sizeof what, "an app-modal dialog (the NSAlert prompts, NSOpenPanel) keeps the player window's popups (the player window was key: %s, the alert took it: %s)",
                     wasKey ? "yes" : "no", alertKey ? "yes" : "no");
            check(during == 1 && mv->host.PopupCount() == 1, what);
            [self after:0.3 run:^{
                check(mv->host.PopupCount() == 1, "and keeps them after it closed");
                mv->host.CloseAllPopups();
                [self->_window makeKeyAndOrderFront:nil];
                [self after:0.3 run:done];
            }];
        }];
        [NSRunLoop.currentRunLoop addTimer:show forMode:NSDefaultRunLoopMode];
    }];
    [steps addObject:^(void (^done)(void)) {
        const bool opened = openPopups();
        const bool real = ![AppDelegate screenLocked];
        if (real) [self->_window miniaturize:nil];
        else [NSNotificationCenter.defaultCenter postNotificationName:NSWindowWillMiniaturizeNotification object:self->_window];
        [self waitFor:^bool { return mv->host.PopupCount() == 0; } seconds:3 then:^{
            char what[200];
            snprintf(what, sizeof what, "miniaturising the player window closes its popups (%s)", real ? "miniaturize:" : "the session is locked: its will-miniaturize notification posted");
            check(opened && mv->host.PopupCount() == 0 && menuwindows::OpenWindows() == 0, what);
            if (self->_window.miniaturized || real) [self->_window deminiaturize:nil];
            [self waitFor:^bool { return !self->_window.miniaturized; } seconds:5 then:^{
                check(!self->_window.miniaturized, "and the window comes back");
                [self->_window makeKeyAndOrderFront:nil];
                [self after:0.3 run:done];
            }];
        }];
    }];
    [steps addObject:^(void (^done)(void)) {
        [self openSettings];
        NSWindow* sw = (__bridge NSWindow*)self->_settings->Window();
        pui::UiHost& host = self->_settings->Ui();
        pui::Widget* page = self->_settings->Page();
        auto* source = dynamic_cast<pui::ToggleSwitch*>(FindById(page, L"SubtitleUseWindowsToggle"));
        const bool sourceWas = source && source->IsOn();
        if (source) source->SetOn(false);
        if (pui::Widget* fontButton = FindById(page, L"SubtitleFontButton")) fontButton->Invoke();
        pui::Widget* editor = host.TopPopup();
        auto* fonts = dynamic_cast<pui::ComboBox*>(FindById(editor, L"SubtitleFontFamilyCombo"));
        if (fonts) fonts->Open();
        const bool opened = editor && fonts && fonts->IsOpen() && host.PopupCount() == 2;
        [self after:0.3 run:^{
            resignKey(sw, ^(const char* how) {
                char what[260];
                snprintf(what, sizeof what, "the Settings window losing key closes its flyout and keeps the caption style editor (%s)", how);
                check(opened && host.PopupCount() == 1 && host.TopPopup() == editor, what);
                if (fonts) fonts->Open();
                self->_settings->Close();
                check(host.PopupCount() == 0 && !self->_settings->IsOpen(), "closing the Settings window closes its popups");
                if (source && sourceWas) source->SetOn(true);
                [self->_window makeKeyAndOrderFront:nil];
                [self after:0.3 run:done];
            });
        }];
    }];
    [steps addObject:^(void (^done)(void)) { (void)done; next(); }];
    [self runSteps:steps];
}

- (void)runSubtitleTests:(void (^)(bool, const char*))check next:(void (^)(void))done {
    if (!_subsDir) { printf("skip external subtitles (no --subs-dir)\n"); done(); return; }
    mainview::MainView* mv = _mv.get();
    wvp::FfPlayer* player = mv->Player();
    NSString* clip = [_subsDir stringByAppendingPathComponent:@"clip.ts"];
    NSString* extra = [_subsDir stringByAppendingPathComponent:@"extra.srt"];
    const std::string korean = [_subsDir stringByAppendingPathComponent:@"clip.ko.srt"].UTF8String;
    auto drawn = ^long long { return OverlayStat(mv->Eng().PresenterStats(), "rects_drawn"); };
    auto selected = ^int {
        if (auto mi = player->GetMediaInfo())
            for (const auto& st : mi->Streams) if (st.Type == wvp::StreamType::Subtitle && st.Selected) return st.Index;
        return -1;
    };
    auto selectedPath = ^std::string {
        if (auto mi = player->GetMediaInfo())
            for (const auto& st : mi->Streams) if (st.Type == wvp::StreamType::Subtitle && st.Selected && st.external) return st.path;
        return {};
    };
    auto count = ^int {
        int n = 0;
        if (auto mi = player->GetMediaInfo()) for (const auto& st : mi->Streams) if (st.external) n++;
        return n;
    };
    auto drop = ^BOOL {
        auto v = mv->Layout().video;
        return [self dropFiles:@[extra] at:{(v.l + v.r) / (2 * mv->Scale()), (v.t + v.b) / (2 * mv->Scale())}];
    };
    NSMutableArray<SelfTestStep>* steps = [NSMutableArray array];
    [steps addObject:^(void (^next)(void)) {
        mv->Execute(app::CommandId::Stop);
        const BOOL accepted = drop();
        [self after:0.3 run:^{
            check(!accepted && !mv->Ctl().Current() && count() == 0 && player->State() != wvp::PlayerState::Playing,
                  "subtitle-only drop with nothing open returns NO and is ignored");
            const long long before = drawn();
            app::OpenPaths(*mv, {ToW(clip)});
            [self waitFor:^bool { return [self playingUrlEnding:"clip.ts" past:0.5] && selectedPath() == korean && drawn() > before; } seconds:12 then:^{
                bool paths = true;
                std::vector<std::string> actual;
                if (auto mi = player->GetMediaInfo()) for (const auto& st : mi->Streams) if (st.external) {
                    actual.push_back(std::filesystem::path(st.path).filename().string());
                    printf("     subtitle index=%d selected=%d external=%d path=%s\n", st.Index, st.Selected, st.external, st.path.c_str());
                }
                std::sort(actual.begin(), actual.end());
                paths = actual == std::vector<std::string>{"clip.ass", "clip.en.srt", "clip.ja.smi", "clip.ko.srt"};
                check(paths && count() == 4, "clip.ts auto-attaches exactly four matching sidecars, not extra.srt (MediaInfo paths)");
                check(selectedPath() == korean && drawn() > before && mv->SubsButtonLit(),
                      "the Korean sidecar is selected and drawn by the presenter; subtitle button lit");
                printf("     default text subtitle font: %s (libass fontselect logged with --log-level verbose --console)\n", mv->Svc().settings.subtitle.font.c_str());
                next();
            }];
        }];
    }];
    // Exercise the actual W binding, including the Off sentinel and wrapping to the first track.
    [steps addObject:^(void (^next)(void)) {
        auto mi = player->GetMediaInfo();
        std::vector<int> indices;
        if (mi) for (const auto& st : mi->Streams) if (st.Type == wvp::StreamType::Subtitle) indices.push_back(st.Index);
        if (indices.size() != 4) { check(false, "W cycle requires four subtitle tracks"); next(); return; }
        player->SelectStream(wvp::StreamType::Subtitle, indices.front());
        NSMutableArray<SelfTestStep>* cycle = [NSMutableArray array];
        for (size_t i = 1; i <= indices.size() + 1; i++) {
            const int want = i == indices.size() ? -1 : indices[i % (indices.size() + 1)];
            [cycle addObject:^(void (^advance)(void)) {
                [self key:13 chars:@"w"];
                [self waitFor:^bool { return selected() == want; } seconds:2 then:^{
                    char what[180];
                    snprintf(what, sizeof what, "W selects subtitle index %d (actual %d); Off then wraps to first", want, selected());
                    check(selected() == want, what);
                    check(mv->SubsButtonLit() == (want >= 0), "subtitle button lit state follows W track selection / Off");
                    advance();
                }];
            }];
        }
        [cycle addObject:^(void (^advance)(void)) { (void)advance; next(); }];
        [self runSteps:cycle];
    }];
    [steps addObject:^(void (^next)(void)) {
        if (auto mi = player->GetMediaInfo()) for (const auto& st : mi->Streams) if (st.path == korean) player->SelectStream(wvp::StreamType::Subtitle, st.Index);
        [self seek:4.5 thenPause:^(bool landed) {
            check(landed, "the 4.5 s seek shows its picture before the pause (bar subtitle button checks)");
            const long long before = drawn();
            const long long presented = JsonInt(mv->Eng().PresenterStats(), "frames_presented");
            auto button = mv->shell->Bar()->ButtonWidget(app::B_Subs)->Bounds();
            [self click:{button.cx(), button.cy()}];
            check(!mv->SubsButtonLit() && !mv->Svc().settings.showCaptions, "bar subtitle button hides text subtitles and unlights");
            [self after:0.4 run:^{
                const long long after = JsonInt(mv->Eng().PresenterStats(), "frames_presented");
                char what[200];
                snprintf(what, sizeof what, "paused subtitle hide redraws without overlays (frames_presented %lld -> %lld; rects_drawn %lld -> %lld)",
                         presented, after, before, drawn());
                check(player->State() == wvp::PlayerState::Paused && after > presented && drawn() == before, what);
                auto b = mv->shell->Bar()->ButtonWidget(app::B_Subs)->Bounds();
                [self click:{b.cx(), b.cy()}];
                [self waitFor:^bool { return drawn() > before; } seconds:3 then:^{
                    check(drawn() > before && mv->SubsButtonLit(), "bar subtitle button shows text subtitles again (presenter rects resume; lit)");
                    [self key:43 chars:@","];
                    check(player->SubtitleDelay() == -500 && mv->LastToast().find(L"-0.5") != std::wstring::npos,
                          "comma sets engine subtitle delay -0.5 s with sync toast");
                    [self key:47 chars:@"."];
                    [self key:47 chars:@"."];
                    check(player->SubtitleDelay() == 500 && mv->LastToast().find(L"+0.5") != std::wstring::npos,
                          "period sets engine subtitle delay +0.5 s with sync toast");
                    [self key:44 chars:@"/"];
                    check(player->SubtitleDelay() == 0 && mv->LastToast().find(L"+0.0") != std::wstring::npos,
                          "slash resets engine subtitle delay with sync toast");
                    next();
                }];
            }];
        }];
    }];
    [steps addObject:^(void (^next)(void)) {
        app::LoadSubtitleFile(*mv, {ToW(extra)});
        [self waitFor:^bool { return selectedPath() == extra.UTF8String; } seconds:3 then:^{
            check(count() == 5 && selectedPath() == extra.UTF8String, "Load Subtitle File path flow attaches and selects extra.srt");
            const bool superimpose = mv->Svc().settings.showSuperimpose;
            mv->Svc().settings.showSuperimpose = !mv->Svc().settings.showSuperimpose;
            app::OnSettingsChanged(*mv);
            [self waitFor:^bool { return player->State() == wvp::PlayerState::Paused && count() == 4 && selectedPath() == korean; } seconds:12 then:^{
                check(mv->Svc().settings.showSuperimpose != superimpose && count() == 4 && selectedPath() == korean,
                      "superimpose setting reopen drops manually loaded extra.srt; the Korean sidecar stays selected");
                player->SelectStream(wvp::StreamType::Subtitle, -1);
                mv->Svc().settings.showSuperimpose = !mv->Svc().settings.showSuperimpose;
                app::OnSettingsChanged(*mv);
                [self waitFor:^bool { return player->State() == wvp::PlayerState::Paused && count() == 4 && selected() == -1; } seconds:12 then:^{
                    check(selected() == -1 && !mv->SubsButtonLit(), "explicit subtitle Off survives superimpose-setting reopen");
                    [self key:47 chars:@"."];
                    app::OpenPaths(*mv, {ToW(clip)});
                    [self waitFor:^bool { return [self playingUrlEnding:"clip.ts" past:0.5] && selectedPath() == korean; } seconds:12 then:^{
                        check(player->SubtitleDelay() == 0, "a new open resets the engine subtitle delay");
                        check(count() == 4, "a new open does not auto-attach extra.srt");
                        const std::string url = mv->Ctl().Current() ? mv->Ctl().Current()->url : "";
                        drop();
                        [self waitFor:^bool { return selectedPath() == extra.UTF8String; } seconds:3 then:^{
                            check(count() == 5 && selectedPath() == extra.UTF8String && mv->Ctl().Current() && mv->Ctl().Current()->url == url,
                                  "dropping extra.srt through ShellView attaches/selects it without replacing the playing video");
                            next();
                        }];
                    }];
                }];
            }];
        }];
    }];
    [steps addObject:^(void (^next)(void)) {
        const std::string english = [self->_subsDir stringByAppendingPathComponent:@"clip.en.srt"].UTF8String;
        if (auto mi = player->GetMediaInfo()) for (const auto& st : mi->Streams) if (st.path == english) player->SelectStream(wvp::StreamType::Subtitle, st.Index);
        const bool superimpose = mv->Svc().settings.showSuperimpose;
        mv->Svc().settings.showSuperimpose = !mv->Svc().settings.showSuperimpose;
        app::OnSettingsChanged(*mv);
        [self waitFor:^bool { return player->State() == wvp::PlayerState::Playing && selectedPath() == english; } seconds:12 then:^{
            check(mv->Svc().settings.showSuperimpose != superimpose && count() == 4 && selectedPath() == english,
                  "superimpose setting reopen restores the selected English sidecar by path instead of Korean auto-selection");
            mv->Svc().settings.showSuperimpose = !mv->Svc().settings.showSuperimpose;
            app::OnSettingsChanged(*mv);
            [self waitFor:^bool { return player->State() == wvp::PlayerState::Playing && selectedPath() == english; } seconds:12 then:^{
                check(mv->Svc().settings.showSuperimpose == superimpose && selectedPath() == english, "sidecar selection also survives restoring the superimpose setting");
                next();
            }];
        }];
    }];
    [steps addObject:^(void (^next)(void)) {
        if (auto mi = player->GetMediaInfo()) for (const auto& st : mi->Streams) if (st.path == korean) player->SelectStream(wvp::StreamType::Subtitle, st.Index);
        [self seek:4.5 thenPause:^(bool landed) {
            check(landed, "the 4.5 s seek shows its picture before the pause (Settings subtitle style checks)");
            const auto before = player->Snapshot(true);
            const auto plain = player->Snapshot(false);
            check(before && plain && before->Data != plain->Data, "snapshot includes the rendered Korean subtitle, unlike the plain video snapshot");
            [self openSettings];
            pui::Widget* page = self->_settings->Page();
            auto* source = dynamic_cast<pui::ToggleSwitch*>(FindById(page, L"SubtitleUseWindowsToggle"));
            if (source) source->SetOn(false);
            auto* fontButton = FindById(page, L"SubtitleFontButton");
            check(source && fontButton && fontButton->IsEffectivelyVisible(), "app subtitle style source shows the font editor button");
            if (fontButton) fontButton->Invoke();
            auto* dialog = self->_settings->Ui().TopPopup();
            auto* fonts = dynamic_cast<pui::ComboBox*>(FindById(dialog, L"SubtitleFontFamilyCombo"));
            bool macFont = false;
            if (fonts) {
                const int original = fonts->SelectedIndex();
                for (int i = 0; i < fonts->ItemCount(); i++) {
                    fonts->SetSelectedIndex(i);
                    const auto text = fonts->SelectedText();
                    macFont = macFont || text == L"Apple SD Gothic Neo" || text == L"Helvetica";
                }
                fonts->SetSelectedIndex(original);
            }
            check(fonts && fonts->ItemCount() > 20 && macFont, "pui subtitle font editor enumerates the installed Mac families");
            auto* size = dynamic_cast<pui::TextBox*>(FindById(dialog, L"SubtitleFontSizeTextBox"));
            auto* hex = dynamic_cast<pui::TextBox*>(FindById(dialog, L"SubtitleColorHexTextBox"));
            auto* accept = FindById(dialog, L"SubtitlePickerApplyButton");
            const auto unchanged = mv->Svc().settings.subtitle;
            if (size && hex && accept) {
                size->SetValue(L"3");
                check(!accept->Enabled(), "font editor rejects sizes below 4 pt");
                size->SetValue(L"401");
                check(!accept->Enabled(), "font editor rejects sizes above 400 pt");
                size->SetValue(L"36.5");
                hex->SetValue(L"#GG0000");
                check(!accept->Enabled(), "font editor rejects invalid RGB text");
            } else check(false, "font editor exposes size and RGB validation controls");
            if (auto* cancel = FindById(dialog, L"SubtitlePickerCancelButton")) cancel->Invoke();
            check(self->_settings->Ui().PopupCount() == 0 && mv->Svc().settings.subtitle == unchanged,
                  "Cancel closes the font editor without changing subtitle settings");
            if (fontButton) fontButton->Invoke();
            self->_settings->Ui().KeyDown(pui::key::Escape, 0);
            check(self->_settings->Ui().PopupCount() == 0 && mv->Svc().settings.subtitle == unchanged,
                  "Escape cancels the font editor without changes");
            if (fontButton) fontButton->Invoke();
            dialog = self->_settings->Ui().TopPopup();
            auto* yellow = FindById(dialog, L"SubtitleColorPresetFFFF00");
            auto* apply = FindById(dialog, L"SubtitlePickerApplyButton");
            check(yellow && apply, "pui subtitle font editor exposes text colour and Apply");
            if (yellow) yellow->Invoke();
            if (apply) apply->Invoke();
            check(mv->Svc().settings.subtitle.primaryRgba == 0xFFFF00FFu && self->_settings->Ui().PopupCount() == 0,
                  "Apply saves yellow text and closes the pui editor");
            auto* fontOverride = dynamic_cast<pui::ToggleSwitch*>(FindById(page, L"SubtitleOverrideFontToggle"));
            if (fontOverride) fontOverride->SetOn(true);
            auto* colors = dynamic_cast<pui::ToggleSwitch*>(FindById(page, L"SubtitleOverrideColorsToggle"));
            if (colors) colors->SetOn(true);
            [self after:0.5 run:^{
                const auto after = player->Snapshot(true);
                const auto plainAfter = player->Snapshot(false);
                const bool restyled = before && after && before->Data != after->Data;
                const bool sameVideo = plain && plainAfter && plain->Data == plainAfter->Data;
                char what[200];
                snprintf(what, sizeof what, "Settings subtitle style applies live: subtitle snapshot changes while paused video pixels stay identical"
                         " (subtitles changed %d, video identical %d)", restyled, sameVideo);
                check(restyled && sameVideo, what);
                if (source) source->SetOn(true);
                [self after:0.4 run:^{
                    const auto systemBefore = player->Snapshot(true);
                    const int refreshes = mainview::LastCaptionRefresh().count;
                    auto appearance = mainview::ReadCaptionAppearance();
                    appearance.textRgba = 0x00FF00FFu;
                    appearance.fontScale = 1.5;
                    mainview::SetCaptionAppearanceOverride(appearance);
                    mainview::PostCaptionAppearanceChanged();
                    [self after:0.7 run:^{
                        const auto systemAfter = player->Snapshot(true);
                        check(systemBefore && systemAfter && systemBefore->Data != systemAfter->Data &&
                              mainview::LastCaptionRefresh().count == refreshes && player->State() == wvp::PlayerState::Paused,
                              "system text colour/size changes redraw paused subtitles without an ARIB decoder refresh");
                        mainview::SetCaptionAppearanceOverride(std::nullopt);
                        mainview::PostCaptionAppearanceChanged();
                        self->_settings->Close();
                        player->Play();
                        next();
                    }];
                }];
            }];
        }];
    }];
    [steps addObject:^(void (^next)(void)) { (void)next; done(); }];
    [self runSteps:steps];
}

- (void)runSelfTest {
    __block int failures = 0;
    void (^check)(bool, const char*) = ^(bool ok, const char* what) {
        printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        fflush(stdout);
        if (!ok) failures++;
    };
    auto centre = [](pui::Widget* w) { return pui::PointF{w->Bounds().cx(), w->Bounds().cy()}; };
    app::ShellRoot* shell = _mv->shell;
    mainview::MainView* mv = _mv.get();
    mainview::Engine& eng = _mv->Eng();
    wvp::FfPlayer* player = _mv->Player();
    NSMutableArray<SelfTestStep>* steps = [NSMutableArray array];

    [steps addObject:^(void (^next)(void)) {
        NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
        NSDictionary* original = [defaults volatileDomainForName:NSArgumentDomain];
        NSMutableDictionary* japanese = [original mutableCopy];
        japanese[@"AppleLanguages"] = @[@"ja-JP"];
        [defaults setVolatileDomain:japanese forName:NSArgumentDomain];   // process-only; never saves user preferences
        wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
        const int n = GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH);
        const auto preferred = app::open::PreferredSubtitleLanguages();
        const int picked = app::open::PickSidecarTrack({{1, "en"}, {2, "ja"}}, preferred);
        [defaults setVolatileDomain:original forName:NSArgumentDomain];
        check(n == 6 && std::wstring(locale) == L"ja-JP", "locale shim returns preferred ja-JP in BCP-47 format despite bundle localizations");
        check(picked == 2, "Japanese preference picks the Japanese sidecar over English when no Korean track exists");
        next();
    }];
    // the app: data directory, bundle resources
    [steps addObject:^(void (^next)(void)) {
        printf("     data dir %s\n", mv->DataDir().c_str());
        check(mv->Svc().settingsStore.Path() == mv->DataDir() + "/settings.json", "settings.json lives in the data directory (SettingsStore)");
        check(mv->Svc().settings.subtitle.font == "Apple SD Gothic Neo", "fresh data directory defaults to Apple SD Gothic Neo for text subtitles");
        NSString* fw = NSBundle.mainBundle.privateFrameworksPath;
        check([NSFileManager.defaultManager fileExistsAtPath:[fw stringByAppendingPathComponent:@"libffplay.dylib"]],
              "libffplay.dylib is inside the bundle (Contents/Frameworks)");
        check([NSBundle.mainBundle pathForResource:@"THIRD-PARTY-NOTICES" ofType:@"txt"] != nil, "THIRD-PARTY-NOTICES.txt is a bundle resource");
        const std::string userLogs = mainview::logmac::LogsDir(wvp::platform::AppDataDir() + "/settings.json");
        printf("     log folders: default data -> %s, test data -> %s\n", userLogs.c_str(), mainview::logmac::LogsDir(mv->Svc().settingsStore.Path()).c_str());
        check(userLogs == std::string(NSHomeDirectory().UTF8String) + "/Library/Logs/aribplayer" &&
              mainview::logmac::LogsDir(mv->Svc().settingsStore.Path()) == mv->DataDir() + "/logs",
              "Debug logging folder: ~/Library/Logs/aribplayer (the user's data), <data dir>/logs (an injected one)");
        next();
    }];
    // startup: probe off the main thread, GPU choice, the first open waited for the plan and plays
    [steps addObject:^(void (^next)(void)) {
        [self waitFor:^bool {
            return [self playingIndex:0 past:0.5] &&
                   JsonInt(eng.PresenterStats(), "frames_presented") >
                       std::max(0LL, JsonInt(eng.PresenterStatsAtPrepared(), "frames_presented"));
        } seconds:15 then:^{
            check(eng.Probed() && !eng.Candidates().empty() && eng.Choice().index >= 0, "startup probe done, a video GPU chosen (SelectVideoGpu)");
            printf("     video GPU %s (%s), probe %llu ms after start\n", eng.Candidates().empty() ? "-" : eng.Candidates()[(size_t)std::max(0, eng.Choice().index)].name.c_str(),
                   eng.Choice().reason.c_str(), (unsigned long long)eng.StartupProbeMs());
            const wvp::Plan* plan = mv->Svc().filters.CurrentPlan();
            check(plan != nullptr, "the planner has a pipeline");
            if (plan) printf("     plan: %s\n", plan->Summary().c_str());
            check(player->State() == wvp::PlayerState::Playing, "the first clip (1080i MPEG-2) plays after launch");
            check(shell->Title()->Title() == L"tc_1080i_mpeg2", "title bar shows the file name");
            check(shell->Bar()->Model().dur > 9 && shell->Bar()->Model().pos > 0.3, "bar model has duration and a moving position");
            const wvp::OpenOptions& o = eng.LastOpenOptions();
            printf("     open options: %s\n", mainview::Engine::FormatOptions(o).c_str());
            check(OptionValue(o, "hwaccel") != "auto", "no hwaccel=auto: the plan decides the decoder");
            const mainview::AribFont& font = eng.CaptionFont();
            check(font.Ok() && OptionValue(o, "arib_font") == font.option && font.option.rfind(font.family + ",", 0) == 0,
                  "arib_font = the registered caption font's CoreText family (+ libaribcaption's Apple defaults)");
            check(!font.resolved.empty() && font.resolved == font.file, "libaribcaption's CoreText lookup of that family resolves to the bundled TTF");
            check(font.file.find(".app/Contents/Resources/fonts/") != std::string::npos, "the caption font is the bundle's Resources/fonts copy");
            printf("     arib_font=%s -> %s\n", font.option.c_str(), font.resolved.c_str());
            const wvp::DebugInfo d = [self debugInfo];
            printf("     engine: decoder %s, hwaccel %s, vf %s\n", d.Video.Decoder.c_str(), d.Video.HwaccelDevice.c_str(), d.Filters.VfGraph.c_str());
            const auto& chips = shell->Bar()->Model().chips;
            check(chips.size() >= 2 && (chips[0].text == L"H/W" || chips[0].text == L"S/W"), "codec chips (H/W | S/W, codec) from the engine's debug info");
            [self presenterCheck:"tc_1080i_mpeg2" expectZeroCopy:false check:check];
            if (ExpectSoftwareDecode()) {
                printf("skip VideoToolbox decode and zero-copy checks: no hardware decoder (ARIBPLAYER_SELFTEST_EXPECT_SW_DECODE=1); "
                       "the H.264 / HEVC clips are checked on the software decode path\n");
                check(!VTIsHardwareDecodeSupported(kCMVideoCodecType_H264) && !VTIsHardwareDecodeSupported(kCMVideoCodecType_HEVC),
                      "ARIBPLAYER_SELFTEST_EXPECT_SW_DECODE=1 only where VideoToolbox has no hardware H.264 / HEVC decoder");
            }
            next();
        }];
    }];
    // play / pause through the bar button
    [steps addObject:^(void (^next)(void)) {
        [self click:centre(shell->Bar()->ButtonWidget(app::B_Play))];
        [self waitFor:^bool { return player->State() == wvp::PlayerState::Paused && !shell->Bar()->Model().playing; } seconds:2 then:^{   // the bar model follows the engine event on the main queue
            check(player->State() == wvp::PlayerState::Paused && !shell->Bar()->Model().playing, "click on Play/Pause pauses (glyph follows)");
            [self click:centre(shell->Bar()->ButtonWidget(app::B_Play))];
            [self waitFor:^bool { return player->State() == wvp::PlayerState::Playing; } seconds:2 then:^{
                check(player->State() == wvp::PlayerState::Playing, "second click resumes");
                next();
            }];
        }];
    }];
    // seek: click at 80 % of the track
    [steps addObject:^(void (^next)(void)) {
        pui::RectF tr = shell->Bar()->Seek()->Track();
        [self click:{tr.l + tr.w() * 0.8f, tr.cy()}];
        [self waitFor:^bool { return player->Duration() > 0 && player->Position() > player->Duration() * 0.6; } seconds:3 then:^{
            const double pos = player->Position(), dur = player->Duration();
            char what[120];
            snprintf(what, sizeof what, "click on the seek slider seeks (pos %.2f of %.2f s)", pos, dur);
            check(dur > 0 && pos > dur * 0.6, what);
            next();
        }];
    }];
    // the title bar's main-menu (hamburger) button, clicked through -[NSWindow sendEvent:] (AppKit's own hit test,
    //     title-bar region included): features/Menus.cpp's main menu opens under the button
    [steps addObject:^(void (^next)(void)) {
        pui::Widget* b = shell->Title()->MainMenuButton();
        const pui::RectF bb = b->Bounds();
        const NSPoint at = [self->_shell convertPoint:NSMakePoint(bb.cx(), bb.cy()) toView:nil];
        NSView* hitView = [self->_window.contentView.superview hitTest:at];
        check(b->IsEffectivelyVisible() && hitView == self->_shell, "the hamburger button is shown and AppKit routes a click there to the shell view");
        check(mv->host.HitTest({bb.cx(), bb.cy()}) == b && mv->host.HitTest({shell->Title()->PinButton()->Bounds().cx(), shell->Title()->PinButton()->Bounds().cy()}) == shell->Title()->PinButton(),
              "the shell's hit test finds the hamburger and the pin buttons (TitleBar zones in the bar's own coordinates)");
        app::nativemenu::ClearLastShown();
        [self->_window sendEvent:[self windowEvent:NSEventTypeLeftMouseDown at:{bb.cx(), bb.cy()}]];
        [self->_window sendEvent:[self windowEvent:NSEventTypeLeftMouseUp at:{bb.cx(), bb.cy()}]];
        auto* menu = dynamic_cast<puimac::MenuPopup*>(mv->host.TopPopup());
        check(mv->host.PopupCount() == 1 && menu && app::nativemenu::FindItem(app::nativemenu::LastShown(), app::Tr(L"Open URL…", L"주소 열기…")),
              "a click on the hamburger opens the main menu (features/Menus.cpp)");
        check(menu && menu->Bounds().t >= bb.b && std::fabs(menu->Bounds().l - bb.l) < 8, "anchored under the button");
        [self key:53 chars:@"\x1b"];
        check(mv->host.PopupCount() == 0, "Escape closes it");
        next();
    }];
    // seek bar drag through -[NSWindow sendEvent:]: press on the thumb, drag, release; the seek happens on the
    //     release (PlayerBar onCommit) inside the slider, over the video and outside the window; paused, the
    //     presenter draws the seek's picture
    [steps addObject:^(void (^next)(void)) {
        app::ShellSlider* seek = shell->Bar()->Seek();
        const pui::RectF tr = seek->Track();
        auto xAt = [tr](double f) { return (float)(tr.l + tr.w() * f); };
        auto drag = ^(double from, pui::PointF to, bool inside) {
            const pui::PointF thumb{xAt(from), tr.cy()};
            [self->_window sendEvent:[self windowEvent:NSEventTypeLeftMouseDown at:thumb]];
            for (int k = 1; k <= 4; k++)
                [self->_window sendEvent:[self windowEvent:NSEventTypeLeftMouseDragged
                                                       at:{thumb.x + (to.x - thumb.x) * k / 4, inside ? thumb.y : thumb.y + (to.y - thumb.y) * k / 4}]];
            [self->_window sendEvent:[self windowEvent:NSEventTypeLeftMouseUp at:to]];
        };
        const double dur = player->Duration();
        drag(seek->Fraction(), {xAt(0.3), tr.cy()}, true);
        [self waitFor:^bool { return player->Position() > dur * 0.25 && player->Position() < dur * 0.45; } seconds:3 then:^{
            char what[200];
            snprintf(what, sizeof what, "dragging the seek thumb to 30 %% and releasing on the slider seeks there (pos %.2f of %.2f s)", player->Position(), dur);
            check(!seek->Dragging() && player->Position() > dur * 0.25 && player->Position() < dur * 0.45, what);
            // released above the slider, over the video: the fraction follows x
            drag(seek->Fraction(), {xAt(0.7), tr.cy() - 200}, false);
            // (and its picture on screen: the next seek starts from a known serial)
            [self waitFor:^bool { return player->Position() > dur * 0.65 && player->Position() < dur * 0.85 && [self shownSerial] >= 0; } seconds:3 then:^{
                char what2[200];
                snprintf(what2, sizeof what2, "released over the video (off the slider): seeks to 70 %% (pos %.2f s)", player->Position());
                check(!seek->Dragging() && player->Position() > dur * 0.65 && player->Position() < dur * 0.85, what2);
                // released outside the window (below / left of it): clamps to the start; paused once the seek's picture
                //     is on screen, as in seek:thenPause:, so the paused drag below starts from a settled picture
                const NSRect f = self->_shell.bounds;
                const std::int64_t beforeStart = [self shownSerial];
                drag(seek->Fraction(), {(float)-40, (float)NSHeight(f) + 40}, false);
                [self waitFor:^bool { return player->Position() < dur * 0.15 && [self shownSerial] > beforeStart; } seconds:5 then:^{
                    char what3[200];
                    snprintf(what3, sizeof what3, "released outside the window: the drag still ends and seeks (clamped to 0 %%: pos %.2f s)", player->Position());
                    check(!seek->Dragging() && player->Position() < dur * 0.15, what3);
                    player->Pause();
                    [self waitFor:^bool { return player->State() == wvp::PlayerState::Paused; } seconds:5 then:^{
                        [self after:0.5 run:^{
                            const std::string st0 = eng.PresenterStats();
                            const long long pr0 = JsonInt(st0, "frames_presented");
                            const std::int64_t s0 = [self shownSerial];
                            const double p0 = player->Position();
                            // the presenter's newest picture is the engine's picture on screen: its serial, at the position
                            auto heldIsShown = ^bool(const std::string& st) {
                                const wvp::DebugFrameTrace t = [self debugInfo].FrameTrace;
                                return t.Presented.Serial && JsonInt(st, "held_serial") == *t.Presented.Serial &&
                                       std::fabs(JsonNum(st, "held_pts") - player->Position()) < 0.25;
                            };
                            drag(seek->Fraction(), {xAt(0.6), tr.cy()}, true);
                            [self waitFor:^bool {
                                const std::string st = eng.PresenterStats();
                                return player->Position() > dur * 0.5 && [self shownSerial] > s0 && JsonInt(st, "frames_presented") > pr0 && heldIsShown(st) &&
                                       std::fabs(shell->Bar()->Model().pos - player->Position()) < 0.5;
                            } seconds:5 then:^{
                                const std::string st1 = eng.PresenterStats();
                                const long long pr1 = JsonInt(st1, "frames_presented");
                                const std::int64_t s1 = [self shownSerial];
                                const bool held = heldIsShown(st1);
                                char what4[400];
                                snprintf(what4, sizeof what4, "paused: a drag to 60 %% seeks (%.2f -> %.2f s, bar %.2f), stays paused (state %d) and the presenter draws "
                                         "the seek's picture (serial %lld -> %lld, held serial %lld pts %.3f%s; frames_presented %lld -> %lld; skipped by the drawable "
                                         "gate %lld, skipped pictures redrawn %lld)",
                                         p0, player->Position(), shell->Bar()->Model().pos, (int)player->State(), (long long)s0, (long long)s1,
                                         JsonInt(st1, "held_serial"), JsonNum(st1, "held_pts"), held ? "" : ", not the engine's picture", pr0, pr1,
                                         JsonInt(st1, "skipped_timeout") - JsonInt(st0, "skipped_timeout"), JsonInt(st1, "skipped_redraws") - JsonInt(st0, "skipped_redraws"));
                                check(player->State() == wvp::PlayerState::Paused && player->Position() > dur * 0.5 && s0 >= 0 && s1 > s0 && held && pr1 > pr0 &&
                                      std::fabs(shell->Bar()->Model().pos - player->Position()) < 0.5, what4);
                                player->Play();
                                [self waitFor:^bool { return player->State() == wvp::PlayerState::Playing; } seconds:2 then:^{ next(); }];
                            }];
                        }];
                    }];
                }];
            }];
        }];
    }];
    // F6 playlist panel; double click on row 2 plays the HEVC 10-bit clip through VideoToolbox, zero-copy
    [steps addObject:^(void (^next)(void)) {
        // Leave room for growth on small hosted displays; resizeForPanel clamps to the work area.
        NSRect frame = self->_window.frame;
        const CGFloat room = NSWidth(self->_window.screen.visibleFrame) -
                             (mv->Svc().settings.playlistPanelWidthDip + app::kBodyGapDip) - 20;
        if (room >= self->_window.minSize.width && NSWidth(frame) > room) {
            frame.size.width = room;
            [self->_window setFrame:frame display:YES];
        }
        const CGFloat w0 = NSWidth(self->_window.frame);
        [self key:0x61 chars:@""];   // F6
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.3 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            check(mv->PanelOpen() && shell->Panel()->Visible(), "F6 opens the playlist panel");
            check(NSWidth(self->_window.frame) > w0, "window grew for the docked panel");
            check(shell->Panel()->Items().size() == 4 && shell->Panel()->List()->ViewCount() == 4, "panel lists the four test clips");
            pui::RectF row = shell->Panel()->List()->RowRect(1);
            [self->_shell mouseDown:[self mouseEvent:NSEventTypeLeftMouseDown at:{row.cx(), row.cy()} clicks:1]];
            [self->_shell mouseUp:[self mouseEvent:NSEventTypeLeftMouseUp at:{row.cx(), row.cy()} clicks:1]];
            [self->_shell mouseDown:[self mouseEvent:NSEventTypeLeftMouseDown at:{row.cx(), row.cy()} clicks:2]];
            [self->_shell mouseUp:[self mouseEvent:NSEventTypeLeftMouseUp at:{row.cx(), row.cy()} clicks:2]];
            [self waitFor:^bool { return [self playingIndex:1 past:2.0]; } seconds:10 then:^{
                check(mv->Ctl().CurrentIndex() == 1 && shell->Title()->Title() == L"tc_1080p_hevc10", "double click on row 2 plays it (title follows)");
                check(player->State() == wvp::PlayerState::Playing, "the HEVC 10-bit clip plays");
                const wvp::DebugInfo d = [self debugInfo];
                printf("     open options: %s\n     engine: decoder %s, hwaccel %s, fmt %s\n", mainview::Engine::FormatOptions(eng.LastOpenOptions()).c_str(),
                       d.Video.Decoder.c_str(), d.Video.HwaccelDevice.c_str(), d.Video.PixFmt.c_str());
                if (ExpectSoftwareDecode()) {
                    check(OptionValue(eng.LastOpenOptions(), "hwaccel").empty() && SoftwareDecoding(d),
                          "without a hardware decoder the plan decodes HEVC 10-bit in software");
                } else {
                    check(OptionValue(eng.LastOpenOptions(), "hwaccel") == "videotoolbox" && d.Video.HwaccelDevice == "videotoolbox",
                          "the plan's hwaccel=videotoolbox reached the decoder");
                }
                [self presenterCheck:"tc_1080p_hevc10" expectZeroCopy:!ExpectSoftwareDecode() check:check];
                next();
            }];
        });
    }];
    // Tab: the debug overlay (features/DebugOverlayModel + PipelineOverlay) over the HEVC clip: engine
    //     info (decoder, hwaccel, vf graph, fps / drops, presenter frame path) and the Metal presenter's zero-copy rows
    [steps addObject:^(void (^next)(void)) {
        const int draws0 = mv->DebugOverlayWidget()->DrawCount();
        [self key:48 chars:@"\t"];   // Tab
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.6 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            [self->_shell display];
            const std::string text = mv->DebugText();
            const wvp::DebugInfo d = [self debugInfo];
            printf("     tab: %s\n", mv->LastTabDecision().c_str());
            for (const auto& row : mv->DebugRows()) printf("     | %s\n", row.text.c_str());
            check(mv->DebugOverlayVisible() && mv->DebugOverlayDrawn() && mv->DebugOverlayWidget()->DrawCount() > draws0,
                  "Tab shows the debug overlay (DecideTab: toggle) and it is drawn");
            const pui::RectF ob = mv->DebugOverlayWidget()->Bounds();
            check(ob.l >= 0 && ob.t >= app::kStripDip && ob.r <= mv->host.Size().w && ob.b <= mv->host.Size().h && mv->DebugCols() >= 40,
                  "the overlay sits below the title strip inside the window (ComputeOverlayGeometry)");
            const bool sw = ExpectSoftwareDecode();
            const size_t hw = text.find("hwaccel   requested ");
            const std::string hwRow = hw == std::string::npos ? std::string() : text.substr(hw, text.find('\n', hw) - hw);
            check(!d.Video.Decoder.empty() && text.find("decoder " + d.Video.Decoder) != std::string::npos &&
                  (sw ? hwRow.rfind("hwaccel   requested - -> actual ", 0) == 0 && hwRow.find("MISMATCH") == std::string::npos
                      : hwRow.rfind("hwaccel   requested videotoolbox -> actual videotoolbox", 0) == 0),
                  sw ? "overlay: the engine's decoder and hwaccel (requested - -> actual, no mismatch)"
                     : "overlay: the engine's decoder and hwaccel (requested -> actual videotoolbox)");
            check(text.find("vf graph") != std::string::npos && text.find(sw ? "frame_path software" : "frame_path hw_direct") != std::string::npos,
                  sw ? "overlay: the vf graph and the frame path (software)" : "overlay: the vf graph and the frame path (hw_direct)");
            check(text.find("fps") != std::string::npos && text.find("displayed") != std::string::npos && text.find("drop") != std::string::npos,
                  "overlay: fps rows (in -> filter out -> displayed, drops)");
            const size_t m = text.find("metal     presented ");
            long long presented = -1, zc = -1, copies = -1;
            if (m != std::string::npos) {
                sscanf(text.c_str() + m, "metal     presented %lld | zero-copy %lld", &presented, &zc);
                const size_t c = text.find("| copies ", m);
                if (c != std::string::npos) sscanf(text.c_str() + c, "| copies %lld", &copies);
            }
            char what[200];
            snprintf(what, sizeof what, "overlay: Metal presenter stats (presented %lld, zero-copy %lld, copies %lld)", presented, zc, copies);
            check(presented > 0 && (sw || (zc > 0 && copies == 0)), what);
            check(text.find("pipeline") != std::string::npos && text.find("adapter: video GPU") != std::string::npos,
                  "overlay: the planner's pipeline section and the adapter row");
            [self key:48 chars:@"\t"];
            check(!mv->DebugOverlayVisible() && !mv->DebugOverlayWidget()->Visible(), "Tab hides it again");
            next();
        });
    }];
    // the splitter between the video and the docked panel: drag = panel width (live), saved on release; double click = 320
    [steps addObject:^(void (^next)(void)) {
        const app::RectPx sp = mv->Layout().splitterPx;
        const float s = mv->Scale();
        check(!sp.Empty() && !mv->Layout().panelOverlay, "the docked panel has a splitter (ShellLayout::splitterPx)");
        const int w0 = mv->Svc().settings.playlistPanelWidthDip;
        const int videoW0 = mv->Layout().video.w();
        const pui::PointF at{(sp.l + sp.r) / 2.f / s, (sp.t + sp.b) / 2.f / s};
        check(mv->SplitterAt(at) && !mv->SplitterAt({at.x - 20, at.y}), "SplitterAt: the 8 DIP gap only");
        [self->_shell mouseDown:[self mouseEvent:NSEventTypeLeftMouseDown at:at clicks:1]];
        [self->_shell mouseDragged:[self mouseEvent:NSEventTypeLeftMouseDragged at:{at.x - 40, at.y} clicks:1]];
        [self->_shell mouseDragged:[self mouseEvent:NSEventTypeLeftMouseDragged at:{at.x - 80, at.y} clicks:1]];
        const int live = mv->Svc().settings.playlistPanelWidthDip;
        const bool videoShrank = mv->Layout().video.w() < videoW0;
        [self->_shell mouseUp:[self mouseEvent:NSEventTypeLeftMouseUp at:{at.x - 80, at.y} clicks:1]];
        char what[200];
        snprintf(what, sizeof what, "dragging the splitter 80 DIP left widens the panel live (%d -> %d DIP), the video column shrinks", w0, live);
        check(std::abs(live - (w0 + 80)) <= 1 && videoShrank && std::lround(mv->Layout().panelPx.w() / s) == live, what);
        const std::string json = [self settingsFileText];
        snprintf(what, sizeof what, "\"playlistPanelWidthDip\": %d", live);
        std::string compact = what;
        compact.erase(std::remove(compact.begin(), compact.end(), ' '), compact.end());
        check(json.find(what) != std::string::npos || json.find(compact) != std::string::npos, "release saves playlistPanelWidthDip to settings.json");
        const pui::PointF at2{(mv->Layout().splitterPx.l + mv->Layout().splitterPx.r) / 2.f / s, at.y};
        [self->_shell mouseDown:[self mouseEvent:NSEventTypeLeftMouseDown at:at2 clicks:1]];
        [self->_shell mouseUp:[self mouseEvent:NSEventTypeLeftMouseUp at:at2 clicks:1]];
        [self->_shell mouseDown:[self mouseEvent:NSEventTypeLeftMouseDown at:at2 clicks:2]];
        [self->_shell mouseUp:[self mouseEvent:NSEventTypeLeftMouseUp at:at2 clicks:2]];
        check(mv->Svc().settings.playlistPanelWidthDip == (int)app::kPanelDefaultDip, "double click on the splitter resets the width to 320");
        next();
    }];
    // search box filters; F6 closes; right click = main menu (features/Menus.cpp), Escape closes it
    [steps addObject:^(void (^next)(void)) {
        [self click:centre(shell->Panel()->Search())];
        [self->_shell insertText:@"hlg" replacementRange:NSMakeRange(NSNotFound, 0)];
        check(shell->Panel()->List()->ViewCount() == 1, "typing in the search box filters the rows");
        [self->_shell keyDown:[NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:self->_window.windowNumber context:nil characters:@"\x1b" charactersIgnoringModifiers:@"\x1b" isARepeat:NO keyCode:53]];
        [self key:0x61 chars:@""];   // F6 closes
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.3 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            check(!mv->PanelOpen() && !shell->Panel()->Visible(), "F6 closes the panel again");
            pui::PointF v{mv->Layout().video.l / mv->Scale() + 100, mv->Layout().video.t / mv->Scale() + 100};
            [self->_shell rightMouseDown:[self mouseEvent:NSEventTypeRightMouseDown at:v clicks:1]];
            [self->_shell rightMouseUp:[self mouseEvent:NSEventTypeRightMouseUp at:v clicks:1]];
            check(mv->host.PopupCount() == 1, "right click on the video opens the main menu");
            const auto& items = app::nativemenu::LastShown();
            check(app::nativemenu::FindItem(items, app::Tr(L"Open URL…", L"주소 열기…")) != nullptr &&
                  app::nativemenu::FindItem(items, app::Tr(L"Speed", L"재생 속도")) != nullptr, "it is features/Menus.cpp's main menu (Open URL, Speed, ...)");
            [self->_shell keyDown:[NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:self->_window.windowNumber context:nil characters:@"\x1b" charactersIgnoringModifiers:@"\x1b" isARepeat:NO keyCode:53]];
            check(mv->host.PopupCount() == 0, "Escape closes it");
            next();
        });
    }];
    // the main menu's submenus (puimac::MenuPopup) like native menus: with a submenu open the parent menu still takes
    //     the pointer (hover switches the highlight and the submenu, a plain item closes it, a click on a parent item
    //     opens its submenu); keys: Left / Escape close one level, Right opens with the first item, Escape on the root closes
    [steps addObject:^(void (^next)(void)) {
        pui::PointF v{mv->Layout().video.l / mv->Scale() + 60, mv->Layout().video.t / mv->Scale() + 40};
        [self->_shell rightMouseDown:[self mouseEvent:NSEventTypeRightMouseDown at:v clicks:1]];
        [self->_shell rightMouseUp:[self mouseEvent:NSEventTypeRightMouseUp at:v clicks:1]];
        auto* root = dynamic_cast<puimac::MenuPopup*>(mv->host.TopPopup());
        int video = -1, playback = -1, plain = -1;
        if (root)
            for (int i = 0; i < root->ItemCount(); i++) {
                const pui::MenuItemDesc& it = root->Items()[(size_t)i];
                if (it.text == app::Tr(L"Video", L"영상")) video = i;
                if (it.text == app::Tr(L"Playback", L"재생")) playback = i;
                if (it.text == app::Tr(L"Equalizer…", L"이퀄라이저…")) plain = i;
            }
        check(root && video >= 0 && playback >= 0 && plain >= 0, "the main menu has Playback, Video (submenus) and Equalizer");
        if (!root || video < 0 || playback < 0 || plain < 0) { mv->host.CloseAllPopups(); next(); return; }
        auto rowCentre = [](puimac::MenuPopup* m, int i) { const pui::RectF r = m->RowRect(i); return pui::PointF{r.cx(), r.cy()}; };
        [self moveTo:rowCentre(root, video)];
        [self after:0.45 run:^{
            puimac::MenuPopup* sub = root->SubMenu();
            check(mv->host.PopupCount() == 2 && root->SubMenuIndex() == video && sub, "hovering Video opens its submenu (hover delay)");
            if (sub) [self moveTo:rowCentre(sub, 0)];
            const bool subLit = sub && sub->Highlight() == 0 && root->Highlight() == video;
            [self moveTo:rowCentre(root, playback)];
            char what[200];
            snprintf(what, sizeof what, "with the submenu open the pointer back on the main menu highlights Playback (highlight %d, want %d)",
                     root->Highlight(), playback);
            check(subLit && root->Highlight() == playback, what);
            [self after:0.45 run:^{
                check(mv->host.PopupCount() == 2 && root->SubMenuIndex() == playback, "and switches the submenu to Playback's after the delay");
                [self moveTo:rowCentre(root, plain)];
                [self after:0.45 run:^{
                    check(mv->host.PopupCount() == 1 && root->Highlight() == plain && root->SubMenuIndex() < 0,
                          "hovering a plain item (Equalizer) closes the submenu and highlights it");
                    [self moveTo:rowCentre(root, playback)];
                    [self after:0.45 run:^{
                        const pui::PointF at = rowCentre(root, video);
                        [self->_shell mouseDown:[self mouseEvent:NSEventTypeLeftMouseDown at:at clicks:1]];
                        [self->_shell mouseUp:[self mouseEvent:NSEventTypeLeftMouseUp at:at clicks:1]];
                        check(mv->host.PopupCount() == 2 && root->SubMenuIndex() == video,
                              "a click on Video while Playback's submenu is open opens Video's submenu at once");
                        [self key:123 chars:@""];   // Left
                        check(mv->host.PopupCount() == 1 && root->Highlight() == video, "Left closes the submenu level, Video stays highlighted");
                        [self key:124 chars:@""];   // Right
                        puimac::MenuPopup* s2 = root->SubMenu();
                        check(mv->host.PopupCount() == 2 && s2 && s2->Highlight() == 0 && mv->host.Focused() == s2,
                              "Right opens the submenu with its first item highlighted and focused");
                        [self key:125 chars:@""];   // Down: inside the submenu
                        check(s2 && root->SubMenu() == s2 && s2->Highlight() == 1, "Down moves inside the submenu");
                        [self key:53 chars:@"\x1b"];   // Escape
                        check(mv->host.PopupCount() == 1 && mv->host.Focused() == root, "Escape closes only the submenu level");
                        [self key:126 chars:@""];   // Up: the main menu again
                        check(root->Highlight() >= 0 && root->Highlight() < video, "Up then moves in the main menu");
                        [self key:53 chars:@"\x1b"];
                        check(mv->host.PopupCount() == 0, "Escape on the main menu closes it");
                        next();
                    }];
                }];
            }];
        }];
    }];
    // the menu bar and the pui menu agree: no Close Window (⌘W unbound), Mini Player with pip.enter / pip.exit,
    //     an SF Symbol on every item of every menu, full screen on ⌃⌘F, and the pui menu shows the Mac shortcuts of the
    //     menu bar (⌘, Settings, ⌘Q Exit, ⇧⌘O ...); each shown key runs its command (a menu-bar key equivalent or the
    //     key table)
    [steps addObject:^(void (^next)(void)) {
        NSMenuItem* closeWindow = nil;
        NSMenuItem* mini = nil;
        NSMenuItem* fullScreen = nil;
        std::vector<std::string> noImage;
        // AppKit adds Services and window-tiling entries whose artwork is owned by macOS.
        // The opt-in skips only those icon checks, leaving the local test unchanged.
        const bool skipSystemIcons = [NSProcessInfo.processInfo.environment[@"ARIBPLAYER_SELFTEST_SKIP_SYSTEM_MENU_ICONS"] isEqualToString:@"1"];
        check(self->_appMenuItems.count > 0, "app-owned menu items are tracked for icon checks");
        int items = 0, skippedIcons = 0;
        for (NSMenuItem* top in NSApp.mainMenu.itemArray)
            for (NSMenuItem* it in MenuItems(top.submenu)) {
                if (it.isSeparatorItem) continue;
                items++;
                if (it.action == @selector(performClose:) || ([it.keyEquivalent isEqualToString:@"w"] && (it.keyEquivalentModifierMask & NSEventModifierFlagCommand)))
                    closeWindow = it;
                if (it.action == @selector(runCommand:) && it.tag == (NSInteger)app::CommandId::ToggleMiniPlayer) mini = it;
                if (it.action == @selector(toggleFullScreen:)) fullScreen = it;
                if (skipSystemIcons && ![self->_appMenuItems containsObject:it]) skippedIcons++;
                else if (!it.image) noImage.push_back(std::string(top.title.UTF8String) + " > " + it.title.UTF8String);
            }
        if (skippedIcons)
            printf("skip SF Symbol checks for %d macOS-owned menu items (Services/window additions; ARIBPLAYER_SELFTEST_SKIP_SYSTEM_MENU_ICONS=1)\n", skippedIcons);
        check(closeWindow == nil, "the File menu has no Close Window and nothing is bound to ⌘W (Commands.cpp binds no Ctrl+W)");
        std::string missing;
        for (const auto& n : noImage) missing += (missing.empty() ? "" : ", ") + n;
        char what[400];
        snprintf(what, sizeof what, "every checked menu bar item has an SF Symbol image (%d checked, %d system items skipped; without: %s)", items - skippedIcons, skippedIcons, missing.empty() ? "none" : missing.c_str());
        check(noImage.empty(), what);
        if (mini) [self validateMenuItem:mini];
        NSString* enterDesc = mini.image.description;
        check(mini && [enterDesc containsString:@"symbol = pip.enter"] && [mini.keyEquivalent isEqualToString:@"m"] &&
              (mini.keyEquivalentModifierMask & (NSEventModifierFlagCommand | NSEventModifierFlagShift)) == (NSEventModifierFlagCommand | NSEventModifierFlagShift),
              "View > Mini Player has an icon (pip.enter) and ⇧⌘M");
        check(fullScreen && [fullScreen.keyEquivalent isEqualToString:@"f"] &&
              (fullScreen.keyEquivalentModifierMask & NSEventModifierFlagDeviceIndependentFlagsMask) == (NSEventModifierFlagCommand | NSEventModifierFlagControl),
              "View > Enter Full Screen is on ⌃⌘F (macOS shows it as Globe-F on keyboards with a Globe key)");
        // the pui main menu's shortcut labels
        pui::PointF v{mv->Layout().video.l / mv->Scale() + 60, mv->Layout().video.t / mv->Scale() + 40};
        [self->_shell rightMouseDown:[self mouseEvent:NSEventTypeRightMouseDown at:v clicks:1]];
        [self->_shell rightMouseUp:[self mouseEvent:NSEventTypeRightMouseUp at:v clicks:1]];
        auto* root = dynamic_cast<puimac::MenuPopup*>(mv->host.TopPopup());
        std::vector<pui::MenuItemDesc> shown = root ? root->Items() : std::vector<pui::MenuItemDesc>{};
        mv->host.CloseAllPopups();
        auto accel = [&](const wchar_t* en, const wchar_t* ko) {
            const pui::MenuItemDesc* d = app::nativemenu::FindItem(shown, app::Tr(en, ko));
            return d ? d->accelerator : std::wstring(L"(missing)");
        };
        const std::wstring settingsA = accel(L"Settings", L"설정"), exitA = accel(L"Exit", L"종료"), folderA = accel(L"Open folder…", L"폴더 열기…"),
                           propsA = accel(L"Properties…", L"재생 정보…"), closeA = accel(L"Close", L"닫기"), panelA = accel(L"Playlist panel", L"재생목록 창"),
                           ctlA = accel(L"Control panel", L"제어창"), prevA = accel(L"Previous file", L"이전 파일"), miniA = accel(L"Mini player", L"미니 플레이어");
        snprintf(what, sizeof what, "the pui menu shows the Mac shortcuts: Settings %s, Exit %s, Open folder %s, Properties %s, Close %s, Playlist panel %s, Control panel %s, Previous %s, Mini player %s",
                 wvp::ToUtf8(settingsA).c_str(), wvp::ToUtf8(exitA).c_str(), wvp::ToUtf8(folderA).c_str(), wvp::ToUtf8(propsA).c_str(), wvp::ToUtf8(closeA).c_str(),
                 wvp::ToUtf8(panelA).c_str(), wvp::ToUtf8(ctlA).c_str(), wvp::ToUtf8(prevA).c_str(), wvp::ToUtf8(miniA).c_str());
        check(settingsA == L"⌘," && exitA == L"⌘Q" && folderA == L"⇧⌘O" && propsA == L"⌘I" && closeA == L"F4" &&
              panelA == L"⇧⌘L" && ctlA == L"⌥⌘E" && prevA == L"⌘[" && miniA == L"⇧⌘M", what);
        // every shown key works: a menu-bar item with that key equivalent, or the key table (bare keys, ⇧E, ...)
        std::vector<std::string> dead;
        std::function<void(const std::vector<pui::MenuItemDesc>&)> walk = [&](const std::vector<pui::MenuItemDesc>& list) {
            for (const auto& d : list) {
                walk(d.sub);
                if (d.accelerator.empty()) continue;
                std::wstring a = d.accelerator;
                NSEventModifierFlags mods = 0;
                unsigned puiMods = 0;
                for (;;) {
                    if (a.rfind(L"⌃", 0) == 0) { mods |= NSEventModifierFlagControl; a.erase(0, 1); }
                    else if (a.rfind(L"⌥", 0) == 0) { mods |= NSEventModifierFlagOption; puiMods |= pui::ModAlt; a.erase(0, 1); }
                    else if (a.rfind(L"⇧", 0) == 0) { mods |= NSEventModifierFlagShift; puiMods |= pui::ModShift; a.erase(0, 1); }
                    else if (a.rfind(L"⌘", 0) == 0) { mods |= NSEventModifierFlagCommand; puiMods |= pui::ModCtrl; a.erase(0, 1); }
                    else break;
                }
                unichar key = 0;
                int vk = 0;
                if (a == L"←") { key = NSLeftArrowFunctionKey; vk = 0x25; }
                else if (a == L"→") { key = NSRightArrowFunctionKey; vk = 0x27; }
                else if (a == L"Space") { key = ' '; vk = 0x20; }
                else if (a.size() >= 2 && a[0] == L'F') { const int n = std::stoi(a.substr(1)); key = (unichar)(NSF1FunctionKey + n - 1); vk = 0x70 + n - 1; }
                else if (a.size() == 1) { key = (unichar)std::towlower(a[0]); vk = (int)std::towupper(a[0]); }
                bool ok = false;
                NSString* k = key ? [NSString stringWithCharacters:&key length:1] : @"";
                for (NSMenuItem* top in NSApp.mainMenu.itemArray)
                    for (NSMenuItem* it in MenuItems(top.submenu))
                        if ([it.keyEquivalent isEqualToString:k] && (it.keyEquivalentModifierMask & NSEventModifierFlagDeviceIndependentFlagsMask) == mods) ok = true;
                if (!ok && vk && !(mods & NSEventModifierFlagControl)) ok = app::LookupKey(vk, puiMods) != app::CommandId::None;
                if (!ok) dead.push_back(wvp::ToUtf8(d.text + L" " + d.accelerator));
            }
        };
        walk(shown);
        std::string deadList;
        for (const auto& n : dead) deadList += (deadList.empty() ? "" : ", ") + n;
        snprintf(what, sizeof what, "every shortcut the pui menu shows is a menu-bar key equivalent or in the key table (dead: %s)", deadList.empty() ? "none" : deadList.c_str());
        check(dead.empty() && !shown.empty(), what);
        // ⌘I through the menu bar's key equivalents runs Properties (the dialog opens)
        NSEvent* cmdI = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:NSEventModifierFlagCommand timestamp:0
                                     windowNumber:self->_window.windowNumber context:nil characters:@"i" charactersIgnoringModifiers:@"i" isARepeat:NO keyCode:34];
        const BOOL handled = [NSApp.mainMenu performKeyEquivalent:cmdI];
        [self after:0.3 run:^{
            check(handled && mv->host.PopupCount() == 1, "⌘I (shown for Properties) opens the properties dialog through the menu bar");
            mv->host.CloseAllPopups();
            // the mini player item's icon follows the mode
            mv->Execute(app::CommandId::ToggleMiniPlayer);
            [self after:0.5 run:^{
                NSMenuItem* m2 = nil;
                for (NSMenuItem* top in NSApp.mainMenu.itemArray)
                    for (NSMenuItem* it in MenuItems(top.submenu))
                        if (it.action == @selector(runCommand:) && it.tag == (NSInteger)app::CommandId::ToggleMiniPlayer) m2 = it;
                if (m2) [self validateMenuItem:m2];
                check(mv->IsMiniPlayer() && [m2.image.description containsString:@"symbol = pip.exit"] &&
                      [m2.title isEqualToString:ToNS(app::Tr(L"Exit Mini Player", L"미니 플레이어 종료"))],
                      "in the mini player the item reads Exit Mini Player with the pip.exit icon");
                mv->Execute(app::CommandId::ToggleMiniPlayer);
                [self after:0.5 run:^{ next(); }];
            }];
        }];
    }];
    // the 1080i MPEG-2 with Deinterlace = IVTC (the control panel's deinterlace choice): the planner's ivtc
    [steps addObject:^(void (^next)(void)) {
        mv->SetDeinterlace(wvp::DeinterlaceMode::Ivtc);
        mv->Ctl().PlayAt(0);
        [self waitFor:^bool { return [self playingIndex:0 past:3.0]; } seconds:12 then:^{
            const wvp::Plan* plan = mv->Svc().filters.CurrentPlan();
            const std::string summary = plan ? plan->Summary() : std::string("(no plan)");
            const wvp::DebugInfo d = [self debugInfo];
            printf("     plan: %s\n     open options: %s\n     engine: decoder %s, hwaccel %s, vf graph %s\n", summary.c_str(),
                   mainview::Engine::FormatOptions(eng.LastOpenOptions()).c_str(), d.Video.Decoder.c_str(), d.Video.HwaccelDevice.c_str(), d.Filters.VfGraph.c_str());
            check(player->State() == wvp::PlayerState::Playing, "the 1080i MPEG-2 plays with Deinterlace = IVTC");
            const std::string vf = OptionValue(eng.LastOpenOptions(), "vf");
            const bool ivtc = vf.find("ivtc") != std::string::npos || vf.find("fieldmatch") != std::string::npos;
            check(ivtc, "the planner put an inverse telecine in the open's vf");
            bool inGraph = false;
            for (const std::string& f : d.Filters.VfFilters) if (f.find("ivtc") != std::string::npos || f == "fieldmatch") inGraph = true;
            check(inGraph, "the engine's running video graph has it");
            [self presenterCheck:"tc_1080i_mpeg2+ivtc" expectZeroCopy:OptionValue(eng.LastOpenOptions(), "vf").find("videotoolbox") != std::string::npos check:check];
            mv->SetDeinterlace(wvp::DeinterlaceMode::Off);
            next();
        }];
    }];
    // audio: the H.264 / AAC clip through libffplay's SDL audio (CoreAudio); A-V sync; volume / mute keys
    [steps addObject:^(void (^next)(void)) {
        mv->Ctl().PlayAt(3);
        [self waitFor:^bool { return [self playingIndex:3 past:4.0]; } seconds:12 then:^{
            const wvp::DebugInfo d = [self debugInfo];
            printf("     audio: %s %s %d Hz -> device '%s' %d Hz %d ch, master %s, A-V %.1f ms, latency %.1f ms\n", d.Audio.Codec.c_str(), d.Audio.ChannelLayout.c_str(),
                   d.Audio.SampleRate.value_or(0), d.Audio.OutDevice.c_str(), d.Audio.OutSampleRate.value_or(0), d.Audio.OutChannels.value_or(0),
                   d.Sync.Master.c_str(), d.Sync.AvDiffMs.value_or(NAN), d.Audio.LatencyMs.value_or(NAN));
            check(player->State() == wvp::PlayerState::Playing && d.HasAudio && d.Audio.Codec == "aac", "the H.264 / AAC clip plays with audio");
            check(d.Audio.OutSampleRate.value_or(0) > 0, "SDL audio output is open");
            [self presenterCheck:"av_1080p_h264_aac" expectZeroCopy:!ExpectSoftwareDecode() check:check];
            next();
        }];
    }];
    // A-V sync of that clip over 2 s. A-V is an instantaneous clock difference: a video frame shown late (scheduling, a
    // present() that waits for a drawable) reads as a transient lag until the engine catches up. The median is the sync
    // offset; the maximum bounds the transients. The clocks against the sampling time show which side moves.
    [steps addObject:^(void (^next)(void)) {
        auto samples = std::make_shared<std::vector<AvSample>>();
        [self sampleAv:40 interval:0.05 into:samples then:^{
            std::vector<double> absMs;
            std::string av, audio, video;
            const AvSample& s0 = samples->front();
            for (const AvSample& s : *samples) {
                if (!std::isnan(s.avMs)) absMs.push_back(std::fabs(s.avMs));
                char one[24];
                snprintf(one, sizeof one, " %.1f", s.avMs);
                av += one;
                snprintf(one, sizeof one, " %.1f", ((s.audio - s0.audio) - (s.at - s0.at)) * 1000);
                audio += one;
                snprintf(one, sizeof one, " %.1f", ((s.video - s0.video) - (s.at - s0.at)) * 1000);
                video += one;
            }
            std::sort(absMs.begin(), absMs.end());
            const size_t n = absMs.size();
            const double median = n ? (n % 2 ? absMs[n / 2] : (absMs[n / 2 - 1] + absMs[n / 2]) / 2) : NAN;
            const double worst = n ? absMs.back() : NAN;
            printf("     A-V over %.2f s: %zu of %zu samples, |A-V| median %.1f ms, max %.1f ms, late drops %lld\n"
                   "     A-V (ms):%s\n     audio clock - elapsed (ms):%s\n     video clock - elapsed (ms):%s\n",
                   samples->back().at - s0.at, n, samples->size(), median, worst, samples->back().dropsLate - s0.dropsLate,
                   av.c_str(), audio.c_str(), video.c_str());
            check(n == samples->size(), "A-V is reported for every sample while the clip plays with the audio master");
            char what[160];
            snprintf(what, sizeof what, "video follows the audio clock (median |A-V| over 2 s %.1f ms < 100 ms)", median);
            check(n && median < 100, what);
            if ([NSProcessInfo.processInfo.environment[@"ARIBPLAYER_SELFTEST_SKIP_AV_REALTIME"] isEqualToString:@"1"]) {
                printf("skip real-time A-V bounds (median |A-V| < 40 ms, max < 60 ms): Apple Paravirtual GPU presents miss "
                       "deadlines (ARIBPLAYER_SELFTEST_SKIP_AV_REALTIME=1); A-V reporting and the 100 ms median bound remain\n");
            } else {
                snprintf(what, sizeof what, "A-V within ffplay's sync threshold (median |A-V| over 2 s %.1f ms < 40 ms)", median);
                check(n && median < 40, what);
                snprintf(what, sizeof what, "no transient A-V error (max |A-V| over 2 s %.1f ms < 60 ms)", worst);
                check(n && worst < 60, what);
            }
            next();
        }];
    }];
    // volume / mute keys on that clip
    [steps addObject:^(void (^next)(void)) {
        const int v0 = mv->Ctl().Volume();
        [self key:125 chars:@""];   // Down arrow = VolumeDown (Commands key table)
        check(mv->Ctl().Volume() == v0 - 5 && player->Volume() == v0 - 5, "Down arrow lowers the volume by 5 (controller and FfPlayer)");
        [self click:centre(shell->Bar()->ButtonWidget(app::B_Mute))];
        check(mv->Ctl().IsMuted() && player->IsMuted() && shell->Bar()->Model().muted, "the bar's mute button mutes the engine");
        [self key:0x2E chars:@"m"];   // M = ToggleMute
        check(!mv->Ctl().IsMuted() && !player->IsMuted(), "M unmutes");
        [self key:126 chars:@""];   // Up arrow = VolumeUp
        check(player->Volume() == v0, "Up arrow restores the volume");
        next();
    }];
    // keyboard seek through the held-key stepper: Right = +5 s
    [steps addObject:^(void (^next)(void)) {
        const double p0 = player->Position();
        [self key:124 chars:@""];   // Right arrow
        [self waitFor:^bool { return player->Position() > p0 + 3.5; } seconds:3 then:^{
            char what[120];
            snprintf(what, sizeof what, "Right arrow skips +5 s (%.1f -> %.1f)", p0, player->Position());
            check(player->Position() > p0 + 3.5, what);
            next();
        }];
    }];
    // resume: the H.264 clip at ~20 s, another clip, back to it -> the history resumes it (AppPlayback.cpp's rules)
    [steps addObject:^(void (^next)(void)) {
        player->Seek(20.0, false);
        [self waitFor:^bool { return player->Position() > 20.5; } seconds:4 then:^{
            mv->Ctl().PlayAt(2);   // beforeLeavingCurrent saves the position of the H.264 clip
            [self waitFor:^bool { return [self playingIndex:2 past:0.5]; } seconds:8 then:^{
                mv->Ctl().PlayAt(3);
                [self waitFor:^bool { return [self playingIndex:3 past:0.2]; } seconds:8 then:^{
                    char what[160];
                    snprintf(what, sizeof what, "replaying the H.264 clip resumes at the saved position (%.1f s; %s)", player->Position(), mv->ResumeDecision().c_str());
                    check(player->Position() > 19 && mv->ResumeDecision().rfind("resume at 2", 0) == 0, what);
                    next();
                }];
            }];
        }];
    }];
    // the Settings window (pages/SettingsPage): Cmd+, ; theme Dark is saved and applied; Windows-only rows hidden
    [steps addObject:^(void (^next)(void)) {
        NSMenuItem* item = nil;
        for (NSMenuItem* top in NSApp.mainMenu.itemArray)
            for (NSMenuItem* it in MenuItems(top.submenu))
                if ([it.keyEquivalent isEqualToString:@","] && it.keyEquivalentModifierMask == NSEventModifierFlagCommand) item = it;
        check(item != nil, "the app menu has Settings... on Cmd+,");
        if (item) [NSApp sendAction:item.action to:item.target ?: self from:item];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            check(self->_settings && self->_settings->IsOpen() && self->_settings->Page() != nullptr, "Cmd+, opens the Settings window with the settings page");
            pui::Widget* page = self->_settings ? self->_settings->Page() : nullptr;
            pui::Widget* gpu = FindById(page, L"VideoGpuCombo");
            check(gpu && gpu->Parent() && !gpu->Parent()->Visible() && !gpu->IsEffectivelyVisible() && !gpu->CanFocus(),
                  "Settings > Playback has no visible Video GPU row or focusable GPU control");
            pui::Widget* filters = FindById(page, L"EditFiltersButton");
            check(filters && filters->CanFocus(), "Settings > Playback exposes the Filters control");
            auto* fontButton = FindById(page, L"SubtitleFontButton");
            auto* source = dynamic_cast<pui::ToggleSwitch*>(FindById(page, L"SubtitleUseWindowsToggle"));
            check(source && source->IsOn() && fontButton && !fontButton->IsEffectivelyVisible() &&
                  mv->Svc().settings.subtitle.font == WVP_DEFAULT_SUBTITLE_FONT,
                  "Settings uses the system caption style by default and hides the app font controls");
            pui::Widget* styleRow = FindById(page, L"CaptionStyleRow");
            auto* link = dynamic_cast<pui::HyperlinkButton*>(FindById(page, L"CaptionStyleSettingsLink"));
            check(styleRow && styleRow->Visible() && link && styleRow->HelpText().find(L"macOS") != std::wstring::npos &&
                  styleRow->HelpText().find(L"Windows") == std::wstring::npos,
                  "the caption-style row is shown with the macOS wording");
            if (link) {
                self->_settings->suppressLaunch = true;
                link->Invoke();
                const auto& launched = self->_settings->launched;
                check(!launched.empty() && launched.back() == mainview::kCaptionSettingsUrl,
                      "its link opens System Settings > Accessibility > Captions (x-apple.systempreferences)");
                if (!launched.empty()) printf("     caption settings link -> %s\n", launched.back().c_str());
            }
            pui::Widget* dbgRow = FindById(page, L"DebugLoggingRow");
            auto* dbgToggle = dynamic_cast<pui::ToggleSwitch*>(FindById(page, L"DebugLoggingToggle"));
            const std::string logsDir = mainview::logmac::LogsDir(mv->Svc().settingsStore.Path());
            check(dbgRow && dbgRow->Visible() && dbgToggle && dbgRow->HelpText().find(L"/logs/aribplayer-YYYYMMDD.log") != std::wstring::npos,
                  "the Debug logging row is shown with the Mac log folder");
            if (dbgToggle) {   // the About section is below the fold of the scrolled page: switch it the way a click does
                dbgToggle->SetOn(true);
                if (dbgToggle->onToggled) dbgToggle->onToggled(true);
            }
            const std::string logPath = mainview::logmac::CurrentDebugLogPath();
            printf("     debug log: %s\n", logPath.c_str());
            check(mv->Svc().settings.debugLogging && logPath == mainview::logmac::DebugLogPath(logsDir) &&
                  [NSFileManager.defaultManager fileExistsAtPath:[NSString stringWithUTF8String:logPath.c_str()]],
                  "turning Debug logging on opens <logs>/aribplayer-YYYYMMDD.log");
            check(app::log::Global().Active() && app::log::EffectiveLevel() == (app::log::Current().level ? *app::log::Current().level : app::log::kInfo),
                  "with a log sink active libffplay is told info (log::EffectiveLevel), as on Windows");
            if (!app::log::Current().level) {   // the test clips are clean: libffplay says nothing at info; verbose shows its lines
                app::log::Settings ls = app::log::Current();
                ls.level = app::log::kVerbose;
                app::log::Configure(ls);
                app::log::InstallEngineHooks();
            }
            auto* theme = dynamic_cast<pui::RadioGroup*>(FindById(page, L"ThemeGroup"));
            check(theme != nullptr, "the page has the app theme choice");
            if (theme) {
                pui::Widget* dark = theme->Option(2);
                const pui::RectF b = dark->Bounds();
                pui::UiHost& host = self->_settings->Ui();
                pui::PointerEvent pe;
                pe.pt = {b.cx(), b.cy()};
                pe.button = pui::PointerButton::Left;
                host.PointerDown(pe);
                host.PointerUp(pe);
            }
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.3 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                check(mv->Svc().settings.theme == wvp::ThemeSetting::Dark && mv->CurrentThemeMode() == pui::ThemeMode::Dark,
                      "clicking Dark sets the theme (OnSettingsChanged applies it)");
                check([self settingsFileText].find("\"theme\": \"Dark\"") != std::string::npos || [self settingsFileText].find("\"theme\":\"Dark\"") != std::string::npos,
                      "settings.json has the new theme (SettingsStore saved it)");
                NSAppearanceName name = [NSApp.effectiveAppearance bestMatchFromAppearancesWithNames:@[NSAppearanceNameAqua, NSAppearanceNameDarkAqua]];
                check([name isEqualToString:NSAppearanceNameDarkAqua], "the app's appearance is dark");
                if (auto* theme = dynamic_cast<pui::RadioGroup*>(FindById(self->_settings->Page(), L"ThemeGroup"))) theme->SetSelectedIndex(0);
                self->_settings->Close();
                check(!self->_settings->IsOpen(), "the Settings window closes");
                next();
            });
        });
    }];
    // Settings scrolling through AppKit's responder entry point, not UiHost::PointerWheel / SetOffset.
    //      Construct events locally: never post input to the desktop. Catch NSExceptions so a broken adapter fails
    //      the checks rather than abandoning the asynchronous self-test in AppKit's exception handler.
    [steps addObject:^(void (^next)(void)) {
        [self openSettings];
        NSWindow* sw = (__bridge NSWindow*)self->_settings->Window();
        NSView* pv = sw.contentView.subviews.firstObject;
        const NSRect savedFrame = sw.frame;
        [sw setContentSize:NSMakeSize(900, 500)];
        auto* sv = FindScrollViewer(self->_settings->Page());
        pui::Widget* bottom = FindById(self->_settings->Page(), L"DebugLoggingRow");
        check(pv && sv && bottom && sv->MaxOffset() > 100, "Settings scrolling: a scrollable page and the bottom Debug logging row exist");
        if (pv && sv && bottom && sv->MaxOffset() > 100) {
            const float savedOffset = sv->Offset();
            auto visible = [&] {
                const pui::RectF r = bottom->Bounds(), v = sv->Bounds();
                return bottom->IsEffectivelyVisible() && r.t >= v.t && r.b <= v.b;
            };
            auto wheel = [&](CGScrollEventUnit unit, int delta, CGScrollPhase phase = kCGScrollPhaseChanged,
                             CGMomentumScrollPhase momentum = kCGMomentumScrollPhaseNone) {
                CGEventRef cg = CGEventCreateScrollWheelEvent2(nullptr, unit, 1, delta, 0, 0);
                if (!cg) return false;
                const pui::RectF v = sv->Bounds();
                const NSPoint at = [pv convertPoint:NSMakePoint(v.cx(), v.cy()) toView:nil];
                // eventWithCGEvent has no window association. Encode the intended window-local location in its
                // bottom-left coordinates for direct scrollWheel: delivery (Quartz uses top-left coordinates).
                CGEventSetLocation(cg, CGPointMake(at.x, NSMaxY(NSScreen.screens.firstObject.frame) - at.y));
                CGEventSetFlags(cg, 0);
                if (unit == kCGScrollEventUnitPixel) {
                    CGEventSetIntegerValueField(cg, kCGScrollWheelEventScrollPhase, phase);
                    CGEventSetIntegerValueField(cg, kCGScrollWheelEventMomentumPhase, momentum);
                }
                NSEvent* e = [NSEvent eventWithCGEvent:cg];
                CFRelease(cg);
                if (!e || e.type != NSEventTypeScrollWheel || e.hasPreciseScrollingDeltas != (unit == kCGScrollEventUnitPixel) ||
                    std::fabs(e.scrollingDeltaY - delta) > 0.01 ||
                    (momentum != kCGMomentumScrollPhaseNone && e.momentumPhase == NSEventPhaseNone)) return false;
                @try { [pv scrollWheel:e]; }
                @catch (NSException* ex) { printf("     Settings scroll exception: %s: %s\n", ex.name.UTF8String, ex.reason.UTF8String); return false; }
                return true;
            };
            auto reachBottom = [&](CGScrollEventUnit unit, int delta) {
                for (int i = 0; i < 256 && sv->Offset() < sv->MaxOffset(); ++i) {
                    const float before = sv->Offset();
                    if (!wheel(unit, delta) || sv->Offset() <= before) return false;
                }
                [sw displayIfNeeded];
                return visible() && std::fabs(sv->Offset() - sv->MaxOffset()) < 0.01f;
            };
            sv->SetOffset(0);   // reset only; every movement under test goes through scrollWheel:
            check(!visible(), "Settings scrolling: Debug logging starts below the viewport");
            bool mouse = wheel(kCGScrollEventUnitLine, -1);
            char what[240];
            snprintf(what, sizeof what, "Settings mouse wheel: one line scrolls down 48 DIP (offset %.1f)", sv->Offset());
            check(mouse && std::fabs(sv->Offset() - 48.f) < 0.01f, what);
            check(mouse && reachBottom(kCGScrollEventUnitLine, -3), "Settings mouse wheel: the bottom Debug logging row becomes visible");
            sv->SetOffset(0);
            bool trackpad = wheel(kCGScrollEventUnitPixel, -17, kCGScrollPhaseBegan);
            snprintf(what, sizeof what, "Settings trackpad: precise -17 scrolls down 17 DIP (offset %.1f)", sv->Offset());
            check(trackpad && std::fabs(sv->Offset() - 17.f) < 0.01f, what);
            bool reverse = wheel(kCGScrollEventUnitPixel, 5);
            check(reverse && std::fabs(sv->Offset() - 12.f) < 0.01f, "Settings trackpad: reversing direction scrolls up 5 DIP");
            bool momentum = wheel(kCGScrollEventUnitPixel, 0, kCGScrollPhaseEnded) &&
                wheel(kCGScrollEventUnitPixel, -9, (CGScrollPhase)0, kCGMomentumScrollPhaseBegin) &&
                wheel(kCGScrollEventUnitPixel, -4, (CGScrollPhase)0, kCGMomentumScrollPhaseContinue) &&
                wheel(kCGScrollEventUnitPixel, 0, (CGScrollPhase)0, kCGMomentumScrollPhaseEnd);
            check(momentum && std::fabs(sv->Offset() - 25.f) < 0.01f, "Settings trackpad: momentum continues scrolling; zero-delta end does not move");
            check(trackpad && reachBottom(kCGScrollEventUnitPixel, -80), "Settings trackpad: the bottom Debug logging row becomes visible");
            sv->SetOffset(savedOffset);
        }
        [sw setFrame:savedFrame display:YES];
        self->_settings->Close();
        next();
    }];
    // Decoder chips show the planner's actual choice and do not accept focus or invocation.
    [steps addObject:^(void (^next)(void)) {
        const auto& chips = shell->Bar()->Model().chips;
        if (ExpectSoftwareDecode())
            check(SoftwareDecoding([self debugInfo]) && !chips.empty() && chips[0].text == L"S/W",
                  "without a hardware decoder H.264 decodes in software and shows S/W");
        else
            check([self debugInfo].Video.HwaccelDevice == "videotoolbox" && !chips.empty() && chips[0].text == L"H/W",
                  "H.264 uses the automatic VideoToolbox plan and shows H/W");
        for (int i = 0; i < (int)chips.size(); i++) {
            auto* chip = shell->Bar()->ChipWidget(i);
            check(chip && !chip->CanFocus() && !chip->SupportsInvoke() && !chip->HitTestVisible(),
                  "decoder and codec chips are display-only");
        }
        next();
    }];
    // a file dropped on the video plays (DropPathsOnVideoView -> the unnamed playlist)
    [steps addObject:^(void (^next)(void)) {
        NSString* clip = [self->_media stringByAppendingPathComponent:@"tc_1080p_hevc10.mp4"];
        pui::PointF v{mv->Layout().video.l / mv->Scale() + 120, mv->Layout().video.t / mv->Scale() + 80};
        const BOOL accepted = [self dropFiles:@[clip] at:v];
        check(accepted, "a file dragged over the video is accepted (copy)");
        [self waitFor:^bool { return [self playingUrlEnding:"tc_1080p_hevc10.mp4" past:0.5] && mv->Ctl().Items().size() == 1; } seconds:10 then:^{
            check([self playingUrlEnding:"tc_1080p_hevc10.mp4" past:0.5] && mv->Ctl().Items().size() == 1,
                  "the dropped file replaces the unnamed playlist and plays");
            next();
        }];
    }];
    // Finder / Dock open: application:openURLs: of this (the running) instance plays the file
    [steps addObject:^(void (^next)(void)) {
        NSURL* u = [NSURL fileURLWithPath:[self->_media stringByAppendingPathComponent:@"av_1080p_h264_aac.ts"]];
        [self application:NSApp openURLs:@[u]];
        [self waitFor:^bool { return [self playingUrlEnding:"av_1080p_h264_aac.ts" past:0.3]; } seconds:10 then:^{
            check([self playingUrlEnding:"av_1080p_h264_aac.ts" past:0.3] && shell->Title()->Title() == L"av_1080p_h264_aac",
                  "application:openURLs: (Finder Open With / Dock drop) plays the file in the running instance");
            next();
        }];
    }];
    // the log file: features/Logging's line format, libffplay's lines (FfPlayer::SetLogCallback) and the app's events
    [steps addObject:^(void (^next)(void)) {
        const std::string path = mainview::logmac::CurrentDebugLogPath();
        NSString* text = [NSString stringWithContentsOfFile:[NSString stringWithUTF8String:path.c_str()] encoding:NSUTF8StringEncoding error:nil];
        const std::string t = text ? std::string(text.UTF8String) : std::string();
        NSRegularExpression* line = [NSRegularExpression regularExpressionWithPattern:@"^\\d{4}-\\d\\d-\\d\\d \\d\\d:\\d\\d:\\d\\d\\.\\d{3} \\[(INFO |WARN |ERROR|VERB |DEBUG)\\] [a-z]+: "
                                                                             options:NSRegularExpressionAnchorsMatchLines error:nil];
        const NSUInteger formatted = text ? [line numberOfMatchesInString:text options:0 range:NSMakeRange(0, text.length)] : 0;
        size_t lines = 0, av = 0;
        for (char ch : t) if (ch == '\n') lines++;
        for (size_t p = t.find("] av: "); p != std::string::npos; p = t.find("] av: ", p + 1)) av++;
        printf("     %s: %zu lines, %lu in the log format, %zu from libffplay (av)\n", path.c_str(), lines, (unsigned long)formatted, av);
        check(formatted > 0 && formatted + 1 >= lines, "every line has features/Logging's format (YYYY-MM-DD hh:mm:ss.mmm [LEVEL] src: message)");
        check(av > 0, "libffplay's log lines reach the file (FfPlayer::SetLogCallback)");
        check(t.find("] open: url: ") != std::string::npos && t.find("] log: debug logging enabled: ") != std::string::npos,
              "the app's events are in it (open, log)");
        next();
    }];
    // the macOS caption style -> arib_ignore_background / arib_force_outline, live (RefreshCaptionStyle)
    [steps addObject:^(void (^next)(void)) {
        using mainview::CaptionAppearance;
        using mainview::MapCaptionAppearance;
        CaptionAppearance def;   // macOS's default style: text background 0, window 0.5, edge none (video may override)
        check(!MapCaptionAppearance(def).ignoreBackground && !MapCaptionAppearance(def).forceOutline,
              "mapping: the default macOS style keeps ARIB's boxes and outline (= Windows Default)");
        CaptionAppearance clear = def;
        clear.windowOpacity = 0;
        CaptionAppearance clearContent = clear;
        clearContent.backgroundUseValue = false;
        CaptionAppearance uniform = def, shadow = def, raised = def, uniformContent = def;
        uniform.edgeStyle = 4; uniform.edgeUseValue = true;
        shadow.edgeStyle = 5; shadow.edgeUseValue = true;
        raised.edgeStyle = 2; raised.edgeUseValue = true;
        uniformContent.edgeStyle = 4;
        check(MapCaptionAppearance(clear).ignoreBackground && MapCaptionAppearance(clearContent).ignoreBackground,
              "mapping: no text background and no window = ignore background (also when the video may override)");
        check(MapCaptionAppearance(uniform).forceOutline && MapCaptionAppearance(shadow).forceOutline && !MapCaptionAppearance(raised).forceOutline &&
              MapCaptionAppearance(uniformContent).forceOutline,
              "mapping: uniform / drop shadow edges = force outline, also video-overridable ones (macOS Outline Text); raised: no");
        CaptionAppearance outlineText = def;   // the built-in Outline Text profile (MediaAccessibility Defaults.plist)
        outlineText.windowOpacity = 0;
        outlineText.edgeStyle = 4;            // Uniform, "Allow video to override" on
        check(MapCaptionAppearance(outlineText).ignoreBackground && MapCaptionAppearance(outlineText).forceOutline,
              "mapping: macOS's Outline Text profile = ignore background + force outline");
        wvp::SubtitleStyle textSettings;
        auto text = mainview::ResolveMacSubtitleStyle(textSettings, shadow);
        check(text.fallbackFlags == (wvp::kSubFont | wvp::kSubSize | wvp::kSubTextColor | wvp::kSubBackground | wvp::kSubBorder) &&
              text.overrideFlags == 0 && text.shadow == wvp::kSystemCaptionShadowPx,
              "text subtitle system style supplies fallback properties without overriding track styles");
        textSettings.overrideFont = textSettings.overrideColors = textSettings.applyToAss = true;
        text = mainview::ResolveMacSubtitleStyle(textSettings, shadow);
        check(text.overrideFlags == (wvp::kSubFont | wvp::kSubTextColor | wvp::kSubBackground) && text.applyToAss,
              "text subtitle override switches select only properties defined by the system source");
        textSettings.useSystemCaptionStyle = false;
        textSettings.fontWeight = 700;
        textSettings.italic = true;
        textSettings.fontSizePt = 36;
        check(mainview::ResolveMacSubtitleStyle(textSettings, shadow) == wvp::ResolveSubtitleStyle(textSettings, {}),
              "app subtitle source uses Core's font, weight, italic, size and colour resolution");
        const int before = mainview::LastCaptionRefresh().count;
        CaptionAppearance user = clear;
        user.edgeStyle = 4; user.edgeUseValue = true;
        mainview::SetCaptionAppearanceOverride(user);
        mainview::PostCaptionAppearanceChanged();   // kMACaptionAppearanceSettingsChangedNotification -> the watch (250 ms debounce)
        [self waitFor:^bool { return mainview::LastCaptionRefresh().count > before; } seconds:3 then:^{
            const mainview::CaptionRefreshRecord& r = mainview::LastCaptionRefresh();
            const wvp::OpenOptions& base = mv->Ctl().BaseOptions;
            printf("     caption style refresh: ignore_background %d force_outline %d rc %d\n", r.flags.ignoreBackground, r.flags.forceOutline, r.rc);
            check(r.count == before + 1 && r.flags.ignoreBackground && r.flags.forceOutline,
                  "a caption-style change refreshes the playing item (RefreshCaptionStyle(1, 1))");
            check(OptionValue(base, "arib_ignore_background") == "1" && OptionValue(base, "arib_force_outline") == "1",
                  "and the next opens get arib_ignore_background=1 arib_force_outline=1");
            mainview::SetCaptionAppearanceOverride(std::nullopt);
            mainview::PostCaptionAppearanceChanged();
            [self waitFor:^bool { return mainview::LastCaptionRefresh().count > before + 1; } seconds:3 then:^{
                const wvp::OpenOptions& base2 = mv->Ctl().BaseOptions;
                const mainview::CaptionStyleFlags real = MapCaptionAppearance(mainview::ReadCaptionAppearance());
                printf("     this Mac's caption style: %s\n", mainview::DescribeCaptionAppearance(mainview::ReadCaptionAppearance()).c_str());
                check(OptionValue(base2, "arib_ignore_background") == (real.ignoreBackground ? "1" : "0") &&
                      OptionValue(base2, "arib_force_outline") == (real.forceOutline ? "1" : "0"), "back to this Mac's caption settings");
                next();
            }];
        }];
    }];
    [steps addObject:^(void (^next)(void)) {
        wvp::PlayerErrorInfo error;
        error.Message = "Self-test notice";
        app::ShowPlaybackError(*mv, error);
        const auto r = mv->Layout().video;
        const pui::PointF v{(r.l + r.r) / (2 * mv->Scale()), (r.t + r.b) / (2 * mv->Scale())};
        check(mv->host.PopupCount() == 1 && mv->UserPopupOpen(), "PlaybackExtras error counts as a user popup and blocks video drag");
        [self->_shell rightMouseDown:[self mouseEvent:NSEventTypeRightMouseDown at:v clicks:1]];
        [self->_shell rightMouseUp:[self mouseEvent:NSEventTypeRightMouseUp at:v clicks:1]];
        check(!dynamic_cast<puimac::MenuPopup*>(mv->host.TopPopup()) && mv->host.PopupCount() == 1,
              "PlaybackExtras error blocks opening the video context menu");
        const bool debug = mv->DebugOverlayVisible();
        [self key:48 chars:@"\t"];
        check(mv->LastTabDecision().rfind("traversal", 0) == 0 && mv->DebugOverlayVisible() == debug,
              "Tab uses focus traversal, not the debug overlay, while a PlaybackExtras error is open");
        mv->host.CloseAllPopups();
        mv->ShowInfoBar(app::InfoSeverity::Informational, L"Self-test notice", L"App-owned notice");
        check(!mv->UserPopupOpen(), "the app-owned InfoBar is excluded from user popups");
        [self->_shell rightMouseDown:[self mouseEvent:NSEventTypeRightMouseDown at:v clicks:1]];
        [self->_shell rightMouseUp:[self mouseEvent:NSEventTypeRightMouseUp at:v clicks:1]];
        check(dynamic_cast<puimac::MenuPopup*>(mv->host.TopPopup()), "video context menu still opens beside the app-owned notice");
        mv->host.CloseAllPopups();
        const bool panelWasOpen = mv->PanelOpen();
        if (!panelWasOpen) mv->Execute(app::CommandId::TogglePlaylistPanel);
        mv->ShowInfoBar(app::InfoSeverity::Informational, L"Self-test notice", L"Panel keys remain available");
        mv->host.SetFocus(nullptr);
        shell->Panel()->SetActive(true);
        shell->Panel()->List()->SetSelection({});
        mv->Key('A', pui::ModCtrl);
        mv->KeyUp('A');
        check(!shell->Panel()->Items().empty() && shell->Panel()->List()->Selection().size() == shell->Panel()->Items().size(),
              "an app-owned notice does not block the active playlist panel's Select All key");
        mv->host.CloseAllPopups();
        if (!panelWasOpen) mv->Execute(app::CommandId::TogglePlaylistPanel);
        mv->SetFullscreenState(true);
        mv->ShowInfoBar(app::InfoSeverity::Informational, L"Self-test notice", L"Chrome may auto-hide");
        mv->ShowToast(L"Self-test toast");
        mv->PointerAt(pui::PointF{mv->host.Size().w / 2, mv->host.Size().h - 10});
        const bool revealed = shell->Bar()->Visible();
        mv->PointerAt(pui::PointF{mv->host.Size().w / 2, mv->host.Size().h / 2});
        check(revealed && !shell->Bar()->Visible() && !shell->Title()->Visible() && mv->host.PopupCount() == 1,
              "full-screen chrome hides after leaving the edge even with an app notice and toast");
        mv->host.CloseAllPopups();
        mv->SetFullscreenState(false);
        next();
    }];
    [steps addObject:^(void (^next)(void)) { [self runSubtitleTests:check next:next]; }];
    [steps addObject:^(void (^next)(void)) {
        mv->SetDeinterlace(wvp::DeinterlaceMode::On);
        app::OpenPaths(*mv, {ToW([self->_media stringByAppendingPathComponent:@"tc_1080p_hevc10.mp4"])});
        [self waitFor:^bool { return [self playingUrlEnding:"tc_1080p_hevc10.mp4" past:0.5]; } seconds:12 then:^{
            const auto debug = mv->DebugContext();
            const auto filters = [self debugInfo].Filters;
            const auto& vf = filters.VfRequested;
            const bool deintNode = std::any_of(filters.VfFilters.begin(), filters.VfFilters.end(), [](const auto& f) {
                return f.find("bwdif") != std::string::npos || f.find("yadif") != std::string::npos;
            });
            printf("     progressive: requested='%s' engine vf='%s' deint node=%d\n", debug.deinterlaceRequested.c_str(), vf.c_str(), deintNode);
            check(debug.deinterlaceRequested.empty() && vf.find("bwdif") == std::string::npos && vf.find("yadif") == std::string::npos && !deintNode,
                  "Deinterlace On + progressive HEVC: no deinterlace requested in Tab overlay or engine filter graph");
            app::OpenPaths(*mv, {ToW([self->_media stringByAppendingPathComponent:@"tc_1080i_mpeg2.ts"])});
            [self waitFor:^bool { return [self playingUrlEnding:"tc_1080i_mpeg2.ts" past:0.5]; } seconds:12 then:^{
                const auto debug = mv->DebugContext();
                const auto filters = [self debugInfo].Filters;
                const auto& vf = filters.VfRequested;
                const bool deintNode = std::any_of(filters.VfFilters.begin(), filters.VfFilters.end(), [](const auto& f) {
                    return f.find("bwdif") != std::string::npos || f.find("yadif") != std::string::npos;
                });
                printf("     interlaced: requested='%s' engine vf='%s' deint node=%d\n", debug.deinterlaceRequested.c_str(), vf.c_str(), deintNode);
                check(debug.deinterlaceRequested == "deinterlace" && deintNode,
                      "Deinterlace On + interlaced MPEG-2 still requests and runs deinterlacing");
                mv->SetDeinterlace(wvp::DeinterlaceMode::Off);
                next();
            }];
        }];
    }];
    // ARIB captions on / off (--caption-clip): the bar's subtitle button (ToggleSubtitles -> showCaptions ->
    //      FfPlayer::SetSubtitleVisible(Caption)), the main menu's Subtitles entries, the native Playback menu's check;
    //      visibility is measured at the Metal presenter (overlay rects_drawn stops / resumes) and settings.json
    [steps addObject:^(void (^next)(void)) {
        if (!self->_captionClip) { printf("skip ARIB caption on / off (no --caption-clip)\n"); next(); return; }
        [self application:NSApp openURLs:@[[NSURL fileURLWithPath:self->_captionClip]]];
        auto drawn = ^long long { return OverlayStat(eng.PresenterStats(), "rects_drawn"); };
        auto captionStream = ^bool {
            auto mi = player->GetMediaInfo();
            if (!mi) return false;
            for (const auto& st : mi->Streams) if (st.Type == wvp::StreamType::Subtitle && st.AribType == "caption") return true;
            return false;
        };
        __block long long d0 = 0;
        [self waitFor:^bool { return [self playingUrlEnding:".ts" past:3.0] && mv->Ctl().Current() && mv->Ctl().Current()->url == self->_captionClip.UTF8String && drawn() > 0; } seconds:12 then:^{
            check(mv->Ctl().Current() && mv->Ctl().Current()->url == self->_captionClip.UTF8String && captionStream(),
                  "the caption clip plays; its MediaInfo has an ARIB caption stream (arib_type caption)");
            d0 = drawn();
            [self after:1.0 run:^{
                const long long d1 = drawn();
                char what[200];
                snprintf(what, sizeof what, "captions shown: the presenter draws the caption layer (rects_drawn %lld -> %lld in 1 s)", d0, d1);
                check(mv->Svc().settings.showCaptions && d1 > d0, what);
                check(mv->SubsButtonLit(), "the bar's subtitle button is lit while captions are on");
                [self click:centre(shell->Bar()->ButtonWidget(app::B_Subs))];
                check(!mv->SubsButtonLit(), "and not lit once they are off");
                const bool off = !mv->Svc().settings.showCaptions;
                check(off && [self settingsFileText].find("\"showCaptions\": false") != std::string::npos,
                      "the bar's subtitle button turns captions off (showCaptions false, saved)");
                [self after:0.4 run:^{
                    const long long h0 = drawn();
                    [self after:1.0 run:^{
                        const long long h1 = drawn();
                        char what2[200];
                        snprintf(what2, sizeof what2, "captions hidden: no caption quads drawn while off (rects_drawn %lld -> %lld in 1 s)", h0, h1);
                        check(h1 == h0 && player->State() == wvp::PlayerState::Playing, what2);
                        NSMenuItem* item = nil;
                        for (NSMenuItem* top in NSApp.mainMenu.itemArray)
                            for (NSMenuItem* it in MenuItems(top.submenu))
                                if (it.action == @selector(runCommand:) && it.tag == (NSInteger)app::CommandId::ToggleSubtitles) item = it;
                        if (item) [self validateMenuItem:item];
                        check(item && item.state == NSControlStateValueOff, "the menu bar's Captions On / Off item is unchecked");
                        const long long s0 = drawn();   // sample before toggling: real broadcasts have gaps between cues
                        [self click:centre(shell->Bar()->ButtonWidget(app::B_Subs))];
                        [self waitFor:^bool { return mv->Svc().settings.showCaptions && drawn() > s0; } seconds:5 then:^{
                            char what3[200];
                            snprintf(what3, sizeof what3, "the button again shows them (showCaptions true, rects_drawn %lld -> %lld)", s0, drawn());
                            check(mv->Svc().settings.showCaptions && drawn() > s0, what3);
                            check(mv->SubsButtonLit(), "the subtitle button is lit again");
                            if (item) [self validateMenuItem:item];
                            check(item && item.state == NSControlStateValueOn, "and the menu bar's item is checked");
                            [self captionsPausedAndMenu:check drawn:drawn next:next];
                        }];
                    }];
                }];
            }];
        }];
    }];
    // parity with the Windows app: keep awake, Space / wheel, chips, the playlist's key target, typed menu keys and Cmd+C,
    //     panel resizing, toasts, the full-screen cursor, the mini bar's auto-hide and aspect, Settings keys and the
    //     always-on-top band
    [steps addObject:^(void (^next)(void)) { [self runParityChecks:check next:next]; }];
    // menus in their own windows: the mini player and a small main window do not clip them
    [steps addObject:^(void (^next)(void)) { [self runMenuWindowTests:check next:next]; }];
    // popups close on deactivation, miniaturise and close (App WM_ACTIVATE / SIZE_MINIMIZED)
    [steps addObject:^(void (^next)(void)) { [self runPopupDismissTests:check next:next]; }];
    // mini player and back
    [steps addObject:^(void (^next)(void)) {
        const NSRect before = self->_window.frame;
        mv->Execute(app::CommandId::ToggleMiniPlayer);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            const NSRect f = self->_window.frame;
            char what[160];
            snprintf(what, sizeof what, "the mini player: a small floating window (%.0fx%.0f pt), the MiniBar shell", NSWidth(f), NSHeight(f));
            check(mv->IsMiniPlayer() && NSWidth(f) <= 420 && self->_window.level == NSFloatingWindowLevel && !shell->Bar()->Visible(), what);
            mv->Execute(app::CommandId::ToggleMiniPlayer);
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                check(!mv->IsMiniPlayer() && NSEqualRects(self->_window.frame, before) && shell->Bar()->Visible(),
                      "leaving it restores the window and the bar");
                next();
            });
        });
    }];
    // full screen (AppKit's) with the edge-revealed bar, and back (Escape). AppKit does not run the full-screen
    //     transition while the session is locked; then the shell's full-screen mode is checked without it.
    [steps addObject:^(void (^next)(void)) {
        mv->Execute(app::CommandId::ToggleFullscreen);
        [self waitFor:^bool { return mv->IsFullscreen(); } seconds:5 then:^{
            const bool appKit = mv->IsFullscreen() && (self->_window.styleMask & NSWindowStyleMaskFullScreen);
            const bool locked = [AppDelegate screenLocked];
            if (appKit) check(true, "Enter / Ctrl+Cmd+F enters AppKit full screen");
            else if (locked) { printf("skip AppKit full screen (the session is locked: no transition); the shell's full-screen mode instead\n"); mv->SetFullscreenState(true); }
            else check(false, "Enter / Ctrl+Cmd+F enters AppKit full screen");
            mv->PointerAt(pui::PointF{mv->host.Size().w / 2, mv->host.Size().h / 2});
            const bool hiddenMid = !shell->Bar()->Visible() && !shell->Title()->Visible();
            mv->PointerAt(pui::PointF{mv->host.Size().w / 2, mv->host.Size().h - 10});
            check(mv->IsFullscreen() && hiddenMid && shell->Bar()->Visible(), "full screen: bar and title hidden over the video, the bar revealed at the bottom edge");
            printf("     edge playlist panel: %s\n", appKit ? "in AppKit full screen" : "in the shell's full-screen mode (no AppKit transition)");
            auto restored = ^(const char* how) {
                char what[200];
                snprintf(what, sizeof what, "%s: no overlay panel, the docked panel follows the setting again (%s), the toggle is enabled",
                         how, mv->PanelOpen() ? "on" : "off");
                check(!mv->PanelPopup() && shell->Panel()->Visible() == (mv->PanelOpen() && !mv->Layout().panelOverlay) &&
                      shell->Bar()->ButtonWidget(app::B_Panel)->Enabled() && shell->Bar()->Model().panelOpen == shell->Panel()->Visible(), what);
            };
            [self edgePanelTests:check next:^{
                if (!appKit) { mv->SetFullscreenState(false); restored("leaving the full-screen mode"); next(); return; }
                [self key:53 chars:@"\x1b"];   // Escape
                [self waitFor:^bool { return !mv->IsFullscreen() && !(self->_window.styleMask & NSWindowStyleMaskFullScreen); } seconds:5 then:^{
                    check(!mv->IsFullscreen() && !(self->_window.styleMask & NSWindowStyleMaskFullScreen), "Escape leaves full screen");
                    restored("Escape left full screen");
                    next();
                }];
            }];
        }];
    }];
    // leaving full screen with the edge panel open restores the docked panel (the shell's full-screen mode: the same
    //     MainView path AppKit's windowDidEnterFullScreen / windowDidExitFullScreen take, without the transition)
    [steps addObject:^(void (^next)(void)) {
        const bool wanted0 = mv->PanelOpen();
        if (!wanted0) mv->Execute(app::CommandId::TogglePlaylistPanel);
        [self after:0.3 run:^{
            const bool docked = mv->PanelOpen() && shell->Panel()->Visible() && !mv->Layout().panelOverlay;
            const int width0 = mv->Svc().settings.playlistPanelWidthDip;
            mv->SetFullscreenState(true);
            const pui::SizeF S = mv->host.Size();
            const bool dockedHidden = !shell->Panel()->Visible();
            [self moveTo:{S.w - 2, S.h / 2}];
            app::PlaylistPanel* p = mv->PanelPopup();
            check(docked && dockedHidden && p && std::fabs(p->Bounds().w() - std::min((float)width0, S.w - 16)) < 0.5f,
                  "panel docked, then full screen: the docked panel hides, the right edge opens the overlay one at the stored width");
            mv->SetFullscreenState(false);
            check(!mv->PanelPopup() && !mv->host.IsPopupOpen(p) && mv->PanelOpen() && shell->Panel()->Visible() && !mv->Layout().panelOverlay &&
                  shell->Bar()->ButtonWidget(app::B_Panel)->Enabled() && shell->Bar()->Model().panelOpen &&
                  mv->Svc().settings.playlistPanelWidthDip == width0,
                  "leaving full screen with the edge panel open: it closes, the docked panel is back (setting and width unchanged), the toggle is pressed");
            [self moveTo:{mv->host.Size().w / 2, mv->host.Size().h / 2}];
            if (!wanted0) mv->Execute(app::CommandId::TogglePlaylistPanel);
            [self after:0.3 run:^{ next(); }];
        }];
    }];
    // a window too narrow to dock the panel (row < 488 DIP, ShellLayout::panelOverlay): the toggle opens it as a
    //     light-dismiss popup over the video's right side (docs/main-window-ui-notes.md: layout table, "narrow window: popup mode")
    [steps addObject:^(void (^next)(void)) {
        const NSRect frame0 = self->_window.frame;
        const bool wanted0 = mv->PanelOpen();
        mv->Svc().settings.playlistPanel = true;
        mv->Svc().settingsStore.Save(mv->Svc().settings);
        NSRect f = frame0;
        f.size.width = self->_window.minSize.width;
        [self->_window setFrame:f display:YES];
        [self->_shell relayout];
        pui::Widget* toggle = shell->Bar()->ButtonWidget(app::B_Panel);
        check(mv->Layout().panelOverlay && !shell->Panel()->Visible() && !mv->PanelPopup() && toggle->Enabled() && toggle->Visible() &&
              !shell->Bar()->Model().panelOpen,
              "a 500 pt window cannot dock the wanted panel: it is hidden and the enabled toggle is not pressed");
        [self click:centre(toggle)];
        app::PlaylistPanel* p = mv->PanelPopup();
        const app::RectPx& v = mv->Layout().video;
        const float s = mv->Scale(), w = std::min((float)mv->Svc().settings.playlistPanelWidthDip, mv->host.Size().w - 16);
        const pui::RectF b = p ? p->Bounds() : pui::RectF{};
        char what[240];
        snprintf(what, sizeof what, "the toggle opens it over the video's right side at the stored width (%.0f..%.0f x %.0f..%.0f, video %.0f..%.0f x %.0f..%.0f)",
                 b.l, b.r, b.t, b.b, v.l / s, v.r / s, v.t / s, v.b / s);
        check(p && std::fabs(b.r - v.r / s) < 0.5f && std::fabs(b.t - v.t / s) < 0.5f && std::fabs(b.b - v.b / s) < 0.5f &&
              std::fabs(b.w() - w) < 0.5f, what);
        const int alpha = p ? [self topLeftAlpha:p] : -1;
        snprintf(what, sizeof what, "with rounded corners (top-left pixel alpha %d)", alpha);
        check(alpha >= 0 && alpha < 40, what);
        check(p && p->Active() && shell->Bar()->Model().panelOpen && mv->PanelOpen(), "the toggle is pressed while it is open; it is the key target");
        [self click:{v.l / s + 40, (v.t + v.b) / 2.f / s}];   // on the video
        check(!mv->PanelPopup() && mv->PanelOpen() && !shell->Bar()->Model().panelOpen, "a click outside closes it (light dismiss) and keeps the setting");
        const CGFloat narrowW = NSWidth(self->_window.frame);
        [self key:0x61 chars:@""];   // F6
        p = mv->PanelPopup();
        check(p && mv->PanelOpen() && p->Bounds() == b && NSWidth(self->_window.frame) == narrowW,
              "F6 opens the same popup (the window keeps its size)");
        [self click:{v.l / s + 40, (v.t + v.b) / 2.f / s}];
        auto cmdKey = [self](NSString* ch, unsigned short code, NSEventType type) {
            return [NSEvent keyEventWithType:type location:NSZeroPoint modifierFlags:NSEventModifierFlagCommand timestamp:0
                                windowNumber:self->_window.windowNumber context:nil characters:ch charactersIgnoringModifiers:ch isARepeat:NO keyCode:code];
        };
        [self->_shell keyDown:cmdKey(@"s", 1, NSEventTypeKeyDown)];   // ⌘S: the key table's Ctrl+S (FocusPlaylistSearch)
        [self->_shell keyUp:cmdKey(@"s", 1, NSEventTypeKeyUp)];
        p = mv->PanelPopup();
        check(p && mv->host.Focused() == p->Search() && pui::TextBox::IsTextInputFocused(mv->host),
              "⌘S (focus playlist search) opens it with the search box focused");
        [self key:53 chars:@"\x1b"];   // Esc on the empty box: the keyboard goes back to the player, the popup stays
        check(mv->PanelPopup() == p && !pui::TextBox::IsTextInputFocused(mv->host), "Esc on the empty box releases it; the popup stays");
        [self click:{v.l / s + 40, (v.t + v.b) / 2.f / s}];
        const BOOL cmdF = [NSApp.mainMenu performKeyEquivalent:cmdKey(@"f", 3, NSEventTypeKeyDown)];   // the menu bar's ⌘F
        p = mv->PanelPopup();
        check(cmdF && p && mv->host.Focused() == p->Search(), "⌘F through the menu bar opens it with the search box focused");
        [self key:53 chars:@"\x1b"];
        [self key:0x61 chars:@""];      // F6
        check(!mv->PanelPopup() && !mv->PanelOpen() && !shell->Panel()->Visible() && NSWidth(self->_window.frame) == narrowW,
              "F6 closes it and clears the setting (the window keeps its size)");
        mv->Svc().settings.playlistPanel = wanted0;
        mv->Svc().settingsStore.Save(mv->Svc().settings);
        [self->_window setFrame:frame0 display:YES];
        [self->_shell relayout];
        check(NSEqualRects(self->_window.frame, frame0) && mv->PanelOpen() == wanted0 && !mv->PanelPopup(), "window and panel setting restored");
        next();
    }];
    [steps addObject:^(void (^next)(void)) {
        (void)next;
        printf("selftest: %d failure(s)\n", failures);
        fflush(stdout);
        exit(failures == 0 ? 0 : 2);
    }];
    [self runSteps:steps];
}

// --- self-test: parity with the Windows app (keep awake, cursor, Settings window, panel resizing, mini player, input) ----

typedef void (^ParityCheck)(bool, const char*);

/// Synthesised key events for a view (never posted to the desktop).
- (NSEvent*)keyEvent:(NSEventType)type code:(unsigned short)code chars:(NSString*)chars mods:(NSEventModifierFlags)mods
              repeat:(BOOL)repeat window:(NSWindow*)w {
    return [NSEvent keyEventWithType:type location:NSZeroPoint modifierFlags:mods timestamp:NSProcessInfo.processInfo.systemUptime
                        windowNumber:w.windowNumber context:nil characters:chars charactersIgnoringModifiers:chars isARepeat:repeat keyCode:code];
}

/// A scroll event at a shell-view point, delivered to the shell view's scrollWheel: (as the Settings scrolling checks).
- (BOOL)scrollShellAt:(pui::PointF)p unit:(CGScrollEventUnit)unit delta:(int)delta momentum:(CGMomentumScrollPhase)momentum {
    CGEventRef cg = CGEventCreateScrollWheelEvent2(nullptr, unit, 1, delta, 0, 0);
    if (!cg) return NO;
    const NSPoint at = [_shell convertPoint:NSMakePoint(p.x, p.y) toView:nil];
    CGEventSetLocation(cg, CGPointMake(at.x, NSMaxY(NSScreen.screens.firstObject.frame) - at.y));
    CGEventSetFlags(cg, 0);
    if (unit == kCGScrollEventUnitPixel) {
        CGEventSetIntegerValueField(cg, kCGScrollWheelEventScrollPhase, momentum == kCGMomentumScrollPhaseNone ? kCGScrollPhaseChanged : 0);
        CGEventSetIntegerValueField(cg, kCGScrollWheelEventMomentumPhase, momentum);
    }
    NSEvent* e = [NSEvent eventWithCGEvent:cg];
    CFRelease(cg);
    if (!e || e.type != NSEventTypeScrollWheel) return NO;
    @try { [_shell scrollWheel:e]; }
    @catch (NSException* ex) { printf("     scroll exception: %s\n", ex.reason.UTF8String); return NO; }
    return YES;
}

/// Front-to-back position of a window among the on-screen windows (the window server's order), else NSApp's order.
- (NSInteger)zIndexOf:(NSWindow*)w {
    NSArray* list = CFBridgingRelease(CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID));
    NSInteger i = 0;
    for (NSDictionary* d in list) {
        if ([d[(__bridge NSString*)kCGWindowNumber] integerValue] == w.windowNumber) return i;
        i++;
    }
    return 100000 + [NSApp.orderedWindows indexOfObject:w];
}

/// `a` is in front of `b` in the window server's order. AppKit sends ordering changes to the server as its run loop turns:
/// up to 0.5 s of it.
- (bool)window:(NSWindow*)a inFrontOf:(NSWindow*)b {
    for (int i = 0; i < 10; i++) {
        if ([self zIndexOf:a] < [self zIndexOf:b]) return true;
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    }
    return false;
}

/// Keys through AppKit's own dispatch when the player is the key window of the active app: posted to this app's event
/// queue (never the desktop's), so -[NSApplication sendEvent:] runs with them as the current event (menu key
/// equivalents, the key window). Without a key window (a locked session) the order the CI runs measured there: a key
/// with Command / Control through the menu bar's key equivalents, any other to the first responder's keyDown:.
/// `done(real)` runs once they were handled.
- (void)routeKeys:(NSArray<NSEvent*>*)events then:(void (^)(bool real))done {
    if (_window.isKeyWindow && NSApp.isActive) {
        for (NSEvent* e in events) [NSApp postEvent:e atStart:NO];
        [self after:0.4 run:^{ done(true); }];
        return;
    }
    NSResponder* fr = _window.firstResponder ?: _shell;
    for (NSEvent* e in events) {
        if (e.type == NSEventTypeKeyUp) { [fr keyUp:e]; continue; }
        const bool command = (e.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) != 0;
        if (!command || ![NSApp.mainMenu performKeyEquivalent:e]) [fr keyDown:e];
    }
    done(false);
}

/// The 30 s H.264 clip from its start: checks that need playback to keep going for several seconds (the other clips
/// are 1-12 s, and an ended clip changes the state they depend on).
- (void)playLongClip:(void (^)(void))then {
    const auto& items = _mv->Ctl().Items();
    for (size_t i = 0; i < items.size(); i++)
        if (items[i].url.size() >= 20 && items[i].url.compare(items[i].url.size() - 20, 20, "av_1080p_h264_aac.ts") == 0) {
            _mv->Ctl().PlayAt((int)i, 0);
            break;
        }
    [self waitFor:^bool { return [self playingUrlEnding:"av_1080p_h264_aac.ts" past:0.3]; } seconds:10 then:then];
}

- (int)ownAssertions:(CFStringRef)type {
    CFDictionaryRef byPid = nullptr;
    if (IOPMCopyAssertionsByProcess(&byPid) != kIOReturnSuccess || !byPid) return -1;
    NSDictionary* all = CFBridgingRelease(byPid);
    int n = 0;
    for (NSDictionary* a in (NSArray*)all[@(getpid())])
        if ([a[(__bridge NSString*)kIOPMAssertionTypeKey] isEqualToString:(__bridge NSString*)type]) n++;
    return n;
}

- (void)runParityChecks:(ParityCheck)check next:(void (^)(void))done {
    NSMutableArray<SelfTestStep>* steps = [NSMutableArray array];
    [steps addObject:^(void (^next)(void)) { [self parityKeepAwake:check next:next]; }];
    [steps addObject:^(void (^next)(void)) { [self paritySpaceAndWheel:check next:next]; }];
    [steps addObject:^(void (^next)(void)) { [self parityChips:check next:next]; }];
    [steps addObject:^(void (^next)(void)) { [self parityPanelTarget:check next:next]; }];
    [steps addObject:^(void (^next)(void)) { [self parityTypingAndCopy:check next:next]; }];
    [steps addObject:^(void (^next)(void)) { [self parityPanelResize:check next:next]; }];
    [steps addObject:^(void (^next)(void)) { [self parityToast:check next:next]; }];
    [steps addObject:^(void (^next)(void)) { [self parityCursor:check next:next]; }];
    [steps addObject:^(void (^next)(void)) { [self parityMiniPlayer:check next:next]; }];
    [steps addObject:^(void (^next)(void)) { [self paritySettingsKeys:check next:next]; }];
    [steps addObject:^(void (^next)(void)) { [self parityTopmost:check next:next]; }];
    [steps addObject:^(void (^next)(void)) { (void)next; done(); }];
    [self runSteps:steps];
}

/// #1 App::UpdateKeepAwake: a display assertion while video plays; none paused, stopped or closed.
- (void)parityKeepAwake:(ParityCheck)check next:(void (^)(void))next {
    mainview::MainView* mv = _mv.get();
    wvp::FfPlayer* player = mv->Player();
    [self queueClips];
    [self waitFor:^bool { return [self playingIndex:0 past:0.5]; } seconds:10 then:^{
        using K = mainview::MainView::KeepAwake;
        const CFStringRef display = kIOPMAssertionTypePreventUserIdleDisplaySleep;
        check(mv->KeepAwakeState() == K::Display && [self ownAssertions:display] == 1,
              "keep awake: playing video holds one PreventUserIdleDisplaySleep assertion (pmset -g assertions lists it)");
        player->Pause();
        [self waitFor:^bool { return player->State() == wvp::PlayerState::Paused && mv->KeepAwakeState() == K::None; } seconds:3 then:^{
            check(mv->KeepAwakeState() == K::None && [self ownAssertions:display] == 0, "keep awake: paused releases it");
            player->Play();
            [self waitFor:^bool { return player->State() == wvp::PlayerState::Playing && mv->KeepAwakeState() == K::Display; } seconds:3 then:^{
                check(mv->KeepAwakeState() == K::Display && [self ownAssertions:display] == 1, "keep awake: playing again takes it again");
                mv->Execute(app::CommandId::Stop);
                [self waitFor:^bool { return mv->Ctl().Current() == nullptr && mv->KeepAwakeState() == K::None; } seconds:3 then:^{
                    check(mv->KeepAwakeState() == K::None && [self ownAssertions:display] == 0 &&
                          [self ownAssertions:kIOPMAssertionTypePreventUserIdleSystemSleep] == 0, "keep awake: closing the media releases it");
                    mv->Ctl().PlayAt(0);
                    [self waitFor:^bool { return [self playingIndex:0 past:0.5]; } seconds:10 then:^{ next(); }];
                }];
            }];
        }];
    }];
}

/// #14 Space: a held key's auto-repeat does not toggle again. #15 the wheel: 1 % per notch over the video, the title strip
/// and the bar's background; trackpad travel adds up (48 DIP a notch); momentum does not change the volume.
- (void)paritySpaceAndWheel:(ParityCheck)check next:(void (^)(void))next {
    [self playLongClip:^{ [self paritySpaceAndWheelPlaying:check next:next]; }];
}

- (void)paritySpaceAndWheelPlaying:(ParityCheck)check next:(void (^)(void))next {
    mainview::MainView* mv = _mv.get();
    wvp::FfPlayer* player = mv->Player();
    app::ShellRoot* shell = mv->shell;
    [_shell keyDown:[self keyEvent:NSEventTypeKeyDown code:49 chars:@" " mods:0 repeat:NO window:_window]];
    [self waitFor:^bool { return player->State() == wvp::PlayerState::Paused; } seconds:3 then:^{
        const bool paused = player->State() == wvp::PlayerState::Paused;
        for (int k = 0; k < 3; k++) [self->_shell keyDown:[self keyEvent:NSEventTypeKeyDown code:49 chars:@" " mods:0 repeat:YES window:self->_window]];
        [self after:0.5 run:^{
            check(paused && player->State() == wvp::PlayerState::Paused, "Space pauses; three auto-repeats of the held key change nothing");
            [self->_shell keyUp:[self keyEvent:NSEventTypeKeyUp code:49 chars:@" " mods:0 repeat:NO window:self->_window]];
            [self->_shell keyDown:[self keyEvent:NSEventTypeKeyDown code:49 chars:@" " mods:0 repeat:NO window:self->_window]];
            [self->_shell keyUp:[self keyEvent:NSEventTypeKeyUp code:49 chars:@" " mods:0 repeat:NO window:self->_window]];
            [self waitFor:^bool { return player->State() == wvp::PlayerState::Playing; } seconds:3 then:^{
                check(player->State() == wvp::PlayerState::Playing, "the next Space press plays again");
                mv->Ctl().SetVolume(50);
                const app::RectPx& v = mv->Layout().video;
                const float s = mv->Scale();
                const pui::PointF video{(v.l + v.r) / (2 * s), (v.t + v.b) / (2 * s)};
                // an empty spot of the title strip and of the bar: no widget under it
                std::optional<pui::PointF> strip, bar;
                const pui::RectF t = mv->Layout().title, b = shell->Bar()->Bounds();
                for (float x = t.r - 200; x > t.l + 90 && !strip; x -= 7) {
                    pui::Widget* h = mv->host.HitTest({x, t.cy()});
                    if (h == nullptr || h == shell || h == shell->Title()) strip = pui::PointF{x, t.cy()};
                }
                for (float x = b.l + 2; x < b.r && !bar; x += 5) {
                    pui::Widget* h = mv->host.HitTest({x, b.b - 3});
                    if (h == nullptr || h == shell || h == shell->Bar()) bar = pui::PointF{x, b.b - 3};
                }
                check(strip && bar, "the title strip and the bar have an empty spot");
                int vol[6];
                [self scrollShellAt:video unit:kCGScrollEventUnitLine delta:1 momentum:kCGMomentumScrollPhaseNone];
                vol[0] = mv->Ctl().Volume();
                if (strip) [self scrollShellAt:*strip unit:kCGScrollEventUnitLine delta:1 momentum:kCGMomentumScrollPhaseNone];
                vol[1] = mv->Ctl().Volume();
                if (bar) [self scrollShellAt:*bar unit:kCGScrollEventUnitLine delta:1 momentum:kCGMomentumScrollPhaseNone];
                vol[2] = mv->Ctl().Volume();
                [self scrollShellAt:video unit:kCGScrollEventUnitLine delta:-1 momentum:kCGMomentumScrollPhaseNone];
                vol[3] = mv->Ctl().Volume();
                [self scrollShellAt:video unit:kCGScrollEventUnitPixel delta:24 momentum:kCGMomentumScrollPhaseNone];
                const int half = mv->Ctl().Volume();
                [self scrollShellAt:video unit:kCGScrollEventUnitPixel delta:24 momentum:kCGMomentumScrollPhaseNone];
                vol[4] = mv->Ctl().Volume();
                [self scrollShellAt:video unit:kCGScrollEventUnitPixel delta:200 momentum:kCGMomentumScrollPhaseContinue];
                vol[5] = mv->Ctl().Volume();
                char what[240];
                snprintf(what, sizeof what, "wheel = volume, 1 %% a notch: video %d, title strip %d, bar background %d, back %d (from 50)", vol[0], vol[1], vol[2], vol[3]);
                check(vol[0] == 51 && vol[1] == 52 && vol[2] == 53 && vol[3] == 52, what);
                snprintf(what, sizeof what, "trackpad: 24 + 24 DIP of travel = one notch (%d -> %d -> %d); momentum leaves it (%d)", vol[3], half, vol[4], vol[5]);
                check(half == 52 && vol[4] == 53 && vol[5] == 53, what);
                mv->Ctl().SetVolume(100);
                next();
            }];
        }];
    }];
}

/// #17 App::ChipsTick: the chips refresh every 2 s while playing, not while paused.
- (void)parityChips:(ParityCheck)check next:(void (^)(void))next {
    // past PREPARED's +0.5 s / +1.5 s refreshes
    [self playLongClip:^{ [self after:1.6 run:^{ [self parityChipsPlaying:check next:next]; }]; }];
}

- (void)parityChipsPlaying:(ParityCheck)check next:(void (^)(void))next {
    mainview::MainView* mv = _mv.get();
    wvp::FfPlayer* player = mv->Player();
    const int c0 = mv->ChipRefreshes();
    [self after:4.3 run:^{
        const int c1 = mv->ChipRefreshes();
        player->Pause();
        [self after:0.3 run:^{
            const int c2 = mv->ChipRefreshes();
            [self after:4.3 run:^{
                const int c3 = mv->ChipRefreshes();
                char what[200];
                snprintf(what, sizeof what, "codec chips refresh every 2 s while playing (%d in 4.3 s) and not while paused (%d)", c1 - c0, c3 - c2);
                check(c1 - c0 >= 2 && c1 - c0 <= 3 && c3 == c2, what);
                player->Play();
                [self waitFor:^bool { return player->State() == wvp::PlayerState::Playing; } seconds:3 then:^{ next(); }];
            }];
        }];
    }];
}

/// #18 a right press on a playlist row makes the panel the keyboard target: Enter then plays that row.
- (void)parityPanelTarget:(ParityCheck)check next:(void (^)(void))next {
    mainview::MainView* mv = _mv.get();
    app::ShellRoot* shell = mv->shell;
    const NSRect frame = _window.frame;
    if (!mv->PanelOpen()) [self key:0x61 chars:@""];   // F6
    [self after:0.3 run:^{
        app::PlaylistPanel* panel = shell->Panel();
        panel->SetActive(false);
        const pui::RectF row = panel->List()->RowRect(2);
        [self->_shell rightMouseDown:[self mouseEvent:NSEventTypeRightMouseDown at:{row.cx(), row.cy()} clicks:1]];
        [self->_shell rightMouseUp:[self mouseEvent:NSEventTypeRightMouseUp at:{row.cx(), row.cy()} clicks:1]];
        const bool menu = mv->host.PopupCount() == 1;
        check(panel->Visible() && panel->Active() && menu, "a right press on a playlist row opens its menu and makes the panel the key target");
        [self key:53 chars:@"\x1b"];
        check(mv->host.PopupCount() == 0 && panel->Active(), "Escape closes the row menu; the panel stays the key target");
        [self key:36 chars:@"\r"];
        [self waitFor:^bool { return [self playingIndex:2 past:0.3]; } seconds:10 then:^{
            check(mv->Ctl().CurrentIndex() == 2, "Enter then plays the right-pressed row (not full screen)");
            check(!mv->IsFullscreen(), "and does not toggle full screen");
            [self->_window setFrame:frame display:YES];
            next();
        }];
    }];
}

/// Menu-bar keys without a modifier (W , . /) type into the focused playlist search box; Cmd+C copies the frame.
- (void)parityTypingAndCopy:(ParityCheck)check next:(void (^)(void))next {
    mainview::MainView* mv = _mv.get();
    app::ShellRoot* shell = mv->shell;
    app::PlaylistPanel* panel = shell->Panel();
    [self click:{panel->Search()->Bounds().cx(), panel->Search()->Bounds().cy()}];
    const long long delay0 = (long long)mv->Player()->SubtitleDelay();
    mv->ShowToast(L"self-test sentinel");
    NSMutableArray<NSEvent*>* keys = [NSMutableArray array];
    struct K { unsigned short code; NSString* chars; };
    for (K k : {K{13, @"w"}, K{43, @","}, K{47, @"."}, K{44, @"/"}}) {
        [keys addObject:[self keyEvent:NSEventTypeKeyDown code:k.code chars:k.chars mods:0 repeat:NO window:_window]];
        [keys addObject:[self keyEvent:NSEventTypeKeyUp code:k.code chars:k.chars mods:0 repeat:NO window:_window]];
    }
    [self routeKeys:keys then:^(bool real) {
        printf("     key routing: %s\n", real ? "AppKit's own (posted to the app's queue; the player is the key window of the active app)"
                                              : "emulated (no key window in this session): bare keys to the first responder");
        char what[260];
        snprintf(what, sizeof what, "W , . / typed into the focused search box (\"%s\"), no subtitle command ran", wvp::ToUtf8(panel->Search()->Text()).c_str());
        check(panel->Search()->Text() == L"w,./" && mv->LastToast() == L"self-test sentinel" && (long long)mv->Player()->SubtitleDelay() == delay0, what);
        [self key:53 chars:@"\x1b"];
        if (!panel->Search()->Text().empty()) [self key:53 chars:@"\x1b"];
        mv->host.SetFocus(nullptr);
        // Cmd+C without text focus: the frame with captions to the clipboard (a recorder instead of the pasteboard)
        auto saved = mv->hooks.copyFrame;
        auto copied = std::make_shared<int>(0);
        mv->hooks.copyFrame = [copied](const wvp::BgraFrame& f) { if (f.Width > 0 && f.Height > 0) (*copied)++; };
        auto finish = ^(bool realCopy) {
            check(*copied == 1 && mv->LastToast() == app::Tr(L"Copied", L"복사됨"),
                  realCopy ? "Cmd+C copies the frame (CaptureFrameWithCaptions), through AppKit's menu and responder chain"
                           : "Cmd+C copies the frame (CaptureFrameWithCaptions): the Edit menu's Copy at the player's first responder");
            mv->hooks.copyFrame = saved;
            [self key:0x61 chars:@""];   // F6 closes the panel
            [self after:0.3 run:^{ next(); }];
        };
        if (self->_window.isKeyWindow && NSApp.isActive) {
            [self routeKeys:@[[self keyEvent:NSEventTypeKeyDown code:8 chars:@"c" mods:NSEventModifierFlagCommand repeat:NO window:self->_window],
                              [self keyEvent:NSEventTypeKeyUp code:8 chars:@"c" mods:NSEventModifierFlagCommand repeat:NO window:self->_window]]
                       then:^(bool) { finish(true); }];
            return;
        }
        // no key window: AppKit has no target for the menu's copy: here, so the item is sent where it would go
        NSMenuItem* copyItem = nil;
        for (NSMenuItem* top in NSApp.mainMenu.itemArray)
            for (NSMenuItem* it in MenuItems(top.submenu))
                if (it.action == @selector(copy:) && [it.keyEquivalent isEqualToString:@"c"]) copyItem = it;
        if (copyItem) [NSApp sendAction:copyItem.action to:self->_window.firstResponder from:copyItem];
        finish(false);
    }];
}

/// #8 hiding the docked panel never shrinks the window below its minimum width; #11/#12 a zoomed window keeps its frame
/// for F6 and for the open-time 100 % sizing (the video resizes instead).
- (void)parityPanelResize:(ParityCheck)check next:(void (^)(void))next {
    [self playLongClip:^{ [self parityPanelResizePlaying:check next:next]; }];   // Next below must not race a clip's end
}

- (void)parityPanelResizePlaying:(ParityCheck)check next:(void (^)(void))next {
    mainview::MainView* mv = _mv.get();
    const NSRect saved = _window.frame;
    if (mv->PanelOpen()) [self key:0x61 chars:@""];
    NSRect f = _window.frame;
    f.size.width = 560;
    [_window setFrame:f display:YES];
    [self key:0x61 chars:@""];   // F6: the panel docks, the window grows
    [self after:0.3 run:^{
        NSRect g = self->_window.frame;
        g.size.width = 560;   // narrowed by hand with the panel still docked
        [self->_window setFrame:g display:YES];
        const bool docked = mv->PanelOpen() && mv->shell->Panel()->Visible() && !mv->Layout().panelOverlay;
        [self key:0x61 chars:@""];   // F6: hide it
        [self after:0.3 run:^{
            char what[200];
            snprintf(what, sizeof what, "hiding the docked panel in a 560 pt window leaves %.0f pt (minimum %.0f)", NSWidth(self->_window.frame), self->_window.minSize.width);
            check(docked && !mv->PanelOpen() && NSWidth(self->_window.frame) == self->_window.minSize.width, what);
            // zoom from a frame well inside the screen (zoom: on a frame that already fills it would restore instead)
            const NSRect vis = self->_window.screen.visibleFrame;
            [self->_window setFrame:NSMakeRect(NSMinX(vis) + 40, NSMinY(vis) + 40, 800, 520) display:YES];
            [self->_window zoom:nil];
            [self waitFor:^bool { return self->_window.zoomed; } seconds:3 then:^{
                if (!self->_window.zoomed) { check(false, "the window zooms (performZoom)"); next(); return; }
                const NSRect z = self->_window.frame;
                const int videoW = mv->Layout().video.w();
                [self key:0x61 chars:@""];
                [self after:0.3 run:^{
                    check(mv->PanelOpen() && NSEqualRects(self->_window.frame, z) && self->_window.zoomed && mv->Layout().video.w() < videoW,
                          "zoomed: F6 docks the panel in the same frame, the video narrows");
                    [self key:0x61 chars:@""];
                    [self after:0.3 run:^{
                        check(!mv->PanelOpen() && NSEqualRects(self->_window.frame, z) && mv->Layout().video.w() == videoW,
                              "zoomed: F6 again hides it, frame unchanged, the video widens back");
                        const int idx = mv->Ctl().CurrentIndex();
                        mv->Execute(app::CommandId::Next);
                        [self waitFor:^bool { return [self playingIndex:(idx + 1) % 4 past:0.8]; } seconds:10 then:^{
                            check(self->_window.zoomed && NSEqualRects(self->_window.frame, z),
                                  "zoomed: an opened video does not size the window to 100 % (it stays zoomed)");
                            [self->_window zoom:nil];
                            [self waitFor:^bool { return !self->_window.zoomed; } seconds:3 then:^{
                                [self->_window setFrame:saved display:YES];
                                next();
                            }];
                        }];
                    }];
                }];
            }];
        }];
    }];
}

/// #21 App::ShowToast: 40 high, the text + 40 wide, centred, 16 DIP above the bar's rect, in full screen too.
- (void)parityToast:(ParityCheck)check next:(void (^)(void))next {
    mainview::MainView* mv = _mv.get();
    auto placed = [mv]() {
        const pui::RectF b = mv->ToastBounds(), bar = mv->Layout().bar;
        return std::fabs(b.h() - 40) < 0.01f && std::fabs(b.b - (bar.t - 16)) < 0.6f && std::fabs(b.cx() - mv->host.Size().w / 2) < 1.f;
    };
    mv->ShowToast(L"Always on top: Off");
    check(placed(), "toast: 40 DIP high, centred, 16 DIP above the docked bar");
    const bool wasFs = mv->IsFullscreen();
    if (!wasFs) mv->SetFullscreenState(true);
    mv->ShowToast(L"Always on top: Off");
    char what[200];
    snprintf(what, sizeof what, "full screen: the toast sits 16 DIP above the bar zone (bottom %.0f, bar top %.0f), shown or not", mv->ToastBounds().b, mv->Layout().bar.t);
    check(mv->IsFullscreen() && placed(), what);
    if (!wasFs) mv->SetFullscreenState(false);
    next();
}

/// #3 full screen: the cursor hides after 2 s without a move over the video, comes back on a move, stays over the bar.
- (void)parityCursor:(ParityCheck)check next:(void (^)(void))next {
    mainview::MainView* mv = _mv.get();
    auto saved = mv->hooks.cursorAt;
    auto at = std::make_shared<std::optional<pui::PointF>>();
    mv->hooks.cursorAt = [at] { return *at; };   // the pointer over the player (the real one is not the test's)
    mv->SetFullscreenState(true);
    const pui::SizeF sz = mv->host.Size();
    *at = pui::PointF{sz.w / 2, sz.h / 2};
    mv->PointerAt(*at);
    [self after:1.5 run:^{
        const bool early = mv->CursorHidden();
        [self after:0.9 run:^{
            check(!early && mv->CursorHidden(), "full screen: the cursor is still shown after 1.5 s, hidden after 2 s without a move");
            *at = pui::PointF{sz.w / 2 + 10, sz.h / 2};
            mv->PointerAt(*at);
            check(!mv->CursorHidden(), "a pointer move shows it at once");
            [self after:2.4 run:^{
                const bool hiddenAgain = mv->CursorHidden();
                *at = pui::PointF{sz.w / 2 + 20, sz.h / 2};   // a move AppKit did not report to the shell (a drag): the tick sees it
                [self after:0.3 run:^{
                    check(hiddenAgain && !mv->CursorHidden(), "hidden again after 2 s; a polled position change shows it");
                    *at = pui::PointF{sz.w / 2, sz.h - 10};
                    mv->PointerAt(*at);   // the bottom edge: the bar shows
                    [self after:2.4 run:^{
                        check(mv->shell->Bar()->Visible() && !mv->CursorHidden(), "over the revealed bar it stays shown");
                        *at = pui::PointF{sz.w / 2, sz.h / 2};
                        mv->PointerAt(*at);
                        [self after:2.4 run:^{
                            const bool hidden = mv->CursorHidden();
                            mv->SetFullscreenState(false);
                            check(hidden && !mv->CursorHidden(), "leaving full screen shows it");
                            [self after:2.3 run:^{
                                check(!mv->CursorHidden(), "in a window it never hides");
                                mv->hooks.cursorAt = saved;
                                next();
                            }];
                        }];
                    }];
                }];
            }];
        }];
    }];
}

/// #9 the mini bar auto-hides after 3 s idle (500 ms slide + fade), a pointer move / the wheel bring it back, keys do not;
/// #10 hover arrives while the window is not key; #13 the mini player's resize aspect is the media's.
- (void)parityMiniPlayer:(ParityCheck)check next:(void (^)(void))next {
    [self playLongClip:^{ [self parityMiniPlayerPlaying:check next:next]; }];
}

- (void)parityMiniPlayerPlaying:(ParityCheck)check next:(void (^)(void))next {
    mainview::MainView* mv = _mv.get();
    app::ShellRoot* shell = mv->shell;
    const wvp::WindowPlacement savedMini = mv->Svc().settings.miniWindow;
    // a square saved mini rect: the aspect must come from the media (16:9 clips), not from the rect
    NSRect vis = _window.screen.visibleFrame;
    const CGFloat top = NSMaxY(NSScreen.screens.firstObject.frame);
    const int l = (int)NSMinX(vis) + 40, t = (int)(top - NSMaxY(vis)) + 40;
    mv->Svc().settings.miniWindow = app::MiniMakePlacement({l, t, l + 360, t + 360}, 1.f);
    mv->Execute(app::CommandId::ToggleMiniPlayer);
    [self after:0.5 run:^{
        const NSSize ar = self->_window.contentAspectRatio;
        char what[220];
        snprintf(what, sizeof what, "mini player: resizing keeps the media's aspect %.3f (contentAspectRatio %.3f) in a %.0fx%.0f window",
                 mv->MediaAspect(), ar.height > 0 ? ar.width / ar.height : 0, NSWidth(self->_window.frame), NSHeight(self->_window.frame));
        check(mv->IsMiniPlayer() && std::fabs(mv->MediaAspect() - 16.0 / 9.0) < 0.01 && ar.height > 0 && std::fabs(ar.width / ar.height - mv->MediaAspect()) < 0.01, what);
        NSTrackingAreaOptions opts = 0;
        for (NSTrackingArea* a in self->_shell.trackingAreas) opts |= a.options;
        check((opts & NSTrackingActiveAlways) && !(opts & NSTrackingActiveInKeyWindow), "the player's hover tracking is active whether or not it is key (NSTrackingActiveAlways)");
        const pui::SizeF sz = mv->host.Size();
        const bool key = self->_window.isKeyWindow;
        [self moveTo:{sz.w / 2, sz.h / 2}];
        [self after:0.4 run:^{
            check(shell->Mini()->Visible() && mv->MiniBarHidden() == 0.f,
                  key ? "a pointer move shows the mini bar" : "a pointer move shows the mini bar while the window is not key");
            const CFAbsoluteTime moved = CFAbsoluteTimeGetCurrent() - 0.4;
            [self waitFor:^bool { return mv->MiniBarHidden() > 0.f; } seconds:4 then:^{
                const double idle = CFAbsoluteTimeGetCurrent() - moved;
                [self after:0.25 run:^{
                    const float p = mv->MiniBarHidden();
                    const float slid = shell->Mini()->Bounds().t - mv->Layout().bar.t;
                    char what2[220];
                    snprintf(what2, sizeof what2, "playing, no move: the hide starts after %.2f s idle; 250 ms in it is %.0f %% hidden, %.1f DIP down",
                             idle, p * 100, slid);
                    check(idle >= 2.9 && idle < 3.5 && p > 0.05f && p < 0.95f && slid > 1 && slid < app::kBarSlideDip && mv->MiniBarAnimating(), what2);
                    // the progress reaches 1 at the end time; the bar is taken away by the next animation frame
                    [self waitFor:^bool { return mv->MiniBarHidden() == 1.f && !shell->Mini()->Visible(); } seconds:1 then:^{
                        const double took = CFAbsoluteTimeGetCurrent() - moved - idle;
                        char what3[200];
                        snprintf(what3, sizeof what3, "it is gone %.2f s after it started (500 ms ease-in) and takes no clicks", took);
                        pui::Widget* play = shell->Mini()->PlayButton();
                        const pui::PointF pc{play->Bounds().cx(), play->Bounds().cy() - app::kBarSlideDip};
                        check(mv->MiniBarHidden() == 1.f && !shell->Mini()->Visible() && took < 0.9 &&
                              mv->host.HitTest(pc) != play, what3);
                        [self key:46 chars:@"m"];   // M: mute, twice
                        [self key:46 chars:@"m"];
                        [self after:0.4 run:^{
                            check(mv->MiniBarHidden() == 1.f, "keys do not bring it back");
                            [self scrollShellAt:{sz.w / 2, sz.h / 2} unit:kCGScrollEventUnitLine delta:1 momentum:kCGMomentumScrollPhaseNone];
                            [self scrollShellAt:{sz.w / 2, sz.h / 2} unit:kCGScrollEventUnitLine delta:-1 momentum:kCGMomentumScrollPhaseNone];
                            [self after:0.4 run:^{
                                check(mv->MiniBarHidden() == 0.f && shell->Mini()->Visible(), "the wheel brings it back (300 ms)");
                                mv->Execute(app::CommandId::ToggleMiniPlayer);
                                mv->Svc().settings.miniWindow = savedMini;
                                [self after:0.5 run:^{ next(); }];
                            }];
                        }];
                    }];
                }];
            }];
        }];
    }];
}

/// #5 Esc in the Settings window closes an open flyout first, then the window; #16 Edit filters closes Settings and opens
/// the filters panel in the player.
- (void)paritySettingsKeys:(ParityCheck)check next:(void (^)(void))next {
    mainview::MainView* mv = _mv.get();
    [self openSettings];
    NSWindow* sw = (__bridge NSWindow*)_settings->Window();
    NSView* pv = sw.contentView.subviews.firstObject;
    auto* licenses = dynamic_cast<pui::Button*>(FindById(_settings->Page(), L"OpenSourceLicensesButton"));
    if (licenses && licenses->onClick) licenses->onClick();
    const bool dialog = _settings->Ui().PopupCount() == 1;
    [pv keyDown:[self keyEvent:NSEventTypeKeyDown code:53 chars:@"\x1b" mods:0 repeat:NO window:sw]];
    check(dialog && _settings->Ui().PopupCount() == 0 && _settings->IsOpen(), "Settings: Esc closes the open licenses dialog first, the window stays");
    [pv keyDown:[self keyEvent:NSEventTypeKeyDown code:53 chars:@"\x1b" mods:0 repeat:NO window:sw]];
    check(!_settings->IsOpen(), "Settings: Esc then closes the window");
    [self openSettings];
    auto* edit = dynamic_cast<pui::Button*>(FindById(_settings->Page(), L"EditFiltersButton"));
    if (edit && edit->onClick) edit->onClick();
    [self waitFor:^bool { return !self->_settings->IsOpen() && app::IsFiltersPanelOpen(*mv); } seconds:2 then:^{
        check(edit && !self->_settings->IsOpen() && app::IsFiltersPanelOpen(*mv), "Settings > Edit filters closes Settings and opens the filters panel in the player");
        mv->Execute(app::CommandId::Escape);
        next();
    }];
}

/// The always-on-top band (App::ApplyWindowTopmost, SettingsWindow's z-order, MiniPlayer, owned dialogs): every mode, a
/// press on the player, a focus cycle, a state change while Settings is open, the mini player, a minimized player, an alert.
- (void)parityTopmost:(ParityCheck)check next:(void (^)(void))next {
    [self playLongClip:^{ [self parityTopmostPlaying:check next:next]; }];
}

- (void)parityTopmostPlaying:(ParityCheck)check next:(void (^)(void))next {
    mainview::MainView* mv = _mv.get();
    wvp::FfPlayer* player = mv->Player();
    const wvp::AlwaysOnTop savedMode = mv->Svc().settings.alwaysOnTop;
    auto setMode = [mv](wvp::AlwaysOnTop m) { mv->Svc().settings.alwaysOnTop = m; app::OnSettingsChanged(*mv); };
    using M = wvp::AlwaysOnTop;
    const NSWindowLevel F = NSFloatingWindowLevel, N = NSNormalWindowLevel;
    check((_window.collectionBehavior & NSWindowCollectionBehaviorManaged) && !(_window.collectionBehavior & NSWindowCollectionBehaviorTransient),
          "the player stays a managed window (its Space, Mission Control, full screen) at the floating level");
    // the four modes x playing / paused x windowed / full screen (ShouldBeTopmost), as the player's level
    bool levels = true;
    for (M m : {M::Disable, M::Enable, M::WhenPlaying, M::WhenFullscreen})
        for (bool fs : {false, true}) {
            setMode(m);
            if (fs) mv->SetFullscreenState(true);
            const bool want = app::sl::ShouldBeTopmost(m, player->State() == wvp::PlayerState::Playing, fs);
            if (_window.level != (want ? F : N)) { levels = false; printf("     mode %d fs %d: level %ld\n", (int)m, fs, (long)_window.level); }
            if (fs) mv->SetFullscreenState(false);
        }
    check(levels && player->State() == wvp::PlayerState::Playing, "playing: the player's level follows each Always on top mode in a window and in full screen");
    setMode(M::Enable);
    [self openSettings];
    NSWindow* sw = (__bridge NSWindow*)_settings->Window();
    check((sw.collectionBehavior & NSWindowCollectionBehaviorManaged) && (sw.collectionBehavior & NSWindowCollectionBehaviorFullScreenAuxiliary),
          "Settings is managed and may join the player's full-screen Space");
    const bool opened = [self window:sw inFrontOf:_window];
    char what[240];
    snprintf(what, sizeof what, "always on top: Settings opens in the player's band (level %ld / %ld), in front of it (z %ld < %ld)",
             (long)sw.level, (long)_window.level, (long)[self zIndexOf:sw], (long)[self zIndexOf:_window]);
    check(sw.level == F && _window.level == F && opened, what);
    // a press on the player (a middle button, posted to this app's own queue: the local monitor sees it as AppKit would)
    const pui::RectF b = mv->shell->Bar()->Bounds();
    for (NSEventType type : {NSEventTypeOtherMouseDown, NSEventTypeOtherMouseUp})
        [NSApp postEvent:[self windowEvent:type at:{b.l + 4, b.b - 3}] atStart:NO];
    [self waitFor:^bool { return self->_settings->PlayerInFront(); } seconds:2 then:^{
        const bool playerFront = [self window:self->_window inFrontOf:sw];
        char what2[200];
        snprintf(what2, sizeof what2, "a press on the player: Settings leaves the band (level %ld), the player is in front (z %ld < %ld)",
                 (long)sw.level, (long)[self zIndexOf:self->_window], (long)[self zIndexOf:sw]);
        check(self->_settings->PlayerInFront() && sw.level == N && playerFront, what2);
        mv->ApplyWindowTopmost();
        check(sw.level == N, "re-applying always on top does not lift Settings over the pressed player");
        [self applicationDidBecomeActive:[NSNotification notificationWithName:NSApplicationDidBecomeActiveNotification object:NSApp]];
        const bool settingsFront = [self window:sw inFrontOf:self->_window];
        char what3[220];
        snprintf(what3, sizeof what3, "the app activated from another app (a focus cycle): Settings is above the player again, in its band (level %ld, z %ld < %ld)",
                 (long)sw.level, (long)[self zIndexOf:sw], (long)[self zIndexOf:self->_window]);
        check(!self->_settings->PlayerInFront() && sw.level == F && settingsFront, what3);
        setMode(M::WhenPlaying);
        player->Pause();
        [self waitFor:^bool { return player->State() == wvp::PlayerState::Paused; } seconds:3 then:^{
            check(self->_window.level == N && sw.level == N && [self window:sw inFrontOf:self->_window],
                  "While playing + pause: the player leaves the band and Settings follows, still in front");
            player->Play();
            [self waitFor:^bool { return player->State() == wvp::PlayerState::Playing; } seconds:3 then:^{
                check(self->_window.level == F && sw.level == F && [self window:sw inFrontOf:self->_window],
                      "playing again: both back in the band, Settings in front");
                setMode(M::Disable);
                check(self->_window.level == N && sw.level == N && [self window:sw inFrontOf:self->_window], "Off: both normal, Settings in front");
                mv->Execute(app::CommandId::ToggleMiniPlayer);
                [self after:0.5 run:^{
                    check(mv->IsMiniPlayer() && self->_window.level == F && [self window:self->_window inFrontOf:sw],
                          "the mini player floats at the top of the band, over Settings (HWND_TOPMOST)");
                    mv->Execute(app::CommandId::ToggleMiniPlayer);
                    [self after:0.5 run:^{
                        check(!mv->IsMiniPlayer() && self->_window.level == N && sw.level == N && [self window:sw inFrontOf:self->_window],
                              "leaving it re-applies Off and puts Settings back above the player");
                        [self parityMinimized:check settings:sw then:^{ [self parityAlertLevel:check then:^{
                            setMode(savedMode);
                            self->_settings->Close();
                            next();
                        }]; }];
                    }];
                }];
            }];
        }];
    }];
}

- (void)parityMinimized:(ParityCheck)check settings:(NSWindow*)sw then:(void (^)(void))next {
    [_window miniaturize:nil];
    [self waitFor:^bool { return self->_window.miniaturized; } seconds:3 then:^{
        const bool real = self->_window.miniaturized;
        if (!real) {   // no Dock animation in this session: the delegate calls AppKit would make
            printf("     minimize: emulated (the window server did not minimize the window in this session)\n");
            [self windowDidMiniaturize:[NSNotification notificationWithName:NSWindowDidMiniaturizeNotification object:self->_window]];
        }
        check(!sw.visible && self->_settings->IsOpen(), "the player minimized: Settings is hidden, still open");
        if (real) [self->_window deminiaturize:nil];
        else [self windowDidDeminiaturize:[NSNotification notificationWithName:NSWindowDidDeminiaturizeNotification object:self->_window]];
        [self waitFor:^bool { return !self->_window.miniaturized && sw.visible; } seconds:3 then:^{
            check(sw.visible && !sw.isKeyWindow && sw.level == self->_window.level && [self window:sw inFrontOf:self->_window],
                  "restored: Settings shows again above the player, not activated");
            next();
        }];
    }];
}

- (void)parityAlertLevel:(ParityCheck)check then:(void (^)(void))next {
    mainview::MainView* mv = _mv.get();
    mv->Svc().settings.alwaysOnTop = wvp::AlwaysOnTop::Enable;
    app::OnSettingsChanged(*mv);
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = @"ARIB Player self-test";
    app::RaiseDialogToOwner((__bridge void*)alert.window, (__bridge void*)_window);   // as PromptText / PickPaths do
    __block NSWindowLevel level = -1;
    __block NSInteger z = -1, zp = -1;
    NSTimer* t = [NSTimer timerWithTimeInterval:0.3 repeats:NO block:^(NSTimer*) {
        level = alert.window.level;
        z = [self zIndexOf:alert.window];
        zp = [self zIndexOf:self->_window];
        [NSApp abortModal];
    }];
    [NSRunLoop.currentRunLoop addTimer:t forMode:NSModalPanelRunLoopMode];
    [alert runModal];
    char what[200];
    snprintf(what, sizeof what, "an owned modal dialog (runModal after RaiseDialogToOwner) is in front of an always-on-top player (level %ld >= %ld, z %ld < %ld)",
             (long)level, (long)_window.level, (long)z, (long)zp);
    check(level >= _window.level && z < zp, what);
    next();
}

// --- the two-launch persistence test -----------------------------------------------------------------------------------

- (void)runPersistPhase:(int)phase {
    __block int failures = 0;
    void (^check)(bool, const char*) = ^(bool ok, const char* what) {
        printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        fflush(stdout);
        if (!ok) failures++;
    };
    mainview::MainView* mv = _mv.get();
    wvp::FfPlayer* player = _mv->Player();
    if (phase == 1) {
        // launch 1: the four clips, the H.264 clip played to ~12 s; Dark theme, captions off, remaining time, panel closed
        [self waitFor:^bool { return [self playingIndex:0 past:0.3]; } seconds:15 then:^{
            mv->Ctl().PlayAt(3);
            [self waitFor:^bool { return [self playingIndex:3 past:0.5]; } seconds:10 then:^{
                player->Seek(12.0, false);
                [self waitFor:^bool { return player->Position() > 12.3; } seconds:5 then:^{
                    [self openSettings];
                    if (auto* theme = dynamic_cast<pui::RadioGroup*>(FindById(self->_settings->Page(), L"ThemeGroup"))) theme->SetSelectedIndex(2);
                    if (auto* captions = dynamic_cast<pui::ToggleSwitch*>(FindById(self->_settings->Page(), L"ShowCaptionsToggle"))) {
                        captions->SetOn(false);
                        if (captions->onToggled) captions->onToggled(false);
                    }
                    if (auto* dbg = dynamic_cast<pui::ToggleSwitch*>(FindById(self->_settings->Page(), L"DebugLoggingToggle"))) {
                        dbg->SetOn(true);
                        if (dbg->onToggled) dbg->onToggled(true);
                    }
                    if (!mv->Svc().settings.timeRemaining) mv->Execute(app::CommandId::ToggleTimeMode);
                    // the playlist panel width through the splitter: open the panel, drag the splitter 90 DIP left
                    if (!mv->PanelOpen()) mv->Execute(app::CommandId::TogglePlaylistPanel);
                    const app::RectPx sp = mv->Layout().splitterPx;
                    const float sc = mv->Scale();
                    const pui::PointF at{(sp.l + sp.r) / 2.f / sc, (sp.t + sp.b) / 2.f / sc};
                    const int w0 = mv->Svc().settings.playlistPanelWidthDip;
                    [self->_shell mouseDown:[self mouseEvent:NSEventTypeLeftMouseDown at:at clicks:1]];
                    [self->_shell mouseDragged:[self mouseEvent:NSEventTypeLeftMouseDragged at:{at.x - 90, at.y} clicks:1]];
                    [self->_shell mouseUp:[self mouseEvent:NSEventTypeLeftMouseUp at:{at.x - 90, at.y} clicks:1]];
                    char what[120];
                    snprintf(what, sizeof what, "phase 1: the splitter drag set the panel width %d -> %d DIP", w0, mv->Svc().settings.playlistPanelWidthDip);
                    check(!sp.Empty() && std::abs(mv->Svc().settings.playlistPanelWidthDip - (w0 + 90)) <= 1, what);
                    if (mv->PanelOpen()) mv->Execute(app::CommandId::TogglePlaylistPanel);
                    check(mv->Svc().settings.theme == wvp::ThemeSetting::Dark && !mv->Svc().settings.showCaptions && mv->Svc().settings.debugLogging,
                          "phase 1: settings changed through the Settings page (Dark, captions off, Debug logging on)");
                    printf("     phase 1: position %.2f s, quitting\n", player->Position());
                    fflush(stdout);
                    if (failures) exit(2);
                    [NSApp terminate:nil];   // applicationWillTerminate: Shutdown saves settings, playlist, history
                }];
            }];
        }];
        return;
    }
    // launch 2: nothing queued; everything comes from the data directory
    [self waitFor:^bool { return mv->Eng().Probed(); } seconds:10 then:^{
        const wvp::AppSettings& s = mv->Svc().settings;
        check(s.theme == wvp::ThemeSetting::Dark && mv->CurrentThemeMode() == pui::ThemeMode::Dark, "phase 2: the Dark theme was loaded and applied");
        check(!s.showCaptions && s.timeRemaining && !s.playlistPanel, "phase 2: captions off, remaining time, panel closed were loaded");
        check(mv->Ctl().Items().size() == 4 && mv->Ctl().Current() == nullptr, "phase 2: the unnamed playlist (4 clips) is restored, idle");
        check(s.window.valid, "phase 2: the window placement was saved");
        check(s.playlistPanelWidthDip == (int)app::kPanelDefaultDip + 90, "phase 2: the splitter's panel width (410 DIP) was loaded");
        const std::string logPath = mainview::logmac::CurrentDebugLogPath();
        NSString* logText = [NSString stringWithContentsOfFile:[NSString stringWithUTF8String:logPath.c_str()] encoding:NSUTF8StringEncoding error:nil];
        check(s.debugLogging && !logPath.empty() && logText && [logText containsString:@"] app: start: libffplay "],
              "phase 2: Debug logging was loaded on: the day's log file is open and has this launch's start line");
        mv->Ctl().PlayAt(3);
        [self waitFor:^bool { return [self playingIndex:3 past:0.2]; } seconds:10 then:^{
            char what[160];
            snprintf(what, sizeof what, "phase 2: the H.264 clip resumes where launch 1 left it (%.1f s; %s)", player->Position(), mv->ResumeDecision().c_str());
            check(player->Position() > 11 && mv->ResumeDecision().rfind("resume at 1", 0) == 0, what);
            printf("persist phase 2: %d failure(s)\n", failures);
            fflush(stdout);
            exit(failures == 0 ? 0 : 2);
        }];
    }];
}

// libffplay's SDL_Init(SDL_INIT_AUDIO) installs SDL's SIGINT / SIGTERM handlers (SDL_HINT_NO_SIGNAL_HANDLERS is not
// set), which only queue an SDL_QUIT event nobody pumps, so `kill` / Ctrl+C would not end the app. Once the player
// exists, the app takes both signals back and quits through NSApp (applicationWillTerminate: tears the player down).
static dispatch_source_t g_signalSources[2];
- (void)handleTerminationSignals {
    const int sigs[2] = {SIGTERM, SIGINT};
    for (int i = 0; i < 2; i++) {
        signal(sigs[i], SIG_IGN);
        g_signalSources[i] = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, (uintptr_t)sigs[i], 0, dispatch_get_main_queue());
        dispatch_source_set_event_handler(g_signalSources[i], ^{ [NSApp terminate:nil]; });
        dispatch_resume(g_signalSources[i]);
    }
}

// --- launch ------------------------------------------------------------------------------------------------------------

- (void)applicationDidFinishLaunching:(NSNotification*)n {
    (void)n;
    NSArray<NSString*>* args = NSProcessInfo.processInfo.arguments;
    NSUInteger i;
    const bool selftest = [args containsObject:@"--selftest"];
    NSString* dataDir = nil;
    if ((i = [args indexOfObject:@"--data-dir"]) != NSNotFound && i + 1 < args.count) dataDir = args[i + 1];
    if ((i = [args indexOfObject:@"--media"]) != NSNotFound && i + 1 < args.count) _media = args[i + 1];
    if ((i = [args indexOfObject:@"--caption-clip"]) != NSNotFound && i + 1 < args.count) _captionClip = args[i + 1];
    if ((i = [args indexOfObject:@"--subs-dir"]) != NSNotFound && i + 1 < args.count) _subsDir = args[i + 1];
    if ((i = [args indexOfObject:@"--clip"]) != NSNotFound && i + 1 < args.count) _clip = args[i + 1].UTF8String;
    if ((i = [args indexOfObject:@"--audio-driver"]) != NSNotFound && i + 1 < args.count) _audioDriver = args[i + 1].UTF8String;
    if ((i = [args indexOfObject:@"--deinterlace"]) != NSNotFound && i + 1 < args.count) _deinterlace = args[i + 1].UTF8String;
    if ((i = [args indexOfObject:@"--stats-every"]) != NSNotFound && i + 1 < args.count) _statsEvery = args[i + 1].doubleValue;
    if ((i = [args indexOfObject:@"--persist-phase"]) != NSNotFound && i + 1 < args.count) _persistPhase = args[i + 1].intValue;
    if ((i = [args indexOfObject:@"--expect-open"]) != NSNotFound && i + 1 < args.count) _expectOpen = args[i + 1].intValue;
    if ((i = [args indexOfObject:@"--lang"]) != NSNotFound && i + 1 < args.count)
        app::SetUiLanguage([args[i + 1] hasPrefix:@"ko"] ? app::UiLanguage::Korean : app::UiLanguage::English);
    // Inputs on the command line (anything that is not an option or an option's value), like the Windows app's.
    static NSSet* valued = [NSSet setWithArray:@[@"--data-dir", @"--media", @"--caption-clip", @"--subs-dir", @"--clip", @"--audio-driver", @"--deinterlace",
                                                 @"--stats-every", @"--persist-phase", @"--expect-open", @"--lang",
                                                 @"--log-file", @"--log-level"]];
    // Main.cpp's diagnostics options -> log::Configure (before the player exists).
    app::log::Settings ls;
    for (NSUInteger k = 1; k < args.count; k++) {
        NSString* a = args[k];
        if ([a isEqualToString:@"--console"]) ls.console = true;
        else if ([a isEqualToString:@"--log-file"] && k + 1 < args.count) ls.file = ToW(args[++k]);
        else if ([a hasPrefix:@"--log-file="]) ls.file = ToW([a substringFromIndex:11]);
        else if (([a isEqualToString:@"--log-level"] && k + 1 < args.count) || [a hasPrefix:@"--log-level="]) {
            NSString* v = [a hasPrefix:@"--log-level="] ? [a substringFromIndex:12] : args[++k];
            if (auto lv = app::log::ParseLevel(ToW(v))) ls.level = lv;
            else { fprintf(stderr, "invalid --log-level value: %s\n", v.UTF8String); exit(2); }
        }
    }
    app::log::Configure(ls);
    if (ls.console) app::log::OpenConsole();
    for (NSUInteger k = 1; k < args.count; k++) {
        NSString* a = args[k];
        if ([valued containsObject:a]) { k++; continue; }
        if ([a hasPrefix:@"-"]) continue;   // --selftest, -psn_..., -NSDocumentRevisionsDebugMode etc.
        NSString* path = [a rangeOfString:@"://"].location != NSNotFound || [a hasPrefix:@"/"] ? a
                         : [NSFileManager.defaultManager.currentDirectoryPath stringByAppendingPathComponent:a];
        _pendingOpen.push_back(ToW(path));
    }

    const bool testing = selftest || _persistPhase == 1;
    if (!_media && (testing || !_clip.empty())) {
        fprintf(stderr, "--media DIR is required; generate fixtures with mac/app/make-testmedia.sh DIR\n");
        exit(2);
    }
    if (_media) {
        BOOL directory = NO;
        if (![NSFileManager.defaultManager fileExistsAtPath:_media isDirectory:&directory] || !directory) {
            fprintf(stderr, "media directory does not exist: %s\n", _media.UTF8String);
            exit(2);
        }
    }
    if (_media && ![_media hasPrefix:@"/"]) _media = [[NSFileManager defaultManager].currentDirectoryPath stringByAppendingPathComponent:_media];
    if (dataDir) {
        _dataDir = dataDir.UTF8String;
    } else if (selftest || _expectOpen > 0) {   // tests never touch the user's data
        _dataDir = (std::filesystem::temp_directory_path() / ("aribplayer-test-" + std::to_string(getpid()))).string();
        _tempDataDir = true;
    }

    [self buildMenuBar];
    [self createWindow];
    [self handleTerminationSignals];
    [NSApp activateIgnoringOtherApps:YES];
    [_window display];
    _launched = true;
    if (_media) [self queueClips];
    if (!_pendingOpen.empty()) {
        app::OpenPaths(*_mv, _pendingOpen);
        _pendingOpen.clear();
    }
    if (_statsEvery > 0) {
        __weak AppDelegate* weakSelf = self;
        _statsTimer = [NSTimer scheduledTimerWithTimeInterval:_statsEvery repeats:YES block:^(NSTimer*) {
            AppDelegate* s = weakSelf;
            if (!s) return;
            printf("STATS %s\n", [s statsLine].c_str());
            fflush(stdout);
        }];
    }
    if (_subsDir) _subsDir = [[NSURL fileURLWithPath:_subsDir] URLByStandardizingPath].path;
    if (selftest) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            if (self->_mv->PanelOpen()) self->_mv->Execute(app::CommandId::TogglePlaylistPanel);   // the test starts with the panel closed
            [self runSelfTest];
        });
        return;
    }
    if (_persistPhase > 0) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ [self runPersistPhase:self->_persistPhase]; });
        return;
    }
    if (_expectOpen > 0) {
        printf("READY\n");
        fflush(stdout);
        __weak AppDelegate* weakSelf = self;
        [self waitFor:^bool {
            AppDelegate* s = weakSelf;
            return s && s->_opensReceived >= s->_expectOpen && s->_mv->Player()->State() == wvp::PlayerState::Playing && s->_mv->Player()->Position() > 0.5;
        } seconds:60 then:^{
            AppDelegate* s = weakSelf;
            const bool ok = s->_opensReceived >= s->_expectOpen && s->_mv->Player()->State() == wvp::PlayerState::Playing;
            const wvp::MediaItem* it = s->_mv->Ctl().Current();
            printf("%s open-with: %d delivery(ies) to this instance, playing %s\n", ok ? "ok  " : "FAIL", s->_opensReceived, it ? it->url.c_str() : "-");
            fflush(stdout);
            exit(ok ? 0 : 2);
        }];
        return;
    }
}

@end

int main(int argc, const char** argv) {
    (void)argc; (void)argv;
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        AppDelegate* delegate = [[AppDelegate alloc] init];
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
