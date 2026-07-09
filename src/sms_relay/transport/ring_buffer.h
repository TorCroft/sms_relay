#pragma once

#include <mutex>
#include <cstdint>
#include <vector>

namespace smsrelay::transport {

/**
 * @brief Fixed-size ring buffer for serial data
 *
 * Thread-safe circular buffer for accumulating serial port data
 * before line parsing.
 */
class RingBuffer {
public:
    static constexpr size_t DEFAULT_CAPACITY = 4096;

    explicit RingBuffer(size_t capacity = DEFAULT_CAPACITY)
        : buffer_(capacity)
        , capacity_(capacity)
        , head_(0)
        , tail_(0)
        , size_(0)
    {}

    /**
     * @brief Write data to ring buffer
     * @return Number of bytes written
     */
    size_t write(const uint8_t* data, size_t length) {
        std::lock_guard<std::mutex> lock(mutex_);

        size_t written = 0;
        for (size_t i = 0; i < length && size_ < capacity_; ++i) {
            buffer_[tail_] = data[i];
            tail_ = (tail_ + 1) % capacity_;
            ++size_;
            ++written;
        }

        return written;
    }

    /**
     * @brief Write vector to ring buffer
     */
    size_t write(const std::vector<uint8_t>& data) {
        return write(data.data(), data.size());
    }

    /**
     * @brief Read data from ring buffer
     * @return Number of bytes read
     */
    size_t read(uint8_t* data, size_t length) {
        std::lock_guard<std::mutex> lock(mutex_);

        size_t read = 0;
        for (size_t i = 0; i < length && size_ > 0; ++i) {
            data[i] = buffer_[head_];
            head_ = (head_ + 1) % capacity_;
            --size_;
            ++read;
        }

        return read;
    }

    /**
     * @brief Read all data into vector
     */
    std::vector<uint8_t> read_all() {
        std::vector<uint8_t> data;
        data.resize(size_);

        std::lock_guard<std::mutex> lock(mutex_);
        size_t read_count = read(data.data(), data.size());
        data.resize(read_count);

        return data;
    }

    /**
     * @brief Get current size
     */
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return size_;
    }

    /**
     * @brief Get capacity
     */
    size_t capacity() const {
        return capacity_;
    }

    /**
     * @brief Check if buffer is empty
     */
    bool empty() const {
        return size() == 0;
    }

    /**
     * @brief Check if buffer is full
     */
    bool full() const {
        return size() == capacity_;
    }

    /**
     * @brief Clear buffer
     */
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        head_ = 0;
        tail_ = 0;
        size_ = 0;
    }

private:
    std::vector<uint8_t> buffer_;
    size_t capacity_;
    size_t head_;
    size_t tail_;
    size_t size_;
    mutable std::mutex mutex_;
};

} // namespace smsrelay::transport
