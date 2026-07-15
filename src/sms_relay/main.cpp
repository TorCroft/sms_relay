/**
 * SMS Relay - SMS Forwarding Service
 *
 * Architecture:
 * - Single IO thread running io_context
 * - Async AT command session
 * - SMS service with callback for new messages
 * - IPC server for CLI communication
 * - Forward service for push notifications
 */

#include "common/app_config.h"
#include "common/config_loader.h"
#include "sms_relay/at/at_session.h"
#include "sms_relay/forward/forward_service.h"
#include "sms_relay/ipc/ipc_server.h"
#include "sms_relay/sms/sms_service.h"
#include "sms_relay/transport/serial_transport.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <future>
#include <iostream>
#include <memory>
#include <thread>

// Forward declaration for signal handler
void signal_handler(int signal);

// ============================================================================
// Application Constants
// ============================================================================

namespace {
// Connection management
constexpr int CONNECTION_TIMEOUT_SECONDS = 5;
constexpr auto MODEM_STABILIZATION_DELAY = std::chrono::milliseconds(1000);

// Message deletion timing
constexpr auto MESSAGE_DELETE_DELAY = std::chrono::milliseconds(50);
} // namespace

using namespace smsrelay;
using namespace smsrelay::transport;
using namespace smsrelay::at;

/**
 * @brief SMS Relay Application class
 *
 * Manages the lifecycle of the SMS relay service:
 * - Serial transport
 * - AT session
 * - SMS service
 * - Forward service
 * - IPC server
 */
class SmsRelayApp
{
public:
    /**
     * @brief Constructor with configuration
     * @param config Application configuration
     */
    explicit SmsRelayApp(const AppConfig &config) : config_(config)
    {
        // Set global instance pointer for signal handler
        instance_ = this;
        // Reset shutdown flag
        reset_shutdown_flag();
    }

    /**
     * @brief Destructor
     */
    ~SmsRelayApp()
    {
        // Clear global instance pointer
        instance_ = nullptr;
    }

    /**
     * @brief Get the current application instance
     * @return Pointer to the current instance, or nullptr if none
     */
    static SmsRelayApp *get_instance() { return instance_; }

    /**
     * @brief Check if shutdown was requested
     * @return true if shutdown was requested
     */
    static bool is_shutdown_requested() { return shutdown_requested_.load(); }

    /**
     * @brief Request shutdown
     */
    static void request_shutdown() { shutdown_requested_ = true; }

    /**
     * @brief Reset shutdown flag (for starting new instance)
     */
    static void reset_shutdown_flag() { shutdown_requested_ = false; }

    /**
     * @brief Stop IO context immediately (for signal handler)
     */
    void stop_io_context()
    {
        if (io_ctx_)
        {
            io_ctx_->stop();
        }
    }

    /**
     * @brief Gracefully shutdown the application
     */
    void shutdown()
    {
        // First, signal all components to stop
        if (ipc_server_)
        {
            ipc_server_->stop();
        }

        // Stop IO context to unblock all async operations
        if (io_ctx_)
        {
            io_ctx_->stop();
        }

        // Note: Serial port and other resources will be cleaned up by destructors
        std::cout << "[System] Shutdown complete" << std::endl;
    }

    /**
     * @brief Start the SMS relay service
     * @return true if started successfully
     */
    bool start()
    {
        std::cout << "SMS Relay - Port: " << config_.serial.port
                  << " | Forward: " << (config_.forward.enabled ? "ON" : "OFF")
                  << std::endl;
        std::cout << std::endl;

        try
        {
            // Create IO context
            io_ctx_ = std::make_unique<asio::io_context>();

            // Create components
            create_components();

            // Set up callbacks
            setup_callbacks();

            // Start IO thread
            start_io_thread();

            // Wait for connection
            if (!wait_for_connection())
            {
                return false;
            }

            // Initialize modem
            if (!initialize_modem())
            {
                return false;
            }

            // Initialize message cache (after modem is ready)
            sms_service_->initialize_cache();

            // Start IPC server
            ipc_server_->start();

            std::cout << "Ready. Listening for SMS..." << std::endl;
            std::cout << std::endl;

            return true;
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error starting service: " << e.what() << std::endl;
            return false;
        }
    }

    /**
     * @brief Run the service (blocking)
     */
    void run()
    {
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);

        std::cout << "[System] Running. Press Ctrl+C to stop." << std::endl;

        // 运行 IO 上下文（在主线程中）
        io_ctx_->run();

        // 正常退出流程
        shutdown();

        // 清理线程
        if (io_thread_ && io_thread_->joinable())
        {
            io_thread_->join();
        }
    }

    /**
     * @brief Stop the service (legacy method, calls shutdown)
     */
    void stop()
    {
        shutdown();
    }

private:
    /**
     * @brief Create all service components
     */
    void create_components()
    {
        // Reset connection promise for new connection attempt
        connection_promise_ = std::promise<bool>();
        connection_established_ = false;

        // Create async serial transport from config
        transport::SerialConfig serial_config(config_.serial.port,
                                              config_.serial.baudrate);
        transport_ = std::make_shared<SerialTransport>(*io_ctx_, serial_config);

        // Create async AT session
        at_session_ = std::make_shared<AtSession>(transport_, *io_ctx_);

        // Create SMS service with config
        sms_service_ = std::make_shared<SmsService>(at_session_, config_.sms);

        // Create Forward service with config
        forward_service_ =
            std::make_unique<smsrelay::forward::ForwardService>(config_.forward);

        // Create IPC server with config
        ipc_server_ = std::make_unique<smsrelay::ipc::IpcServer>(
            config_.ipc_server.port, sms_service_);
    }

    /**
     * @brief Set up callbacks for new SMS and URC
     */
    void setup_callbacks()
    {
        // Set up connection callback
        transport_->set_connection_callback([this](bool connected) {
            if (connected)
            {
                std::cout << "[Transport] Connected to " << config_.serial.port
                          << std::endl;
                connection_established_ = true;
                connection_promise_.set_value(true);
            }
            else
            {
                std::cerr << "[Transport] Disconnected from " << config_.serial.port
                          << std::endl;
                if (!connection_established_)
                {
                    // Connection failed during initial attempt
                    connection_promise_.set_value(false);
                }
            } });

        // Callback for new SMS
        sms_service_->set_new_sms_callback([this](const IncomingSms &sms) { on_new_sms(sms); });

        // URC callback to forward +CMTI to SMS service
        at_session_->set_urc_callback(
            [this](const std::string &urc, const std::string &args) {
                on_urc(urc, args);
            });
    }

    /**
     * @brief Start the IO thread
     */
    void start_io_thread()
    {
        io_thread_ = std::make_unique<std::thread>([this]() { io_ctx_->run(); });
        at_session_->start();
    }

    /**
     * @brief Wait for serial connection (async with timeout)
     * @return true if connected successfully
     */
    bool wait_for_connection()
    {
        std::cout << "Waiting for serial connection..." << std::endl;

        // Wait for connection with timeout
        auto future = connection_promise_.get_future();
        if (future.wait_for(std::chrono::seconds(CONNECTION_TIMEOUT_SECONDS)) !=
            std::future_status::timeout)
        {
            bool connected = future.get();
            if (connected)
            {
                std::cout << "[System] Serial connection established" << std::endl;
                // Give modem a moment to stabilize after connection
                std::this_thread::sleep_for(MODEM_STABILIZATION_DELAY);
                return true;
            }
            else
            {
                std::cerr << "[System] Serial connection failed" << std::endl;
                return false;
            }
        }
        else
        {
            // Timeout occurred
            std::cerr << "[System] Timeout waiting for serial connection ("
                      << CONNECTION_TIMEOUT_SECONDS << "s)" << std::endl;
            return false;
        }
    }

    /**
     * @brief Initialize modem with required settings
     * @return true if initialization successful
     */
    bool initialize_modem()
    {
        // Initialize modem
        if (!at_session_->initialize_modem().get().success)
            return false;

        // Get modem info
        get_modem_info();

        // Set PDU mode and new message indication
        if (!at_session_->set_sms_pdu_mode(true).get().success)
            return false;
        if (!at_session_->set_new_message_indication().get().success)
            return false;

        return true;
    }

    /**
     * @brief Get modem manufacturer and model
     */
    void get_modem_info()
    {
        auto manufacturer_future = at_session_->get_manufacturer();
        auto manufacturer_response = manufacturer_future.get();
        if (manufacturer_response.success && !manufacturer_response.data.empty())
        {
            std::cout << "Modem: " << manufacturer_response.data[0];
        }

        auto model_future = at_session_->get_model();
        auto model_response = model_future.get();
        if (model_response.success && !model_response.data.empty())
        {
            std::cout << " " << model_response.data[0] << std::endl;
        }
    }

    /**
     * @brief Callback for new SMS
     */
    void on_new_sms(const IncomingSms &sms)
    {
        std::cout << "[SMS] " << sms.decoded.number << ": " << sms.decoded.text
                  << std::endl;

        // Forward to configured targets
        bool forward_success = false;
        if (forward_service_)
        {
            int count = forward_service_->forward(sms);
            forward_success = (count > 0);
        }

        // Auto delete after forwarding (if enabled)
        if (config_.sms.auto_delete && forward_success)
        {
            // Delete all parts if multipart
            std::vector<uint8_t> indices_to_delete;
            if (!sms.original_indices.empty())
            {
                indices_to_delete = sms.original_indices;
            }
            else
            {
                indices_to_delete = {sms.index};
            }

            for (uint8_t idx : indices_to_delete)
            {
                sms_service_->delete_message(sms_service_->get_storage(), idx);
                std::this_thread::sleep_for(MESSAGE_DELETE_DELAY);
            }
            std::cout << "[SMS] Deleted" << std::endl;
        }
    }

    /**
     * @brief Callback for URC
     */
    void on_urc(const std::string &urc, const std::string &args)
    {
        std::cout << "[URC] " << urc << ": " << args << std::endl;
        if (urc == "+CMTI")
        {
            sms_service_->handle_cmti(urc, args);
        }
    }

    /**
     * @brief Execute a command with logging
     * @param description Command description
     * @param command Function that returns the command future
     * @return true if command succeeded
     */
    template <typename CommandFunc>
    bool execute_command(const std::string &description, CommandFunc &&command)
    {
        std::cout << "\n"
                  << description << "..." << std::endl;
        auto future = command();
        auto response = future.get();
        bool success = response.success;
        std::cout << "  Result: " << (success ? "OK" : "FAILED") << std::endl;
        return success;
    }

    // Configuration
    AppConfig config_;

    // Member variables
    std::unique_ptr<asio::io_context> io_ctx_;
    std::unique_ptr<std::thread> io_thread_;

    std::shared_ptr<SerialTransport> transport_;
    std::shared_ptr<AtSession> at_session_;
    std::shared_ptr<SmsService> sms_service_;
    std::unique_ptr<smsrelay::ipc::IpcServer> ipc_server_;
    std::unique_ptr<smsrelay::forward::ForwardService> forward_service_;

    // Connection handling
    std::promise<bool> connection_promise_;
    std::atomic<bool> connection_established_{false};

    // Global instance pointer for signal handler
    static SmsRelayApp *instance_;
    static std::atomic<bool> shutdown_requested_;
};

// Static member initialization
SmsRelayApp *SmsRelayApp::instance_ = nullptr;
std::atomic<bool> SmsRelayApp::shutdown_requested_{false};

// Signal handler function
void signal_handler(int signal)
{
    if (signal == SIGINT || signal == SIGTERM)
    {
        static std::atomic<bool> first_signal{true};
        if (first_signal.exchange(false))
        {
            std::cout << "\n[Signal] Received signal " << signal << std::endl;

            // 先设置标志
            SmsRelayApp::request_shutdown();

            // 获取实例并停止
            auto *app = SmsRelayApp::get_instance();
            if (app)
            {
                // 直接停止，不等待
                app->stop_io_context();
            }
        }
        else
        {
            // 第二次按 Ctrl+C，强制退出
            std::cout << "[Signal] Force exit" << std::endl;
            std::exit(1);
        }
    }
}

/**
 * @brief Print usage information
 */
void print_usage(const char *program_name)
{
    std::cout << "SMS Relay - SMS Forwarding Service\n\n";
    std::cout << "Usage:\n";
    std::cout << "  " << program_name << " [config_file]\n\n";
    std::cout << "Arguments:\n";
    std::cout << "  config_file  Path to YAML configuration file\n";
    std::cout << "               (default: ./config/smsrelay.yaml\n\n";
    std::cout << "Examples:\n";
    std::cout << "  " << program_name << "\n";
    std::cout << "  " << program_name << " config/smsrelay.yaml\n";
    std::cout << "Configuration file must contain serial port settings.\n";
    std::cout << "See config/smsrelay.yaml for example.\n";
}

/**
 * @brief Main entry point
 */
int main(int argc, char *argv[])
{
    // Load configuration
    AppConfig config;

    try
    {
        if (argc >= 2)
        {
            // Load from specified path
            config = load_config(argv[1]);
        }
        else
        {
            // Load from default locations
            config = load_config_default();
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "Configuration error: " << e.what() << std::endl;
        std::cerr << std::endl;
        print_usage(argv[0]);
        return 1;
    }

    // Create and start application
    SmsRelayApp app(config);

    if (!app.start())
    {
        std::cerr << "Failed to start SMS relay service" << std::endl;
        return 1;
    }

    // Run service (blocking)
    app.run();

    return 0;
}
