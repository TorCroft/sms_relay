#pragma once

#include <string>
#include <functional>
#include <unordered_map>
#include <mutex>

namespace smsrelay::transport {

/**
 * @brief Dispatcher for Unsolicited Result Codes (URC)
 *
 * Handles async notifications from modem like +CMTI, +CMT, +CPIN, etc.
 * Thread-safe registration and dispatching of URC handlers.
 */
class URCDispatcher {
public:
    using URCHandler = std::function<void(const std::string& urc, const std::string& args)>;

    URCDispatcher() = default;

    /**
     * @brief Register handler for specific URC type
     * @param prefix URC prefix (e.g., "+CMTI", "+CMT")
     * @param handler Callback to handle the URC
     */
    void register_handler(const std::string& prefix, URCHandler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        handlers_[prefix] = std::move(handler);
    }

    /**
     * @brief Unregister handler for URC type
     */
    void unregister_handler(const std::string& prefix) {
        std::lock_guard<std::mutex> lock(mutex_);
        handlers_.erase(prefix);
    }

    /**
     * @brief Dispatch line to appropriate handler
     * @return true if line was a URC and was handled, false otherwise
     */
    bool dispatch(const std::string& line) {
        if (line.empty()) {
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        // Check if line starts with a known URC prefix
        for (const auto& [prefix, handler] : handlers_) {
            if (line.find(prefix) == 0) {
                // Extract arguments (everything after prefix and ": ")
                size_t colon_pos = line.find(':');
                if (colon_pos != std::string::npos && colon_pos + 2 < line.length()) {
                    std::string args = line.substr(colon_pos + 2);
                    if (handler) {
                        handler(prefix, args);
                    }
                } else if (handler) {
                    // URC without arguments
                    handler(prefix, "");
                }
                return true;
            }
        }

        return false;
    }

    /**
     * @brief Check if line is a URC (without dispatching)
     */
    bool is_urc(const std::string& line) const {
        if (line.empty()) {
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        for (const auto& [prefix, _] : handlers_) {
            if (line.find(prefix) == 0) {
                return true;
            }
        }

        return false;
    }

    /**
     * @brief Get list of registered URC prefixes
     */
    std::vector<std::string> registered_urcs() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> result;
        result.reserve(handlers_.size());
        for (const auto& [prefix, _] : handlers_) {
            result.push_back(prefix);
        }
        return result;
    }

private:
    std::unordered_map<std::string, URCHandler> handlers_;
    mutable std::mutex mutex_;
};

} // namespace smsrelay::transport
