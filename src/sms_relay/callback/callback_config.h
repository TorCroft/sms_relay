#pragma once

#include <map>
#include <string>
#include <vector>

namespace smsrelay::callback {

struct CallbackConfig
{
    std::string type; // "http"
    std::string url;
    std::string method = "POST";
    std::map<std::string, std::string> headers;
    std::string body_template;
    int retry_count = 3;
    int retry_delay_ms = 1000;
};

struct CallbackServiceConfig
{
    bool enabled = true;
    std::vector<CallbackConfig> callbacks;
};

} // namespace smsrelay::callback
