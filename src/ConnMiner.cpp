#include "ConnMiner.hpp"

#include "ConnRpc.hpp"
#include "Hashing/RandomXV2.hpp"
#include "Log.hpp"

#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace {

// ------------------------------------------------------------
// Helper hex / hash / serialisasi Bitcoin-style
// ------------------------------------------------------------
int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool hexToBytes(const std::string& hex, std::vector<uint8_t>& out) {
    if (hex.size() % 2 != 0) return false;
    out.clear();
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nibble(hex[i]);
        const int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

std::string toHex(const uint8_t* p, size_t n) {
    static const char hx[] = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) {
        s += hx[p[i] >> 4];
        s += hx[p[i] & 0xf];
    }
    return s;
}

void sha256d(const uint8_t* data, size_t len, uint8_t out[32]) {
    uint8_t mid[32];
    SHA256(data, len, mid);
    SHA256(mid, sizeof(mid), out);
}

void putVarint(std::vector<uint8_t>& v, uint64_t n) {
    if (n < 0xfd) {
        v.push_back(static_cast<uint8_t>(n));
    } else if (n <= 0xffff) {
        v.push_back(0xfd);
        v.push_back(static_cast<uint8_t>(n));
        v.push_back(static_cast<uint8_t>(n >> 8));
    } else if (n <= 0xffffffffULL) {
        v.push_back(0xfe);
        for (int i = 0; i < 4; i++) v.push_back(static_cast<uint8_t>(n >> (8 * i)));
    } else {
        v.push_back(0xff);
        for (int i = 0; i < 8; i++) v.push_back(static_cast<uint8_t>(n >> (8 * i)));
    }
}

void putPush(std::vector<uint8_t>& v, const std::vector<uint8_t>& data) {
    const size_t n = data.size();
    if (n < 0x4c) {
        v.push_back(static_cast<uint8_t>(n));
    } else if (n <= 0xff) {
        v.push_back(0x4c);
        v.push_back(static_cast<uint8_t>(n));
    } else {
        v.push_back(0x4d);
        v.push_back(static_cast<uint8_t>(n));
        v.push_back(static_cast<uint8_t>(n >> 8));
    }
    v.insert(v.end(), data.begin(), data.end());
}

// CScript << nHeight: angka minimal little-endian (BIP34).
void putScriptNum(std::vector<uint8_t>& v, int64_t n) {
    if (n == 0) {
        v.push_back(0x00);
        return;
    }
    const bool neg = n < 0;
    uint64_t abs = neg ? static_cast<uint64_t>(-(n + 1)) + 1 : static_cast<uint64_t>(n);
    std::vector<uint8_t> b;
    while (abs) {
        b.push_back(static_cast<uint8_t>(abs & 0xff));
        abs >>= 8;
    }
    if (b.back() & 0x80)
        b.push_back(neg ? 0x80 : 0x00);
    else if (neg)
        b.back() |= 0x80;
    v.push_back(static_cast<uint8_t>(b.size()));
    v.insert(v.end(), b.begin(), b.end());
}

void merkleRoot(std::vector<std::array<uint8_t, 32>>& leaves, uint8_t out[32]) {
    if (leaves.empty()) {
        memset(out, 0, 32);
        return;
    }
    while (leaves.size() > 1) {
        if (leaves.size() % 2 != 0) leaves.push_back(leaves.back());
        std::vector<std::array<uint8_t, 32>> next;
        next.reserve(leaves.size() / 2);
        for (size_t i = 0; i < leaves.size(); i += 2) {
            uint8_t buf[64];
            memcpy(buf, leaves[i].data(), 32);
            memcpy(buf + 32, leaves[i + 1].data(), 32);
            std::array<uint8_t, 32> h{};
            sha256d(buf, sizeof(buf), h.data());
            next.push_back(h);
        }
        leaves.swap(next);
    }
    memcpy(out, leaves[0].data(), 32);
}

// Compact "bits" -> target 256-bit little-endian.
bool compactToTarget(uint32_t bits, uint8_t targetLE[32]) {
    memset(targetLE, 0, 32);
    const uint32_t exp = bits >> 24;
    const uint32_t mant = bits & 0x007fffffU;
    if (mant == 0) return false;
    if (exp <= 3) {
        const uint32_t v = mant >> (8 * (3 - exp));
        for (int i = 0; i < 4; i++) targetLE[i] = static_cast<uint8_t>(v >> (8 * i));
        return true;
    }
    const int shift = static_cast<int>(exp) - 3;
    for (int i = 0; i < 3; i++) {
        const int idx = shift + i;
        if (idx >= 0 && idx < 32) targetLE[idx] = static_cast<uint8_t>((mant >> (8 * i)) & 0xff);
    }
    return true;
}

// hash (LE 256-bit) <= target (LE)
bool leqTarget(const uint8_t hash[32], const uint8_t target[32]) {
    for (int i = 31; i >= 0; i--) {
        if (hash[i] < target[i]) return true;
        if (hash[i] > target[i]) return false;
    }
    return true;
}

struct CoinbaseResult {
    std::string hex;    // serialisasi penuh (dengan witness)
    uint8_t txid[32]{}; // sha256d serialisasi tanpa witness
};

// ScriptPubKey type-1: OP_1 <32-byte x-only key> = 5120<key>.
bool parseRewardScript(const std::string& scriptHex, uint8_t key[32], std::string& err) {
    if (scriptHex.size() != 68 || scriptHex.compare(0, 4, "5120") != 0) {
        err = "scriptPubKey bukan P2TR/type-1 (5120<key>): " + scriptHex.substr(0, 20);
        return false;
    }
    std::vector<uint8_t> raw;
    if (!hexToBytes(scriptHex, raw) || raw.size() != 34) {
        err = "scriptPubKey tidak valid";
        return false;
    }
    memcpy(key, raw.data() + 2, 32);
    return true;
}

}  // namespace

// ============================================================
// Impl
// ============================================================
struct ConnMiner::Impl {
    ConnMinerConfig cfg;
    std::unique_ptr<ConnRpc> rpc;
    uint8_t rewardKey[32]{};

    struct Engine {
        std::shared_ptr<rxv2::Context> ctx;
        std::vector<std::unique_ptr<rxv2::Context::VM>> vms;
    };

    struct Work {
        uint64_t gen = 0;
        int height = 0;
        uint8_t header[80]{};
        std::string coinbaseHex;
        std::vector<std::string> txData;
        uint8_t targetLE[32]{};
        std::atomic<uint32_t> nonce{0};
    };

    struct Snapshot {
        std::shared_ptr<Engine> engine;
        std::shared_ptr<Work> work;
    };

    std::mutex mu;
    Snapshot snap;
    uint64_t genCounter = 0;
    std::string curKey;
    std::string curPrev;
    int curHeight = -1;

    std::atomic<bool> run{false};
    std::vector<std::thread> workers;
    std::thread templateThread;

    std::atomic<uint64_t> hashes{0};
    std::atomic<uint64_t> found{0};
    std::atomic<uint64_t> accepted{0};
    std::atomic<uint64_t> rejected{0};
    std::atomic<uint64_t> stale{0};
    std::mutex submitMu;

    bool buildEngine(const std::string& keyHex) {
        std::vector<uint8_t> key;
        if (!hexToBytes(keyHex, key) || key.size() != 32) {
            lg::error("conn", "randomxkey bukan 32 byte");
            return false;
        }
        std::shared_ptr<rxv2::Context> ctx;
        if (cfg.fastMem) {
            lg::info("conn", "RandomX V2 dataset FAST (~2 GiB) init dengan " +
                                 std::to_string(cfg.threads) + " thread...");
            ctx = std::make_shared<rxv2::Context>(key.data(), true, cfg.largePages, cfg.threads);
            if (!ctx->ok()) {
                lg::warn("conn", "FAST gagal (" + ctx->error() + "), fallback LIGHT");
                ctx = std::make_shared<rxv2::Context>(key.data(), false, false, 1);
            }
        } else {
            ctx = std::make_shared<rxv2::Context>(key.data(), false, cfg.largePages, 1);
        }
        if (!ctx->ok()) {
            lg::error("conn", "RandomX V2 context gagal: " + ctx->error());
            return false;
        }
        lg::info("conn", std::string("RandomX V2 siap (") + (ctx->fast() ? "FAST" : "LIGHT") +
                             (ctx->usesLargePages() ? ", hugepages" : ", regular pages") + ")");

        auto engine = std::make_shared<Engine>();
        engine->ctx = ctx;
        engine->vms.reserve(static_cast<size_t>(cfg.threads));
        for (int i = 0; i < cfg.threads; i++) {
            auto vm = ctx->createVM();
            if (!vm || !vm->ok()) {
                lg::error("conn", "gagal membuat VM RandomX untuk worker " + std::to_string(i));
                return false;
            }
            engine->vms.push_back(std::move(vm));
        }
        {
            std::lock_guard<std::mutex> lock(mu);
            snap.engine = engine;
            curKey = keyHex;
        }
        return true;
    }

    bool buildWork(const ConnTemplate& t) {
        // --- target ---
        uint8_t targetLE[32]{};
        std::vector<uint8_t> targetBE;
        if (hexToBytes(t.target, targetBE) && targetBE.size() == 32) {
            for (int i = 0; i < 32; i++) targetLE[i] = targetBE[31 - static_cast<size_t>(i)];
        } else {
            uint32_t bits = 0;
            for (char c : t.bits) bits = (bits << 4) | static_cast<uint32_t>(nibble(c));
            if (!compactToTarget(bits, targetLE)) {
                lg::error("conn", "target/bits template tidak valid");
                return false;
            }
        }

        // --- coinbase scriptSig: height + flags + witness commitment ---
        std::vector<uint8_t> scriptSig;
        putScriptNum(scriptSig, t.height);
        if (auto it = t.coinbaseaux.find("flags"); it != t.coinbaseaux.end()) {
            std::vector<uint8_t> flags;
            if (hexToBytes(it->second, flags) && !flags.empty()) putPush(scriptSig, flags);
        }

        // Witness merkle root: leaf coinbase = 0x00*32, lalu wtxid tiap tx.
        // Commitment = SHA256d(root || 32-byte reserved value 0).
        {
            std::vector<std::array<uint8_t, 32>> wleaves;
            std::array<uint8_t, 32> zero{};
            wleaves.push_back(zero);
            for (const auto& tx : t.txs) {
                std::vector<uint8_t> w;
                if (!hexToBytes(tx.wtxid, w) || w.size() != 32) {
                    lg::error("conn", "wtxid template tidak valid");
                    return false;
                }
                std::array<uint8_t, 32> leaf{};
                for (int i = 0; i < 32; i++) leaf[i] = w[31 - static_cast<size_t>(i)];
                wleaves.push_back(leaf);
            }
            uint8_t wroot[32];
            merkleRoot(wleaves, wroot);
            uint8_t buf[64];
            memcpy(buf, wroot, 32);
            memset(buf + 32, 0, 32);
            uint8_t commitment[32];
            sha256d(buf, sizeof(buf), commitment);

            // Canonical 36-byte push: aa21a9ed || commitment, dibungkus push 0x24.
            std::vector<uint8_t> commit;
            commit.push_back(0xaa);
            commit.push_back(0x21);
            commit.push_back(0xa9);
            commit.push_back(0xed);
            commit.insert(commit.end(), commitment, commitment + 32);
            putPush(scriptSig, commit);
        }

        // --- coinbase (output typed type-1) ---
        std::vector<uint8_t> coinbaseNoWit;
        std::vector<uint8_t> coinbaseWit;
        auto putFixed = [](std::vector<uint8_t>& v, uint32_t x, int n) {
            for (int i = 0; i < n; i++) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
        };
        auto putI64 = [](std::vector<uint8_t>& v, uint64_t x) {
            for (int i = 0; i < 8; i++) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
        };
        auto putOutput = [&](std::vector<uint8_t>& v) {
            putI64(v, t.coinbasevalue);
            v.push_back(0x01);  // type-1: P2PK
            v.insert(v.end(), rewardKey, rewardKey + 32);
        };

        // versi + input
        putFixed(coinbaseNoWit, 1, 4);       // version
        putVarint(coinbaseNoWit, 1);         // vin count
        coinbaseNoWit.insert(coinbaseNoWit.end(), 32, 0x00);  // prevout hash
        putFixed(coinbaseNoWit, 0xffffffff, 4);               // prevout index
        putVarint(coinbaseNoWit, scriptSig.size());
        coinbaseNoWit.insert(coinbaseNoWit.end(), scriptSig.begin(), scriptSig.end());
        putFixed(coinbaseNoWit, 0xffffffff, 4);  // sequence
        putVarint(coinbaseNoWit, 1);             // vout count
        putOutput(coinbaseNoWit);
        putFixed(coinbaseNoWit, 0, 4);  // locktime

        // versi + marker/flag + body yang sama + witness + locktime
        putFixed(coinbaseWit, 1, 4);
        coinbaseWit.push_back(0x00);
        coinbaseWit.push_back(0x01);
        putVarint(coinbaseWit, 1);
        coinbaseWit.insert(coinbaseWit.end(), 32, 0x00);
        putFixed(coinbaseWit, 0xffffffff, 4);
        putVarint(coinbaseWit, scriptSig.size());
        coinbaseWit.insert(coinbaseWit.end(), scriptSig.begin(), scriptSig.end());
        putFixed(coinbaseWit, 0xffffffff, 4);
        putVarint(coinbaseWit, 1);
        putOutput(coinbaseWit);
        // witness: satu item 32-byte reserved value 0
        putVarint(coinbaseWit, 1);
        putVarint(coinbaseWit, 32);
        coinbaseWit.insert(coinbaseWit.end(), 32, 0x00);
        putFixed(coinbaseWit, 0, 4);

        CoinbaseResult cb;
        cb.hex = toHex(coinbaseWit.data(), coinbaseWit.size());
        sha256d(coinbaseNoWit.data(), coinbaseNoWit.size(), cb.txid);

        // --- merkle root ---
        std::vector<std::array<uint8_t, 32>> leaves;
        std::array<uint8_t, 32> cbLeaf{};
        memcpy(cbLeaf.data(), cb.txid, 32);
        leaves.push_back(cbLeaf);
        for (const auto& tx : t.txs) {
            std::vector<uint8_t> id;
            if (!hexToBytes(tx.txid, id) || id.size() != 32) {
                lg::error("conn", "txid template tidak valid");
                return false;
            }
            std::array<uint8_t, 32> leaf{};
            for (int i = 0; i < 32; i++) leaf[i] = id[31 - static_cast<size_t>(i)];
            leaves.push_back(leaf);
        }
        uint8_t root[32];
        merkleRoot(leaves, root);

        // --- header 80-byte ---
        auto work = std::make_shared<Work>();
        work->height = t.height;
        memcpy(work->targetLE, targetLE, 32);
        {
            const uint32_t version = static_cast<uint32_t>(t.version);
            uint8_t* h = work->header;
            for (int i = 0; i < 4; i++) h[i] = static_cast<uint8_t>(version >> (8 * i));
            std::vector<uint8_t> prev;
            if (!hexToBytes(t.prevhash, prev) || prev.size() != 32) {
                lg::error("conn", "prevhash template tidak valid");
                return false;
            }
            for (int i = 0; i < 32; i++) h[4 + i] = prev[31 - static_cast<size_t>(i)];
            memcpy(h + 36, root, 32);
            for (int i = 0; i < 4; i++) h[68 + i] = static_cast<uint8_t>(t.curtime >> (8 * i));
            uint32_t bits = 0;
            for (char c : t.bits) bits = (bits << 4) | static_cast<uint32_t>(nibble(c));
            for (int i = 0; i < 4; i++) h[72 + i] = static_cast<uint8_t>(bits >> (8 * i));
            for (int i = 0; i < 4; i++) h[76 + i] = 0;
        }
        work->coinbaseHex = cb.hex;
        work->txData.reserve(t.txs.size());
        for (const auto& tx : t.txs) work->txData.push_back(tx.data);

        {
            std::lock_guard<std::mutex> lock(mu);
            work->gen = ++genCounter;
            snap.work = work;
            curPrev = t.prevhash;
            curHeight = t.height;
        }
        lg::info("conn", "New job height " + std::to_string(t.height) + " txs " +
                             std::to_string(t.txs.size()) + " reward " +
                             std::to_string(t.coinbasevalue) + " connects");
        return true;
    }

    void submit(const std::shared_ptr<Work>& w, const uint8_t header[80], uint32_t nonce) {
        found.fetch_add(1);
        std::vector<uint8_t> block;
        block.insert(block.end(), header, header + 80);
        putVarint(block, 1 + w->txData.size());
        std::vector<uint8_t> cb;
        if (!hexToBytes(w->coinbaseHex, cb)) return;
        block.insert(block.end(), cb.begin(), cb.end());
        for (const auto& tx : w->txData) {
            std::vector<uint8_t> raw;
            if (!hexToBytes(tx, raw)) continue;
            block.insert(block.end(), raw.begin(), raw.end());
        }
        const std::string blockHex = toHex(block.data(), block.size());

        std::lock_guard<std::mutex> lock(submitMu);
        std::string reason, err;
        bool acc = false;
        if (!rpc->submitBlock(blockHex, reason, acc, err)) {
            rejected.fetch_add(1);
            lg::warn("conn", "submitblock gagal: " + err);
            return;
        }
        if (acc) {
            accepted.fetch_add(1);
            lg::good("conn", "BLOK DITERIMA height=" + std::to_string(w->height) + " nonce=" +
                                 std::to_string(nonce) + " (" + blockHex.substr(0, 16) + "...)");
        } else if (reason == "duplicate" || reason == "duplicate-invalid") {
            stale.fetch_add(1);
            lg::debug("conn", "Blok duplikat (sudah ada miner lain): " + reason);
        } else {
            rejected.fetch_add(1);
            lg::warn("conn", "Blok ditolak node: " + reason);
        }
    }

    void workerLoop(int id) {
        std::shared_ptr<Engine> engine;
        std::shared_ptr<Work> work;
        uint64_t seenGen = 0;
        while (run.load()) {
            {
                std::lock_guard<std::mutex> lock(mu);
                engine = snap.engine;
                work = snap.work;
            }
            if (!engine || !work) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            if (static_cast<size_t>(id) >= engine->vms.size()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            auto& vm = engine->vms[static_cast<size_t>(id)];
            if (!vm || !vm->ok()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            seenGen = work->gen;
            uint64_t local = 0;
            for (int i = 0; i < 4096 && run.load(); i++) {
                uint8_t header[80];
                memcpy(header, work->header, sizeof(header));
                const uint32_t nonce = work->nonce.fetch_add(1, std::memory_order_relaxed);
                for (int k = 0; k < 4; k++) header[76 + k] = static_cast<uint8_t>(nonce >> (8 * k));
                uint8_t out[32];
                vm->hash(header, sizeof(header), out);
                local++;
                if (leqTarget(out, work->targetLE)) {
                    submit(work, header, nonce);
                    // Template mungkin sudah berganti; keluar dari batch.
                    break;
                }
                if ((i & 255) == 0) {
                    uint64_t g;
                    {
                        std::lock_guard<std::mutex> lock(mu);
                        g = snap.work ? snap.work->gen : 0;
                    }
                    if (g != seenGen) break;
                }
            }
            hashes.fetch_add(local);
        }
    }

    void templateLoop() {
        auto lastLog = std::chrono::steady_clock::now();
        uint64_t lastHashes = 0;
        int gbtFails = 0;
        while (run.load()) {
            ConnTemplate t;
            std::string err;
            if (!rpc->getBlockTemplate(t, err)) {
                // Node IBD menolak GBT; jangan banjiri log tiap 2 detik.
                if ((gbtFails++ % 15) == 0)
                    lg::warn("conn", "getblocktemplate gagal (" + std::to_string(gbtFails) +
                                         "x): " + err);
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }
            gbtFails = 0;
            const bool newKey = (t.randomxkey != curKey);
            const bool newTip = (t.prevhash != curPrev) || (t.height != curHeight);
            if (newKey && !buildEngine(t.randomxkey)) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                continue;
            }
            if (newTip && !buildWork(t)) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }

            const auto now = std::chrono::steady_clock::now();
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - lastLog).count();
            if (elapsed >= 10000) {
                const uint64_t h = hashes.load();
                const double hr = static_cast<double>(h - lastHashes) * 1000.0 /
                                  static_cast<double>(elapsed);
                lastHashes = h;
                lastLog = now;
                int height = 0, txs = 0;
                {
                    std::lock_guard<std::mutex> lock(mu);
                    height = curHeight;
                }
                lg::info("conn", "HR " + std::to_string(static_cast<uint64_t>(hr)) + " H/s | A " +
                                     std::to_string(accepted.load()) + " R " +
                                     std::to_string(rejected.load()) + " S " +
                                     std::to_string(stale.load()) + " | height " +
                                     std::to_string(height) + " | found " +
                                     std::to_string(found.load()));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        }
    }
};

// ============================================================
// ConnMiner
// ============================================================
ConnMiner::ConnMiner() = default;

ConnMiner::~ConnMiner() { stop(); }

bool ConnMiner::start(const ConnMinerConfig& cfg) {
    if (m_running.load()) return false;
    if (cfg.address.empty()) {
        lg::error("conn", "alamat reward kosong (pakai -u <alamat> / --address)");
        return false;
    }
    if (cfg.threads < 1) {
        lg::error("conn", "thread harus >= 1");
        return false;
    }

    m_impl = std::make_unique<Impl>();
    m_impl->cfg = cfg;

    std::string user = cfg.rpcUser;
    std::string pass = cfg.rpcPass;
    if (user.empty()) {
        // Cookie auth standar Bitcoin-style: ~/.connectcoin/.cookie berisi
        // "user:pass". --conn-user/--conn-pass menimpa ini.
        std::string p = cfg.cookie;
        if (!p.empty() && p[0] == '~') {
            const char* home = getenv("HOME");
            if (home) p = std::string(home) + p.substr(1);
        }
        FILE* f = std::fopen(p.c_str(), "r");
        if (!f) {
            lg::error("conn", "tidak bisa membaca cookie " + p +
                                  " (isi --conn-user/--conn-pass atau --conn-cookie)");
            return false;
        }
        char line[512] = {0};
        const bool got = std::fgets(line, sizeof(line), f) != nullptr;
        std::fclose(f);
        if (!got) {
            lg::error("conn", "cookie kosong: " + p);
            return false;
        }
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        const auto colon = s.find(':');
        if (colon == std::string::npos) {
            lg::error("conn", "format cookie salah (butuh user:pass): " + p);
            return false;
        }
        user = s.substr(0, colon);
        pass = s.substr(colon + 1);
    }

    m_impl->rpc = std::make_unique<ConnRpc>(cfg.rpcUrl, user, pass);

    std::string scriptHex, err;
    if (!m_impl->rpc->getAddressScript(cfg.address, scriptHex, err)) {
        lg::error("conn", "alamat reward ditolak node: " + err);
        return false;
    }
    if (!parseRewardScript(scriptHex, m_impl->rewardKey, err)) {
        lg::error("conn", "alamat reward: " + err);
        return false;
    }
    lg::info("conn", "Reward address " + cfg.address + " -> key " +
                         toHex(m_impl->rewardKey, 32).substr(0, 16) + "...");

    std::string chain;
    int blocks = 0, headers = 0;
    if (m_impl->rpc->getBlockchainInfo(chain, blocks, headers, err)) {
        lg::info("conn", "Node chain=" + chain + " blocks=" + std::to_string(blocks) +
                             " headers=" + std::to_string(headers));
        if (blocks < headers)
            lg::warn("conn", "Node masih sinkronisasi: blok yang ditambang bisa orphan sampai "
                             "node menyusul tip jaringan");
    } else {
        lg::warn("conn", "getblockchaininfo gagal: " + err);
    }

    // Node yang masih initial block download menolak getblocktemplate
    // ("ConnectCoin Core is in initial sync"). Jangan matikan miner: template
    // thread akan terus mencoba dan mulai hashing begitu node siap. Pool
    // stratum konvensional tidak punya keadaan ini, jadi perilakunya dibuat
    // eksplisit di log.
    ConnTemplate first;
    if (m_impl->rpc->getBlockTemplate(first, err)) {
        if (!m_impl->buildEngine(first.randomxkey)) return false;
        if (!m_impl->buildWork(first)) return false;
    } else {
        lg::warn("conn", "Belum bisa ambil template (" + err + ") — menunggu node selesai sync");
    }

    m_running.store(true);
    m_impl->run.store(true);
    for (int i = 0; i < cfg.threads; i++)
        m_impl->workers.emplace_back([this, i]() { m_impl->workerLoop(i); });
    m_impl->templateThread = std::thread([this]() { m_impl->templateLoop(); });
    lg::info("conn", "Mining solo CONN dengan " + std::to_string(cfg.threads) + " thread");
    return true;
}

void ConnMiner::stop() {
    if (!m_impl) return;
    m_impl->run.store(false);
    m_running.store(false);
    for (auto& t : m_impl->workers)
        if (t.joinable()) t.join();
    m_impl->workers.clear();
    if (m_impl->templateThread.joinable()) m_impl->templateThread.join();
}

std::string ConnMiner::finalSummary() {
    if (!m_impl) return "CONN miner tidak berjalan";
    return "CONN: hashes " + std::to_string(m_impl->hashes.load()) + ", found " +
           std::to_string(m_impl->found.load()) + ", accepted " +
           std::to_string(m_impl->accepted.load()) + ", rejected " +
           std::to_string(m_impl->rejected.load()) + ", stale " +
           std::to_string(m_impl->stale.load());
}
