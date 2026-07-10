#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace smsrelay {

// =============================================================================
// Serial Configuration
// =============================================================================

struct SerialConfig
{
    std::string port = "/dev/ttyUSB0";
    uint32_t baudrate = 115200;
};

// =============================================================================
// SMS Configuration
// =============================================================================

struct SmsConfig
{
    std::string storage = "ME"; // ME = Equipment, SM = SIM card
    bool auto_delete = false;   // Auto delete after receiving new SMS
    bool text_mode = false;     // false = PDU mode, true = Text mode

    // Timing delays (in milliseconds)
    int read_delay_ms = 50;       // Delay between reading each message
    int read_delay_long_ms = 100; // Longer delay between operations

    // Cache configuration
    bool enable_cache = true; // Enable message caching
};

// =============================================================================
// Forwarding Configuration
// =============================================================================

struct BarkConfig
{
    std::string server = "https://api.day.app"; // Bark server URL
    std::string key;                            // Bark device key
    std::string title = "${sender}";            // Notification title
    std::string body = "${text}";               // Notification body
    std::string sound = "telegraphnotify";      // Notification sound
    std::string icon;                           // Icon URL (optional)
    std::string group = "sms_relay";            // Group for notifications
    std::string url;                            // URL to open when tapped (optional)
    int badge = 1;                              // Badge count
    std::string level = "active";               // iOS interruption level
    int timeout_ms = 5000;                      // HTTP request timeout
};

struct ForwardTargetConfig
{
    std::string type; // "bark", etc.
    bool enabled = true;
    BarkConfig bark; // Bark-specific config (when type == "bark")
};

struct ForwardConfig
{
    bool enabled = false;                     // Master switch for forwarding
    std::vector<ForwardTargetConfig> targets; // List of forwarding targets
};

// =============================================================================
// IPC Server Configuration
// =============================================================================

struct IpcServerConfig
{
    int port = 7896;          // IPC server port
    std::string host = "::1"; // IPC server host (IPv6 loopback)
};

// =============================================================================
// Main Application Configuration
// =============================================================================

struct AppConfig
{
    SerialConfig serial;
    SmsConfig sms;
    ForwardConfig forward;
    IpcServerConfig ipc_server;
};

} // namespace smsrelay
