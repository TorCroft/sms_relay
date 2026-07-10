#pragma once

#include "common/app_config.h"
#include "ipc/ipc_client.h"
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <functional>

// Include platform-specific headers
#ifdef _WIN32
#include <Windows.h>
#endif

// Include replxx header
#include <replxx.hxx>

namespace smsrelay::cli {

/**
 * @brief Interactive shell for SMS relay CLI
 *
 * Provides persistent connection and interactive command execution
 * with history, auto-completion, and syntax highlighting.
 */
class InteractiveShell {
public:
    /**
     * @brief Constructor
     * @param server Server address in "host:port" format
     */
    explicit InteractiveShell(const std::string& server);

    ~InteractiveShell();

    /**
     * @brief Run the interactive shell
     * @return Exit code
     */
    int run();

    /**
     * @brief Execute a single command
     * @param command_line Command line string
     * @return Exit code (0 for success)
     */
    int execute_command(const std::string& command_line);

private:
    /**
     * @brief Initialize the REPL
     */
    void initialize_repl();

    /**
     * @brief Set up command completion
     */
    void setup_completion();

    /**
     * @brief Set up syntax highlighting
     */
    void setup_highlighting();

    /**
     * @brief Connect to server
     * @return true if connected successfully
     */
    bool connect_to_server();

    /**
     * @brief Parse server string to host and port
     * @param server Server string in "host:port" format
     * @param host Output host
     * @param port Output port
     */
    void parse_server_string(const std::string& server, std::string& host, int& port);

    /**
     * @brief Parse command line into command and arguments
     * @param input Input string
     * @param command Output command
     * @param args Output arguments
     */
    void parse_command_line(const std::string& input, std::string& command,
                           std::vector<std::string>& args);

    /**
     * @brief Get command handler for a command
     * @param command Command name
     * @return Handler function or nullptr
     */
    std::function<int(const std::vector<std::string>&)>
    get_command_handler(const std::string& command);

    /**
     * @brief Print welcome message
     */
    void print_welcome();

    /**
     * @brief Print help information
     */
    void print_help();

    /**
     * @brief Handle 'exit' command
     * @param args Command arguments
     * @return Exit code
     */
    int handle_exit(const std::vector<std::string>& args);

    /**
     * @brief Handle 'help' command
     * @param args Command arguments
     * @return Exit code
     */
    int handle_help(const std::vector<std::string>& args);

    /**
     * @brief Handle 'list' command
     * @param args Command arguments
     * @return Exit code
     */
    int handle_list(const std::vector<std::string>& args);

    /**
     * @brief Handle 'read' command
     * @param args Command arguments
     * @return Exit code
     */
    int handle_read(const std::vector<std::string>& args);

    /**
     * @brief Handle 'delete' command
     * @param args Command arguments
     * @return Exit code
     */
    int handle_delete(const std::vector<std::string>& args);

    /**
     * @brief Handle 'send' command
     * @param args Command arguments
     * @return Exit code
     */
    int handle_send(const std::vector<std::string>& args);

    /**
     * @brief Handle 'status' command
     * @param args Command arguments
     * @return Exit code
     */
    int handle_status(const std::vector<std::string>& args);

    /**
     * @brief Handle 'reconnect' command
     * @param args Command arguments
     * @return Exit code
     */
    int handle_reconnect(const std::vector<std::string>& args);

    // Member variables
    std::string server_;
    std::string host_;
    int port_;
    std::unique_ptr<replxx::Replxx> replx_;
    std::unique_ptr<IpcClient> client_;
    bool running_;

    // Command registry
    std::map<std::string, std::function<int(const std::vector<std::string>&)>> commands_;
};

} // namespace smsrelay::cli
