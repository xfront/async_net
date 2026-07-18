// HPACK — Header Compression for HTTP/2 (RFC 7541)
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <utility>
#include <cstddef>
#include <deque>

namespace async_net::http::h2 {

// ---------------------------------------------------------------------------
// HPACK Integer encoding/decoding (RFC 7541 §5.1)
// ---------------------------------------------------------------------------

// Encode integer I with N-bit prefix into buf. Returns bytes written.
int hpack_encode_int(uint8_t* buf, size_t bufsize, uint64_t value, uint8_t prefix_bits,
                     uint8_t first_byte_mask = 0);

// Decode integer with N-bit prefix from buf. Returns bytes consumed, 0 on error.
// Updates `value` with decoded integer.
int hpack_decode_int(const uint8_t* buf, size_t len, uint8_t prefix_bits, uint64_t& value);

// ---------------------------------------------------------------------------
// HPACK Huffman encoding/decoding (RFC 7541 §5.2, Appendix B)
// ---------------------------------------------------------------------------

// Encode a string using HPACK Huffman coding. Returns the encoded bytes.
std::string huffman_encode(const std::string& input);

// Decode a Huffman-encoded string. Returns decoded string, empty on error.
std::string huffman_decode(const uint8_t* data, size_t len);

// Check if Huffman encoding would produce a shorter result
bool huffman_is_shorter(const std::string& input);

// ---------------------------------------------------------------------------
// HPACK Static Table (RFC 7541 Appendix A)
// ---------------------------------------------------------------------------

struct hpack_entry {
    std::string name;
    std::string value;
};

// Returns the static table (61 entries, index 1..61)
const std::vector<hpack_entry>& hpack_static_table();

// Find name in static table. Returns (index, has_value_match).
// index=0 means not found.
std::pair<int, bool> hpack_find_static(const std::string& name, const std::string& value);

// ---------------------------------------------------------------------------
// HPACK Dynamic Table (RFC 7541 §2.3.2)
// ---------------------------------------------------------------------------

class hpack_dynamic_table {
public:
    explicit hpack_dynamic_table(size_t max_size = 4096);

    // Add entry to the front of the table
    void add(const std::string& name, const std::string& value);

    // Resize the table (evicts entries if needed)
    void resize(size_t new_max_size);

    // Get entry by dynamic table index (1-based, relative to dynamic table start)
    const hpack_entry* get(size_t index) const;

    // Find name+value in dynamic table. Returns (index, exact_match).
    // index is 1-based relative to dynamic table. 0 = not found.
    std::pair<size_t, bool> find(const std::string& name, const std::string& value) const;

    size_t size() const { return current_size_; }
    size_t max_size() const { return max_size_; }
    size_t count() const { return entries_.size(); }

private:
    size_t max_size_;
    size_t current_size_ = 0;
    std::deque<hpack_entry> entries_;  // front = newest (index 1)

    static size_t entry_size(const std::string& name, const std::string& value) {
        return name.size() + value.size() + 32;  // RFC 7541 §4.1
    }

    void evict();
};

// ---------------------------------------------------------------------------
// HPACK Encoder
// ---------------------------------------------------------------------------

class hpack_encoder {
public:
    explicit hpack_encoder(size_t max_table_size = 4096);

    // Encode a list of header name-value pairs into HPACK binary format
    std::string encode(const std::vector<std::pair<std::string, std::string>>& headers);

    // Update max table size (emits dynamic table size update)
    void set_max_table_size(size_t size);

private:
    hpack_dynamic_table dynamic_table_;
    size_t max_table_size_;
    bool table_size_updated_ = false;
};

// ---------------------------------------------------------------------------
// HPACK Decoder
// ---------------------------------------------------------------------------

class hpack_decoder {
public:
    explicit hpack_decoder(size_t max_table_size = 4096);

    // Decode HPACK binary data into header name-value pairs.
    // Returns empty vector on error.
    std::vector<std::pair<std::string, std::string>> decode(const uint8_t* data, size_t len);

    // Update max table size
    void set_max_table_size(size_t size);

private:
    hpack_dynamic_table dynamic_table_;
    size_t max_table_size_;

    // Look up an entry by HPACK index (static + dynamic)
    bool get_indexed(size_t index, std::string& name, std::string& value);
};

} // namespace async_net::http::h2
