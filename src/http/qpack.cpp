// QPACK implementation — RFC 9204
#include "qpack.hpp"
#include "hpack.hpp"  // Reuse Huffman table
#include <cstring>
#include <algorithm>

namespace async_net::http::h3 {

// ============================================================================
// QPACK Static Table (RFC 9204 Appendix A) — 99 entries
// ============================================================================

const std::vector<std::pair<std::string, std::string>>& qpack_static_table() {
    static const std::vector<std::pair<std::string, std::string>> table = {
        {":authority", ""},                                     // 0
        {":path", "/"},                                         // 1
        {"age", "0"},                                           // 2
        {"content-disposition", ""},                            // 3
        {"content-length", "0"},                                // 4
        {"cookie", ""},                                         // 5
        {"date", ""},                                           // 6
        {"etag", ""},                                           // 7
        {"if-modified-since", ""},                              // 8
        {"if-none-match", ""},                                  // 9
        {"last-modified", ""},                                  // 10
        {"link", ""},                                           // 11
        {"location", ""},                                       // 12
        {"referer", ""},                                        // 13
        {"set-cookie", ""},                                     // 14
        {":method", "CONNECT"},                                 // 15
        {":method", "DELETE"},                                  // 16
        {":method", "GET"},                                     // 17
        {":method", "HEAD"},                                    // 18
        {":method", "OPTIONS"},                                 // 19
        {":method", "POST"},                                    // 20
        {":method", "PUT"},                                     // 21
        {":scheme", "http"},                                    // 22
        {":scheme", "https"},                                   // 23
        {":status", "103"},                                     // 24
        {":status", "200"},                                     // 25
        {":status", "304"},                                     // 26
        {":status", "404"},                                     // 27
        {":status", "503"},                                     // 28
        {"accept", "*/*"},                                      // 29
        {"accept", "application/dns-message"},                  // 30
        {"accept-encoding", "gzip, deflate, br"},               // 31
        {"accept-ranges", "bytes"},                             // 32
        {"access-control-allow-headers", "cache-control"},      // 33
        {"access-control-allow-headers", "content-type"},       // 34
        {"access-control-allow-origin", "*"},                   // 35
        {"cache-control", "max-age=0"},                         // 36
        {"cache-control", "max-age=2592000"},                   // 37
        {"cache-control", "max-age=604800"},                    // 38
        {"cache-control", "no-cache"},                          // 39
        {"cache-control", "no-store"},                          // 40
        {"cache-control", "public, max-age=31536000"},          // 41
        {"content-encoding", "br"},                             // 42
        {"content-encoding", "gzip"},                           // 43
        {"content-type", "application/dns-message"},            // 44
        {"content-type", "application/javascript"},             // 45
        {"content-type", "application/json"},                   // 46
        {"content-type", "application/x-www-form-urlencoded"},  // 47
        {"content-type", "image/gif"},                          // 48
        {"content-type", "image/jpeg"},                         // 49
        {"content-type", "image/png"},                          // 50
        {"content-type", "text/css"},                           // 51
        {"content-type", "text/html; charset=utf-8"},           // 52
        {"content-type", "text/plain"},                         // 53
        {"content-type", "text/plain;charset=utf-8"},           // 54
        {"range", "bytes=0-"},                                  // 55
        {"strict-transport-security", "max-age=31536000"},      // 56
        {"strict-transport-security", "max-age=31536000; includesubdomains"}, // 57
        {"strict-transport-security", "max-age=31536000; includesubdomains; preload"}, // 58
        {"vary", "accept-encoding"},                            // 59
        {"vary", "origin"},                                     // 60
        {"x-content-type-options", "nosniff"},                  // 61
        {"x-xss-protection", "1; mode=block"},                  // 62
        {":status", "100"},                                     // 63
        {":status", "204"},                                     // 64
        {":status", "206"},                                     // 65
        {":status", "302"},                                     // 66
        {":status", "400"},                                     // 67
        {":status", "403"},                                     // 68
        {":status", "421"},                                     // 69
        {":status", "425"},                                     // 70
        {":status", "500"},                                     // 71
        {"accept-language", ""},                                // 72
        {"access-control-allow-credentials", "FALSE"},          // 73
        {"access-control-allow-credentials", "TRUE"},           // 74
        {"access-control-allow-headers", "*"},                  // 75
        {"access-control-allow-methods", "get"},                // 76
        {"access-control-allow-methods", "get, post, options"}, // 77
        {"access-control-allow-methods", "options"},            // 78
        {"access-control-expose-headers", "content-length"},    // 79
        {"access-control-request-headers", "content-type"},     // 80
        {"access-control-request-method", "get"},               // 81
        {"access-control-request-method", "post"},              // 82
        {"alt-svc", "clear"},                                   // 83
        {"authorization", ""},                                  // 84
        {"content-security-policy", "script-src 'none'; object-src 'none'; base-uri 'none'"}, // 85
        {"early-data", "1"},                                    // 86
        {"expect-ct", ""},                                      // 87
        {"forwarded", ""},                                      // 88
        {"if-range", ""},                                       // 89
        {"origin", ""},                                         // 90
        {"purpose", "prefetch"},                                // 91
        {"server", ""},                                         // 92
        {"timing-allow-origin", "*"},                           // 93
        {"upgrade-insecure-requests", "1"},                     // 94
        {"user-agent", ""},                                     // 95
        {"x-forwarded-for", ""},                                // 96
        {"x-frame-options", "deny"},                            // 97
        {"x-frame-options", "sameorigin"},                      // 98
    };
    return table;
}

// ============================================================================
// Integer encoding/decoding (same as HPACK RFC 7541 §5.1)
// ============================================================================

int qpack_encode_int(uint8_t* buf, size_t bufsize, uint64_t value,
                     uint8_t prefix_bits, uint8_t first_byte_mask) {
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

int qpack_decode_int(const uint8_t* buf, size_t len, uint8_t prefix_bits,
                     uint64_t& value) {
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
// String encoding/decoding (QPACK format: bit 7 = H for huffman)
// ============================================================================

std::string qpack_encode_string(const std::string& s) {
    std::string result;
    if (h2::huffman_is_shorter(s)) {
        std::string encoded = h2::huffman_encode(s);
        uint8_t tmp[16];
        int n = qpack_encode_int(tmp, sizeof(tmp), encoded.size(), 7, 0x80);
        result.append(reinterpret_cast<const char*>(tmp), n);
        result.append(encoded);
    } else {
        uint8_t tmp[16];
        int n = qpack_encode_int(tmp, sizeof(tmp), s.size(), 7, 0x00);
        result.append(reinterpret_cast<const char*>(tmp), n);
        result.append(s);
    }
    return result;
}

int qpack_decode_string(const uint8_t* buf, size_t len, std::string& out) {
    if (len == 0) return 0;
    bool huffman = (buf[0] & 0x80) != 0;
    uint64_t str_len;
    int consumed = qpack_decode_int(buf, len, 7, str_len);
    if (consumed == 0) return 0;
    if (consumed + str_len > len) return 0;

    if (huffman) {
        out = h2::huffman_decode(buf + consumed, static_cast<size_t>(str_len));
        if (out.empty() && str_len > 0) return 0; // Huffman decode error
    } else {
        out.assign(reinterpret_cast<const char*>(buf + consumed),
                   static_cast<size_t>(str_len));
    }
    return consumed + static_cast<int>(str_len);
}

// ============================================================================
// Dynamic Table
// ============================================================================

void qpack_dynamic_table::insert(const std::string& name, const std::string& value) {
    entry e{name, value};
    size_t entry_size = e.size();
    // Evict entries if needed
    while (current_size_ + entry_size > max_size_ && !entries_.empty()) {
        current_size_ -= entries_.back().size();
        entries_.pop_back();
    }
    if (entry_size <= max_size_) {
        entries_.push_front(e);
        current_size_ += entry_size;
    }
    insert_count_++;
}

void qpack_dynamic_table::set_max_size(size_t max_size) {
    max_size_ = max_size;
    evict();
}

const qpack_dynamic_table::entry* qpack_dynamic_table::get(size_t index) const {
    if (index >= entries_.size()) return nullptr;
    return &entries_[index];
}

void qpack_dynamic_table::evict() {
    while (current_size_ > max_size_ && !entries_.empty()) {
        current_size_ -= entries_.back().size();
        entries_.pop_back();
    }
}

// ============================================================================
// Encoder
// ============================================================================

int qpack_encoder::find_static(const std::string& name, const std::string& value) const {
    auto& table = qpack_static_table();
    for (size_t i = 0; i < table.size(); i++) {
        if (table[i].first == name && table[i].second == value) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int qpack_encoder::find_static_name(const std::string& name) const {
    auto& table = qpack_static_table();
    for (size_t i = 0; i < table.size(); i++) {
        if (table[i].first == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::string qpack_encoder::encode_static(
    const std::vector<std::pair<std::string, std::string>>& headers) {
    // QPACK field section prefix: Required Insert Count (encoded) + Delta Base
    // For static-only encoding: RIC=0, DeltaBase=0
    std::string result;

    // Encoded Required Insert Count = 0
    result.push_back('\x00');
    // Encoded Delta Base = 0 (sign bit = 0, delta = 0)
    result.push_back('\x00');

    uint8_t tmp[64];
    for (auto& [name, value] : headers) {
        // Try indexed field line (exact match in static table)
        int idx = find_static(name, value);
        if (idx >= 0) {
            // Indexed Field Line (static): 1 1 T(=1) index
            // Pattern: 111xxxxx with 6-bit prefix
            int n = qpack_encode_int(tmp, sizeof(tmp), static_cast<uint64_t>(idx), 6, 0xC0);
            result.append(reinterpret_cast<const char*>(tmp), n);
            continue;
        }

        // Try literal with name reference (static)
        int name_idx = find_static_name(name);
        if (name_idx >= 0) {
            // Literal with Name Reference (static): 01 N(=0) T(=1) name_index value
            // Pattern: 0101xxxxx with 4-bit prefix for name index
            result.push_back(0x50); // 0101 0000 — N=0, T=1
            int n = qpack_encode_int(tmp, sizeof(tmp), static_cast<uint64_t>(name_idx), 4, 0x00);
            result.append(reinterpret_cast<const char*>(tmp), n);
            result.append(qpack_encode_string(value));
            continue;
        }

        // Literal with no name reference: 001 N(=0) name value
        // Pattern: 001xxxxx with 3-bit prefix for name length
        result.push_back(0x20); // 0010 0000 — N=0
        result.append(qpack_encode_string(name));
        result.append(qpack_encode_string(value));
    }

    return result;
}

qpack_encoder::encode_result qpack_encoder::encode(
    const std::vector<std::pair<std::string, std::string>>& headers) {
    // For now, use static-only encoding
    encode_result res;
    res.field_section = encode_static(headers);
    return res;
}

// ============================================================================
// Decoder
// ============================================================================

// QPACK field line patterns:
//   Indexed Field Line (static):          1 1 1 index         (6-bit prefix)
//   Indexed Field Line (dynamic post):    0 0 0 index         (6-bit prefix)
//   Indexed Field Line (dynamic):         1 0 0 index         (6-bit prefix)
//   Literal with Name Ref (static):       0 1 N 1 name_idx    (3-bit prefix)
//   Literal with Name Ref (dynamic):      0 1 N 0 name_idx    (3-bit prefix)
//   Literal with Name Ref (post-base):    0 0 N 0 name_idx    (3-bit prefix)
//   Literal with no Name Ref:             0 0 1 N name value  (3-bit prefix)
//   Literal with never-indexed:           0 0 0 1 N name val  (4-bit prefix)

int qpack_decoder::decode_field_line(const uint8_t* data, size_t len,
                                     std::pair<std::string, std::string>& out,
                                     size_t base) {
    if (len == 0) return 0;
    uint8_t b = data[0];

    // Indexed Field Line (static): 1 1 1 x x x x x (pattern: 1100_0000 mask)
    if ((b & 0xC0) == 0xC0) {
        uint64_t idx;
        int consumed = qpack_decode_int(data, len, 6, idx);
        if (consumed == 0) return 0;
        auto& table = qpack_static_table();
        if (idx >= table.size()) return 0;
        out = table[idx];
        return consumed;
    }

    // Indexed Field Line (dynamic): 1 0 0 x x x x x (pattern: 1000_0000, mask 0xE0 = 0xA0)
    if ((b & 0xE0) == 0xA0) {
        uint64_t idx;
        int consumed = qpack_decode_int(data, len, 5, idx);
        if (consumed == 0) return 0;
        // Post-base relative
        if (base + idx >= dynamic_table_.size()) return 0;
        auto* entry = dynamic_table_.get(base + idx);
        if (!entry) return 0;
        out = {entry->name, entry->value};
        return consumed;
    }

    // Literal with Name Reference (static): 0 1 N T name_idx value
    if ((b & 0xD0) == 0x50) {
        bool never_index = (b & 0x10) != 0;
        bool is_static = (b & 0x08) != 0;
        (void)never_index;
        uint64_t name_idx;
        int consumed = qpack_decode_int(data, len, 3, name_idx);
        if (consumed == 0) return 0;

        std::string name;
        if (is_static) {
            auto& table = qpack_static_table();
            if (name_idx >= table.size()) return 0;
            name = table[name_idx].first;
        } else {
            // Dynamic table reference
            auto* entry = dynamic_table_.get(name_idx);
            if (!entry) return 0;
            name = entry->name;
        }

        std::string value;
        int slen = qpack_decode_string(data + consumed, len - consumed, value);
        if (slen == 0) return 0;
        consumed += slen;
        out = {name, value};
        return consumed;
    }

    // Literal with Name Reference (post-base): 0 0 N 0 name_idx value
    if ((b & 0xF0) == 0x00 && (b & 0x08) == 0x00 && (b & 0x04) != 0) {
        // This overlaps with other patterns, need careful check
    }

    // Literal with no Name Reference: 0 0 1 N name value
    if ((b & 0xE0) == 0x20) {
        bool never_index = (b & 0x10) != 0;
        (void)never_index;
        std::string name, value;
        int consumed = 1; // Skip the first byte (prefix consumed for N bit)
        int nlen = qpack_decode_string(data + consumed, len - consumed, name);
        if (nlen == 0) return 0;
        consumed += nlen;
        int vlen = qpack_decode_string(data + consumed, len - consumed, value);
        if (vlen == 0) return 0;
        consumed += vlen;
        out = {name, value};
        return consumed;
    }

    // Indexed Field Line (dynamic, post-base): 0 0 0 x x x x x
    if ((b & 0xF0) == 0x00 && (b & 0x0C) == 0x00) {
        // 6-bit prefix
        uint64_t idx;
        int consumed = qpack_decode_int(data, len, 4, idx);
        if (consumed == 0) return 0;
        auto* entry = dynamic_table_.get(idx);
        if (!entry) return 0;
        out = {entry->name, entry->value};
        return consumed;
    }

    return 0; // Unknown pattern
}

std::vector<std::pair<std::string, std::string>> qpack_decoder::decode(
    const uint8_t* data, size_t len) {
    std::vector<std::pair<std::string, std::string>> headers;
    if (len < 2) return headers;

    // Parse field section prefix:
    // Encoded Required Insert Count (RIC) — integer with 8-bit prefix
    uint64_t ric_encoded;
    int consumed = qpack_decode_int(data, len, 8, ric_encoded);
    if (consumed <= 0) return headers;

    // Decode RIC (with wrapping)
    size_t req_insert_count = 0;
    if (ric_encoded == 0) {
        req_insert_count = 0;
    } else {
        // Simplified: for small tables, RIC = ric_encoded
        // Full algorithm: RFC 9204 §4.5.1
        size_t max_entries = max_table_size_ / 32;
        size_t full_range = 2 * max_entries;
        if (full_range == 0) full_range = 1;
        size_t total_inserts = dynamic_table_.insert_count();
        size_t max_value = total_inserts + max_entries;
        size_t rounded = (max_value / full_range) * full_range;
        req_insert_count = rounded + (ric_encoded - 1);
        if (req_insert_count > max_value) {
            req_insert_count -= full_range;
        }
        req_insert_count += 1; // RIC is 1-based
    }

    // Delta Base
    uint64_t delta_base;
    bool sign_negative = (data[consumed] & 0x80) != 0;
    int db_consumed = qpack_decode_int(data + consumed, len - consumed, 7, delta_base);
    if (db_consumed <= 0) return headers;
    consumed += db_consumed;

    size_t base = 0;
    if (req_insert_count == 0) {
        base = 0;
    } else {
        if (sign_negative) {
            base = req_insert_count - 1 - delta_base;
        } else {
            base = req_insert_count - 1 + delta_base;
        }
    }

    // Decode field lines
    while (consumed < static_cast<int>(len)) {
        std::pair<std::string, std::string> header;
        int n = decode_field_line(data + consumed, len - consumed, header, base);
        if (n <= 0) break;
        consumed += n;
        headers.push_back(std::move(header));
    }

    return headers;
}

void qpack_decoder::process_encoder_instructions(const uint8_t* data, size_t len) {
    size_t pos = 0;
    while (pos < len) {
        uint8_t b = data[pos];

        // Insert with Name Reference (static): 1 T name_idx value
        if ((b & 0xC0) == 0xC0) {
            bool is_static = (b & 0x20) != 0;
            uint64_t name_idx;
            int consumed = qpack_decode_int(data + pos, len - pos, 5, name_idx);
            if (consumed <= 0) break;
            pos += consumed;

            std::string name;
            if (is_static) {
                auto& table = qpack_static_table();
                if (name_idx < table.size()) name = table[name_idx].first;
            }

            std::string value;
            int slen = qpack_decode_string(data + pos, len - pos, value);
            if (slen <= 0) break;
            pos += slen;

            dynamic_table_.insert(name, value);
            known_received_count_++;
            continue;
        }

        // Insert with Name Reference (dynamic): 0 0 name_idx value
        if ((b & 0xC0) == 0x00 && (b & 0x20) != 0) {
            uint64_t name_idx;
            int consumed = qpack_decode_int(data + pos, len - pos, 5, name_idx);
            if (consumed <= 0) break;
            pos += consumed;

            std::string name;
            auto* entry = dynamic_table_.get(name_idx);
            if (entry) name = entry->name;

            std::string value;
            int slen = qpack_decode_string(data + pos, len - pos, value);
            if (slen <= 0) break;
            pos += slen;

            dynamic_table_.insert(name, value);
            known_received_count_++;
            continue;
        }

        // Insert with Literal Name: 0 1 N name value
        if ((b & 0xE0) == 0x40) {
            pos += 1;
            std::string name, value;
            int nlen = qpack_decode_string(data + pos, len - pos, name);
            if (nlen <= 0) break;
            pos += nlen;
            int vlen = qpack_decode_string(data + pos, len - pos, value);
            if (vlen <= 0) break;
            pos += vlen;

            dynamic_table_.insert(name, value);
            known_received_count_++;
            continue;
        }

        // Set Dynamic Table Capacity: 0 0 1 capacity
        if ((b & 0xE0) == 0x20) {
            uint64_t capacity;
            int consumed = qpack_decode_int(data + pos, len - pos, 5, capacity);
            if (consumed <= 0) break;
            pos += consumed;
            dynamic_table_.set_max_size(static_cast<size_t>(capacity));
            continue;
        }

        // Duplicate: 0 0 0 index
        if ((b & 0xE0) == 0x00 && (b & 0x10) == 0x00) {
            uint64_t idx;
            int consumed = qpack_decode_int(data + pos, len - pos, 4, idx);
            if (consumed <= 0) break;
            pos += consumed;
            auto* entry = dynamic_table_.get(idx);
            if (entry) {
                dynamic_table_.insert(entry->name, entry->value);
            }
            known_received_count_++;
            continue;
        }

        break; // Unknown instruction
    }
}

void qpack_decoder::process_decoder_instructions(const uint8_t* /*data*/, size_t /*len*/) {
    // Decoder stream instructions are acknowledgements from peer
    // For now, we don't track these (no dynamic table in encoder)
}

std::string qpack_decoder::get_decoder_instructions() {
    // Send Section Acknowledgement for completed sections
    // For now, return empty (no dynamic table usage in encoder)
    return "";
}

} // namespace async_net::http::h3
