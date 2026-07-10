#include "bark_target.h"
#include "sms_relay/forward/template_renderer.h"
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>

using json = nlohmann::json;

namespace smsrelay::forward::targets {

BarkTarget::BarkTarget(const BarkConfig &config,
                       std::shared_ptr<http::HttpClient> http_client)
    : config_(config), http_client_(std::move(http_client)) {}

bool BarkTarget::send(const IncomingSms &sms)
{
    try
    {
        std::string url = build_url(sms);
        std::string body = build_body(sms);

        std::map<std::string, std::string> headers;
        headers["Content-Type"] = "application/json; charset=utf-8";

        auto response = http_client_->post(url, body, headers, config_.timeout_ms);

        if (response.success)
        {
            return true;
        }
        else
        {
            std::cerr << "[Bark] HTTP " << response.status_code << std::endl;
            return false;
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "[Bark] " << e.what() << std::endl;
        return false;
    }
}

std::string BarkTarget::build_url(const IncomingSms &sms)
{
    // Note: sms parameter is reserved for future URL customization based on
    // message content
    (void)sms; // Suppress unused parameter warning

    // Bark API format: POST https://api.day.app/{key} with JSON body
    std::stringstream url;

    // Remove trailing slash if present
    std::string server = config_.server;
    if (!server.empty() && server.back() == '/')
    {
        server.pop_back();
    }

    url << server << "/" << config_.key;

    return url.str();
}

std::string BarkTarget::build_body(const IncomingSms &sms)
{
    // Build JSON body using nlohmann/json for proper UTF-8 encoding
    // {
    //   "title": "notification title",
    //   "body": "notification body",
    //   "sound": "sound name",
    //   "icon": "https://...",
    //   "group": "group name",
    //   "url": "https://...",
    //   "badge": 1,
    //   "level": "active"
    // }

    json j;

    // Title (required)
    std::string title = TemplateRenderer::render(config_.title, sms);
    j["title"] = title;

    // Body (required)
    std::string body_text = TemplateRenderer::render(config_.body, sms);
    j["body"] = body_text;

    // Sound (optional)
    if (!config_.sound.empty())
    {
        j["sound"] = config_.sound;
    }

    // Icon (optional)
    if (!config_.icon.empty())
    {
        j["icon"] = config_.icon;
    }

    // Group (optional)
    if (!config_.group.empty())
    {
        j["group"] = config_.group;
    }

    // URL (optional)
    if (!config_.url.empty())
    {
        j["url"] = config_.url;
    }

    // Badge (optional)
    j["badge"] = config_.badge;

    // Level (optional)
    if (!config_.level.empty())
    {
        j["level"] = config_.level;
    }

    // Dump JSON with UTF-8 encoding
    return j.dump();
}

std::unique_ptr<ForwardTarget>
BarkTarget::create(const ForwardTargetConfig &config,
                   const std::shared_ptr<http::HttpClient> &http_client)
{
    // Extract bark-specific configuration
    if (config.type != "bark")
    {
        throw std::runtime_error("Invalid target type for BarkTarget");
    }
    return std::make_unique<BarkTarget>(config.bark, http_client);
}

} // namespace smsrelay::forward::targets
