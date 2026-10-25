#include "Yespower.hpp"

#include <cstring>

namespace ypw {

Hasher::~Hasher() {
    if (m_inited) {
        yespower_free_local(&m_local);
        m_inited = false;
    }
}

bool Hasher::init(const Params& p) {
    if (m_inited) {
        yespower_free_local(&m_local);
        m_inited = false;
    }
    m_params.version = YESPOWER_1_0;
    m_params.N = p.N;
    m_params.r = p.R;
    m_params.pers = nullptr;
    m_params.perslen = 0;
    if (yespower_init_local(&m_local)) return false;
    m_inited = true;
    return true;
}

bool Hasher::hash(const uint8_t* src, size_t srclen, uint8_t dst32[32]) {
    yespower_binary_t out;
    if (yespower(&m_local, src, srclen, &m_params, &out)) return false;
    memcpy(dst32, out.uc, 32);
    return true;
}

bool selftest() {
    // Vektor dari upstream TESTS-OK:
    //   yespower(10, 2048, 32, NULL) =
    //   d5 ef b8 13 cd 26 3e 9b 34 54 01 30 23 3c bb c6
    //   a9 21 fb ff 34 31 e5 ec 1a 1a bd e2 ae a6 ff 4d
    static const uint8_t kExpect[32] = {
        0xd5, 0xef, 0xb8, 0x13, 0xcd, 0x26, 0x3e, 0x9b,
        0x34, 0x54, 0x01, 0x30, 0x23, 0x3c, 0xbb, 0xc6,
        0xa9, 0x21, 0xfb, 0xff, 0x34, 0x31, 0xe5, 0xec,
        0x1a, 0x1a, 0xbd, 0xe2, 0xae, 0xa6, 0xff, 0x4d};

    Hasher h;
    if (!h.init(Params{2048, 32})) return false;

    uint8_t src[80];
    for (size_t i = 0; i < sizeof(src); i++) src[i] = static_cast<uint8_t>(i * 3);

    uint8_t out[32];
    if (!h.hash(src, sizeof(src), out)) return false;
    return memcmp(out, kExpect, sizeof(out)) == 0;
}

}  // namespace ypw
