#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

// Klien JSON-RPC minimal untuk connectcoind (ConnectCoin Core).
//
// Dipakai jalur mining SOLO: tidak ada pool/stratum untuk CONN, jadi miner
// memanggil getblocktemplate / submitblock langsung ke node lokal (atau node
// jarak jauh lewat SSH tunnel). Tidak ada dependensi HTTP baru: satu socket
// TCP + Basic auth sudah cukup untuk RPC Bitcoin-style.
class Json {
public:
    enum class T { Null, Bool, Num, Str, Arr, Obj };

    T t = T::Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    bool isNull() const { return t == T::Null; }
    bool isObj() const { return t == T::Obj; }
    bool isArr() const { return t == T::Arr; }
    const Json* get(const std::string& key) const;
    bool getStr(const std::string& key, std::string& out) const;
    bool getNum(const std::string& key, double& out) const;
    bool getBool(const std::string& key, bool& out) const;
};

// Parse satu dokumen JSON. Mengembalikan Null dan mengisi err bila gagal.
Json parseJson(const std::string& text, std::string* err = nullptr);

struct ConnTemplateTx {
    std::string txid;  // hex, urutan tampilan (BE) seperti dikirim node
    std::string wtxid; // hex, urutan tampilan (BE)
    std::string data;  // transaksi terserialisasi penuh (hex)
};

struct ConnTemplate {
    int version = 0;
    int height = 0;
    uint32_t curtime = 0;
    std::string prevhash;   // hex BE
    std::string bits;       // hex 8 char
    std::string target;     // hex 64 char (BE)
    std::string randomxkey; // hex 64 char (32 byte key epoch)
    uint64_t coinbasevalue = 0;
    std::map<std::string, std::string> coinbaseaux;  // key -> hex
    std::vector<ConnTemplateTx> txs;
    std::string longpollid;
};

class ConnRpc {
public:
    ConnRpc(std::string url, std::string user, std::string pass);

    // Baca user:pass dari file cookie (mis. ~/.connectcoin/.cookie).
    // Mengembalikan false (dengan err) bila file tidak bisa dibaca.
    bool loadCookie(const std::string& path, std::string& err);

    bool getBlockTemplate(ConnTemplate& out, std::string& err);
    // accepted=true bila node menerima blok (result null). reason berisi
    // alasan penolakan node (mis. "high-hash", "duplicate") bila tidak.
    bool submitBlock(const std::string& blockHex, std::string& reason, bool& accepted,
                     std::string& err);
    // Ambil scriptPubKey dari sebuah alamat (5120<32-byte key> untuk P2TR /
    // output type-1 ConnectCoin).
    bool getAddressScript(const std::string& address, std::string& scriptHex, std::string& err);
    bool getBlockchainInfo(std::string& chain, int& blocks, int& headers, std::string& err);

private:
    bool call(const std::string& method, const std::string& paramsJson, Json& result, Json& error,
              std::string& err);
    bool httpPost(const std::string& body, std::string& response, std::string& err);

    std::string m_host = "127.0.0.1";
    int m_port = 48172;
    std::string m_path = "/";
    std::string m_user;
    std::string m_pass;
    uint64_t m_id = 1;
};
