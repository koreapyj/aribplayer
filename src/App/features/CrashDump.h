#pragma once
// Crash diagnostics: %LOCALAPPDATA%\aribplayer\crashes\aribplayer-YYYYMMDD-HHMMSS-<pid>.dmp plus one log line for an
// unhandled SEH exception, std::terminate, an invalid CRT parameter, a pure virtual call, abort(), and an exit() that
// bypasses wWinMain's normal return (the log line names it). Fast-fail (0xC0000409) kills the process without any
// user-mode handler: WER LocalDumps is the only source for those.
namespace app::crash {
void Install();
void MarkCleanExit();   // wWinMain is returning normally
}
