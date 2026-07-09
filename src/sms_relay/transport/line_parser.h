#pragma once

#include <string>
#include <vector>
#include <functional>
#include <mutex>

namespace smsrelay::transport {

/**
 * @brief Parser for AT protocol lines from serial data
 *
 * Accumulates raw bytes and extracts complete lines delimited by \r\n
 * Handles partial lines and ensures thread-safe operation.
 */
class LineParser {
public:
    using LineCallback = std::function<void(const std::string&)>;

    LineParser() = default;

    /**
     * @brief Feed raw data to parser
     *
     * Accumulates data and extracts complete lines.
     * Calls line_callback for each complete line.
     */
    void feed(const uint8_t* data, size_t length) {
        std::lock_guard<std::mutex> lock(mutex_);

        buffer_.append(reinterpret_cast<const char*>(data), length);
        extract_lines();
    }

    /**
     * @brief Feed vector data to parser
     */
    void feed(const std::vector<uint8_t>& data) {
        feed(data.data(), data.size());
    }

    /**
     * @brief Feed string data to parser
     */
    void feed(const std::string& data) {
        feed(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    }

    /**
     * @brief Set callback for complete lines
     */
    void set_line_callback(LineCallback cb) {
        std::lock_guard<std::mutex> lock(mutex_);
        line_callback_ = std::move(cb);
    }

    /**
     * @brief Get accumulated buffer (for debugging)
     */
    std::string get_buffer() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return buffer_;
    }

    /**
     * @brief Clear buffer
     */
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        buffer_.clear();
    }

private:
    void extract_lines() {
        size_t pos = 0;
        while ((pos = buffer_.find("\r\n")) != std::string::npos) {
            std::string line = buffer_.substr(0, pos);
            buffer_.erase(0, pos + 2);  // Remove line + \r\n

            // Remove trailing \r if present (some modems use \r\r\n)
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            if (line_callback_) {
                line_callback_(line);
            }
        }
    }

    std::string buffer_;
    LineCallback line_callback_;
    mutable std::mutex mutex_;
};

} // namespace smsrelay::transport
