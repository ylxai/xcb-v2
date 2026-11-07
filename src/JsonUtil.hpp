#pragma once

#include <string>

// Escape a string for embedding inside a JSON string literal. Covers the two
// characters that break the payload (quote and backslash); wallet, worker and
// job ids are otherwise printable.
inline std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}
