// =============================================================================
// src/core/base64.cpp — base64 encode/decode
// =============================================================================

#include "base64.h"
#include <array>

namespace funes {

namespace {
constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::array<int, 256> build_decode_table() {
    std::array<int, 256> t;
    t.fill(-1);
    for (int i = 0; i < 64; ++i) t[static_cast<unsigned char>(kTable[i])] = i;
    return t;
}
} // namespace

std::string base64_encode(const std::string& data) {
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);

    size_t i = 0;
    while (i + 2 < data.size()) {
        const unsigned n = (static_cast<unsigned char>(data[i]) << 16)
                          | (static_cast<unsigned char>(data[i + 1]) << 8)
                          |  static_cast<unsigned char>(data[i + 2]);
        out += kTable[(n >> 18) & 0x3F];
        out += kTable[(n >> 12) & 0x3F];
        out += kTable[(n >> 6)  & 0x3F];
        out += kTable[n & 0x3F];
        i += 3;
    }

    const size_t remaining = data.size() - i;
    if (remaining == 1) {
        const unsigned n = static_cast<unsigned char>(data[i]) << 16;
        out += kTable[(n >> 18) & 0x3F];
        out += kTable[(n >> 12) & 0x3F];
        out += "==";
    } else if (remaining == 2) {
        const unsigned n = (static_cast<unsigned char>(data[i]) << 16)
                          | (static_cast<unsigned char>(data[i + 1]) << 8);
        out += kTable[(n >> 18) & 0x3F];
        out += kTable[(n >> 12) & 0x3F];
        out += kTable[(n >> 6)  & 0x3F];
        out += '=';
    }
    return out;
}

bool base64_decode(const std::string& data, std::string& out) {
    static const std::array<int, 256> decode_table = build_decode_table();

    std::string clean;
    clean.reserve(data.size());
    for (char c : data) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        clean += c;
    }
    if (clean.size() % 4 == 1) return false;  // impossible length

    out.clear();
    out.reserve((clean.size() / 4) * 3);

    unsigned buffer = 0;
    int bits = 0;
    for (unsigned char c : clean) {
        const int v = decode_table[c];
        if (v < 0) return false;
        buffer = (buffer << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buffer >> bits) & 0xFF);
        }
    }
    return true;
}

} // namespace funes
