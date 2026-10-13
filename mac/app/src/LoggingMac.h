#pragma once
// The Mac side of the diagnostic log. The log itself is src/App's features/Logging.cpp + LoggingFormat.cpp
// (the Sink, the line format "YYYY-MM-DD hh:mm:ss.mmm [INFO ] src: message", the daily file name
// aribplayer-YYYYMMDD.log, the level rules, the libffplay av_log hook through FfPlayer::SetLogCallback); the Win32 names
// it mentions (GetLocalTime, std handles, console) are in shim/windows.h. Two of its helpers build Windows paths
// (LogsDir appends "\logs", DebugLogPath joins with '\'), so the folder and the path of the Debug logging file are made
// here, the Mac way:
//   * the default data directory (~/Library/Application Support/aribplayer) logs to ~/Library/Logs/aribplayer/ (the
//     macOS convention: Console.app lists it under Log Reports);
//   * an injected data directory (--data-dir, the tests) logs to <data dir>/logs/ (Windows' rule: next to settings.json).
// ApplyDebugLogging is log::ApplyDebugLogging with that path; the rest (Event, InstallEngineHooks, Global) is features/Logging's.
#include <string>

namespace mainview::logmac {

/// The folder of the Debug logging file for a settings.json path (see above).
std::string LogsDir(const std::string& settingsFilePath);
/// `<dir>/aribplayer-YYYYMMDD.log` of today (features/Logging's naming, '/' joined).
std::string DebugLogPath(const std::string& dir);
/// Settings > About > Debug logging: opens / closes the day's file in the sink's debug slot, then
/// log::InstallEngineHooks (libffplay's lines at info level while a sink is active). Idempotent.
void ApplyDebugLogging(bool on, const std::string& logsDir);
/// The open Debug logging file ("" when off).
std::string CurrentDebugLogPath();

}
