#pragma once

#include "app_config.h"
#include <string>

namespace smsrelay {

/**
 * @brief Load application configuration from YAML file
 *
 * @param config_path Path to YAML configuration file
 * @return AppConfig Loaded configuration
 * @throws std::runtime_error if file cannot be loaded or parsed
 */
AppConfig load_config(const std::string &config_path);

/**
 * @brief Load application configuration from default location
 *
 * Tries to load from:
 * 1. ./config/smsrelay.yaml (current directory)
 * 2. ../config/smsrelay.yaml (parent directory, for build output)
 *
 * @return AppConfig Loaded configuration
 * @throws std::runtime_error if file cannot be found or loaded
 */
AppConfig load_config_default();

} // namespace smsrelay
