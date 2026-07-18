// HPACK implementation — RFC 7541
#include "hpack.hpp"
#include <cstring>
#include <algorithm>
#include <stdexcept>

namespace async_net::http::h2 {

// ============================================================================
// HPACK Huffman Table (RFC 7541 Appendix B) — {code, bit_length} per symbol
// ============================================================================

struct huff_sym { uint32_t code; uint8_t bits; };

#define HUFF_ENTRY(code,bits) {code,bits}

static const huff_sym HUFF[257] = {
    HUFF_ENTRY(0x1ff8,13),HUFF_ENTRY(0x7fffd8,23),HUFF_ENTRY(0xfffffe2,28),HUFF_ENTRY(0xfffffe3,28),
    HUFF_ENTRY(0xfffffe4,28),HUFF_ENTRY(0xfffffe5,28),HUFF_ENTRY(0xfffffe6,28),HUFF_ENTRY(0xfffffe7,28),
    HUFF_ENTRY(0xfffffe8,28),HUFF_ENTRY(0xffffea,24),HUFF_ENTRY(0x3ffffffc,30),HUFF_ENTRY(0xfffffe9,28),
    HUFF_ENTRY(0xfffffea,28),HUFF_ENTRY(0x3ffffffd,30),HUFF_ENTRY(0xfffffeb,28),HUFF_ENTRY(0xfffffec,28),
    HUFF_ENTRY(0xfffffed,28),HUFF_ENTRY(0xfffffee,28),HUFF_ENTRY(0xfffffef,28),HUFF_ENTRY(0xffffff0,28),
    HUFF_ENTRY(0xffffff1,28),HUFF_ENTRY(0xffffff2,28),HUFF_ENTRY(0x3ffffffe,30),HUFF_ENTRY(0xffffff3,28),
    HUFF_ENTRY(0xffffff4,28),HUFF_ENTRY(0xffffff5,28),HUFF_ENTRY(0xffffff6,28),HUFF_ENTRY(0xffffff7,28),
    HUFF_ENTRY(0xffffff8,28),HUFF_ENTRY(0xffffff9,28),HUFF_ENTRY(0xffffffa,28),HUFF_ENTRY(0xffffffb,28),
    HUFF_ENTRY(0x14,6),HUFF_ENTRY(0x3f8,10),HUFF_ENTRY(0x3f9,10),HUFF_ENTRY(0xffa,12),
    HUFF_ENTRY(0x1ff9,13),HUFF_ENTRY(0x15,6),HUFF_ENTRY(0xf8,8),HUFF_ENTRY(0x7fa,11),
    HUFF_ENTRY(0x3fa,10),HUFF_ENTRY(0x3fb,10),HUFF_ENTRY(0xf9,8),HUFF_ENTRY(0x7fb,11),
    HUFF_ENTRY(0xfa,8),HUFF_ENTRY(0x16,6),HUFF_ENTRY(0x17,6),HUFF_ENTRY(0x18,6),
    HUFF_ENTRY(0x0,5),HUFF_ENTRY(0x1,5),HUFF_ENTRY(0x2,5),HUFF_ENTRY(0x19,6),
    HUFF_ENTRY(0x1a,6),HUFF_ENTRY(0x1b,6),HUFF_ENTRY(0x1c,6),HUFF_ENTRY(0x1d,6),
    HUFF_ENTRY(0x1e,6),HUFF_ENTRY(0x1f,6),HUFF_ENTRY(0x5c,7),HUFF_ENTRY(0xfb,8),
    HUFF_ENTRY(0x7ffc,15),HUFF_ENTRY(0x20,6),HUFF_ENTRY(0xffb,12),HUFF_ENTRY(0x3fc,10),
    HUFF_ENTRY(0x1ffa,13),HUFF_ENTRY(0x21,6),HUFF_ENTRY(0x5d,7),HUFF_ENTRY(0x5e,7),
    HUFF_ENTRY(0x5f,7),HUFF_ENTRY(0x60,7),HUFF_ENTRY(0x61,7),HUFF_ENTRY(0x62,7),
    HUFF_ENTRY(0x63,7),HUFF_ENTRY(0x64,7),HUFF_ENTRY(0x65,7),HUFF_ENTRY(0x66,7),
    HUFF_ENTRY(0x67,7),HUFF_ENTRY(0x68,7),HUFF_ENTRY(0x69,7),HUFF_ENTRY(0x6a,7),
    HUFF_ENTRY(0x6b,7),HUFF_ENTRY(0x6c,7),HUFF_ENTRY(0x6d,7),HUFF_ENTRY(0x6e,7),
    HUFF_ENTRY(0x6f,7),HUFF_ENTRY(0x70,7),HUFF_ENTRY(0x71,7),HUFF_ENTRY(0x72,7),
    HUFF_ENTRY(0xfc,8),HUFF_ENTRY(0x73,7),HUFF_ENTRY(0xfd,8),HUFF_ENTRY(0x1ffb,13),
    HUFF_ENTRY(0x7fff0,19),HUFF_ENTRY(0x1ffc,13),HUFF_ENTRY(0x3ffc,14),HUFF_ENTRY(0x22,6),
    HUFF_ENTRY(0x7ffd,15),HUFF_ENTRY(0x3,5),HUFF_ENTRY(0x23,6),HUFF_ENTRY(0x4,5),
    HUFF_ENTRY(0x24,6),HUFF_ENTRY(0x5,5),HUFF_ENTRY(0x25,6),HUFF_ENTRY(0x26,6),
    HUFF_ENTRY(0x27,6),HUFF_ENTRY(0x6,5),HUFF_ENTRY(0x74,7),HUFF_ENTRY(0x75,7),
    HUFF_ENTRY(0x28,6),HUFF_ENTRY(0x29,6),HUFF_ENTRY(0x2a,6),HUFF_ENTRY(0x7,5),
    HUFF_ENTRY(0x2b,6),HUFF_ENTRY(0x76,7),HUFF_ENTRY(0x2c,6),HUFF_ENTRY(0x8,5),
    HUFF_ENTRY(0x9,5),HUFF_ENTRY(0x2d,6),HUFF_ENTRY(0x77,7),HUFF_ENTRY(0x78,7),
    HUFF_ENTRY(0x79,7),HUFF_ENTRY(0x7a,7),HUFF_ENTRY(0x7b,7),HUFF_ENTRY(0x7fffe,19),
    HUFF_ENTRY(0x7fc,11),HUFF_ENTRY(0x3ffd,14),HUFF_ENTRY(0x1ffd,13),HUFF_ENTRY(0xffffffc,28),
    HUFF_ENTRY(0xfffe6,20),HUFF_ENTRY(0x3fffd2,22),HUFF_ENTRY(0xfffe7,20),HUFF_ENTRY(0xfffe8,20),
    HUFF_ENTRY(0x3fffd3,22),HUFF_ENTRY(0x3fffd4,22),HUFF_ENTRY(0x3fffd5,22),HUFF_ENTRY(0x7fffd9,23),
    HUFF_ENTRY(0x3fffd6,22),HUFF_ENTRY(0x7fffda,23),HUFF_ENTRY(0x7fffdb,23),HUFF_ENTRY(0x7fffdc,23),
    HUFF_ENTRY(0x7fffdd,23),HUFF_ENTRY(0x7fffde,23),HUFF_ENTRY(0xffffeb,24),HUFF_ENTRY(0x7fffdf,23),
    HUFF_ENTRY(0xffffec,24),HUFF_ENTRY(0xffffed,24),HUFF_ENTRY(0x3fffd7,22),HUFF_ENTRY(0x7fffe0,23),
    HUFF_ENTRY(0xffffee,24),HUFF_ENTRY(0x7fffe1,23),HUFF_ENTRY(0x7fffe2,23),HUFF_ENTRY(0x7fffe3,23),
    HUFF_ENTRY(0x7fffe4,23),HUFF_ENTRY(0x1fffdc,21),HUFF_ENTRY(0x3fffd8,22),HUFF_ENTRY(0x7fffe5,23),
    HUFF_ENTRY(0x3fffd9,22),HUFF_ENTRY(0x7fffe6,23),HUFF_ENTRY(0x7fffe7,23),HUFF_ENTRY(0xffffef,24),
    HUFF_ENTRY(0x3fffda,22),HUFF_ENTRY(0x1fffdd,21),HUFF_ENTRY(0xfffe9,20),HUFF_ENTRY(0x3fffdb,22),
    HUFF_ENTRY(0x3fffdc,22),HUFF_ENTRY(0x7fffe8,23),HUFF_ENTRY(0x7fffe9,23),HUFF_ENTRY(0x1fffde,21),
    HUFF_ENTRY(0x7fffea,23),HUFF_ENTRY(0x3fffdd,22),HUFF_ENTRY(0x3fffde,22),HUFF_ENTRY(0xfffff0,24),
    HUFF_ENTRY(0x1fffdf,21),HUFF_ENTRY(0x3fffdf,22),HUFF_ENTRY(0x7fffeb,23),HUFF_ENTRY(0x7fffec,23),
    HUFF_ENTRY(0x1fffe0,21),HUFF_ENTRY(0x1fffe1,21),HUFF_ENTRY(0x3fffe0,22),HUFF_ENTRY(0x1fffe2,21),
    HUFF_ENTRY(0x7fffed,23),HUFF_ENTRY(0x3fffe1,22),HUFF_ENTRY(0x7fffee,23),HUFF_ENTRY(0x7fffef,23),
    HUFF_ENTRY(0xfffea,20),HUFF_ENTRY(0x3fffe2,22),HUFF_ENTRY(0x3fffe3,22),HUFF_ENTRY(0x3fffe4,22),
    HUFF_ENTRY(0x7ffff0,23),HUFF_ENTRY(0x3fffe5,22),HUFF_ENTRY(0x3fffe6,22),HUFF_ENTRY(0x7ffff1,23),
    HUFF_ENTRY(0x3ffffe0,26),HUFF_ENTRY(0x3ffffe1,26),HUFF_ENTRY(0xfffeb,20),HUFF_ENTRY(0x7fff1,19),
    HUFF_ENTRY(0x3fffe7,22),HUFF_ENTRY(0x7ffff2,23),HUFF_ENTRY(0x3fffe8,22),HUFF_ENTRY(0x1ffffec,25),
    HUFF_ENTRY(0x3ffffe2,26),HUFF_ENTRY(0x3ffffe3,26),HUFF_ENTRY(0x3ffffe4,26),HUFF_ENTRY(0x7ffffde,27),
    HUFF_ENTRY(0x7ffffdf,27),HUFF_ENTRY(0x3ffffe5,26),HUFF_ENTRY(0xfffff1,24),HUFF_ENTRY(0x1ffffed,25),
    HUFF_ENTRY(0x7fff2,19),HUFF_ENTRY(0x1fffe3,21),HUFF_ENTRY(0x3ffffe6,26),HUFF_ENTRY(0x7ffffe0,27),
    HUFF_ENTRY(0x7ffffe1,27),HUFF_ENTRY(0x3ffffe7,26),HUFF_ENTRY(0x7ffffe2,27),HUFF_ENTRY(0xfffff2,24),
    HUFF_ENTRY(0x1fffe4,21),HUFF_ENTRY(0x1fffe5,21),HUFF_ENTRY(0x3ffffe8,26),HUFF_ENTRY(0x3ffffe9,26),
    HUFF_ENTRY(0xffffffd,28),HUFF_ENTRY(0x7ffffe3,27),HUFF_ENTRY(0x7ffffe4,27),HUFF_ENTRY(0x7ffffe5,27),
    HUFF_ENTRY(0xfffec,20),HUFF_ENTRY(0xfffff3,24),HUFF_ENTRY(0xfffed,20),HUFF_ENTRY(0x1fffe6,21),
    HUFF_ENTRY(0x3fffe9,22),HUFF_ENTRY(0x1fffe7,21),HUFF_ENTRY(0x1fffe8,21),HUFF_ENTRY(0x7ffff3,23),
    HUFF_ENTRY(0x3fffea,22),HUFF_ENTRY(0x3fffeb,22),HUFF_ENTRY(0x1ffffee,25),HUFF_ENTRY(0x1ffffef,25),
    HUFF_ENTRY(0xfffff4,24),HUFF_ENTRY(0xfffff5,24),HUFF_ENTRY(0x3ffffea,26),HUFF_ENTRY(0x7ffff4,23),
    HUFF_ENTRY(0x3ffffeb,26),HUFF_ENTRY(0x7ffffe6,27),HUFF_ENTRY(0x3ffffec,26),HUFF_ENTRY(0x3ffffed,26),
    HUFF_ENTRY(0x7ffffe7,27),HUFF_ENTRY(0x7ffffe8,27),HUFF_ENTRY(0x7ffffe9,27),HUFF_ENTRY(0x7ffffea,27),
    HUFF_ENTRY(0x7ffffeb,27),HUFF_ENTRY(0xffffffe,28),HUFF_ENTRY(0x7ffffec,27),HUFF_ENTRY(0x7ffffed,27),
    HUFF_ENTRY(0x7ffffee,27),HUFF_ENTRY(0x7ffffef,27),HUFF_ENTRY(0x7fffff0,27),HUFF_ENTRY(0x3ffffee,26),
    // EOS (symbol 256)
    HUFF_ENTRY(0x3fffffff,30),
};

// ============================================================================
// HPACK Integer encoding (RFC 7541 §5.1)
// ============================================================================

int hpack_encode_int(uint8_t* buf, size_t bufsize, uint64_t value, uint8_t prefix_bits,
                     uint8_t first_byte_mask) {
    if (bufsize == 0) return 0;
    uint8_t max_prefix = (1 << prefix_bits) - 1;
    int pos = 0;
    if (value < max_prefix) {
        buf[pos] = first_byte_mask | static_cast<uint8_t>(value);
        return 1;
    }
    buf[pos] = first_byte_mask | static_cast<uint8_t>(max_prefix);
    pos++;
    value -= max_prefix;
    while (value >= 128) {
        if (static_cast<size_t>(pos) >= bufsize) return 0;
        buf[pos++] = static_cast<uint8_t>((value & 0x7F) | 0x80);
        value >>= 7;
    }
    if (static_cast<size_t>(pos) >= bufsize) return 0;
    buf[pos++] = static_cast<uint8_t>(value);
    return pos;
}

int hpack_decode_int(const uint8_t* buf, size_t len, uint8_t prefix_bits, uint64_t& value) {
    if (len == 0) return 0;
    uint8_t max_prefix = (1 << prefix_bits) - 1;
    value = buf[0] & max_prefix;
    if (value < max_prefix) return 1;
    int pos = 1;
    uint64_t m = 0;
    do {
        if (static_cast<size_t>(pos) >= len) return 0;
        value += (uint64_t(buf[pos] & 0x7F)) << m;
        m += 7;
    } while (buf[pos++] & 0x80);
    return pos;
}

// ============================================================================
// Huffman encoding/decoding
// ============================================================================

std::string huffman_encode(const std::string& input) {
    // Encode to bits
    uint64_t bit_buf = 0;
    int bit_count = 0;
    std::string output;
    for (unsigned char ch : input) {
        auto& s = HUFF[ch];
        bit_buf = (bit_buf << s.bits) | s.code;
        bit_count += s.bits;
        while (bit_count >= 8) {
            bit_count -= 8;
            output.push_back(static_cast<char>((bit_buf >> bit_count) & 0xFF));
        }
    }
    // Pad with EOS prefix (all 1s)
    if (bit_count > 0) {
        bit_buf <<= (8 - bit_count);
        bit_buf |= ((1 << (8 - bit_count)) - 1);  // fill with 1s (EOS prefix)
        output.push_back(static_cast<char>(bit_buf & 0xFF));
    }
    return output;
}

bool huffman_is_shorter(const std::string& input) {
    size_t bits = 0;
    for (unsigned char ch : input) {
        bits += HUFF[ch].bits;
    }
    return ((bits + 7) / 8) < input.size();
}

// Huffman decoder — bit-by-bit state machine
std::string huffman_decode(const uint8_t* data, size_t len) {
    std::string result;
    // Simple bit-by-bit decoder using prefix matching
    uint32_t code = 0;
    int code_bits = 0;
    for (size_t i = 0; i < len; ++i) {
        for (int bit = 7; bit >= 0; --bit) {
            code = (code << 1) | ((data[i] >> bit) & 1);
            code_bits++;
            // Search for matching symbol
            for (int sym = 0; sym < 256; ++sym) {
                if (HUFF[sym].bits == code_bits && HUFF[sym].code == code) {
                    result.push_back(static_cast<char>(sym));
                    code = 0;
                    code_bits = 0;
                    goto next_bit;
                }
            }
            if (code_bits > 30) return {};  // Error: too many bits without match
            next_bit:;
        }
    }
    // Verify remaining bits are EOS padding (all 1s, max 7 bits)
    if (code_bits > 7) return {};  // Invalid padding
    // Check that remaining bits are all 1s
    uint32_t mask = (1 << code_bits) - 1;
    if ((code & mask) != mask) return {};
    return result;
}

// ============================================================================
// HPACK Static Table (RFC 7541 Appendix A)
// ============================================================================

const std::vector<hpack_entry>& hpack_static_table() {
    static const std::vector<hpack_entry> table = {
        {"", ""},  // index 0 (unused, placeholder)
        {":authority", ""},              // 1
        {":method", "GET"},              // 2
        {":method", "POST"},             // 3
        {":path", "/"},                  // 4
        {":path", "/index.html"},        // 5
        {":scheme", "http"},             // 6
        {":scheme", "https"},            // 7
        {":status", "200"},              // 8
        {":status", "204"},              // 9
        {":status", "206"},              // 10
        {":status", "304"},              // 11
        {":status", "400"},              // 12
        {":status", "404"},              // 13
        {":status", "500"},              // 14
        {"accept-charset", ""},          // 15
        {"accept-encoding", "gzip, deflate"}, // 16
        {"accept-language", ""},         // 17
        {"accept-ranges", ""},           // 18
        {"accept", ""},                  // 19
        {"access-control-allow-origin", ""}, // 20
        {"age", ""},                     // 21
        {"allow", ""},                   // 22
        {"authorization", ""},           // 23
        {"cache-control", ""},           // 24
        {"content-disposition", ""},     // 25
        {"content-encoding", ""},        // 26
        {"content-language", ""},        // 27
        {"content-length", ""},          // 28
        {"content-location", ""},        // 29
        {"content-range", ""},           // 30
        {"content-type", ""},            // 31
        {"cookie", ""},                  // 32
        {"date", ""},                    // 33
        {"etag", ""},                    // 34
        {"expect", ""},                  // 35
        {"expires", ""},                 // 36
        {"from", ""},                    // 37
        {"host", ""},                    // 38
        {"if-match", ""},               // 39
        {"if-modified-since", ""},       // 40
        {"if-none-match", ""},           // 41
        {"if-range", ""},               // 42
        {"if-unmodified-since", ""},     // 43
        {"last-modified", ""},           // 44
        {"link", ""},                    // 45
        {"location", ""},               // 46
        {"max-forwards", ""},            // 47
        {"proxy-authenticate", ""},      // 48
        {"proxy-authorization", ""},     // 49
        {"range", ""},                   // 50
        {"referer", ""},                 // 51
        {"refresh", ""},                 // 52
        {"retry-after", ""},             // 53
        {"server", ""},                  // 54
        {"set-cookie", ""},              // 55
        {"strict-transport-security", ""}, // 56
        {"transfer-encoding", ""},       // 57
        {"user-agent", ""},              // 58
        {"vary", ""},                    // 59
        {"via", ""},                     // 60
        {"www-authenticate", ""},        // 61
    };
    return table;
}

std::pair<int, bool> hpack_find_static(const std::string& name, const std::string& value) {
    auto& table = hpack_static_table();
    int name_match = 0;
    for (size_t i = 1; i < table.size(); ++i) {
        if (table[i].name == name) {
            if (table[i].value == value) return {static_cast<int>(i), true};
            if (name_match == 0) name_match = static_cast<int>(i);
        }
    }
    return {name_match, false};
}

// ============================================================================
// HPACK Dynamic Table
// ============================================================================

hpack_dynamic_table::hpack_dynamic_table(size_t max_size) : max_size_(max_size) {}

void hpack_dynamic_table::add(const std::string& name, const std::string& value) {
    size_t esz = entry_size(name, value);
    // Evict until there's room
    while (current_size_ + esz > max_size_ && !entries_.empty()) {
        evict();
    }
    if (esz <= max_size_) {
        entries_.push_front({name, value});
        current_size_ += esz;
    }
}

void hpack_dynamic_table::resize(size_t new_max_size) {
    max_size_ = new_max_size;
    while (current_size_ > max_size_ && !entries_.empty()) {
        evict();
    }
}

const hpack_entry* hpack_dynamic_table::get(size_t index) const {
    if (index == 0 || index > entries_.size()) return nullptr;
    return &entries_[index - 1];
}

std::pair<size_t, bool> hpack_dynamic_table::find(const std::string& name,
                                                    const std::string& value) const {
    size_t name_match = 0;
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].name == name) {
            if (entries_[i].value == value) return {i + 1, true};
            if (name_match == 0) name_match = i + 1;
        }
    }
    return {name_match, false};
}

void hpack_dynamic_table::evict() {
    if (entries_.empty()) return;
    auto& back = entries_.back();
    current_size_ -= entry_size(back.name, back.value);
    entries_.pop_back();
}

// ============================================================================
// HPACK String encoding/decoding (RFC 7541 §5.2)
// ============================================================================

static std::string encode_string(const std::string& s) {
    std::string result;
    if (huffman_is_shorter(s)) {
        auto encoded = huffman_encode(s);
        uint8_t buf[16];
        int n = hpack_encode_int(buf, sizeof(buf), encoded.size(), 7, 0x80);  // H=1
        result.append(reinterpret_cast<char*>(buf), n);
        result.append(encoded);
    } else {
        uint8_t buf[16];
        int n = hpack_encode_int(buf, sizeof(buf), s.size(), 7, 0x00);  // H=0
        result.append(reinterpret_cast<char*>(buf), n);
        result.append(s);
    }
    return result;
}

static std::pair<std::string, int> decode_string(const uint8_t* data, size_t len) {
    if (len == 0) return {"", 0};
    bool huffman = (data[0] & 0x80) != 0;
    uint64_t str_len = 0;
    int consumed = hpack_decode_int(data, len, 7, str_len);
    if (consumed == 0 || consumed + str_len > len) return {"", 0};
    std::string s;
    if (huffman) {
        s = huffman_decode(data + consumed, str_len);
    } else {
        s.assign(reinterpret_cast<const char*>(data + consumed), str_len);
    }
    return {s, consumed + static_cast<int>(str_len)};
}

// ============================================================================
// HPACK Encoder
// ============================================================================

hpack_encoder::hpack_encoder(size_t max_table_size)
    : dynamic_table_(max_table_size), max_table_size_(max_table_size) {}

void hpack_encoder::set_max_table_size(size_t size) {
    max_table_size_ = size;
    dynamic_table_.resize(size);
    table_size_updated_ = true;
}

std::string hpack_encoder::encode(
        const std::vector<std::pair<std::string, std::string>>& headers) {
    std::string result;

    // Emit table size update if needed
    if (table_size_updated_) {
        uint8_t buf[16];
        int n = hpack_encode_int(buf, sizeof(buf), max_table_size_, 5, 0x20);
        result.append(reinterpret_cast<char*>(buf), n);
        table_size_updated_ = false;
    }

    for (auto& [name, value] : headers) {
        // Try static table
        auto [static_idx, static_exact] = hpack_find_static(name, value);

        // Try dynamic table
        auto [dyn_idx, dyn_exact] = dynamic_table_.find(name, value);
        size_t dyn_offset = hpack_static_table().size();  // dynamic indices start after static

        if (static_exact) {
            // Indexed Header Field (RFC 7541 §6.1)
            uint8_t buf[16];
            int n = hpack_encode_int(buf, sizeof(buf), static_idx, 7, 0x80);
            result.append(reinterpret_cast<char*>(buf), n);
        } else if (dyn_exact) {
            uint8_t buf[16];
            int n = hpack_encode_int(buf, sizeof(buf), dyn_idx + dyn_offset, 7, 0x80);
            result.append(reinterpret_cast<char*>(buf), n);
        } else {
            // Literal Header Field with Incremental Indexing (§6.2.1)
            int name_idx = static_idx > 0 ? static_idx :
                          (dyn_idx > 0 ? static_cast<int>(dyn_idx + dyn_offset) : 0);

            uint8_t buf[16];
            int n = hpack_encode_int(buf, sizeof(buf), name_idx, 6, 0x40);
            result.append(reinterpret_cast<char*>(buf), n);

            if (name_idx == 0) {
                result.append(encode_string(name));
            }
            result.append(encode_string(value));

            // Add to dynamic table
            dynamic_table_.add(name, value);
        }
    }

    return result;
}

// ============================================================================
// HPACK Decoder
// ============================================================================

hpack_decoder::hpack_decoder(size_t max_table_size)
    : dynamic_table_(max_table_size), max_table_size_(max_table_size) {}

void hpack_decoder::set_max_table_size(size_t size) {
    max_table_size_ = size;
    dynamic_table_.resize(size);
}

bool hpack_decoder::get_indexed(size_t index, std::string& name, std::string& value) {
    auto& st = hpack_static_table();
    if (index > 0 && index < st.size()) {
        name = st[index].name;
        value = st[index].value;
        return true;
    }
    size_t dyn_index = index - st.size();
    auto* entry = dynamic_table_.get(dyn_index);
    if (!entry) return false;
    name = entry->name;
    value = entry->value;
    return true;
}

std::vector<std::pair<std::string, std::string>>
hpack_decoder::decode(const uint8_t* data, size_t len) {
    std::vector<std::pair<std::string, std::string>> result;
    size_t pos = 0;

    while (pos < len) {
        uint8_t byte = data[pos];

        if (byte & 0x80) {
            // Indexed Header Field (§6.1)
            uint64_t index = 0;
            int consumed = hpack_decode_int(data + pos, len - pos, 7, index);
            if (consumed == 0 || index == 0) return {};
            pos += consumed;
            std::string name, value;
            if (!get_indexed(index, name, value)) return {};
            result.emplace_back(std::move(name), std::move(value));
        } else if (byte & 0x40) {
            // Literal with Incremental Indexing (§6.2.1)
            uint64_t name_index = 0;
            int consumed = hpack_decode_int(data + pos, len - pos, 6, name_index);
            if (consumed == 0) return {};
            pos += consumed;
            std::string name, value;
            if (name_index > 0) {
                std::string dummy_val;
                if (!get_indexed(name_index, name, dummy_val)) return {};
            } else {
                auto [s, c] = decode_string(data + pos, len - pos);
                if (c == 0) return {};
                name = std::move(s);
                pos += c;
            }
            auto [s, c] = decode_string(data + pos, len - pos);
            if (c == 0) return {};
            value = std::move(s);
            pos += c;
            dynamic_table_.add(name, value);
            result.emplace_back(std::move(name), std::move(value));
        } else if (byte & 0x20) {
            // Dynamic Table Size Update (§6.3)
            uint64_t new_size = 0;
            int consumed = hpack_decode_int(data + pos, len - pos, 5, new_size);
            if (consumed == 0) return {};
            pos += consumed;
            dynamic_table_.resize(new_size);
        } else {
            // Literal without Indexing (§6.2.2) or Never Indexed (§6.2.3)
            uint8_t prefix = (byte & 0x10) ? 4 : 4;  // Both use 4-bit prefix
            uint64_t name_index = 0;
            int consumed = hpack_decode_int(data + pos, len - pos, prefix, name_index);
            if (consumed == 0) return {};
            pos += consumed;
            std::string name, value;
            if (name_index > 0) {
                std::string dummy_val;
                if (!get_indexed(name_index, name, dummy_val)) return {};
            } else {
                auto [s, c] = decode_string(data + pos, len - pos);
                if (c == 0) return {};
                name = std::move(s);
                pos += c;
            }
            auto [s, c] = decode_string(data + pos, len - pos);
            if (c == 0) return {};
            value = std::move(s);
            pos += c;
            result.emplace_back(std::move(name), std::move(value));
        }
    }
    return result;
}

} // namespace async_net::http::h2
