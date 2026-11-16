#include "ZcdJson.hpp"

#include <cstdlib>
#include <cstring>

namespace zcd {

namespace {

bool isWs(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

struct Parser {
    const std::string& s;
    size_t i = 0;
    std::string err;

    explicit Parser(const std::string& text) : s(text) {}

    bool fail(const std::string& msg) {
        if (err.empty()) err = msg + " @" + std::to_string(i);
        return false;
    }

    void ws() {
        while (i < s.size() && isWs(s[i])) i++;
    }

    bool parseString(std::string& out) {
        if (i >= s.size() || s[i] != '"') return fail("string butuh '\"'");
        i++;
        out.clear();
        while (i < s.size()) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (c == '"') {
                i++;
                return true;
            }
            if (c == '\\') {
                i++;
                if (i >= s.size()) return fail("escape terpotong");
                switch (s[i]) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        if (i + 4 >= s.size()) return fail("\\u terpotong");
                        unsigned cp = 0;
                        for (int k = 1; k <= 4; k++) {
                            const char h = s[i + k];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                            else return fail("\\u bukan hex");
                        }
                        i += 4;
                        // UTF-8 encode; substitute untuk surrogate supaya tidak
                        // menghasilkan byte rusak.
                        if (cp < 0x80) {
                            out += static_cast<char>(cp);
                        } else if (cp < 0x800) {
                            out += static_cast<char>(0xC0 | (cp >> 6));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                            out += '?';
                        } else {
                            out += static_cast<char>(0xE0 | (cp >> 12));
                            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: return fail("escape tidak dikenal");
                }
                i++;
                continue;
            }
            if (c < 0x20) return fail("kontrol di string");
            out += static_cast<char>(c);
            i++;
        }
        return fail("string tidak ditutup");
    }

    bool parseValue(Json& out, int depth) {
        if (depth > 64) return fail("kedalaman JSON berlebih");
        ws();
        if (i >= s.size()) return fail("JSON habis");
        const char c = s[i];
        if (c == '{') return parseObj(out, depth);
        if (c == '[') return parseArr(out, depth);
        if (c == '"') {
            out.t = Json::T::Str;
            return parseString(out.str);
        }
        if (c == 't') {
            if (s.compare(i, 4, "true") != 0) return fail("bukan true");
            i += 4;
            out.t = Json::T::Bool;
            out.b = true;
            return true;
        }
        if (c == 'f') {
            if (s.compare(i, 5, "false") != 0) return fail("bukan false");
            i += 5;
            out.t = Json::T::Bool;
            out.b = false;
            return true;
        }
        if (c == 'n') {
            if (s.compare(i, 4, "null") != 0) return fail("bukan null");
            i += 4;
            out.t = Json::T::Null;
            return true;
        }
        return parseNum(out);
    }

    bool parseNum(Json& out) {
        const size_t start = i;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) i++;
        bool digits = false;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            i++;
            digits = true;
        }
        if (i < s.size() && s[i] == '.') {
            i++;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
                i++;
                digits = true;
            }
        }
        if (digits && i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            i++;
            if (i < s.size() && (s[i] == '-' || s[i] == '+')) i++;
            // Eksponen "1e" tanpa digit adalah JSON tidak valid; menerimanya
            // membuat strtod membaca "1" dan job yang rusak lolos.
            const size_t expStart = i;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
            if (i == expStart) return fail("eksponen kosong");
        }
        if (!digits) return fail("angka tidak valid");
        out.t = Json::T::Num;
        out.num = std::strtod(s.substr(start, i - start).c_str(), nullptr);
        // Overflow eksponen (1e9999) menghasilkan inf; simpan sebagai angka
        // tapi pemakai wajib memvalidasi rentang sebelum konversi ke integer.
        return true;
    }

    bool parseArr(Json& out, int depth) {
        out.t = Json::T::Arr;
        i++;  // '['
        ws();
        if (i < s.size() && s[i] == ']') {
            i++;
            return true;
        }
        while (true) {
            Json v;
            if (!parseValue(v, depth + 1)) return false;
            out.arr.push_back(std::move(v));
            ws();
            if (i >= s.size()) return fail("array tidak ditutup");
            if (s[i] == ',') {
                i++;
                continue;
            }
            if (s[i] == ']') {
                i++;
                return true;
            }
            return fail("array: butuh ',' atau ']'");
        }
    }

    bool parseObj(Json& out, int depth) {
        out.t = Json::T::Obj;
        i++;  // '{'
        ws();
        if (i < s.size() && s[i] == '}') {
            i++;
            return true;
        }
        while (true) {
            ws();
            std::string key;
            if (!parseString(key)) return false;
            ws();
            if (i >= s.size() || s[i] != ':') return fail("objek: butuh ':'");
            i++;
            Json v;
            if (!parseValue(v, depth + 1)) return false;
            out.obj.emplace_back(std::move(key), std::move(v));
            ws();
            if (i >= s.size()) return fail("objek tidak ditutup");
            if (s[i] == ',') {
                i++;
                continue;
            }
            if (s[i] == '}') {
                i++;
                return true;
            }
            return fail("objek: butuh ',' atau '}'");
        }
    }
};

}  // namespace

const Json* Json::get(const std::string& key) const {
    if (t != T::Obj) return nullptr;
    for (const auto& kv : obj)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

bool Json::getStr(const std::string& key, std::string& out) const {
    const Json* v = get(key);
    if (!v || v->t != T::Str) return false;
    out = v->str;
    return true;
}

bool Json::getNum(const std::string& key, double& out) const {
    const Json* v = get(key);
    if (!v || v->t != T::Num) return false;
    out = v->num;
    return true;
}

bool Json::getBool(const std::string& key, bool& out) const {
    const Json* v = get(key);
    if (!v || v->t != T::Bool) return false;
    out = v->b;
    return true;
}

Json parseJson(const std::string& text, std::string* err) {
    Parser p(text);
    Json out;
    if (!p.parseValue(out, 0)) {
        if (err) *err = p.err.empty() ? "parse error" : p.err;
        return Json{};
    }
    p.ws();
    if (p.i != text.size()) {
        if (err) *err = "data sisa setelah JSON @" + std::to_string(p.i);
        return Json{};
    }
    return out;
}

std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (const char ch : in) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", ch & 0xff);
                    out += buf;
                } else {
                    out += ch;
                }
        }
    }
    return out;
}

}  // namespace zcd