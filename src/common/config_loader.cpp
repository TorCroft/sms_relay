#include "config_loader.h"
#include <yaml-cpp/yaml.h>
#include <iostream>
#include <fstream>

namespace smsrelay
{
    namespace
    {
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
            return app_config;
        }
        catch (const YAML::Exception &e)
        {
            throw std::runtime_error("Failed to parse YAML: " + std::string(e.what()));
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

        throw std::runtime_error("Configuration file not found in config/smsrelay.yaml");
    }

} // namespace smsrelay
