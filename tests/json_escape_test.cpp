// Unit test untuk json_escape(). Dijalankan lewat ctest (xcb-json-escape).
// Payload stratum/RPC adalah JSON per baris: karakter kontrol mentah bukan
// hanya JSON invalid, newline juga menambah baris baru ke stream.
#include <cstdio>
#include <string>

#include "JsonUtil.hpp"

static int g_fail = 0;

static void expect(const char* what, const std::string& got, const std::string& want) {
    if (got == want) return;
    std::fprintf(stderr, "FAIL %s\n  got  [%s]\n  want [%s]\n", what, got.c_str(),
                 want.c_str());
    ++g_fail;
}

int main() {
    expect("plain", json_escape("abc123"), "abc123");
    expect("empty", json_escape(""), "");
    expect("quote", json_escape("a\"b"), "a\\\"b");
    expect("backslash", json_escape("a\\b"), "a\\\\b");
    expect("newline", json_escape("a\nb"), "a\\nb");
    expect("cr", json_escape("a\rb"), "a\\rb");
    expect("tab", json_escape("a\tb"), "a\\tb");
    expect("backspace", json_escape("a\bb"), "a\\bb");
    expect("formfeed", json_escape("a\fb"), "a\\fb");
    expect("ctrl-0x01", json_escape(std::string("a\x01" "b", 3)), "a\\u0001b");
    expect("ctrl-0x1f", json_escape(std::string("a\x1f" "b", 3)), "a\\u001fb");
    expect("nul", json_escape(std::string("a\0b", 3)), "a\\u0000b");
    // Byte >= 0x80 adalah UTF-8 dan harus lewat apa adanya.
    expect("utf8", json_escape("caf\xc3\xa9"), "caf\xc3\xa9");
    // Semua karakter kontrol sekaligus: tidak boleh ada byte mentah < 0x20.
    for (int c = 0; c < 0x20; ++c) {
        const std::string in(1, static_cast<char>(c));
        const std::string out = json_escape(in);
        if (out.find(static_cast<char>(c)) != std::string::npos) {
            std::fprintf(stderr, "FAIL raw 0x%02x lolos: [%s]\n", c, out.c_str());
            ++g_fail;
        }
    }

    if (g_fail) {
        std::fprintf(stderr, "%d kasus gagal\n", g_fail);
        return 1;
    }
    std::printf("json_escape: semua kasus lulus\n");
    return 0;
}
