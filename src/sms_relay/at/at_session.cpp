#include "at_session.h"
#include "at_commands.h"
#include <iostream>

namespace smsrelay::at
{

    AtSession::AtSession(std::shared_ptr<transport::SerialTransport> transport,
                         asio::io_context &io_ctx)
        : io_ctx_(io_ctx), transport_(transport), response_builder_(), dispatcher_(response_builder_, urc_dispatcher_)
    {
        // Set up line parser to call on_line_received
        line_parser_.set_line_callback([this](const std::string &line)
                                       {
        // Post to IO thread for processing
        asio::post(io_ctx_, [this, line]() {
            on_line_received(line);
        }); });

        // Set up transport data callback
        transport_->set_data_callback([this](const std::vector<uint8_t> &data) { process_data_from_transport(data); });

        // Set up transport error callback to handle connection failures
        transport_->set_error_callback([this](const std::string &error)
                                       {
        std::cout << "[Transport] " << error << std::endl;
        // If there's a command in progress, fulfill it with an error
        if (command_in_progress_ && current_command_ && current_command_->promise) {
            AtResponse error_response;
            error_response.success = false;
            error_response.status = "TRANSPORT_ERROR";
            error_response.data.push_back(error);
            current_command_->promise->set_value(error_response);

            // Reset state
            command_in_progress_ = false;
            current_command_.reset();
            response_builder_.reset();

            // Process next command (will likely fail too)
            asio::post(io_ctx_, [this]() {
                process_next_command();
            });
        } });

        // Register common URC handlers
        urc_dispatcher_.register_handler("+CMTI",
                                         [this](const std::string &urc, const std::string &args)
                                         {
                                             if (urc_callback_)
                                             {
                                                 urc_callback_(urc, args);
                                             }
                                         });
    }

    AtSession::~AtSession()
    {
        stop();
    }

    void AtSession::start()
    {
        // Open serial port (posted to IO thread)
        transport_->open();
    }

    void AtSession::stop()
    {
        // Close serial port (posted to IO thread)
        transport_->close();

        // Cancel current command if any
        if (current_command_)
        {
            current_command_->timer.cancel();
        }
    }

    std::future<AtSession::AtResponse> AtSession::send_command(std::string cmd, std::chrono::milliseconds timeout)
    {

        // Add to command queue (this creates the promise and returns the future)
        auto future = command_queue_.enqueue(std::move(cmd));

        // Post to IO thread to process the queue
        asio::post(io_ctx_, [this, timeout]()
        {
            // Store timeout for next command
            pending_timeout_ = timeout;

            // Try to process next command
            process_next_command();
        });

        return future;
    }

    void AtSession::set_urc_callback(URCallback cb)
    {
        urc_callback_ = std::move(cb);
    }

    void AtSession::process_next_command()
    {
        // Check if there's a command in progress
        if (command_in_progress_)
        {
            return;
        }

        // Check if queue has commands
        if (command_queue_.empty())
        {
            return;
        }

        // Get next command from queue
        auto queued_cmd = command_queue_.dequeue();
        if (!queued_cmd.promise)
        {
            return; // Empty command, skip
        }

        // Start current command
        current_command_ = std::make_unique<CurrentCommand>(
            CurrentCommand{
                queued_cmd.text,
                queued_cmd.promise,
                pending_timeout_,
                asio::steady_timer(io_ctx_)});

        // Start response builder
        response_builder_.start_command(current_command_->text);

        // Start timeout timer
        current_command_->timer.expires_after(current_command_->timeout);
        current_command_->timer.async_wait(
            [this](const asio::error_code &ec)
            {
                on_timeout(ec);
            });

        command_in_progress_ = true;

        // Send command to transport
        std::string full_cmd = current_command_->text + "\r";
        transport_->write(full_cmd);
    }

    void AtSession::on_line_received(const std::string &line)
    {
        // Filter empty lines to reduce noise
        if (line.empty())
        {
            return;
        }

        // Check if we have a command in progress
        if (!command_in_progress_)
        {
            // No command in progress, must be URC
            urc_dispatcher_.dispatch(line);
            return;
        }

        // Dispatch line (will decide URC vs response)
        dispatcher_.dispatch(line);

        // Check if response is complete
        if (response_builder_.state() == ResponseBuilder::State::Complete)
        {
            // Get the response
            auto response = response_builder_.get_response();

            // Fulfill the promise
            if (current_command_ && current_command_->promise)
            {
                current_command_->promise->set_value(response);
            }

            // Reset state
            command_in_progress_ = false;
            current_command_.reset();
            response_builder_.reset();

            // Process next command
            process_next_command();
        }
        else if (response_builder_.state() == ResponseBuilder::State::WaitingPrompt)
        {
            // Cancel timeout timer temporarily
            if (current_command_)
            {
                current_command_->timer.cancel();
            }

            // Signal caller that prompt is ready
            // For now, we'll handle this specially in send_message()
        }
    }

    void AtSession::on_timeout(const asio::error_code &ec)
    {
        if (ec == asio::error::operation_aborted)
        {
            // Timer was cancelled (command completed successfully)
            return;
        }

        std::cerr << "[AT] Command timeout" << std::endl;

        // Create timeout response
        AtResponse timeout_response;
        timeout_response.success = false;
        timeout_response.status = "TIMEOUT";

        // Fulfill promise with timeout
        if (current_command_ && current_command_->promise)
        {
            current_command_->promise->set_value(timeout_response);
        }

        // Reset state
        command_in_progress_ = false;
        current_command_.reset();
        response_builder_.reset();

        // Process next command
        process_next_command();
    }

    void AtSession::process_data_from_transport(const std::vector<uint8_t> &data)
    {
        // Feed to line parser
        line_parser_.feed(data);
    }

    // High-level command wrappers
    std::future<AtSession::AtResponse> AtSession::initialize_modem()
    {
        return send_command(commands::INIT);
    }

    std::future<AtSession::AtResponse> AtSession::get_urc_port()
    {
        return send_command(commands::GET_URC_PORT);
    }

    std::future<AtSession::AtResponse> AtSession::set_urc_port(const std::string &port)
    {
        return send_command(commands::set_urc_port(port));
    }

    std::future<AtSession::AtResponse> AtSession::check_and_set_urc_port()
    {
        // This is a placeholder - the logic should be implemented in the caller
        // Use get_urc_port() first, check the response, then call set_urc_port() if needed
        return send_command(commands::set_urc_port(commands::URC_PORT_UART1));
    }

    std::future<AtSession::AtResponse> AtSession::set_sms_pdu_mode(bool pdu_mode)
    {
        return send_command(pdu_mode ? commands::SET_SMS_PDU_MODE : commands::SET_SMS_TEXT_MODE);
    }

    std::future<AtSession::AtResponse> AtSession::set_new_message_indication()
    {
        return send_command(commands::SET_NEW_MSG_IND);
    }

    std::future<AtSession::AtResponse> AtSession::list_messages(const std::string &stat)
    {
        // PDU mode uses integer status codes:
        // 0 = REC UNREAD (received but unread)
        // 1 = REC READ (received and read)
        // 2 = STO UNSENT (stored but not sent)
        // 3 = STO SENT (stored and sent)
        // 4 = ALL (all messages)

        uint8_t status_code = 4; // Default to ALL

        if (stat == "ALL" || stat == "4")
        {
            status_code = 4;
        }
        else if (stat == "REC UNREAD" || stat == "0")
        {
            status_code = 0;
        }
        else if (stat == "REC READ" || stat == "1")
        {
            status_code = 1;
        }
        else if (stat == "STO UNSENT" || stat == "2")
        {
            status_code = 2;
        }
        else if (stat == "STO SENT" || stat == "3")
        {
            status_code = 3;
        }
        else
        {
            status_code = 4; // Default to ALL
        }

        return send_command(commands::list_messages(status_code));
    }

    std::future<AtSession::AtResponse> AtSession::read_message(uint8_t index)
    {
        return send_command(commands::read_message(index));
    }

    std::future<AtSession::AtResponse> AtSession::delete_message(uint8_t index)
    {
        return send_command(commands::delete_message(index));
    }

    std::future<AtSession::AtResponse> AtSession::send_message(const std::string &pdu)
    {
        // Calculate PDU length
        size_t pdu_length = (pdu.length() / 2) - 1;

        // Send AT+CMGS command
        auto future = send_command(commands::send_message(static_cast<uint16_t>(pdu_length)));

        // Wait for response and handle PDU sending
        // Note: This is a simplified version. A proper implementation would:
        // 1. Wait for > prompt
        // 2. Send PDU + Ctrl+Z
        // 3. Wait for final OK

        return future;
    }

    std::future<AtSession::AtResponse> AtSession::get_signal_quality()
    {
        return send_command(commands::SIGNAL_QUALITY);
    }

    std::future<AtSession::AtResponse> AtSession::get_manufacturer()
    {
        return send_command(commands::MANUFACTURER);
    }

    std::future<AtSession::AtResponse> AtSession::get_model()
    {
        return send_command(commands::MODEL);
    }

} // namespace smsrelay::at
