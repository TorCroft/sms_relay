#pragma once

#include <string>

namespace smsrelay::callback {

struct CallbackResult
{
    bool success = false;
    int status_code = 0;
    std::string response;
    std::string error_message;
};

} // namespace smsrelay::callback
