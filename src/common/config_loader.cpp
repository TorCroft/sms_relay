#include "config_loader.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <regex>
#include <yaml-cpp/yaml.h>

namespace smsrelay {
namespace {
// ============================================================================
// Configuration Validation Functions
// ============================================================================

/**
 * @brief Validate serial port configuration
 */
void validate_serial_config(const SerialConfig &config)
{
    if (config.port.empty())
    {
        throw std::runtime_error("Serial port cannot be empty");
    }

    // Check if port format is valid (basic check for common patterns)
    std::regex port_pattern(R"(^(COM\d+|/dev/tty\S+|/dev/ttyUSB\d+)$)",
                            std::regex::icase);
    if (!std::regex_search(config.port, port_pattern))
    {
        std::cerr << "[Config] Warning: Unusual serial port format: " << config.port
                  << std::endl;
    }

    // Validate baudrate range (common values: 9600, 19200, 38400, 57600, 115200,
    // 230400, 460800, 921600)
    constexpr uint32_t MIN_BAUDRATE = 9600;
    constexpr uint32_t MAX_BAUDRATE = 4000000;

    if (config.baudrate < MIN_BAUDRATE || config.baudrate > MAX_BAUDRATE)
    {
        throw std::runtime_error(
            "Invalid baudrate: " + std::to_string(config.baudrate) +
            ". Must be between " + std::to_string(MIN_BAUDRATE) + " and " +
            std::to_string(MAX_BAUDRATE));
    }

    // Warn if using unusual baudrate
    const std::vector<uint32_t> common_baudrates = {
        9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600};
    if (std::find(common_baudrates.begin(), common_baudrates.end(),
                  config.baudrate) == common_baudrates.end())
    {
        std::cerr << "[Config] Warning: Unusual baudrate: " << config.baudrate
                  << std::endl;
    }
}

/**
 * @brief Validate SMS configuration
 */
void validate_sms_config(const SmsConfig &config)
{
    // Validate storage location
    if (config.storage != "ME" && config.storage != "SM" &&
        config.storage != "MT")
    {
        std::cerr << "[Config] Warning: Unusual storage location: "
                  << config.storage << ". Common values are ME, SM, MT"
                  << std::endl;
    }

    // Validate timing delays
    if (config.read_delay_ms < 0 || config.read_delay_ms > 10000)
    {
        throw std::runtime_error(
            "Invalid read_delay_ms: " + std::to_string(config.read_delay_ms) +
            ". Must be between 0 and 10000");
    }

    if (config.read_delay_long_ms < 0 || config.read_delay_long_ms > 60000)
    {
        throw std::runtime_error("Invalid read_delay_long_ms: " +
                                 std::to_string(config.read_delay_long_ms) +
                                 ". Must be between 0 and 60000");
    }
}

/**
 * @brief Validate URL format
 */
void validate_url(const std::string &url, const std::string &field_name)
{
    if (url.empty())
    {
        throw std::runtime_error(field_name + " cannot be empty");
    }

    // Basic URL validation
    std::regex url_pattern(R"(^(https?://).+)");
    if (!std::regex_search(url, url_pattern))
    {
        throw std::runtime_error("Invalid " + field_name + " format: " + url +
                                 ". Must start with http:// or https://");
    }
}

/**
 * @brief Validate Bark configuration
 */
void validate_bark_config(const BarkConfig &config)
{
    if (config.key.empty())
    {
        throw std::runtime_error("Bark key cannot be empty");
    }

    // Key should be reasonable length (Bark keys are typically device-specific
    // tokens)
    if (config.key.length() < 10 || config.key.length() > 256)
    {
        std::cerr << "[Config] Warning: Unusual Bark key length: "
                  << config.key.length() << std::endl;
    }

    validate_url(config.server, "Bark server");

    // Validate timeout
    if (config.timeout_ms < 100 || config.timeout_ms > 60000)
    {
        throw std::runtime_error(
            "Invalid Bark timeout_ms: " + std::to_string(config.timeout_ms) +
            ". Must be between 100 and 60000");
    }

    // Validate optional URL if provided
    if (!config.url.empty())
    {
        validate_url(config.url, "Bark redirect URL");
    }

    // Validate optional icon if provided
    if (!config.icon.empty())
    {
        validate_url(config.icon, "Bark icon URL");
    }
}

/**
 * @brief Validate forward target configuration
 */
void validate_forward_target_config(const ForwardTargetConfig &config)
{
    if (config.type.empty())
    {
        throw std::runtime_error("Forward target type cannot be empty");
    }

    // Validate type-specific configuration
    if (config.enabled)
    {
        if (config.type == "bark")
        {
            validate_bark_config(config.bark);
        }
        else
        {
            std::cerr << "[Config] Warning: Unknown forward target type: "
                      << config.type << std::endl;
        }
    }
}

/**
 * @brief Validate forward configuration
 */
void validate_forward_config(const ForwardConfig &config)
{
    if (!config.enabled)
    {
        return; // Skip validation if forwarding is disabled
    }

    if (config.targets.empty())
    {
        throw std::runtime_error("Forwarding is enabled but no targets configured");
    }

    // Check if at least one target is enabled
    bool has_enabled_target = false;
    for (const auto &target : config.targets)
    {
        if (target.enabled)
        {
            has_enabled_target = true;
            validate_forward_target_config(target);
        }
    }

    if (!has_enabled_target)
    {
        throw std::runtime_error(
            "Forwarding is enabled but no targets are enabled");
    }
}

/**
 * @brief Validate IPC server configuration
 */
void validate_ipc_server_config(const IpcServerConfig &config)
{
    // Validate port range
    if (config.port < 1024 || config.port > 65535)
    {
        throw std::runtime_error(
            "Invalid IPC port: " + std::to_string(config.port) +
            ". Must be between 1024 and 65535");
    }

    // Validate host format
    if (config.host.empty())
    {
        throw std::runtime_error("IPC host cannot be empty");
    }

    // Warn if using public IP (should be localhost for security)
    if (config.host != "::1" && config.host != "127.0.0.1" &&
        config.host != "localhost")
    {
        std::cerr << "[Config] Warning: IPC host is not set to localhost. "
                  << "This may expose the IPC server to network connections. Host: "
                  << config.host << std::endl;
    }
}

/**
 * @brief Validate complete application configuration
 */
void validate_app_config(const AppConfig &config)
{
    try
    {
        validate_serial_config(config.serial);

        validate_sms_config(config.sms);

        validate_forward_config(config.forward);

        validate_ipc_server_config(config.ipc_server);
    }
    catch (const std::exception &e)
    {
        throw std::runtime_error("Configuration validation failed: " +
                                 std::string(e.what()));
    }
}

// ============================================================================
// Configuration Parsing Functions
// ============================================================================

/**
 * @brief Parse SerialConfig from YAML node
 */
SerialConfig parse_serial_config(const YAML::Node &node)
{
    SerialConfig config;

    if (node["port"])
    {
        config.port = node["port"].as<std::string>();
    }

    if (node["baudrate"])
    {
        config.baudrate = node["baudrate"].as<uint32_t>();
    }

    return config;
}

/**
 * @brief Parse SmsConfig from YAML node
 */
SmsConfig parse_sms_config(const YAML::Node &node)
{
    SmsConfig config;

    if (node["storage"])
    {
        config.storage = node["storage"].as<std::string>();
    }

    if (node["auto_delete"])
    {
        config.auto_delete = node["auto_delete"].as<bool>();
    }

    if (node["text_mode"])
    {
        config.text_mode = node["text_mode"].as<bool>();
    }

    if (node["read_delay_ms"])
    {
        config.read_delay_ms = node["read_delay_ms"].as<int>();
    }

    if (node["read_delay_long_ms"])
    {
        config.read_delay_long_ms = node["read_delay_long_ms"].as<int>();
    }

    if (node["enable_cache"])
    {
        config.enable_cache = node["enable_cache"].as<bool>();
    }

    return config;
}

/**
 * @brief Parse BarkConfig from YAML node
 */
BarkConfig parse_bark_config(const YAML::Node &node)
{
    BarkConfig config;

    if (node["server"])
    {
        config.server = node["server"].as<std::string>();
    }

    if (node["key"])
    {
        config.key = node["key"].as<std::string>();
    }

    if (node["title"])
    {
        config.title = node["title"].as<std::string>();
    }

    if (node["body"])
    {
        config.body = node["body"].as<std::string>();
    }

    if (node["sound"])
    {
        config.sound = node["sound"].as<std::string>();
    }

    if (node["icon"])
    {
        config.icon = node["icon"].as<std::string>();
    }

    if (node["group"])
    {
        config.group = node["group"].as<std::string>();
    }

    if (node["url"])
    {
        config.url = node["url"].as<std::string>();
    }

    if (node["badge"])
    {
        config.badge = node["badge"].as<int>();
    }

    if (node["level"])
    {
        config.level = node["level"].as<std::string>();
    }

    if (node["timeout_ms"])
    {
        config.timeout_ms = node["timeout_ms"].as<int>();
    }

    return config;
}

/**
 * @brief Parse ForwardTargetConfig from YAML node
 */
ForwardTargetConfig parse_forward_target_config(const YAML::Node &node)
{
    ForwardTargetConfig config;

    if (node["type"])
    {
        config.type = node["type"].as<std::string>();
    }

    if (node["enabled"])
    {
        config.enabled = node["enabled"].as<bool>();
    }

    // Parse type-specific config
    if (config.type == "bark" && node["bark"])
    {
        config.bark = parse_bark_config(node["bark"]);
    }

    return config;
}

/**
 * @brief Parse ForwardConfig from YAML node
 */
ForwardConfig parse_forward_config(const YAML::Node &node)
{
    ForwardConfig config;

    if (node["enabled"])
    {
        config.enabled = node["enabled"].as<bool>();
    }

    if (node["targets"] && node["targets"].IsSequence())
    {
        for (const auto &target_node : node["targets"])
        {
            auto target = parse_forward_target_config(target_node);
            config.targets.push_back(target);
        }
    }

    return config;
}

/**
 * @brief Parse IpcServerConfig from YAML node
 */
IpcServerConfig parse_ipc_server_config(const YAML::Node &node)
{
    IpcServerConfig config;

    if (node["port"])
    {
        config.port = node["port"].as<int>();
    }

    if (node["host"])
    {
        config.host = node["host"].as<std::string>();
    }

    return config;
}

} // anonymous namespace

AppConfig load_config(const std::string &config_path)
{
    std::cout << "[Config] Loading from: " << config_path << std::endl;

    try
    {
        YAML::Node config = YAML::LoadFile(config_path);
        AppConfig app_config;

        // Parse serial configuration
        if (config["serial"])
        {
            app_config.serial = parse_serial_config(config["serial"]);
        }

        // Parse SMS configuration
        if (config["sms"])
        {
            app_config.sms = parse_sms_config(config["sms"]);
        }

        // Parse forwarding configuration
        if (config["forward"])
        {
            app_config.forward = parse_forward_config(config["forward"]);
        }

        // Parse IPC server configuration
        if (config["server"])
        {
            app_config.ipc_server = parse_ipc_server_config(config["server"]);
        }

        std::cout << "[Config] Loaded successfully" << std::endl;

        // Validate all configuration values
        validate_app_config(app_config);

        return app_config;
    }
    catch (const YAML::Exception &e)
    {
        throw std::runtime_error("Failed to parse YAML: " + std::string(e.what()));
    }
    catch (const std::exception &e)
    {
        // Re-throw validation errors with clear context
        throw;
    }
}

AppConfig load_config_default()
{
    std::string default_config_path = "./config/smsrelay.yaml";

    std::ifstream file(default_config_path);
    if (file.good())
    {
        return load_config(default_config_path);
    }

    throw std::runtime_error(
        "Configuration file not found in config/smsrelay.yaml");
}

} // namespace smsrelay
