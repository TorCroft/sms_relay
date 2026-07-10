#pragma once

#include "common/app_config.h"
#include "forward_target.h"
#include "sms_relay/http/http_client.h"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace smsrelay::forward {

/**
 * @brief Factory pattern for creating ForwardTarget instances
 *
 * This factory allows dynamic registration of target types, making it easy
 * to add new forwarding targets without modifying existing code.
 */
class ForwardTargetFactory
{
public:
    using Creator = std::function<std::unique_ptr<ForwardTarget>(
        const ForwardTargetConfig &config,
        const std::shared_ptr<smsrelay::http::HttpClient> &http_client)>;

    /**
     * @brief Create a target from configuration
     * @param config Target configuration
     * @param http_client HTTP client for network operations
     * @return Unique pointer to created target, or nullptr if type not registered
     */
    static std::unique_ptr<ForwardTarget>
    create(const ForwardTargetConfig &config,
           const std::shared_ptr<smsrelay::http::HttpClient> &http_client)
    {
        auto it = creators().find(config.type);
        if (it != creators().end())
        {
            return it->second(config, http_client);
        }
        return nullptr;
    }

    /**
     * @brief Register a target type creator
     * @param type Target type identifier (e.g., "bark", "telegram")
     * @param creator Function that creates the target
     */
    static void register_type(const std::string &type, Creator creator)
    {
        creators()[type] = std::move(creator);
    }

    /**
     * @brief Check if a type is registered
     * @param type Target type identifier
     * @return true if type is registered
     */
    static bool is_registered(const std::string &type)
    {
        return creators().find(type) != creators().end();
    }

    /**
     * @brief Get all registered types
     * @return Vector of registered type names
     */
    static std::vector<std::string> registered_types()
    {
        std::vector<std::string> types;
        for (const auto &[type, _] : creators())
        {
            types.push_back(type);
        }
        return types;
    }

private:
    // Static registry map ( Meyers' singleton pattern for thread safety)
    static std::map<std::string, Creator> &creators()
    {
        static std::map<std::string, Creator> registry;
        return registry;
    }
};

// ============================================================================
// Type Registration Helper
// ============================================================================

/**
 * @brief RAII helper for automatic target type registration
 *
 * Use this in a static context to automatically register target types at
 * startup. Specialized for each target type to handle specific constructor
 * requirements.
 */
template <typename TargetType>
class ForwardTargetRegistrar
{
public:
    ForwardTargetRegistrar(const std::string &type)
    {
        ForwardTargetFactory::register_type(type, &TargetType::create);
    }

private:
    // Static creation function that must be implemented by each target type
    static std::unique_ptr<ForwardTarget>
    create(const ForwardTargetConfig &config,
           const std::shared_ptr<smsrelay::http::HttpClient> &http_client);
};

} // namespace smsrelay::forward
