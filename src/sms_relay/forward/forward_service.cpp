#include "forward_service.h"
#include "common/app_config.h"
#include "sms_relay/forward/target_factory.h"
#include "sms_relay/forward/targets/bark_target.h"
#include <iostream>

// Register Bark target type with factory using static method
namespace {
using smsrelay::ForwardTargetConfig;
using smsrelay::forward::ForwardTarget;
using smsrelay::http::HttpClient;

// Register Bark target using its static create method
smsrelay::forward::ForwardTargetFactory::Creator bark_creator =
    [](const ForwardTargetConfig &config,
       const std::shared_ptr<HttpClient> &http_client)
    -> std::unique_ptr<ForwardTarget> {
    return smsrelay::forward::targets::BarkTarget::create(config, http_client);
};

// Static registration
struct BarkTargetRegistrar
{
    BarkTargetRegistrar()
    {
        smsrelay::forward::ForwardTargetFactory::register_type("bark",
                                                               bark_creator);
    }
};

static BarkTargetRegistrar registrar;
} // namespace

namespace smsrelay::forward {

ForwardService::ForwardService(const ForwardConfig &config)
    : config_(config), http_client_(http::create_default_client())
{
    create_targets();
}

ForwardService::~ForwardService() = default;

int ForwardService::forward(const IncomingSms &sms)
{
    if (!config_.enabled || targets_.empty())
    {
        return 0;
    }

    int success_count = 0;

    std::lock_guard<std::mutex> lock(targets_mutex_);
    for (auto &target : targets_)
    {
        try
        {
            if (target->send(sms))
            {
                success_count++;
                std::cout << "[Bark] OK" << std::endl;
            }
            else
            {
                std::cout << "[Bark] FAILED" << std::endl;
            }
        }
        catch (const std::exception &e)
        {
            std::cerr << "[Bark] Error: " << e.what() << std::endl;
        }
    }

    return success_count;
}

void ForwardService::create_targets()
{
    for (const auto &target_config : config_.targets)
    {
        if (!target_config.enabled)
            continue;

        auto target = create_target(target_config);
        if (target)
        {
            targets_.push_back(std::move(target));
        }
    }
}

std::unique_ptr<ForwardTarget>
ForwardService::create_target(const ForwardTargetConfig &target_config)
{
    // Use factory to create target (supports dynamic registration)
    auto target = ForwardTargetFactory::create(target_config, http_client_);

    if (!target)
    {
        std::cerr << "[Forward] Unknown target type: " << target_config.type
                  << std::endl;
        std::cerr << "[Forward] Available types: ";
        for (const auto &type : ForwardTargetFactory::registered_types())
        {
            std::cerr << type << " ";
        }
        std::cerr << std::endl;
    }

    return target;
}

} // namespace smsrelay::forward
