/**
 * SMS Relay CLI - Command-line interface for sms_relay service
 *
 * Usage:
 *   sms_relay_cli <command> [options]
 *
 * Commands:
 *   list [--status STATUS]       List all SMS
 *   read --index INDICES         Read specific SMS
 *   delete --index INDICES       Delete SMS
 *   send --to NUMBER --text TEXT  Send SMS
 *   status                          Get service status
 *
 * Options:
 *   --server SERVER              Server address in "host:port" format
 *                                 (default: ::1:7896)
 *
 * Default port: 7896 (defined by smsrelay::IPCServerDefaults::DEFAULT_PORT constant)
 */

#include "common/app_config.h"
#include "common/ipc/ipc_protocol.h"
#include "common/ipc/ipc_serialization.h"
#include "ipc/ipc_client.h"
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// Default server configuration (shared with server via app_config.h)
// Use fully qualified names: smsrelay::IPCServerDefaults::*

using namespace smsrelay::cli;
using namespace smsrelay::ipc;

// ============================================================================
// Utility Functions
// ============================================================================

/**
 * @brief Parse comma-separated indices
 */
std::vector<uint8_t> parse_indices(const std::string &indices_str)
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
std::string status_to_string(uint8_t status)
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
 * @brief Print SMS info (short format for list)
 */
void print_sms_info_short(const SmsInfo &sms)
{
    std::cout << "  [" << static_cast<int>(sms.index) << "] "
              << "From: " << sms.sender << " | Text: " << sms.text.substr(0, 50)
              << (sms.text.length() > 50 ? "..." : "")
              << " | Time: " << sms.timestamp << std::endl;
}

/**
 * @brief Print SMS info in table format
 */
void print_sms_table(const std::vector<SmsInfo> &messages)
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
void print_sms_info_full(const SmsInfo &sms)
{
    std::cout << "\n========== SMS ==========" << std::endl;
    std::cout << "From: " << sms.sender << std::endl;
    std::cout << "Time: " << sms.timestamp << std::endl;
    std::cout << "Text: " << sms.text << std::endl;
    std::cout << "============================\n" << std::endl;
}

// ============================================================================
// Command Handler
// ============================================================================

/**
 * @brief Base class for command handlers
 */
class CommandHandler
{
public:
    virtual ~CommandHandler() = default;

    /**
     * @brief Execute the command
     * @param client IPC client connection
     * @param args Command arguments
     * @return Exit code (0 for success)
     */
    virtual int execute(IpcClient &client, const std::vector<std::string> &args) = 0;

protected:
    /**
     * @brief Parse response header
     */
    bool parse_header(const std::vector<uint8_t> &response, uint32_t &magic, uint32_t &length, uint32_t &status_code, uint32_t &sequence_id, size_t &offset) const
    {
        offset = 0;
        if (!IpcSerializer::deserialize_u32(response.data(), response.size(), magic, offset))
            return false;
        if (!IpcSerializer::deserialize_u32(response.data(), response.size(), length, offset))
            return false;
        if (!IpcSerializer::deserialize_u32(response.data(), response.size(), status_code, offset))
            return false;
        if (!IpcSerializer::deserialize_u32(response.data(), response.size(), sequence_id, offset))
            return false;
        return true;
    }

    /**
     * @brief Check if status is SUCCESS
     */
    bool is_success(uint32_t status_code) const
    {
        return static_cast<Status>(status_code) == Status::SUCCESS;
    }
};

/**
 * @brief List SMS command handler
 */
class ListCommand : public CommandHandler
{
public:
    int execute(IpcClient &client, const std::vector<std::string> &args) override
    {
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
        auto request =
            IpcSerializer::serialize_request(CommandType::LIST_SMS, 0, payload);

        // Send and receive
        auto response = client.send_command(request);
        if (response.empty())
        {
            std::cerr << "Error: No response from server" << std::endl;
            return 1;
        }

        // Parse response
        size_t offset = 0;
        uint32_t magic, length, status_code, sequence_id, count;

        if (!parse_header(response, magic, length, status_code, sequence_id,
                          offset))
        {
            std::cerr << "Error: Invalid response" << std::endl;
            return 1;
        }

        if (!is_success(status_code))
        {
            std::cerr << "Error: Server returned error status" << std::endl;
            return 1;
        }

        if (!IpcSerializer::deserialize_u32(response.data(), response.size(), count, offset))
        {
            std::cerr << "Error: Invalid response" << std::endl;
            return 1;
        }

        std::cout << "Total messages: " << count << "\n" << std::endl;

        // Collect all messages first
        std::vector<SmsInfo> messages;
        for (uint32_t i = 0; i < count; ++i)
        {
            SmsInfo sms;
            if (PayloadSerializer::deserialize_sms_info(
                    response.data(), response.size(), sms, offset))
            {
                messages.push_back(sms);
            }
        }

        // Print in table format
        print_sms_table(messages);

        return 0;
    }
};

/**
 * @brief Read SMS command handler
 */
class ReadCommand : public CommandHandler
{
public:
    int execute(IpcClient &client, const std::vector<std::string> &args) override
    {
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
        auto request =
            IpcSerializer::serialize_request(CommandType::READ_SMS, 0, payload);

        // Send and receive
        auto response = client.send_command(request);
        if (response.empty())
        {
            std::cerr << "Error: No response from server" << std::endl;
            return 1;
        }

        // Parse response
        size_t offset = 0;
        uint32_t magic, length, status_code, sequence_id, count;

        if (!parse_header(response, magic, length, status_code, sequence_id, offset))
        {
            std::cerr << "Error: Invalid response" << std::endl;
            return 1;
        }

        if (!is_success(status_code))
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
};

/**
 * @brief Delete SMS command handler
 */
class DeleteCommand : public CommandHandler
{
public:
    int execute(IpcClient &client, const std::vector<std::string> &args) override
    {
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
        auto request =
            IpcSerializer::serialize_request(CommandType::DELETE_SMS, 0, payload);

        // Send and receive
        auto response = client.send_command(request);
        if (response.empty())
        {
            std::cerr << "Error: No response from server" << std::endl;
            return 1;
        }

        // Parse response
        size_t offset = 0;
        uint32_t magic, length, status_code, sequence_id;

        if (!parse_header(response, magic, length, status_code, sequence_id,
                          offset))
        {
            std::cerr << "Error: Invalid response" << std::endl;
            return 1;
        }

        if (!is_success(status_code))
        {
            std::cerr << "Error: Delete operation failed" << std::endl;
            return 1;
        }

        // Parse deleted and failed indices
        std::vector<uint8_t> deleted, failed;
        IpcSerializer::deserialize_vec(response.data(), response.size(), deleted,
                                       offset);
        IpcSerializer::deserialize_vec(response.data(), response.size(), failed,
                                       offset);

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
};

/**
 * @brief Send SMS command handler
 */
class SendCommand : public CommandHandler
{
public:
    int execute(IpcClient &client,
                const std::vector<std::string> &args) override
    {
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
        auto request =
            IpcSerializer::serialize_request(CommandType::SEND_SMS, 0, payload);

        // Send and receive
        auto response = client.send_command(request);
        if (response.empty())
        {
            std::cerr << "Error: No response from server" << std::endl;
            return 1;
        }

        // Parse response
        size_t offset = 0;
        uint32_t magic, length, status_code, sequence_id;

        if (!parse_header(response, magic, length, status_code, sequence_id, offset))
        {
            std::cerr << "Error: Invalid response" << std::endl;
            return 1;
        }

        if (!is_success(status_code))
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
};

/**
 * @brief Status command handler
 */
class StatusCommand : public CommandHandler
{
public:
    int execute(IpcClient &client, const std::vector<std::string> & /* args */) override
    {
        // Build request
        auto payload = std::vector<uint8_t>{};
        auto request =
            IpcSerializer::serialize_request(CommandType::STATUS, 0, payload);

        // Send and receive
        auto response = client.send_command(request);
        if (response.empty())
        {
            std::cerr << "Error: No response from server" << std::endl;
            return 1;
        }

        // Parse response
        size_t offset = 0;
        uint32_t magic, length, status_code, sequence_id;

        if (!parse_header(response, magic, length, status_code, sequence_id, offset))
        {
            std::cerr << "Error: Invalid response" << std::endl;
            return 1;
        }

        if (!is_success(status_code))
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
};

// ============================================================================
// CLI Application
// ============================================================================

/**
 * @brief CLI Application class
 */
class CliApp
{
public:
    CliApp() = default;

    /**
     * @brief Run the CLI application
     * @param argc Argument count
     * @param argv Argument values
     * @return Exit code
     */
    int run(int argc, char *argv[])
    {
        if (argc < 2)
        {
            print_usage();
            return 1;
        }

        // Parse arguments
        CommandLineArgs args = parse_arguments(argc, argv);

        // Connect to server
        IpcClient client(args.host, args.port);
        if (!client.connect())
        {
            std::cerr << "Error: Failed to connect to sms_relay service at " << args.server << std::endl;
            std::cerr << "Make sure sms_relay is running" << std::endl;
            return 1;
        }

        // Execute command
        int exit_code = execute_command(client, args.command, args.command_args);

        client.disconnect();
        return exit_code;
    }

private:
    /**
     * @brief Command line arguments
     */
    struct CommandLineArgs
    {
        std::string server = smsrelay::IPCServerDefaults::DEFAULT_SERVER;
        std::string host;
        int port = smsrelay::IPCServerDefaults::DEFAULT_PORT;
        std::string command;
        std::vector<std::string> command_args;
    };

    /**
     * @brief Parse server string (format: "host:port")
     */
    void parse_server_string(const std::string &server, std::string &host, int &port)
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
                std::cerr << "Warning: Invalid port in server string, using default port " << smsrelay::IPCServerDefaults::DEFAULT_PORT << std::endl;
                port = smsrelay::IPCServerDefaults::DEFAULT_PORT;
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

    /**
     * @brief Parse command line arguments
     */
    CommandLineArgs parse_arguments(int argc, char *argv[])
    {
        CommandLineArgs args;

        for (int i = 1; i < argc; ++i)
        {
            std::string arg = argv[i];

            if (arg == "--server" && i + 1 < argc)
            {
                args.server = argv[++i];
            }
            else if (args.command.empty())
            {
                args.command = arg;
            }
            else
            {
                args.command_args.push_back(arg);
            }
        }

        // Parse server string to extract host and port
        parse_server_string(args.server, args.host, args.port);

        return args;
    }

    /**
     * @brief Execute a command
     */
    int execute_command(IpcClient &client, const std::string &command,
                        const std::vector<std::string> &args)
    {
        // Get command handler
        auto handler = get_command_handler(command);
        if (!handler)
        {
            std::cerr << "Error: Unknown command '" << command << "'" << std::endl;
            print_usage();
            return 1;
        }

        // Execute command
        try
        {
            return handler->execute(client, args);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Exception: " << e.what() << std::endl;
            return 1;
        }
    }

    /**
     * @brief Get command handler for a command
     */
    std::unique_ptr<CommandHandler>
    get_command_handler(const std::string &command)
    {
        static const std::map<std::string,
                              std::function<std::unique_ptr<CommandHandler>()>>
            handlers = {
                {"list", []() { return std::make_unique<ListCommand>(); }},
                {"read", []() { return std::make_unique<ReadCommand>(); }},
                {"delete", []() { return std::make_unique<DeleteCommand>(); }},
                {"send", []() { return std::make_unique<SendCommand>(); }},
                {"status", []() { return std::make_unique<StatusCommand>(); }}};

        auto it = handlers.find(command);
        if (it != handlers.end())
        {
            return it->second();
        }
        return nullptr;
    }

    /**
     * @brief Print usage information
     */
    void print_usage() const
    {
        std::cout
            << "SMS Relay CLI - Command-line interface for sms_relay service\n\n";
        std::cout << "Usage:\n";
        std::cout << "  sms_relay_cli <command> [options]\n\n";
        std::cout << "Commands:\n";
        std::cout << "  list [--status STATUS]       List all SMS (status: ALL, REC UNREAD, etc.)\n";
        std::cout << "  read --index INDICES         Read specific SMS (comma-separated: 1,2,3)\n";
        std::cout << "  delete --index INDICES       Delete SMS (comma-separated: " "1,2,3)\n";
        std::cout << "  send --to NUMBER --text TEXT  Send new SMS\n";
        std::cout << "  status                          Get service status\n\n";
        std::cout << "Options:\n";
        std::cout << "  --server SERVER     Server address in \"host:port\" format (default: " << smsrelay::IPCServerDefaults::DEFAULT_SERVER << ")\n";
        std::cout
            << "                      Can be hostname, IPv4, or IPv6 address\n";
        std::cout << "                      IPv6 addresses must be in brackets: [::1]:" << smsrelay::IPCServerDefaults::DEFAULT_PORT << "\n\n";
        std::cout << "Examples:\n";
        std::cout << "  sms_relay_cli list\n";
        std::cout << "  sms_relay_cli list --status \"REC UNREAD\"\n";
        std::cout << "  sms_relay_cli read --index 1,2,5\n";
        std::cout << "  sms_relay_cli delete --index 1,2\n";
        std::cout
            << "  sms_relay_cli send --to \"+1234567890\" --text \"Hello World\"\n";
        std::cout << "  sms_relay_cli status\n";
        std::cout << "\nRemote server examples:\n";
        std::cout << "  sms_relay_cli --server sms-relay.example.com:" << smsrelay::IPCServerDefaults::DEFAULT_PORT << " list\n";
        std::cout << "  sms_relay_cli --server 192.168.1.100:9999 status\n";
        std::cout << "  sms_relay_cli --server [2001:db8::1]:" << smsrelay::IPCServerDefaults::DEFAULT_PORT << " list\n";
    }
};

// ============================================================================
// Main Entry Point
// ============================================================================

int main(int argc, char *argv[])
{
    CliApp app;
    return app.run(argc, argv);
}
