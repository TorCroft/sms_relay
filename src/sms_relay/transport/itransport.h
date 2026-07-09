#pragma once

#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace smsrelay::transport {

/**
 * @brief Abstract interface for transport layer
 *
 * Decouples AT session from concrete transport implementation.
 * Future implementations: SerialPort, TCP, USB CDC, PTY, etc.
 */
class ITransport {
public:
    using DataCallback = std::function<void(const std::vector<uint8_t>&)>;
    using ErrorCallback = std::function<void(const std::string&)>;

    virtual ~ITransport() = default;

    /**
     * @brief Open transport connection
     */
    virtual void open() = 0;

    /**
     * @brief Close transport connection
     */
    virtual void close() = 0;

    /**
     * @brief Check if transport is connected
     */
    virtual bool is_connected() const = 0;

    /**
     * @brief Write data to transport (async)
     */
    virtual void write(const std::vector<uint8_t>& data) = 0;

    /**
     * @brief Write string data to transport (async)
     */
    virtual void write(const std::string& data) = 0;

    /**
     * @brief Set callback for received data
     */
    virtual void set_data_callback(DataCallback cb) = 0;

    /**
     * @brief Set callback for errors
     */
    virtual void set_error_callback(ErrorCallback cb) = 0;
};

} // namespace smsrelay::transport
