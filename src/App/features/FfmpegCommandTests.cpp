#include "LogicTest.h"
#include "features/FfmpegCommand.h"

using namespace app;

LOGIC_TEST(ffmpeg_frame_command_line) {
    // Floor, not round: 10.0103 -> .010 and 12.3799 - 10.010 -> 2.369
    LT_CHECK_EQ(FfmpegFrameCommandLine(L"C:\v\a.ts", {12.3799, 10.0103}, false), std::wstring(L"ffmpeg -ss 00:00:10.010 -i \"C:\v\a.ts\" -ss 2.369"));
    LT_CHECK_EQ(FfmpegFrameCommandLine(L"a.ts", {3725.5, 3725.5}, false), std::wstring(L"ffmpeg -ss 01:02:05.500 -i \"a.ts\" -ss 0.000"));
    LT_CHECK_EQ(FfmpegFrameCommandLine(L"a.ts", {360000.75, 360000.0}, false), std::wstring(L"ffmpeg -ss 100:00:00.000 -i \"a.ts\" -ss 0.750"));
    LT_CHECK_EQ(FfmpegFrameCommandLine(L"a.ts", {1.0, -0.5}, false), std::wstring(L"ffmpeg -ss 00:00:00.000 -i \"a.ts\" -ss 1.000"));
    LT_CHECK_EQ(FfmpegFrameCommandLine(L"/m/it's.ts", {5.0, 4.0}, true), std::wstring(L"ffmpeg -ss 00:00:04.000 -i '/m/it'\''s.ts' -ss 1.000"));
}
