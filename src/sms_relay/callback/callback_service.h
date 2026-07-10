#pragma once

#include "callback_config.h"
#include "callback_result.h"
#include "sms/incoming_sms.h"
#include <asio/io_context.hpp>
#include <memory>
#include <string>

namespace smsrelay::callback {

class CallbackService : public std::enable_shared_from_this<CallbackService>
{
public:
    CallbackService(asio::io_context &io_ctx, const CallbackServiceConfig &config);

    // Send callback for new SMS
    void send_callback(const sms::IncomingSms &sms);

    // Check if callbacks are enabled
    bool is_enabled() const { return config_.enabled; }

private:
    CallbackResult send_http_callback(const CallbackConfig &config, const sms::IncomingSms &sms);

    std::string render_template(const std::string &tmpl, const sms::IncomingSms &sms);

    // Simple HTTP POST implementation using asio
    bool http_post(const std::string &url, const std::string &body, const std::map<std::string, std::string> &headers, CallbackResult &result);

    asio::io_context &io_ctx_;
    CallbackServiceConfig config_;
};

using CallbackServicePtr = std::shared_ptr<CallbackService>;

} // namespace smsrelay::callback
