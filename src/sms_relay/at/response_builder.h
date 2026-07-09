#pragma once

#include <string>
#include <vector>
#include <atomic>

namespace smsrelay::at {

/**
 * @brief Builds AT command response from incoming lines
 *
 * Implements state machine to handle different response phases:
 * - Waiting for response lines
 * - Waiting for prompt (>) for AT+CMGS
 * - Waiting for final OK after PDU send
 * - Complete when OK/ERROR received
 */
class ResponseBuilder {
public:
    enum class State {
        Idle,            // No command in progress
        WaitingResponse, // Waiting for response lines
        WaitingPrompt,   // Waiting for > prompt (AT+CMGS)
        WaitingFinal,    // Waiting for final OK (after PDU)
        Complete         // Response complete
    };

    ResponseBuilder() = default;

    /**
     * @brief Start building response for new command
     */
    void start_command(const std::string& cmd);

    /**
     * @brief Add a line to current response
     * @return true if response is complete
     */
    bool add_line(const std::string& line);

    /**
     * @brief Get current state
     */
    State state() const { return state_; }

    /**
     * @brief Get current command being processed
     */
    const std::string& current_command() const { return current_command_; }

    /**
     * @brief Get completed response
     */
    struct AtResponse {
        bool success = false;
        std::string status;           // "OK", "ERROR", "TIMEOUT"
        std::string error_code;       // Error code if any
        std::vector<std::string> data; // Response data lines
    };

    AtResponse get_response() const;

    /**
     * @brief Reset to idle state
     */
    void reset();

private:
    std::atomic<State> state_{State::Idle};
    std::string current_command_;
    std::vector<std::string> response_lines_;

    bool is_complete_line(const std::string& line) const;
};

} // namespace smsrelay::at
