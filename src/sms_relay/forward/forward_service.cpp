#include "forward_service.h"
#include "sms_relay/forward/targets/bark_target.h"
#include <iostream>
#include <algorithm>

namespace smsrelay::forward {

ForwardService::ForwardService(const ForwardConfig& config)
    : config_(config)
    , http_client_(http::create_default_client())
{
    create_targets();
}

ForwardService::~ForwardService() = default;

int ForwardService::forward(const IncomingSms& sms) {
    if (!config_.enabled || targets_.empty()) {
        return 0;
    }

    int success_count = 0;

    std::lock_guard<std::mutex> lock(targets_mutex_);
    for (auto& target : targets_) {
        try {
            if (target->send(sms)) {
                success_count++;
                std::cout << "[Bark] OK" << std::endl;
            } else {
                std::cout << "[Bark] FAILED" << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "[Bark] Error: " << e.what() << std::endl;
        }
    }

    return success_count;
}

void ForwardService::create_targets() {
    for (const auto& target_config : config_.targets) {
        if (!target_config.enabled) continue;

        auto target = create_target(target_config);
        if (target) {
            targets_.push_back(std::move(target));
        }
    }
}

std::unique_ptr<ForwardTarget> ForwardService::create_target(const ForwardTargetConfig& target_config) {
    if (target_config.type == "bark") {
        return std::make_unique<targets::BarkTarget>(target_config.bark, http_client_);
    }

    std::cerr << "[Forward] Unknown target type: " << target_config.type << std::endl;
    return nullptr;
}

} // namespace smsrelay::forward
