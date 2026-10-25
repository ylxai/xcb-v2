#include "ConnRpc.hpp"

#include "Log.hpp"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <fstream>

// ============================================================
// JSON parser minimal (objek/array/string/angka/bool/null).
// Cukup untuk getblocktemplate; tidak ada dependensi eksternal.
// ============================================================
namespace {

struct JParser {
    const std::string& s;
    size_t i = 0;
    std::string err;

    explicit JParser(const std::string& text) : s(text) {}

    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
    }

    bool fail(const std::string& msg) {
        if (err.empty()) err = msg + " @" + std::to_string(i);
        return false;
    }

    static void appendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool parseString(std::string& out) {
        if (i >= s.size() || s[i] != '"') return fail("expected string");
        i++;
        out.clear();
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) return fail("bad escape");
            char e = s[i++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    if (i + 4 > s.size()) return fail("short \\u");
                    unsigned cp = 0;
                    for (int k = 0; k < 4; k++) {
                        char h = s[i++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                        else return fail("bad \\u hex");
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return fail("unknown escape");
            }
        }
        return fail("unterminated string");
    }

    bool parseNumber(Json& out) {
        size_t start = i;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) i++;
        while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' ||
                               s[i] == 'E' || s[i] == '-' || s[i] == '+'))
            i++;
        if (start == i) return fail("bad number");
        out.t = Json::T::Num;
        out.num = std::strtod(s.substr(start, i - start).c_str(), nullptr);
        return true;
    }

    bool parseValue(Json& out, int depth) {
        if (depth > 64) return fail("nesting too deep");
        ws();
        if (i >= s.size()) return fail("unexpected end");
        char c = s[i];
        if (c == '{') {
            i++;
            out.t = Json::T::Obj;
            ws();
            if (i < s.size() && s[i] == '}') { i++; return true; }
            while (true) {
                ws();
                std::string key;
                if (!parseString(key)) return false;
                ws();
                if (i >= s.size() || s[i] != ':') return fail("expected ':'");
                i++;
                Json v;
                if (!parseValue(v, depth + 1)) return false;
                out.obj.emplace_back(std::move(key), std::move(v));
                ws();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == '}') { i++; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            i++;
            out.t = Json::T::Arr;
            ws();
            if (i < s.size() && s[i] == ']') { i++; return true; }
            while (true) {
                Json v;
                if (!parseValue(v, depth + 1)) return false;
                out.arr.push_back(std::move(v));
                ws();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == ']') { i++; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            out.t = Json::T::Str;
            return parseString(out.str);
        }
        if (s.compare(i, 4, "true") == 0) { i += 4; out.t = Json::T::Bool; out.b = true; return true; }
        if (s.compare(i, 5, "false") == 0) { i += 5; out.t = Json::T::Bool; out.b = false; return true; }
        if (s.compare(i, 4, "null") == 0) { i += 4; out.t = Json::T::Null; return true; }
        return parseNumber(out);
    }
};

// ------------------------------------------------------------
// Helper socket / HTTP
// ------------------------------------------------------------
std::string base64(const std::string& in) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    while (i + 2 < in.size()) {
        unsigned v = (static_cast<unsigned char>(in[i]) << 16) |
                     (static_cast<unsigned char>(in[i + 1]) << 8) |
                     static_cast<unsigned char>(in[i + 2]);
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += tbl[v & 63];
        i += 3;
    }
    if (i + 1 == in.size()) {
        unsigned v = static_cast<unsigned char>(in[i]) << 16;
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == in.size()) {
        unsigned v = (static_cast<unsigned char>(in[i]) << 16) |
                     (static_cast<unsigned char>(in[i + 1]) << 8);
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

std::string expandHome(const std::string& path) {
    if (path.empty() || path[0] != '~') return path;
    const char* home = getenv("HOME");
    if (!home) return path;
    return std::string(home) + path.substr(1);
}

// Decode Transfer-Encoding: chunked.
bool dechunk(const std::string& body, std::string& out) {
    out.clear();
    size_t i = 0;
    while (i < body.size()) {
        size_t eol = body.find("\r\n", i);
        if (eol == std::string::npos) return false;
        const std::string lenHex = body.substr(i, eol - i);
        char* endp = nullptr;
        const unsigned long len = std::strtoul(lenHex.c_str(), &endp, 16);
        if (endp == lenHex.c_str()) return false;
        i = eol + 2;
        if (len == 0) return true;
        if (i + len > body.size()) return false;
        out.append(body, i, len);
        i += len;
        if (body.compare(i, 2, "\r\n") != 0) return false;
        i += 2;
    }
    return true;
}

}  // namespace

// ------------------------------------------------------------
// Json accessors
// ------------------------------------------------------------
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
    JParser p(text);
    Json out;
    if (!p.parseValue(out, 0)) {
        if (err) *err = p.err.empty() ? "parse error" : p.err;
        return Json{};
    }
    p.ws();
    if (p.i != text.size()) {
        if (err) *err = "trailing data @" + std::to_string(p.i);
        return Json{};
    }
    return out;
}

// ------------------------------------------------------------
// ConnRpc
// ------------------------------------------------------------
ConnRpc::ConnRpc(std::string url, std::string user, std::string pass)
    : m_user(std::move(user)), m_pass(std::move(pass)) {
    // http://host:port/path
    std::string s = url;
    const auto scheme = s.find("://");
    if (scheme != std::string::npos) s = s.substr(scheme + 3);
    const auto slash = s.find('/');
    if (slash != std::string::npos) {
        m_path = s.substr(slash);
        s = s.substr(0, slash);
    }
    const auto colon = s.rfind(':');
    if (colon != std::string::npos) {
        m_host = s.substr(0, colon);
        const long p = std::strtol(s.substr(colon + 1).c_str(), nullptr, 10);
        if (p > 0 && p <= 65535) m_port = static_cast<int>(p);
    } else {
        m_host = s;
    }
}

bool ConnRpc::loadCookie(const std::string& path, std::string& err) {
    const std::string p = expandHome(path);
    std::ifstream f(p);
    if (!f.is_open()) {
        err = "tidak bisa membuka cookie " + p;
        return false;
    }
    std::string line;
    std::getline(f, line);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    const auto colon = line.find(':');
    if (colon == std::string::npos) {
        err = "format cookie salah (butuh user:pass): " + p;
        return false;
    }
    m_user = line.substr(0, colon);
    m_pass = line.substr(colon + 1);
    return true;
}

bool ConnRpc::httpPost(const std::string& body, std::string& response, std::string& err) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    const std::string port = std::to_string(m_port);
    if (::getaddrinfo(m_host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
        err = "DNS/gagal resolve " + m_host + ":" + port;
        if (res) ::freeaddrinfo(res);
        return false;
    }
    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        ::freeaddrinfo(res);
        err = "socket() gagal";
        return false;
    }

    struct timeval tv {};
    tv.tv_sec = 30;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (::connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        err = "connect ke " + m_host + ":" + port + " gagal: " + std::strerror(errno);
        ::freeaddrinfo(res);
        ::close(fd);
        return false;
    }
    ::freeaddrinfo(res);

    std::string req;
    req += "POST " + m_path + " HTTP/1.1\r\n";
    req += "Host: " + m_host + ":" + port + "\r\n";
    req += "Authorization: Basic " + base64(m_user + ":" + m_pass) + "\r\n";
    req += "Content-Type: application/json\r\n";
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "Connection: close\r\n\r\n";
    req += body;

    size_t off = 0;
    while (off < req.size()) {
        const ssize_t n = ::send(fd, req.data() + off, req.size() - off, MSG_NOSIGNAL);
        if (n <= 0) {
            err = "send gagal";
            ::close(fd);
            return false;
        }
        off += static_cast<size_t>(n);
    }

    std::string raw;
    char buf[16384];
    while (true) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            raw.append(buf, static_cast<size_t>(n));
            if (raw.size() > (64u << 20)) {
                err = "respons RPC terlalu besar";
                ::close(fd);
                return false;
            }
            continue;
        }
        break;
    }
    ::close(fd);

    const auto sep = raw.find("\r\n\r\n");
    if (sep == std::string::npos) {
        err = "respons HTTP tidak valid";
        return false;
    }
    const std::string head = raw.substr(0, sep);
    std::string payload = raw.substr(sep + 4);
    // status
    if (head.size() < 12) {
        err = "status HTTP tidak valid";
        return false;
    }
    const int status = std::atoi(head.substr(9, 3).c_str());
    const bool chunked = head.find("Transfer-Encoding: chunked") != std::string::npos ||
                         head.find("transfer-encoding: chunked") != std::string::npos;
    if (chunked) {
        std::string decoded;
        if (!dechunk(payload, decoded)) {
            err = "chunked body rusak";
            return false;
        }
        payload.swap(decoded);
    }
    response.swap(payload);
    if (status >= 400 && status != 500) {
        err = "HTTP " + std::to_string(status);
        return false;
    }
    return true;
}

bool ConnRpc::call(const std::string& method, const std::string& paramsJson, Json& result,
                   Json& error, std::string& err) {
    const uint64_t id = m_id++;
    const std::string body = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
                             ",\"method\":\"" + method + "\",\"params\":" + paramsJson + "}";
    std::string resp;
    if (!httpPost(body, resp, err)) return false;

    std::string perr;
    Json doc = parseJson(resp, &perr);
    if (!doc.isObj()) {
        err = "JSON RPC tidak valid: " + perr;
        return false;
    }
    error = Json{};
    if (const Json* e = doc.get("error")) error = *e;
    if (const Json* r = doc.get("result")) result = *r;
    else result = Json{};

    if (!error.isNull()) {
        std::string msg;
        if (!error.getStr("message", msg)) msg = "error RPC";
        err = method + ": " + msg;
        return false;
    }
    return true;
}

bool ConnRpc::getBlockTemplate(ConnTemplate& out, std::string& err) {
    Json result, error;
    if (!call("getblocktemplate", "[{\"rules\":[\"segwit\",\"typedoutputs\"]}]", result, error, err))
        return false;
    if (!result.isObj()) {
        err = "getblocktemplate: result bukan objek";
        return false;
    }

    double d = 0;
    if (!result.getNum("version", d)) {
        err = "template: version hilang";
        return false;
    }
    out.version = static_cast<int>(d);
    if (!result.getNum("height", d)) {
        err = "template: height hilang";
        return false;
    }
    out.height = static_cast<int>(d);
    if (!result.getNum("curtime", d)) {
        err = "template: curtime hilang";
        return false;
    }
    out.curtime = static_cast<uint32_t>(d);
    if (!result.getNum("coinbasevalue", d)) {
        err = "template: coinbasevalue hilang";
        return false;
    }
    out.coinbasevalue = static_cast<uint64_t>(d);

    if (!result.getStr("previousblockhash", out.prevhash) || out.prevhash.size() != 64) {
        err = "template: previousblockhash tidak valid";
        return false;
    }
    if (!result.getStr("bits", out.bits) || out.bits.size() != 8) {
        err = "template: bits tidak valid";
        return false;
    }
    if (!result.getStr("target", out.target) || out.target.size() != 64) {
        err = "template: target tidak valid";
        return false;
    }
    if (!result.getStr("randomxkey", out.randomxkey) || out.randomxkey.size() != 64) {
        err = "template: randomxkey tidak valid";
        return false;
    }
    result.getStr("longpollid", out.longpollid);

    out.coinbaseaux.clear();
    if (const Json* aux = result.get("coinbaseaux"); aux && aux->isObj()) {
        for (const auto& kv : aux->obj)
            if (kv.second.t == Json::T::Str) out.coinbaseaux[kv.first] = kv.second.str;
    }

    out.txs.clear();
    if (const Json* txs = result.get("transactions"); txs && txs->isArr()) {
        for (const auto& tx : txs->arr) {
            ConnTemplateTx t;
            if (!tx.getStr("txid", t.txid)) continue;
            if (!tx.getStr("data", t.data)) continue;
            if (!tx.getStr("hash", t.wtxid)) t.wtxid = t.txid;
            out.txs.push_back(std::move(t));
        }
    }
    return true;
}

bool ConnRpc::submitBlock(const std::string& blockHex, std::string& reason, bool& accepted,
                          std::string& err) {
    Json result, error;
    accepted = false;
    reason.clear();
    if (!call("submitblock", "[\"" + blockHex + "\"]", result, error, err)) return false;
    if (result.isNull()) {
        accepted = true;
        return true;
    }
    if (result.t == Json::T::Str) {
        accepted = false;
        reason = result.str;
        return true;
    }
    accepted = false;
    reason = "respons submitblock tak dikenal";
    return true;
}

bool ConnRpc::getAddressScript(const std::string& address, std::string& scriptHex,
                               std::string& err) {
    Json result, error;
    if (!call("getaddressinfo", "[\"" + address + "\"]", result, error, err)) {
        // Node lama / tanpa wallet: coba validateaddress.
        Json r2, e2;
        std::string err2;
        if (!call("validateaddress", "[\"" + address + "\"]", r2, e2, err2)) return false;
        if (!r2.getStr("scriptPubKey", scriptHex)) {
            err = "alamat tanpa scriptPubKey";
            return false;
        }
        return true;
    }
    if (!result.getStr("scriptPubKey", scriptHex)) {
        err = "getaddressinfo: scriptPubKey hilang";
        return false;
    }
    return true;
}

bool ConnRpc::getBlockchainInfo(std::string& chain, int& blocks, int& headers, std::string& err) {
    Json result, error;
    if (!call("getblockchaininfo", "[]", result, error, err)) return false;
    double d = 0;
    if (result.getStr("chain", chain) && result.getNum("blocks", d)) blocks = static_cast<int>(d);
    if (result.getNum("headers", d)) headers = static_cast<int>(d);
    return true;
}
