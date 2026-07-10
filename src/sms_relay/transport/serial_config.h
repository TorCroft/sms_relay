#pragma once

#include <cstdint>
#include <string>

namespace smsrelay::serial {

struct SerialConfig
{
    std::string port;
    uint32_t baudrate;

    SerialConfig(const std::string &p = "/dev/ttyUSB0", uint32_t br = 115200)
        : port(p), baudrate(br) {}
};

} // namespace smsrelay::serial
