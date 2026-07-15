#include "interactive_shell.h"
#include "common/ipc/ipc_protocol.h"
#include "common/ipc/ipc_serialization.h"
#include <atomic>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <sstream>

// Platform-specific UTF-8 setup
#ifdef _WIN32
#include <Windows.h>
#else
#include <locale.h>
#endif

// ============================================================================
// Signal Handling for Interactive Shell
// ============================================================================

namespace {
std::atomic<bool> shell_interrupt_requested{false};

void shell_signal_handler(int signal)
{
    if (signal == SIGINT || signal == SIGTERM)
    {
        // Only print on first signal
        static std::atomic<bool> first_signal{true};
        if (first_signal.exchange(false))
        {
            std::cout << "\n[Signal] Interrupt received. Type 'exit' to quit or press Ctrl+C again to force exit." << std::endl;
            shell_interrupt_requested.store(true);
        }
        else
        {
            // Force exit on second signal
            std::cout << "\n[Signal] Force exit..." << std::endl;
            std::exit(0);
        }
    }
}

void setup_shell_signal_handlers()
{
    std::signal(SIGINT, shell_signal_handler);
    std::signal(SIGTERM, shell_signal_handler);
}

void reset_shell_signal_handlers()
{
    std::signal(SIGINT, SIG_DFL);
    std::signal(SIGTERM, SIG_DFL);
}
}

using namespace smsrelay::cli;
using namespace smsrelay::ipc;

// Replxx type alias for convenience
using Replxx = replxx::Replxx;

// ============================================================================
// Utility Functions
// ============================================================================

/**
 * @brief Parse comma-separated indices
 */
static std::vector<uint8_t> parse_indices(const std::string &indices_str)
{
    std::vector<uint8_t> result;
    std::stringstream ss(indices_str);
    std::string token;

    while (std::getline(ss, token, ','))
    {
        try
        {
            int idx = std::stoi(token);
            if (idx >= 0 && idx <= 255)
            {
                result.push_back(static_cast<uint8_t>(idx));
            }
        }
        catch (...)
        {
            std::cerr << "Error: Invalid index '" << token << "'" << std::endl;
        }
    }

    return result;
}

/**
 * @brief Convert status code to readable string
 */
static std::string status_to_string(uint8_t status)
{
    switch (status)
    {
        case 0:
            return "UNREAD";
        case 1:
            return "READ";
        case 2:
            return "UNSENT";
        case 3:
            return "SENT";
        default:
            return "UNKNOWN";
    }
}

/**
 * @brief Print SMS info in table format
 */
static void print_sms_table(const std::vector<SmsInfo> &messages)
{
    if (messages.empty())
    {
        std::cout << "No messages found" << std::endl;
        return;
    }

    // Print table header
    std::cout << "Index  Status       Sender               Time" << std::endl;
    std::cout << "------------------------------------------------------------"
              << std::endl;

    // Print each message
    for (const auto &sms : messages)
    {
        std::cout << std::left;
        // Index column (width 6)
        std::cout << std::setw(6) << static_cast<int>(sms.index) << " ";
        // Status column (width 12)
        std::cout << std::setw(12) << status_to_string(sms.status) << " ";
        // Sender column (width 20)
        std::cout << std::setw(20) << (sms.sender.length() > 20 ? sms.sender.substr(0, 17) + "..." : sms.sender) << " ";
        // Time column
        std::cout << sms.timestamp << std::endl;
    }
}

/**
 * @brief Print SMS info (full format for read)
 */
static void print_sms_info_full(const SmsInfo &sms)
{
    std::cout << "\n========== SMS ==========" << std::endl;
    std::cout << "From: " << sms.sender << std::endl;
    std::cout << "Time: " << sms.timestamp << std::endl;
    std::cout << "Text: " << sms.text << std::endl;
    std::cout << "============================\n"
              << std::endl;
}

// ============================================================================
// InteractiveShell Implementation
// ============================================================================

InteractiveShell::InteractiveShell(const std::string &server)
    : server_(server), running_(true)
{
    parse_server_string(server, host_, port_);
}

InteractiveShell::~InteractiveShell()
{
    if (client_ && client_->is_connected())
    {
        client_->disconnect();
    }
}

void InteractiveShell::parse_server_string(const std::string &server, std::string &host, int &port)
{
    size_t colon_pos = server.find_last_of(':');
    if (colon_pos != std::string::npos)
    {
        host = server.substr(0, colon_pos);
        try
        {
            port = std::stoi(server.substr(colon_pos + 1));
        }
        catch (...)
        {
            std::cerr << "Warning: Invalid port in server string, using default port "
                      << IPCServerDefaults::DEFAULT_PORT << std::endl;
            port = IPCServerDefaults::DEFAULT_PORT;
        }
    }
    else
    {
        host = server;
    }

    // Handle IPv6 addresses in brackets (e.g., "[::1]:7896")
    if (!host.empty() && host[0] == '[' && host[host.length() - 1] == ']')
    {
        host = host.substr(1, host.length() - 2);
    }
}

void InteractiveShell::initialize_repl()
{
    replx_ = std::make_unique<replxx::Replxx>();

    // Set max history size
    replx_->set_max_history_size(100);

    // Set up completion and highlighting
    setup_completion();
    setup_highlighting();
}

void InteractiveShell::setup_completion()
{
    // Register command completion
    std::vector<std::string> commands = {
        "list", "read", "delete", "send", "status", "help", "exit", "reconnect"};

    replx_->set_completion_callback([&](const std::string &input, int &contextLen) {
        std::vector<Replxx::Completion> completions;

        // Find the last word boundary
        size_t cursor_pos = input.length();
        size_t word_start = cursor_pos;

        while (word_start > 0 && input[word_start - 1] != ' ')
        {
            word_start--;
        }

        std::string prefix = input.substr(word_start);

        // Complete commands
        if (word_start == 0 || input[word_start - 1] == ' ')
        {
            for (const auto &cmd : commands)
            {
                if (cmd.rfind(prefix, 0) == 0)
                {
                    completions.emplace_back(cmd.c_str());
                }
            }
            contextLen = static_cast<int>(word_start);
        }

        return completions;
    });
}

void InteractiveShell::setup_highlighting()
{
    // Set up syntax highlighting for commands
    replx_->set_highlighter_callback([&](const std::string &input, std::vector<Replxx::Color> &colors) {
        // Simple command highlighting
        std::vector<std::string> commands = {
            "list", "read", "delete", "send", "status", "help", "exit", "reconnect"};

        // Initialize all colors to DEFAULT
        colors.resize(input.length(), Replxx::Color::DEFAULT);

        // Find and highlight commands
        size_t i = 0;
        while (i < input.length())
        {
            // Skip leading spaces
            while (i < input.length() && input[i] == ' ')
            {
                ++i;
            }

            // Find word boundaries
            size_t word_start = i;
            while (i < input.length() && input[i] != ' ')
            {
                ++i;
            }
            size_t word_end = i;

            if (word_start < word_end)
            {
                std::string word = input.substr(word_start, word_end - word_start);

                // Check if it matches a command
                for (const auto &cmd : commands)
                {
                    if (word == cmd)
                    {
                        // Color the command in BRIGHTCYAN
                        for (size_t j = word_start; j < word_end && j < colors.size(); ++j)
                        {
                            colors[j] = Replxx::Color::BRIGHTCYAN;
                        }
                        break;
                    }
                }
            }
        }
    });
}

bool InteractiveShell::connect_to_server()
{
    client_ = std::make_unique<IpcClient>(host_, port_);

    if (!client_->connect())
    {
        std::cerr << "Error: Failed to connect to sms_relay service at " << server_ << std::endl;
        std::cerr << "Make sure sms_relay is running" << std::endl;
        return false;
    }

    return true;
}

void InteractiveShell::parse_command_line(const std::string &input, std::string &command,
                                          std::vector<std::string> &args)
{
    std::stringstream ss(input);
    std::string token;

    if (ss >> command)
    {
        while (ss >> token)
        {
            args.push_back(token);
        }
    }
}

void InteractiveShell::print_welcome()
{
    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << "  SMS Relay CLI - Interactive Mode\n";
    std::cout << "  Connected to: " << server_ << "\n";
    std::cout << "========================================\n";
    std::cout << "Type 'help' for available commands\n";
    std::cout << "Type 'exit' to quit\n\n";
}

void InteractiveShell::print_help()
{
    std::cout << "\nAvailable commands:\n\n";
    std::cout << "  list [--status STATUS]       List all SMS (status: ALL, REC UNREAD, etc.)\n";
    std::cout << "  read --index INDICES         Read specific SMS (comma-separated: 1,2,3)\n";
    std::cout << "  delete --index INDICES       Delete SMS (comma-separated: 1,2,3)\n";
    std::cout << "  send --to NUMBER --text TEXT Send new SMS\n";
    std::cout << "  status                          Get service status\n";
    std::cout << "  reconnect                       Reconnect to server\n";
    std::cout << "  help                            Show this help\n";
    std::cout << "  exit                            Exit interactive mode\n\n";
    std::cout << "Tips:\n";
    std::cout << "  - Use TAB for command completion\n";
    std::cout << "  - Use UP/DOWN arrows for command history\n";
    std::cout << "  - Commands are highlighted in cyan color\n\n";
}

int InteractiveShell::run()
{
    // Setup signal handlers
    setup_shell_signal_handlers();

    // Initialize REPL
    initialize_repl();

    // Connect to server
    if (!connect_to_server())
    {
        reset_shell_signal_handlers();
        return 1;
    }

    // Print welcome message
    print_welcome();

    // Main command loop
    while (running_)
    {
        // Check for interrupt signal
        if (shell_interrupt_requested.load())
        {
            std::cout << "\nInterrupt detected. Exiting gracefully..." << std::endl;
            shell_interrupt_requested.store(false);
            break;
        }

        // Prompt
        std::string prompt = "sms> ";

        // Read input
        auto *input = replx_->input(prompt);

        if (input == nullptr)
        {
            // EOF (Ctrl+D on Unix, Ctrl+Z on Windows)
            std::cout << "\n";
            break;
        }

        std::string input_str(input);

        // Skip empty lines
        if (input_str.empty())
        {
            continue;
        }

        // Add to history
        replx_->history_add(input_str);

        // Parse and execute command
        std::string command;
        std::vector<std::string> args;
        parse_command_line(input_str, command, args);

        if (!command.empty())
        {
            // Execute command
            auto handler = get_command_handler(command);
            if (handler)
            {
                try
                {
                    int exit_code = handler(args);
                    if (exit_code != 0 && command != "exit")
                    {
                        std::cout << "Command failed with exit code " << exit_code << "\n";
                    }
                }
                catch (const std::exception &e)
                {
                    std::cerr << "Exception: " << e.what() << std::endl;
                }
            }
            else
            {
                std::cerr << "Unknown command: " << command << std::endl;
                std::cerr << "Type 'help' for available commands\n";
            }
        }
    }

    // Cleanup
    if (client_ && client_->is_connected())
    {
        std::cout << "Disconnecting from server..." << std::endl;
        client_->disconnect();
    }

    std::cout << "Interactive shell exited." << std::endl;

    // Reset signal handlers
    reset_shell_signal_handlers();

    return 0;
}

int InteractiveShell::execute_command(const std::string &command_line)
{
    std::string command;
    std::vector<std::string> args;
    parse_command_line(command_line, command, args);

    if (!command.empty())
    {
        auto handler = get_command_handler(command);
        if (handler)
        {
            try
            {
                return handler(args);
            }
            catch (const std::exception &e)
            {
                std::cerr << "Exception: " << e.what() << std::endl;
                return 1;
            }
        }
        else
        {
            std::cerr << "Unknown command: " << command << std::endl;
            return 1;
        }
    }

    return 0;
}

std::function<int(const std::vector<std::string> &)>
InteractiveShell::get_command_handler(const std::string &command)
{
    static const std::map<std::string, std::function<int(const std::vector<std::string> &)>>
        handlers = {
            {"exit", [this](const auto &args) { return handle_exit(args); }},
            {"help", [this](const auto &args) { return handle_help(args); }},
            {"list", [this](const auto &args) { return handle_list(args); }},
            {"read", [this](const auto &args) { return handle_read(args); }},
            {"delete", [this](const auto &args) { return handle_delete(args); }},
            {"send", [this](const auto &args) { return handle_send(args); }},
            {"status", [this](const auto &args) { return handle_status(args); }},
            {"reconnect", [this](const auto &args) { return handle_reconnect(args); }},
        };

    auto it = handlers.find(command);
    if (it != handlers.end())
    {
        return it->second;
    }
    return nullptr;
}

int InteractiveShell::handle_exit(const std::vector<std::string> & /*args*/)
{
    running_ = false;
    std::cout << "Exiting...\n";

    // Disconnect from server
    if (client_ && client_->is_connected())
    {
        std::cout << "Closing connection..." << std::endl;
        client_->disconnect();
    }

    std::cout << "Goodbye!\n";
    return 0;
}

int InteractiveShell::handle_help(const std::vector<std::string> & /*args*/)
{
    print_help();
    return 0;
}

int InteractiveShell::handle_list(const std::vector<std::string> &args)
{
    if (!client_ || !client_->is_connected())
    {
        std::cerr << "Error: Not connected to server" << std::endl;
        return 1;
    }

    std::string status = "ALL";

    // Parse --status option
    for (size_t i = 0; i < args.size(); ++i)
    {
        if (args[i] == "--status" && i + 1 < args.size())
        {
            status = args[i + 1];
        }
    }

    // Build request
    auto payload = PayloadSerializer::serialize_list({status});
    auto request = IpcSerializer::serialize_request(CommandType::LIST_SMS, 0, payload);

    // Send and receive
    auto response = client_->send_command(request);
    if (response.empty())
    {
        std::cerr << "Error: No response from server" << std::endl;
        return 1;
    }

    // Parse response
    size_t offset = 0;
    uint32_t magic, length, status_code, sequence_id, count;

    if (!IpcSerializer::deserialize_u32(response.data(), response.size(), magic, offset) ||
        !IpcSerializer::deserialize_u32(response.data(), response.size(), length, offset) ||
        !IpcSerializer::deserialize_u32(response.data(), response.size(), status_code, offset) ||
        !IpcSerializer::deserialize_u32(response.data(), response.size(), sequence_id, offset))
    {
        std::cerr << "Error: Invalid response" << std::endl;
        return 1;
    }

    if (static_cast<Status>(status_code) != Status::SUCCESS)
    {
        std::cerr << "Error: Server returned error status" << std::endl;
        return 1;
    }

    if (!IpcSerializer::deserialize_u32(response.data(), response.size(), count, offset))
    {
        std::cerr << "Error: Invalid response" << std::endl;
        return 1;
    }

    std::cout << "Total messages: " << count << "\n"
              << std::endl;

    // Collect all messages first
    std::vector<SmsInfo> messages;
    for (uint32_t i = 0; i < count; ++i)
    {
        SmsInfo sms;
        if (PayloadSerializer::deserialize_sms_info(response.data(), response.size(), sms, offset))
        {
            messages.push_back(sms);
        }
    }

    // Print in table format
    print_sms_table(messages);

    return 0;
}

int InteractiveShell::handle_read(const std::vector<std::string> &args)
{
    if (!client_ || !client_->is_connected())
    {
        std::cerr << "Error: Not connected to server" << std::endl;
        return 1;
    }

    std::vector<uint8_t> indices;

    // Parse --index options
    for (size_t i = 0; i < args.size(); ++i)
    {
        if (args[i] == "--index" && i + 1 < args.size())
        {
            auto idx_list = parse_indices(args[i + 1]);
            indices.insert(indices.end(), idx_list.begin(), idx_list.end());
        }
    }

    if (indices.empty())
    {
        std::cerr << "Error: No indices specified" << std::endl;
        return 1;
    }

    // Build request
    auto payload = PayloadSerializer::serialize_read({indices});
    auto request = IpcSerializer::serialize_request(CommandType::READ_SMS, 0, payload);

    // Send and receive
    auto response = client_->send_command(request);
    if (response.empty())
    {
        std::cerr << "Error: No response from server" << std::endl;
        return 1;
    }

    // Parse response
    size_t offset = 0;
    uint32_t magic, length, status_code, sequence_id, count;

    if (!IpcSerializer::deserialize_u32(response.data(), response.size(), magic, offset) ||
        !IpcSerializer::deserialize_u32(response.data(), response.size(), length, offset) ||
        !IpcSerializer::deserialize_u32(response.data(), response.size(), status_code, offset) ||
        !IpcSerializer::deserialize_u32(response.data(), response.size(), sequence_id, offset))
    {
        std::cerr << "Error: Invalid response" << std::endl;
        return 1;
    }

    if (static_cast<Status>(status_code) != Status::SUCCESS)
    {
        std::cerr << "Error: Server returned error status" << std::endl;
        return 1;
    }

    if (!IpcSerializer::deserialize_u32(response.data(), response.size(), count, offset))
    {
        std::cerr << "Error: Invalid response" << std::endl;
        return 1;
    }

    if (count == 0)
    {
        std::cout << "No messages found" << std::endl;
        return 0;
    }

    for (uint32_t i = 0; i < count; ++i)
    {
        SmsInfo sms;
        if (PayloadSerializer::deserialize_sms_info(response.data(), response.size(), sms, offset))
        {
            print_sms_info_full(sms);
        }
    }

    return 0;
}

int InteractiveShell::handle_delete(const std::vector<std::string> &args)
{
    if (!client_ || !client_->is_connected())
    {
        std::cerr << "Error: Not connected to server" << std::endl;
        return 1;
    }

    std::vector<uint8_t> indices;

    // Parse --index options
    for (size_t i = 0; i < args.size(); ++i)
    {
        if (args[i] == "--index" && i + 1 < args.size())
        {
            auto idx_list = parse_indices(args[i + 1]);
            indices.insert(indices.end(), idx_list.begin(), idx_list.end());
        }
    }

    if (indices.empty())
    {
        std::cerr << "Error: No indices specified" << std::endl;
        return 1;
    }

    // Build request
    auto payload = PayloadSerializer::serialize_delete({indices});
    auto request = IpcSerializer::serialize_request(CommandType::DELETE_SMS, 0, payload);

    // Send and receive
    auto response = client_->send_command(request);
    if (response.empty())
    {
        std::cerr << "Error: No response from server" << std::endl;
        return 1;
    }

    // Parse response
    size_t offset = 0;
    uint32_t magic, length, status_code, sequence_id;

    if (!IpcSerializer::deserialize_u32(response.data(), response.size(), magic, offset) ||
        !IpcSerializer::deserialize_u32(response.data(), response.size(), length, offset) ||
        !IpcSerializer::deserialize_u32(response.data(), response.size(), status_code, offset) ||
        !IpcSerializer::deserialize_u32(response.data(), response.size(), sequence_id, offset))
    {
        std::cerr << "Error: Invalid response" << std::endl;
        return 1;
    }

    if (static_cast<Status>(status_code) != Status::SUCCESS)
    {
        std::cerr << "Error: Delete operation failed" << std::endl;
        return 1;
    }

    // Parse deleted and failed indices
    std::vector<uint8_t> deleted, failed;
    IpcSerializer::deserialize_vec(response.data(), response.size(), deleted, offset);
    IpcSerializer::deserialize_vec(response.data(), response.size(), failed, offset);

    std::cout << "Delete operation completed:" << std::endl;
    if (!deleted.empty())
    {
        std::cout << "  Deleted: ";
        for (uint8_t idx : deleted)
        {
            std::cout << static_cast<int>(idx) << " ";
        }
        std::cout << std::endl;
    }
    if (!failed.empty())
    {
        std::cout << "  Failed: ";
        for (uint8_t idx : failed)
        {
            std::cout << static_cast<int>(idx) << " ";
        }
        std::cout << std::endl;
    }

    return 0;
}

int InteractiveShell::handle_send(const std::vector<std::string> &args)
{
    if (!client_ || !client_->is_connected())
    {
        std::cerr << "Error: Not connected to server" << std::endl;
        return 1;
    }

    std::string recipient;
    std::string text;

    // Parse --to and --text options
    for (size_t i = 0; i < args.size(); ++i)
    {
        if (args[i] == "--to" && i + 1 < args.size())
        {
            recipient = args[i + 1];
        }
        else if (args[i] == "--text" && i + 1 < args.size())
        {
            text = args[i + 1];
        }
    }

    if (recipient.empty() || text.empty())
    {
        std::cerr << "Error: Both --to and --text must be specified" << std::endl;
        return 1;
    }

    // Build request
    auto payload = PayloadSerializer::serialize_send({recipient, text});
    auto request = IpcSerializer::serialize_request(CommandType::SEND_SMS, 0, payload);

    // Send and receive
    auto response = client_->send_command(request);
    if (response.empty())
    {
        std::cerr << "Error: No response from server" << std::endl;
        return 1;
    }

    // Parse response
    size_t offset = 0;
    uint32_t magic, length, status_code, sequence_id;

    if (!IpcSerializer::deserialize_u32(response.data(), response.size(), magic, offset) ||
        !IpcSerializer::deserialize_u32(response.data(), response.size(), length, offset) ||
        !IpcSerializer::deserialize_u32(response.data(), response.size(), status_code, offset) ||
        !IpcSerializer::deserialize_u32(response.data(), response.size(), sequence_id, offset))
    {
        std::cerr << "Error: Invalid response" << std::endl;
        return 1;
    }

    if (static_cast<Status>(status_code) != Status::SUCCESS)
    {
        std::cerr << "Error: Failed to send SMS" << std::endl;
        std::string error_msg;
        if (IpcSerializer::deserialize_str(response.data(), response.size(), error_msg, offset))
        {
            std::cerr << "  " << error_msg << std::endl;
        }
        return 1;
    }

    std::cout << "SMS sent successfully" << std::endl;
    return 0;
}

int InteractiveShell::handle_status(const std::vector<std::string> & /*args*/)
{
    if (!client_ || !client_->is_connected())
    {
        std::cerr << "Error: Not connected to server" << std::endl;
        return 1;
    }

    // Build request
    auto payload = std::vector<uint8_t>{};
    auto request = IpcSerializer::serialize_request(CommandType::STATUS, 0, payload);

    // Send and receive
    auto response = client_->send_command(request);
    if (response.empty())
    {
        std::cerr << "Error: No response from server" << std::endl;
        return 1;
    }

    // Parse response
    size_t offset = 0;
    uint32_t magic, length, status_code, sequence_id;

    if (!IpcSerializer::deserialize_u32(response.data(), response.size(), magic, offset) ||
        !IpcSerializer::deserialize_u32(response.data(), response.size(), length, offset) ||
        !IpcSerializer::deserialize_u32(response.data(), response.size(), status_code, offset) ||
        !IpcSerializer::deserialize_u32(response.data(), response.size(), sequence_id, offset))
    {
        std::cerr << "Error: Invalid response" << std::endl;
        return 1;
    }

    if (static_cast<Status>(status_code) != Status::SUCCESS)
    {
        std::cerr << "Error: Failed to get status" << std::endl;
        return 1;
    }

    uint8_t connected = 0;
    std::string port_str;
    uint32_t msg_count = 0;

    IpcSerializer::deserialize_u8(response.data(), response.size(), connected, offset);
    IpcSerializer::deserialize_str(response.data(), response.size(), port_str, offset);
    IpcSerializer::deserialize_u32(response.data(), response.size(), msg_count, offset);

    std::cout << "Service Status:" << std::endl;
    std::cout << "  Connected: " << (connected ? "Yes" : "No") << std::endl;
    std::cout << "  Listening: " << port_str << std::endl;
    std::cout << "  Message count: " << msg_count << std::endl;

    return 0;
}

int InteractiveShell::handle_reconnect(const std::vector<std::string> & /*args*/)
{
    std::cout << "Reconnecting to " << server_ << "..." << std::endl;

    // Disconnect if connected
    if (client_ && client_->is_connected())
    {
        client_->disconnect();
    }

    // Reconnect
    if (!connect_to_server())
    {
        std::cerr << "Reconnection failed" << std::endl;
        return 1;
    }

    std::cout << "Reconnected successfully" << std::endl;
    return 0;
}
