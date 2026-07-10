#pragma once

#include "sms_relay/at/command_queue.h"
#include "sms_relay/at/dispatcher.h"
#include "sms_relay/at/response_builder.h"
#include "sms_relay/transport/serial_transport.h"
#include "sms_relay/transport/urc_dispatcher.h"
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <functional>
#include <future>
#include <memory>
#include <string>

namespace smsrelay::at {

/**
 * @brief Fully asynchronous AT command session
 *
 * All operations are non-blocking and run in IO thread.
 * Returns std::future for response retrieval.
 *
 * Key features:
 * - Single command execution at a time (FIFO)
 * - Automatic timeout handling with steady_timer
 * - Intelligent URC/response dispatching
 * - Thread-safe: business thread and IO thread
 */
class AtSession : public std::enable_shared_from_this<AtSession>
{
public:
    using URCallback =
        std::function<void(const std::string &urc, const std::string &args)>;
    using AtResponse = ResponseBuilder::AtResponse;

    AtSession(std::shared_ptr<transport::SerialTransport> transport,
              asio::io_context &io_ctx);
    ~AtSession();

    /**
     * @brief Start the session (opens serial port)
     */
    void start();

    /**
     * @brief Stop the session
     */
    void stop();

    /**
     * @brief Send AT command asynchronously
     * @param cmd Command to send (without \r)
     * @param timeout Timeout for response
     * @return future for the response
     */
    std::future<AtResponse> send_command(
        std::string cmd,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

    /**
     * @brief Set callback for URC notifications
     */
    void set_urc_callback(URCallback cb);

    // High-level AT command wrappers (all async)
    std::future<AtResponse> initialize_modem();
    std::future<AtResponse> get_urc_port();
    std::future<AtResponse> set_urc_port(const std::string &port);
    std::future<AtResponse> check_and_set_urc_port();
    std::future<AtResponse> set_sms_pdu_mode(bool pdu_mode = true);
    std::future<AtResponse> set_new_message_indication();
    std::future<AtResponse> list_messages(const std::string &stat = "ALL");
    std::future<AtResponse> read_message(uint8_t index);
    std::future<AtResponse> delete_message(uint8_t index);
    std::future<AtResponse> send_message(const std::string &pdu);
    std::future<AtResponse> get_signal_quality();
    std::future<AtResponse> get_manufacturer();
    std::future<AtResponse> get_model();

private:
    struct CurrentCommand
    {
        std::string text;
        std::shared_ptr<std::promise<AtResponse>> promise;
        std::chrono::milliseconds timeout{5000}; // Default 5 seconds
        asio::steady_timer timer;
    };

    void process_next_command();
    void on_line_received(const std::string &line);
    void on_timeout(const asio::error_code &ec);
    void process_data_from_transport(const std::vector<uint8_t> &data);

    // Callback handlers (extracted from lambdas for better readability)
    void on_transport_data_received(const std::vector<uint8_t> &data);
    void on_transport_error(const std::string &error);
    void on_line_parser_line(const std::string &line);
    void on_cmti_urc(const std::string &urc, const std::string &args);

    asio::io_context &io_ctx_;
    std::shared_ptr<transport::SerialTransport> transport_;

    // Core components
    ResponseBuilder response_builder_;
    Dispatcher dispatcher_;
    transport::URCDispatcher urc_dispatcher_;
    CommandQueue command_queue_;

    // Current command state
    std::unique_ptr<CurrentCommand> current_command_;
    bool command_in_progress_ = false;
    std::chrono::milliseconds pending_timeout_{5000}; // Default timeout

    // URC callback
    URCallback urc_callback_;

    // Line parser for incoming data
    transport::LineParser line_parser_;
};

} // namespace smsrelay::at
