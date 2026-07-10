#pragma once

#include "common/app_config.h"
#include "sms_relay/forward/forward_target.h"
#include "sms_relay/http/http_client.h"
#include "sms_relay/sms/sms_service.h"
#include <memory>
#include <mutex>
#include <vector>

namespace smsrelay::forward {

/**
 * @brief Forward service for sending notifications
 *
 * Manages multiple forwarding targets and sends notifications
 * when new SMS messages arrive.
 */
class ForwardService
{
public:
    /**
     * @brief Constructor
     * @param config Forward configuration
     */
    explicit ForwardService(const ForwardConfig &config);

    /**
     * @brief Destructor
     */
    ~ForwardService();

    /**
     * @brief Forward SMS to all configured targets
     * @param sms Incoming SMS to forward
     * @return Number of targets that successfully received the notification
     */
    int forward(const IncomingSms &sms);

    /**
     * @brief Check if forwarding is enabled
     */
    bool is_enabled() const { return config_.enabled; }

    /**
     * @brief Get number of configured targets
     */
    size_t target_count() const { return targets_.size(); }

private:
    /**
     * @brief Create forwarding targets from configuration
     */
    void create_targets();

    /**
     * @brief Create a single target from config
     */
    std::unique_ptr<ForwardTarget>
    create_target(const ForwardTargetConfig &target_config);

    ForwardConfig config_;
    std::vector<std::unique_ptr<ForwardTarget>> targets_;
    std::shared_ptr<http::HttpClient> http_client_;
    mutable std::mutex targets_mutex_;
};

} // namespace smsrelay::forward
