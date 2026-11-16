#pragma once

#include <string>
#include <utility>
#include <vector>

// Parser JSON minimal untuk dialek stratum Zycord.
//
// Ditulis khusus di sini, bukan memakai ConnRpc.hpp, karena ConnRpc adalah
// modul backend CONN: mengikutkannya akan menyeret state RPC/template blok
// CONN ke dalam biner zcd-miner dan menghapus kemandirian backend. Yang
// dibutuhkan stratum hanya objek, array, string, angka, bool, null.
namespace zcd {

class Json {
public:
    enum class T { Null, Bool, Num, Str, Arr, Obj };

    T t = T::Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    bool isObj() const { return t == T::Obj; }
    bool isArr() const { return t == T::Arr; }

    const Json* get(const std::string& key) const;
    bool getStr(const std::string& key, std::string& out) const;
    bool getNum(const std::string& key, double& out) const;
    bool getBool(const std::string& key, bool& out) const;
};

// Parse satu dokumen. Mengembalikan Null dan mengisi err bila gagal.
Json parseJson(const std::string& text, std::string* err = nullptr);

// Escape isi string JSON (tanpa tanda kutip pembungkus).
std::string jsonEscape(const std::string& in);

}  // namespace zcd
