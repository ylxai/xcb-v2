#include "Log.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <unistd.h>

namespace lg {
namespace {

std::mutex g_mu;
Level g_level = Level::Info;
bool g_json = false;
bool g_color = false;
bool g_inited = false;

const char* levelName(Level lvl) {
    switch (lvl) {
        case Level::Debug: return "debug";
        case Level::Info: return "info";
        case Level::Warn: return "warn";
        case Level::Error: return "error";
    }
    return "info";
}

const char* levelColor(Level lvl) {
    switch (lvl) {
        case Level::Debug: return "\x1b[90m";  // gray
        case Level::Info: return "\x1b[97m";   // white
        case Level::Warn: return "\x1b[93m";   // yellow
        case Level::Error: return "\x1b[91m";  // red
    }
    return "";
}

const char* colorCode(Color c) {
    switch (c) {
        case Color::Green: return "\x1b[92m";
        case Color::Red: return "\x1b[91m";
        case Color::Yellow: return "\x1b[93m";
        case Color::Cyan: return "\x1b[96m";
        default: return "";
    }
}

// JSON-escape a string (quotes, backslash, control chars).
std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

void timestamp(char* buf, size_t n) {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_r(&t, &tm);
    strftime(buf, n, "%H:%M:%S", &tm);
    size_t len = strlen(buf);
    snprintf(buf + len, n - len, ".%03u", (unsigned)ms.count());
}

}  // namespace

void init() {
    if (g_inited) return;
    std::lock_guard<std::mutex> lock(g_mu);

    if (const char* e = getenv("LOG_LEVEL")) {
        if (strcmp(e, "debug") == 0) g_level = Level::Debug;
        else if (strcmp(e, "info") == 0) g_level = Level::Info;
        else if (strcmp(e, "warn") == 0) g_level = Level::Warn;
        else if (strcmp(e, "error") == 0) g_level = Level::Error;
    }
    if (const char* e = getenv("LOG_FORMAT")) {
        g_json = (strcmp(e, "json") == 0);
    }
    const char* noColor = getenv("NO_COLOR");
    bool noColorSet = noColor && noColor[0] != '\0' && strcmp(noColor, "0") != 0 &&
                      strcmp(noColor, "false") != 0;
    g_color = !noColorSet && isatty(1) != 0;
    g_inited = true;
}

Level level() { return g_level; }
bool jsonMode() { return g_json; }
bool colorEnabled() { return g_color; }

void line(Level lvl, const std::string& tag, const std::string& msg, Color c) {
    if (g_level > lvl) return;
    if (!g_inited) init();

    char ts[32];
    timestamp(ts, sizeof(ts));

    std::lock_guard<std::mutex> lock(g_mu);
    if (g_json) {
        // {"ts":"...","level":"info","tag":"miner","msg":"..."}
        printf("{\"ts\":\"%s\",\"level\":\"%s\",\"tag\":\"%s\",\"msg\":\"%s\"}\n", ts,
               levelName(lvl), jsonEscape(tag).c_str(), jsonEscape(msg).c_str());
    } else {
        if (g_color) {
            const char* lc = levelColor(lvl);
            const char* cc = colorCode(c);
            const char* reset = "\x1b[0m";
            if (cc[0]) {
                printf("[%s] %s[%s%s%s] %s%s%s\n", ts, lc, cc, tag.c_str(), reset, cc, msg.c_str(),
                       reset);
            } else {
                printf("[%s] %s[%s%s] %s%s\n", ts, lc, tag.c_str(), reset, lc, msg.c_str());
            }
        } else {
            printf("[%s] [%s] %s\n", ts, tag.c_str(), msg.c_str());
        }
    }
    fflush(stdout);
}

}  // namespace lg
