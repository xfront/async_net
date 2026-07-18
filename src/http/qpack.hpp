// QPACK — RFC 9204 header compression for HTTP/3
// Zero-dependency implementation (reuses HPACK Huffman table)
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <utility>
#include <deque>

namespace async_net::http::h3 {

// QPACK static table (RFC 9204 Appendix A) — 99 entries (index 0-98)
const std::vector<std::pair<std::string, std::string>>& qpack_static_table();

// Integer encoding/decoding (same as HPACK §5.1)
int qpack_encode_int(uint8_t* buf, size_t bufsize, uint64_t value,
                     uint8_t prefix_bits, uint8_t first_byte_mask);
int qpack_decode_int(const uint8_t* buf, size_t len, uint8_t prefix_bits,
                     uint64_t& value);

// Huffman encoding/decoding (same table as HPACK RFC 7541 Appendix B)
std::string qpack_huffman_encode(const std::string& input);
bool qpack_huffman_is_shorter(const std::string& input);
std::string qpack_huffman_decode(const uint8_t* data, size_t len);

// String encoding (QPACK: H=1 bit prefix for huffman, not HPACK's)
std::string qpack_encode_string(const std::string& s);
int qpack_decode_string(const uint8_t* buf, size_t len, std::string& out);

// Dynamic table (managed via encoder/decoder instruction streams)
class qpack_dynamic_table {
public:
    struct entry {
        std::string name;
        std::string value;
        size_t size() const { return name.size() + value.size() + 32; }
    };

    void insert(const std::string& name, const std::string& value);
    void set_max_size(size_t max_size);
    const entry* get(size_t index) const;  // 0-based relative index
    size_t size() const { return entries_.size(); }
    size_t current_size() const { return current_size_; }
    size_t insert_count() const { return insert_count_; }
    size_t base() const { return base_; }
    void set_base(size_t b) { base_ = b; }

private:
    std::deque<entry> entries_;
    size_t max_size_ = 4096;
    size_t current_size_ = 0;
    size_t insert_count_ = 0;
    size_t base_ = 0;

    void evict();
};

// Encoder — encodes headers to QPACK format
class qpack_encoder {
public:
    // Encode headers for a given Required Insert Count and Base
    // Returns: {encoded field section, encoder instructions to send}
    struct encode_result {
        std::string field_section;  // The encoded field section
        std::string instructions;   // Encoder stream instructions (if any)
    };

    encode_result encode(const std::vector<std::pair<std::string, std::string>>& headers);

    // Simple mode: encode using only static table + literals (no dynamic table)
    std::string encode_static(const std::vector<std::pair<std::string, std::string>>& headers);

private:
    qpack_dynamic_table dynamic_table_;
    size_t max_table_size_ = 4096;

    // Find entry in static table. Returns index or -1.
    int find_static(const std::string& name, const std::string& value) const;
    int find_static_name(const std::string& name) const;
};

// Decoder — decodes QPACK field sections
class qpack_decoder {
public:
    // Decode a QPACK-encoded field section
    // req_insert_count and base are from the field section prefix
    std::vector<std::pair<std::string, std::string>> decode(
        const uint8_t* data, size_t len);

    // Process encoder stream instructions
    void process_encoder_instructions(const uint8_t* data, size_t len);

    // Process decoder stream instructions (acknowledgements)
    void process_decoder_instructions(const uint8_t* data, size_t len);

    // Generate decoder stream instructions (insert count increment)
    std::string get_decoder_instructions();

private:
    qpack_dynamic_table dynamic_table_;
    size_t max_table_size_ = 4096;
    size_t known_received_count_ = 0;

    // Decode a single header instruction
    int decode_field_line(const uint8_t* data, size_t len,
                         std::pair<std::string, std::string>& out,
                         size_t base);
};

} // namespace async_net::http::h3
