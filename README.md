# SMS Relay

A robust SMS forwarding service that receives SMS messages via GSM modem and forwards them to multiple targets (Bark, Telegram, etc.). Includes both a service daemon and CLI tool for SMS management.

## Features

- **SMS Reception**: Real-time SMS receiving via GSM modem over serial port
- **Message Forwarding**: Forward incoming SMS to multiple notification services
  - **Bark**: iOS push notifications (primary target)
  - Extensible architecture for adding more targets (Telegram, Discord, etc.)
- **Multipart SMS**: Automatic concatenation of multi-part messages
- **CLI Tool**: Full-featured command-line interface for SMS management
- **IPC Server**: Service exposes control interface via local TCP
- **Cross-platform**: Support for Linux and Windows

## Architecture

```
┌─────────────────┐        Serial       ┌──────────────┐
│   GSM Modem     │   ◄──────────────►  │  SMS Relay   │
└─────────────────┘       (USB/COM)     │   Service    │
                                        └──────┬───────┘
                                               │
                    ┌──────────────────────────┼───────────────────────┐
                    │                          │                       │
                    ▼                          ▼                       ▼
            ┌───────────────┐         ┌─────────────┐         ┌─────────────┐
            │  IPC Server   │         │Forward Svc  │         │  CLI Tool   │
            │  (Port 7896)  │         │  (Bark...)  │         │ sms_relay_  │
            └───────────────┘         └─────────────┘         │    cli      │
                                                              └─────────────┘
```

### Components

- **Transport Layer**: Serial communication with modem using ASIO
- **AT Session**: Command queue and response dispatcher
- **SMS Service**: Message reading, PDU decoding, multipart handling
- **Forward Service**: Multi-target notification dispatcher
- **IPC Server**: Local TCP server for CLI communication
- **HTTP Client**: For forwarding to web-based services

## Building

### Prerequisites

- C++20 compiler (Clang, GCC, or MSVC)
- CMake 3.20+
- Ninja (recommended) or Make
- yaml-cpp (auto-downloaded if not found)
- libcurl (optional, for HTTP client)

### Build Steps

```bash
# Clone repository
git clone https://github.com/TorCroft/sms_relay.git
cd sms_relay

# Configure (Release build)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build --config Release

# Binaries are in build/bin/
```

### Build on Windows

```bash
# Using Visual Studio
cmake -B build -G "Visual Studio 17 2022" -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

# Using Ninja (recommended)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

## Configuration

The service loads configuration from `config/smsrelay.yaml`. You can also specify a custom path:

```bash
sms_relay config.yaml
```

### Configuration Structure

```yaml
# Serial Port Configuration
serial:
  port: /dev/ttyUSB0         # Linux: /dev/ttyUSB0, Windows: COM3
  baudrate: 115200

# SMS Configuration
sms:
  storage: "ME"              # ME=Equipment, SM=SIM card, MT=Both
  auto_delete: false          # Auto-delete after processing
  text_mode: false           # false=PDU mode (recommended)
  read_delay_ms: 50          # Delay between reads
  enable_cache: true         # Message caching

# Forwarding Configuration
forward:
  enabled: true              # Master switch
  targets:
    - type: bark
      enabled: true
      bark:
        server: "https://api.day.app"
        key: "YOUR_BARK_DEVICE_KEY"
        title: "SMS from ${sender}"
        body: "${text}"
        sound: "telegraphnotify"
        group: "sms_relay"
        badge: 1
        level: "active"
        timeout_ms: 5000

# IPC Server Configuration
server:
  port: 7896                 # CLI connection port
  host: "::1"                # "::1"=loopback, "::"=all interfaces
```

### Template Variables

The following variables can be used in notification templates:

- `${sender}` - Sender phone number
- `${text}` - SMS text content
- `${timestamp}` - SMS timestamp
- `${index}` - SMS index in storage

## Usage

### Starting the Service

```bash
# Using default config
./build/bin/sms_relay

# Using custom config
./build/bin/sms_relay /path/to/config.yaml
```

Expected output:
```
SMS Relay - Port: /dev/ttyUSB0 | Forward: ON

[2026-07-09 10:30:15] Connecting to /dev/ttyUSB0...
[2026-07-09 10:30:16] Connected
[2026-07-09 10:30:17] Modem initialized
[2026-07-09 10:30:18] Ready. Listening for SMS...
```

### Using the CLI Tool

The CLI tool connects to the running service via IPC.

```bash
# List all SMS
./build/bin/sms_relay_cli list

# List only unread messages
./build/bin/sms_relay_cli list --status "REC UNREAD"

# Read specific messages
./build/bin/sms_relay_cli read --index 1,2,5

# Delete messages
./build/bin/sms_relay_cli delete --index 1,2

# Send SMS
./build/bin/sms_relay_cli send --to "+1234567890" --text "Hello World"

# Get service status
./build/bin/sms_relay_cli status
```

### CLI Output Examples

```bash
$ sms_relay_cli list

Total messages: 3

Index  Status       Sender               Time
------------------------------------------------------------
1      READ         +1234567890          2026-07-09 10:30:17
2      UNREAD       giffgaff             2026-07-09 10:15:42
3      READ         +8612345678901       2026-07-09 08:42:25

$ sms_relay_cli read --index 1

========== SMS ==========
From: +1234567890
Time: 2026-07-09 10:30:17
Text: Your verification code is 123456
============================

$ sms_relay_cli status

Service Status:
  Connected: Yes
  Listening: /dev/ttyUSB0
  Message count: 3
```

## Setting Up Bark Forwarding

1. **Install Bark** from the App Store on your iOS device
2. **Get your device key** from the Bark app
3. **Update the config**:
   ```yaml
   forward:
     enabled: true
     targets:
       - type: bark
         enabled: true
         bark:
           key: "your_actual_device_key"
           title: "SMS from ${sender}"
           body: "${text}"
   ```
4. **Restart the service**

### Testing Bark Configuration

```bash
curl -X POST "https://api.day.app/YOUR_KEY/test/test%20message"
```

### Multiple Forwarding Targets

You can configure multiple forwarding targets:

```yaml
forward:
  enabled: true
  targets:
    # Primary iOS device
    - type: bark
      enabled: true
      bark:
        key: "primary_device_key"
        title: "SMS: ${sender}"
        body: "${text}"

    # Backup iOS device
    - type: bark
      enabled: true
      bark:
        key: "backup_device_key"
        title: "SMS Backup"
        body: "From ${sender}: ${text}"
```

## Technical Details

### GSM-7 and PDU Encoding

The service includes a complete GSM 03.40 PDU codec implementation:

- **GSM-7 encoding/decoding**: 7-bit alphabet with escape sequences
- **UCS-2 encoding/decoding**: UTF-16 for non-Latin characters
- **Septet packing/unpacking**: Proper bit-level packing
- **Semi-octet encoding**: For phone numbers and timestamps
- **User Data Header (UDH)**: Message concatenation, port addressing

### Multipart SMS Handling

- Automatic detection and caching of multipart messages
- UDH IE parsing (Concatenation, Port Address)
- Proper septet alignment with UDH
- 2-minute timeout for incomplete messages
- Background thread for non-blocking processing

### Serial Communication

- ASIO-based async I/O
- Custom line parser for AT responses
- Ring buffer for efficient data handling
- URC (Unsolicited Result Code) dispatcher
- Cross-platform serial port handling

## Dependencies

| Dependency | Version | Purpose |
|------------|---------|---------|
| ASIO | 1.34.2 | Async I/O |
| nlohmann_json | v3.12.0 | Json Lib |
| yaml-cpp | master | Configuration |
| CMake | 3.20+ | Build system |
| C++ | 20 | Language standard |

## Platform Support

| Platform | Compilers | Status |
|----------|-----------|--------|
| Linux (Ubuntu 22.04+) | Clang, GCC | ✅ Tested |
| Windows 10/11 | MSVC, Clang, MinGW | ✅ Supported |

## Project Structure

```
sms_relay/
├── src/
│   ├── common/           # Shared utilities (PDU codec, config loader)
│   ├── sms_relay/       # Main service
│   │   ├── at/          # AT command session
│   │   ├── sms/         # SMS service
│   │   ├── forward/     # Forwarding service
│   │   ├── transport/   # Serial transport
│   │   ├── http/        # HTTP client
│   │   └── ipc/         # IPC server
│   └── sms_relay_cli/   # CLI tool
├── config/              # Configuration files
├── CMakeLists.txt       # Build configuration
└── README.md
```

## License

This project follows GSM 03.40 specification for PDU encoding/decoding.

## Contributing

Contributions are welcome! Please ensure:

- Code follows project style (C++20 standard)
- All builds pass on both Linux and Windows
- Features are tested with real hardware
