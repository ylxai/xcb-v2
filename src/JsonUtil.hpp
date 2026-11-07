#pragma once

#include <string>

// Escape a string for embedding inside a JSON string literal. Quote and
// backslash are the obvious cases; control characters must also be escaped
// because JSON forbids raw bytes below 0x20 and a bare newline would inject an
// extra line into the newline-delimited JSON-RPC stream.
inline std::string json_escape(const std::string& s) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out += kHex[c >> 4];
                    out += kHex[c & 0x0f];
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}
