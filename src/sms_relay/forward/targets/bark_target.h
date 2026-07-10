#pragma once

#include "common/app_config.h"
#include "sms_relay/forward/forward_target.h"
#include "sms_relay/http/http_client.h"
#include <nlohmann/json.hpp>

namespace smsrelay::forward::targets {

/**
 * @brief Bark forwarding target implementation
 *
 * Sends push notifications to iOS devices via Bark API.
 * Bark API format: POST https://api.day.app/{key} with JSON body
 * Uses nlohmann/json for proper UTF-8 encoding and JSON serialization
 */
class BarkTarget : public ForwardTarget
{
public:
    explicit BarkTarget(const BarkConfig &config,
                        std::shared_ptr<http::HttpClient> http_client);

    /**
     * @brief Send notification via Bark API
     * @param sms Incoming SMS to forward
     * @return true if sending was successful
     */
    bool send(const IncomingSms &sms) override;

    /**
     * @brief Get target name
     */
    const char *name() const override { return "Bark"; }

    /**
     * @brief Static creation function for factory pattern
     * @param config Target configuration
     * @param http_client HTTP client for network operations
     * @return Unique pointer to created target
     */
    static std::unique_ptr<ForwardTarget>
    create(const ForwardTargetConfig &config,
           const std::shared_ptr<http::HttpClient> &http_client);

private:
    /**
     * @brief Build Bark API URL
     */
    std::string build_url(const IncomingSms &sms);

    /**
     * @brief Build JSON body for Bark request using nlohmann/json
     * @return JSON string with UTF-8 encoding
     */
    std::string build_body(const IncomingSms &sms);

    BarkConfig config_;
    std::shared_ptr<http::HttpClient> http_client_;
};

} // namespace smsrelay::forward::targets
