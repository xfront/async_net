#pragma once

#include "../detail/config.hpp"
#include <cstddef>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>

namespace async_net {

// Mutable buffer - a non-owning reference to a writable memory region
class mutable_buffer {
public:
    mutable_buffer() : data_(nullptr), size_(0) {}
    mutable_buffer(void* data, size_t size) : data_(data), size_(size) {}

    void* data() const { return data_; }
    size_t size() const { return size_; }

    mutable_buffer operator+(size_t offset) const {
        return mutable_buffer(static_cast<char*>(data_) + offset, size_ - offset);
    }

private:
    void* data_;
    size_t size_;
};

// Const buffer - a non-owning reference to a readable memory region
class const_buffer {
public:
    const_buffer() : data_(nullptr), size_(0) {}
    const_buffer(const void* data, size_t size) : data_(data), size_(size) {}
    const_buffer(const mutable_buffer& mb) : data_(mb.data()), size_(mb.size()) {}

    const void* data() const { return data_; }
    size_t size() const { return size_; }

    const_buffer operator+(size_t offset) const {
        return const_buffer(static_cast<const char*>(data_) + offset, size_ - offset);
    }

private:
    const void* data_;
    size_t size_;
};

// Helper to create buffers from various types
inline mutable_buffer buffer(void* data, size_t size) {
    return mutable_buffer(data, size);
}

inline mutable_buffer buffer(void* data, size_t size, size_t max_size) {
    return mutable_buffer(data, std::min(size, max_size));
}

inline const_buffer buffer(const void* data, size_t size) {
    return const_buffer(data, size);
}

inline const_buffer buffer(const void* data, size_t size, size_t max_size) {
    return const_buffer(data, std::min(size, max_size));
}

template<size_t N>
inline mutable_buffer buffer(char (&data)[N]) {
    return mutable_buffer(data, N);
}

template<size_t N>
inline mutable_buffer buffer(unsigned char (&data)[N]) {
    return mutable_buffer(data, N);
}

template<size_t N>
inline const_buffer buffer(const char (&data)[N]) {
    return const_buffer(data, N);
}

inline mutable_buffer buffer(std::vector<char>& vec) {
    return mutable_buffer(vec.data(), vec.size());
}

inline const_buffer buffer(const std::vector<char>& vec) {
    return const_buffer(vec.data(), vec.size());
}

inline const_buffer buffer(const std::string& str) {
    return const_buffer(str.data(), str.size());
}

// Dynamic buffer - a growable buffer for reading
class dynamic_buffer {
public:
    explicit dynamic_buffer(size_t initial_capacity = 4096)
        : data_(initial_capacity), read_pos_(0), write_pos_(0) {}

    // Get a mutable buffer for reading into
    mutable_buffer prepare(size_t n) {
        if (write_pos_ + n > data_.size()) {
            data_.resize(std::max(data_.size() * 2, write_pos_ + n));
        }
        return mutable_buffer(data_.data() + write_pos_, n);
    }

    // Commit n bytes that were read
    void commit(size_t n) {
        write_pos_ += std::min(n, data_.size() - write_pos_);
    }

    // Get a const buffer of available data
    const_buffer data() const {
        return const_buffer(data_.data() + read_pos_, write_pos_ - read_pos_);
    }

    // Consume n bytes
    void consume(size_t n) {
        read_pos_ += std::min(n, write_pos_ - read_pos_);
        if (read_pos_ == write_pos_) {
            read_pos_ = write_pos_ = 0;
        }
    }

    // Available bytes to read
    size_t size() const { return write_pos_ - read_pos_; }

    // Available capacity for writing
    size_t capacity() const { return data_.size() - write_pos_; }

    // Clear the buffer
    void clear() { read_pos_ = write_pos_ = 0; }

    // Access raw data
    char* raw_data() { return data_.data(); }
    const char* raw_data() const { return data_.data(); }

private:
    std::vector<char> data_;
    size_t read_pos_;
    size_t write_pos_;
};

} // namespace async_net
