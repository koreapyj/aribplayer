#pragma once
// Minimal AppContext for tests that build a ShellRoot (its playlist panel keeps a context reference) without the real App:
// every call is a no-op, Svc() uses a private empty data directory.
#include <filesystem>

#include "AppContext.h"
#include "pui/Buttons.h"

namespace app {

class NullShellContext final : public AppContext {
public:
    NullShellContext()
        : dir_(std::filesystem::temp_directory_path() / (L"aribplayer-shell-test-" + std::to_wstring(GetCurrentProcessId()))),
          svc_(dir_.wstring()), theme_(pui::Theme::Make(false)) {}
    HWND MainHwnd() const override { return nullptr; }
    pui::UiHost& Ui() override { return host_; }
    wvp::PlaybackController* Controller() override { return nullptr; }
    wvp::FfPlayer* Player() override { return nullptr; }
    Services& Svc() override { return svc_; }
    pui::IRenderer& Measurer() override { return *pui::DefaultMeasurer(); }
    void SetFullscreen(bool) override {}
    bool IsFullscreen() const override { return false; }
    void SetMiniPlayer(bool) override {}
    bool IsMiniPlayer() const override { return false; }
    ShellMode Mode() const override { return ShellMode::NowPlaying; }
    void AppendToCurrentPlaylist(std::vector<wvp::MediaItem>) override {}
    void ReplaceUnnamedPlaylistAtPosition(std::vector<wvp::MediaItem>, double) override {}
    void ShowInfoBar(InfoSeverity, const std::wstring&, const std::wstring&) override {}
    void ShowToast(const std::wstring&) override {}
    void ApplyTheme(pui::ThemeMode, uint32_t) override {}
    const pui::Theme& CurrentTheme() const override { return theme_; }
    pui::ThemeMode CurrentThemeMode() const override { return pui::ThemeMode::Light; }
    float Scale() const override { return 1.f; }
    void RequestRedraw() override {}
    void Execute(CommandId) override {}
    void PostToUi(std::function<void()> fn) override { if (fn) fn(); }

private:
    std::filesystem::path dir_;
    Services svc_;
    pui::UiHost host_;
    pui::Theme theme_;
};

}
