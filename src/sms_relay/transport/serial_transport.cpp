#include "serial_transport.h"
#include <asio/post.hpp>
#include <asio/write.hpp>
#include <system_error>

namespace smsrelay::transport {

SerialTransport::SerialTransport(asio::io_context &io_ctx,
                                 const SerialConfig &config)
    : io_ctx_(io_ctx), port_(io_ctx), config_(config), connected_(false),
      writing_(false)
{
    read_buffer_.fill(0);

    // Set up line parser callback to forward received data
    line_parser_.set_line_callback([this](const std::string &line) {
        if (data_callback_)
        {
            // Convert line back to bytes for callback
            std::string line_with_crlf = line + "\r\n";
            std::vector<uint8_t> data(line_with_crlf.begin(), line_with_crlf.end());
            data_callback_(data);
        }
    });
}

SerialTransport::~SerialTransport() { close(); }

void SerialTransport::open()
{
    // Post to IO thread
    asio::post(io_ctx_, [this]() { do_open(); });
}

void SerialTransport::close()
{
    // Post to IO thread
    asio::post(io_ctx_, [this]() {
        if (connected_)
        {
            connected_ = false;

            // Cancel pending operations
            asio::error_code ec;
            port_.cancel(ec);
            if (ec && error_callback_)
            {
                error_callback_("Cancel error: " + ec.message());
            }

            // Close the port
            port_.close(ec);
            // Bad file descriptor is expected if port was already closed
            if (ec && ec != asio::error::bad_descriptor && error_callback_)
            {
                error_callback_("Error closing serial port: " + ec.message());
            }

            // Notify connection state change
            notify_connection_state(false);
        }
    });
}

void SerialTransport::write(const std::vector<uint8_t> &data)
{
    // Convert to string and post to IO thread
    std::string str(data.begin(), data.end());
    write(str);
}

void SerialTransport::write(const std::string &data)
{
    // Post to IO thread
    asio::post(io_ctx_, [this, data]() {
        write_queue_.push(data);
        if (!writing_)
        {
            do_write();
        }
    });
}

void SerialTransport::do_open()
{
    try
    {
        port_.open(config_.port);

        // Configure serial port parameters
        port_.set_option(asio::serial_port::baud_rate(config_.baudrate));
        port_.set_option(asio::serial_port::character_size(8));
        port_.set_option(
            asio::serial_port::parity(asio::serial_port::parity::none));
        port_.set_option(
            asio::serial_port::stop_bits(asio::serial_port::stop_bits::one));
        port_.set_option(
            asio::serial_port::flow_control(asio::serial_port::flow_control::none));

        connected_ = true;

        if (error_callback_)
        {
            error_callback_("Serial port opened: " + config_.port);
        }

        // Notify connection state change
        notify_connection_state(true);

        // Start async read (runs forever)
        do_read();
    }
    catch (const std::system_error &e)
    {
        connected_ = false;
        std::string error_msg =
            "Failed to open serial port " + config_.port + ": " + e.what();
        if (error_callback_)
        {
            error_callback_(error_msg);
        }

        // Notify connection failure
        notify_connection_state(false);
    }
}

void SerialTransport::do_write()
{
    if (write_queue_.empty() || !connected_)
    {
        writing_ = false;
        return;
    }

    // Get next data to write
    std::string data = write_queue_.front();
    write_queue_.pop();

    // Convert to bytes
    current_write_data_.assign(data.begin(), data.end());

    // Start async write
    asio::async_write(port_, asio::buffer(current_write_data_),
                      [this, self = shared_from_this()](
                          const asio::error_code &ec, size_t bytes_transferred) {
                          handle_write(ec, bytes_transferred);
                      });
}

void SerialTransport::handle_write(const asio::error_code &ec,
                                   size_t /* bytes_transferred */)
{
    if (ec)
    {
        if (error_callback_)
        {
            error_callback_("Write error: " + ec.message() + " - closing port");
        }
        // Close port on write error - something is wrong
        connected_ = false;
        port_.close();
        writing_ = false;
        return;
    }

    // Continue writing next item
    do_write();
}

void SerialTransport::do_read()
{
    if (!connected_)
    {
        return;
    }

    port_.async_read_some(
        asio::buffer(read_buffer_),
        [this, self = shared_from_this()](const asio::error_code &ec,
                                          size_t bytes_transferred) {
            handle_read(ec, bytes_transferred);
        });
}

void SerialTransport::handle_read(const asio::error_code &ec,
                                  size_t bytes_transferred)
{
    if (ec)
    {
        if (ec != asio::error::operation_aborted)
        {
            connected_ = false;
            if (error_callback_)
            {
                error_callback_("Read error: " + ec.message());
            }
        }
        return;
    }

    // Feed data to line parser
    if (bytes_transferred > 0)
    {
        line_parser_.feed(read_buffer_.data(), bytes_transferred);
    }

    // Continue reading (runs forever)
    if (connected_)
    {
        do_read();
    }
}

void SerialTransport::notify_connection_state(bool connected)
{
    // Post callback to IO thread to ensure thread-safe execution
    asio::post(io_ctx_, [this, connected]() {
        if (connection_callback_)
        {
            connection_callback_(connected);
        }
    });
}

} // namespace smsrelay::transport
