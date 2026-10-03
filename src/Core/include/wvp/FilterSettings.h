#pragma once
// User-facing filter intents shared by FilterManager and PipelinePlanner without a header cycle.
#include <array>
#include <cstddef>
#include <string>

namespace wvp {

/// One "Deinterlace" option: Off, On (a plain deinterlacer, chosen by the planner from DeinterlacePriority) or Ivtc
/// (inverse telecine with residual-combing cleanup). deinterlaceRate / deinterlaceOnlyInterlaced / deinterlacePriority
/// apply only to On; IvtcTuning only to Ivtc.
enum class DeinterlaceMode { Off, On, Ivtc };
/// On a GPU deinterlacer the planner always uses bwdif; CPU only: Efficiency = yadif, Quality = bwdif.
enum class DeinterlacePriority { Efficiency, Quality };
/// Frame = one output frame per input frame (send_frame); Field = one per field (send_field, double rate).
enum class DeinterlaceRate { Frame, Field };
enum class Rotation { None = 0, Cw90 = 90, Rotate180 = 180, Ccw90 = 270 };

namespace filters {
inline constexpr std::size_t kEqBandCount = 10;
inline constexpr double kBrightnessMin = -1, kBrightnessMax = 1, kBrightnessDefault = 0;
inline constexpr double kContrastMin = -1000, kContrastMax = 1000, kContrastDefault = 1;
inline constexpr double kSaturationMin = 0, kSaturationMax = 3, kSaturationDefault = 1;
inline constexpr double kGammaMin = 0.1, kGammaMax = 10, kGammaDefault = 1;
inline constexpr double kEqGainMinDb = -20, kEqGainMaxDb = 20;
inline constexpr double kMinSpeed = 0.1, kMaxSpeed = 100.0, kDefaultSpeed = 1.0;   // 100 = atempo's tempo range (af_atempo.c YAE_ATEMPO_MAX)
inline constexpr std::array<double, 8> kSpeedPresets = { 0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0 };
inline constexpr const wchar_t* kEqInstance = L"eq@color";
}

struct FilterSettings {
    DeinterlaceMode deinterlace = DeinterlaceMode::Off;
    DeinterlaceRate deinterlaceRate = DeinterlaceRate::Frame;   // On only
    bool deinterlaceOnlyInterlaced = false;   // deint=interlaced instead of all (On only)
    DeinterlacePriority deinterlacePriority = DeinterlacePriority::Efficiency;   // On only
    double brightness = filters::kBrightnessDefault;   // -1..1
    double contrast = filters::kContrastDefault;       // -1000..1000
    double saturation = filters::kSaturationDefault;   // 0..3
    double gamma = filters::kGammaDefault;             // 0.1..10
    Rotation rotation = Rotation::None;
    bool flipHorizontal = false;
    bool flipVertical = false;
    bool normalize = false;
    std::array<double, filters::kEqBandCount> eqGainsDb{};   // -20..20 dB per band, see FilterManager::kEqBandsHz
    double speed = filters::kDefaultSpeed;                   // kMinSpeed..kMaxSpeed
    bool advancedMode = false;                               // custom strings replace presets
    std::wstring customVideoFilter;                          // empty = none
    std::wstring customAudioFilter;

    bool operator==(const FilterSettings&) const = default;
};

struct IvtcTuning {
    std::wstring pixelFormat = L"nv12";   // hwupload/hwdownload round-trip format
    int fieldOrder = -1;                  // -1 auto, 0 bff, 1 tff
    double combThreshold = 9.0;           // 0..255
    double dupThreshold = 1.1;            // 0..100
    double sceneThreshold = 15.0;         // 0..100
    bool operator==(const IvtcTuning&) const = default;
};

}
