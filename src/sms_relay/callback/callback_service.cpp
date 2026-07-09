#include "callback_service.h"
#include <asio/ip/tcp.hpp>
#include <asio/write.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <sstream>
#include <regex>

namespace smsrelay::callback {

CallbackService::CallbackService(asio::io_context& io_ctx,
                                 const CallbackServiceConfig& config)
    : io_ctx_(io_ctx)
    , config_(config)
{
}

void CallbackService::send_callback(const sms::IncomingSms& sms) {
    if (!config_.enabled || config_.callbacks.empty()) {
        return;
    }

    for (const auto& callback_config : config_.callbacks) {
        if (callback_config.type == "http") {
            auto result = send_http_callback(callback_config, sms);
            // Log result if needed
        }
    }
}

std::string CallbackService::render_template(const std::string& tmpl,
                                             const sms::IncomingSms& sms) {
    std::string result = tmpl;

    // Simple variable replacement
    std::regex re(R"(\$\{([^}]+)\})");
    std::smatch match;

    while (std::regex_search(result, match, re)) {
        std::string var_name = match[2].str();
        std::string replacement;

        if (var_name == "sender") {
            replacement = sms.sender;
        } else if (var_name == "text") {
            replacement = sms.text;
        } else if (var_name == "timestamp") {
            replacement = sms.timestamp;
        } else if (var_name == "index") {
            replacement = std::to_string(sms.index);
        } else if (var_name == "concat_ref") {
            replacement = std::to_string(sms.concat_ref);
        } else if (var_name == "concat_seq") {
            replacement = std::to_string(sms.concat_seq);
        } else if (var_name == "concat_total") {
            replacement = std::to_string(sms.concat_total);
        }

        result = std::regex_replace(result, std::regex(match[0].str()), replacement);
    }

    return result;
}

CallbackResult CallbackService::send_http_callback(const CallbackConfig& config,
                                                    const sms::IncomingSms& sms) {
    CallbackResult result;

    // Render template
    std::string body = render_template(config.body_template, sms);

    // Send HTTP POST
    bool success = http_post(config.url, body, config.headers, result);

    result.success = success;
    return result;
}

bool CallbackService::http_post(const std::string& url,
                                const std::string& body,
                                const std::map<std::string, std::string>& headers,
                                CallbackResult& result) {
    try {
        using asio::ip::tcp;

        // Simple URL parsing (assumes http://host:port/path format)
        std::string host, port, path;
        size_t host_start = url.find("://");
        if (host_start != std::string::npos) {
            host_start += 3;
        } else {
            host_start = 0;
        }

        size_t path_start = url.find('/', host_start);
        if (path_start != std::string::npos) {
            host = url.substr(host_start, path_start - host_start);
            path = url.substr(path_start);

            // Check for port in host
            size_t port_start = host.find(':');
            if (port_start != std::string::npos) {
                port = host.substr(port_start + 1);
                host = host.substr(0, port_start);
            } else {
                port = "80";  // Default HTTP port
            }
        } else {
            host = url.substr(host_start);
            path = "/";
            port = "80";
        }

        // Resolve host
        asio::ip::tcp::resolver resolver(io_ctx_);
        auto endpoints = resolver.resolve(host, port);

        // Connect
        asio::ip::tcp::socket socket(io_ctx_);
        asio::error_code ec;
        (void)socket.connect(*endpoints.begin(), ec);
        if (ec) {
            result.success = false;
            result.error_message = ec.message();
            return false;
        }

        // Build HTTP request
        std::ostringstream request;
        request << "POST " << path << " HTTP/1.1\r\n";
        request << "Host: " << host << "\r\n";
        request << "Content-Length: " << body.length() << "\r\n";

        // Add custom headers
        for (const auto& [key, value] : headers) {
            request << key << ": " << value << "\r\n";
        }

        request << "Content-Type: application/json\r\n";
        request << "Connection: close\r\n";
        request << "\r\n";
        request << body;

        // Send request
        std::string request_str = request.str();
        asio::write(socket, asio::buffer(request_str));

        // Read response
        asio::streambuf response_buffer;
        asio::read_until(socket, response_buffer, "\r\n\r\n");

        std::istream response_stream(&response_buffer);
        std::string http_version;
        unsigned int status_code = 0;
        std::string status_message;

        response_stream >> http_version;
        response_stream >> status_code;
        std::getline(response_stream, status_message);

        result.status_code = static_cast<int>(status_code);
        result.success = (status_code >= 200 && status_code < 300);

        // Read remaining response (if any)
        std::string response_body;
        std::string line;
        while (std::getline(response_stream, line) && line != "\r") {
            // Skip headers
        }

        // Read body
        std::ostringstream body_stream;
        body_stream << response_stream.rdbuf();
        result.response = body_stream.str();

    } catch (const std::exception& e) {
        result.success = false;
        result.error_message = e.what();
        return false;
    }

    return true;
}

} // namespace smsrelay::callback
