#pragma once
#include <string>

// Lightweight thread-safe logger.
//
// - One mutex-guarded writer: lines never interleave between threads.
// - Timestamps, level filtering, optional ANSI colors.
// - LOG_LEVEL  = debug | info | warn | error   (default: info)
// - LOG_FORMAT = text | json                   (default: text; json => one
//                JSON object per line, handy for docker/CI monitoring)
// - NO_COLOR=1 disables ANSI colors (auto-disabled when not a tty).

namespace lg {

enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3 };
enum class Color { Default, Green, Red, Yellow, Cyan };

void init();  // reads env once; safe to call multiple times

Level level();
bool jsonMode();
bool colorEnabled();

void line(Level lvl, const std::string& tag, const std::string& msg, Color c = Color::Default);

// Convenience wrappers.
inline void debug(const std::string& tag, const std::string& msg) { line(Level::Debug, tag, msg); }
inline void info(const std::string& tag, const std::string& msg) { line(Level::Info, tag, msg); }
inline void warn(const std::string& tag, const std::string& msg) { line(Level::Warn, tag, msg, Color::Yellow); }
inline void error(const std::string& tag, const std::string& msg) { line(Level::Error, tag, msg, Color::Red); }
inline void good(const std::string& tag, const std::string& msg) { line(Level::Info, tag, msg, Color::Green); }
inline void bad(const std::string& tag, const std::string& msg) { line(Level::Warn, tag, msg, Color::Red); }

}  // namespace lg
