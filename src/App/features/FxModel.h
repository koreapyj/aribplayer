#pragma once
// Pure models behind the Filters panel, the menus and the playback extras (no Win32, no UI). Tested by --logictest (FxLogicTests.cpp).
#include <array>
#include <functional>
#include <string>
#include <vector>

#include "wvp/FilterManager.h"
#include "wvp/PlayerTypes.h"

namespace app::fx {

struct EqPreset {
    const wchar_t* name;
    std::array<double, wvp::filters::kEqBandCount> gainsDb;
};
/// Flat, Bass boost, Treble boost, Vocal, Rock, Pop, Classical, Loudness (UI range +-12 dB).
const std::vector<EqPreset>& EqPresets();
/// Index into EqPresets() whose gains equal `g` (tolerance 0.01 dB), -1 = custom.
int MatchEqPreset(const std::array<double, wvp::filters::kEqBandCount>& g);
/// Index into kSpeedPresets, -1 when the speed is not a preset.
int SpeedIndex(double speed);
/// "1x", "0.25x", "1.5x".
std::wstring SpeedLabel(double speed);
/// "31 Hz", "1 kHz", "16 kHz".
std::wstring BandLabel(int hz);
/// Fixed decimals value text ("0.50"), used next to sliders.
std::wstring Fixed(double v, int decimals = 2);
/// "+3 dB" / "-6 dB" / "0 dB".
std::wstring DbLabel(double db);

/// Which sub-controls of the single Deinterlace group are shown for a mode: priority + output rate + only-interlaced for On,
/// the IVTC tuning (tff / cthresh / dupthresh / scthresh / pixel format) for IVTC, nothing for Off.
struct DeinterlaceControls { bool priority = false, rate = false, onlyInterlaced = false, ivtcTuning = false; };
DeinterlaceControls DeinterlaceControlsFor(wvp::DeinterlaceMode mode);
/// Combo labels in DeinterlaceMode order: "Off", "On", "IVTC".
const std::vector<std::wstring>& DeinterlaceLabels();
/// Combo labels in DeinterlacePriority order: "Efficiency", "Quality".
const std::vector<std::wstring>& DeinterlacePriorityLabels();

/// Video tab "Reset": color, rotation, flips, deinterlace, IVTC back to defaults (audio/custom kept).
wvp::FilterSettings ResetVideoPart(wvp::FilterSettings s);
/// Audio tab "Reset": normalize, EQ and speed back to defaults.
wvp::FilterSettings ResetAudioPart(wvp::FilterSettings s);
wvp::FilterSettings WithEqPreset(wvp::FilterSettings s, int presetIndex);
bool HasEffect(const wvp::FilterChange& c);        // rebuild flag or commands (the only case worth sending to the player)

// libffplay has no aspect/zoom option (docs/libffplay-api.md): zoom to fill is a centred crop filter computed from the
// video area aspect and appended to the custom video filter. Limitation: the crop is recomputed on a ~400 ms poll
// while zoom is on (no resize hook in AppContext), and a custom vf that itself resizes after it may defeat it.
struct ZoomState {
    bool on = false;
    double aspect = 16.0 / 9.0;   // video area (window) aspect the crop is computed for
};
ZoomState& Zoom();
inline constexpr const wchar_t* kZoomInstance = L"crop@wvpzoom";
/// "crop@wvpzoom=w=min(iw\,ih*A/sar):h=min(ih\,iw*sar/A)": centred crop of the filtered frame to display aspect A.
std::wstring ZoomCropFilter(double windowAspect);
/// Removes the zoom crop piece from a filter string (the panel shows/edits the user's custom string without it).
std::wstring StripZoom(const std::wstring& vf);
/// user custom vf + the zoom crop (when on); user string first.
std::wstring ComposeCustomVideo(const std::wstring& user, bool zoomOn, double aspect);
/// Video area aspect: Now playing leaves a stripDip title strip, full screen/mini use the whole client.
double VideoAreaAspect(float clientWDip, float clientHDip, float stripDip, bool fullscreenOrMini);
/// The settings the user edits: manager settings with the zoom piece stripped from customVideoFilter.
wvp::FilterSettings UserView(wvp::FilterSettings managerSettings);
/// Applies user-view settings to the manager, adding the zoom crop to the custom video filter. Returns the minimal change.
wvp::FilterChange ApplyUser(wvp::FilterManager& fm, wvp::FilterSettings user, const ZoomState& z);

/// Superimpose subtitle streams are visible but are not members of ffp_cycle_stream's subtitle list.
bool IsCycleableSubtitle(const wvp::StreamInfo& stream);
std::vector<wvp::StreamInfo> CycleableSubtitles(const std::vector<wvp::StreamInfo>& streams);
/// The selected stream's zero-based index among cycleable subtitle streams, or -1 when none is selected.
int SelectedCycleableSubtitleIndex(const std::vector<wvp::StreamInfo>& streams);
/// Toggle the caption visibility value only when the media has an ARIB caption stream; returns whether it toggled.
bool ToggleAribCaptionVisibility(const wvp::MediaInfo& media, bool& visible);
/// Number of CycleStream calls needed to go from the currently selected stream to `targetIndex` among `streams`
/// (same-type streams in ascending order). For subtitles the cycle has an extra "off" state after the last stream;
/// targetIndex -1 = off (subtitles). Current = the stream with Selected (none selected = off for subtitles, 0 for audio).
int CyclesNeeded(const std::vector<wvp::StreamInfo>& streams, int targetIndex, bool subtitle);
std::wstring TrackName(const wvp::StreamInfo& s, int ordinalOneBased);
/// "Audio: English (2/3)", "Subtitles: Off", "Subtitles: Forced (1/2)"; "" when there is nothing to report.
std::wstring TrackToast(wvp::StreamType type, const std::vector<wvp::StreamInfo>& streams);
std::wstring LanguageName(const std::wstring& code);
/// ARIB audio item labels: the track names of today's list for the two roles (a role without a stream is "Track 1" /
/// "Track 2"); when both come out equal they are told apart as "<name> (Audio 1)" / "<name> (Audio 2)".
void AribAudioNames(const wvp::StreamInfo& audio1, bool has1, const wvp::StreamInfo& audio2, bool has2,
                    std::wstring& name1, std::wstring& name2);

}
