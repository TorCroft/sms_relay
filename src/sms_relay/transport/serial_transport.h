#pragma once

#include "itransport.h"
#include "line_parser.h"
#include <asio/io_context.hpp>
#include <asio/serial_port.hpp>
#include <atomic>
#include <functional>
#include <queue>
#include <string>

namespace smsrelay::transport {

struct SerialConfig
{
    std::string port;
    uint32_t baudrate;

    SerialConfig(const std::string &p, uint32_t b) : port(p), baudrate(b) {}
};

/**
 * @brief Fully asynchronous serial port transport
 *
 * Key features:
 * - Single async_read_some() running continuously
 * - Lock-free write queue (all operations in IO thread)
 * - All callbacks execute in IO thread
 * - No mutex needed for internal state
 * - Connection state notification via callback
 */
class SerialTransport : public ITransport,
                        public std::enable_shared_from_this<SerialTransport>
{
public:
    // Connection state callback
    using ConnectionCallback = std::function<void(bool connected)>;

    SerialTransport(asio::io_context &io_ctx, const SerialConfig &config);
    ~SerialTransport() override;

    // ITransport interface (all operations posted to IO thread)
    void open() override;
    void close() override;
    bool is_connected() const override { return connected_; }
    void write(const std::vector<uint8_t> &data) override;
    void write(const std::string &data) override;
    void set_data_callback(DataCallback cb) override
    {
        data_callback_ = std::move(cb);
    }
    void set_error_callback(ErrorCallback cb) override
    {
        error_callback_ = std::move(cb);
    }

    // Connection callback (called when connection state changes)
    void set_connection_callback(ConnectionCallback cb)
    {
        connection_callback_ = std::move(cb);
    }

    // Get line parser for external use
    LineParser &line_parser() { return line_parser_; }

private:
    // All private methods run in IO thread
    void do_open();
    void do_write();
    void do_read();
    void handle_write(const asio::error_code &ec, size_t bytes_transferred);
    void handle_read(const asio::error_code &ec, size_t bytes_transferred);
    void notify_connection_state(bool connected);

    asio::io_context &io_ctx_;
    asio::serial_port port_;
    SerialConfig config_;
    std::atomic<bool> connected_;

    // Read buffer
    static constexpr size_t READ_BUFFER_SIZE = 1024;
    std::array<uint8_t, READ_BUFFER_SIZE> read_buffer_;

    // Write queue (no mutex! all access from IO thread)
    std::queue<std::string> write_queue_;
    bool writing_ = false;
    std::vector<uint8_t> current_write_data_;

    // Callbacks
    DataCallback data_callback_;
    ErrorCallback error_callback_;
    ConnectionCallback connection_callback_;

    // Line parser
    LineParser line_parser_;
};

} // namespace smsrelay::transport
