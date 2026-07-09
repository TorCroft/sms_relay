#include "response_builder.h"

namespace smsrelay::at {

void ResponseBuilder::start_command(const std::string& cmd) {
    current_command_ = cmd;
    response_lines_.clear();
    state_ = State::WaitingResponse;

    // Special case: AT+CMGS needs to wait for > prompt
    if (cmd.find("AT+CMGS") == 0) {
        state_ = State::WaitingPrompt;
    }
}

bool ResponseBuilder::add_line(const std::string& line) {
    switch (state_) {
        case State::WaitingPrompt:
            // Waiting for > prompt before sending PDU
            if (line == ">") {
                state_ = State::WaitingFinal;
                return false;  // Not complete yet
            }
            // Some modems might send other lines, ignore them
            return false;

        case State::WaitingResponse:
        case State::WaitingFinal:
            // Check for completion markers
            if (line == "OK") {
                state_ = State::Complete;
                return true;  // Response complete
            }
            if (line.find("ERROR") == 0) {
                response_lines_.push_back(line);
                state_ = State::Complete;
                return true;  // Response complete
            }
            if (line.find(">") == 0) {
                // Received > as part of data, not as prompt
                response_lines_.push_back(line);
                return false;
            }
            // Regular response line, add to data
            if (!line.empty()) {
                response_lines_.push_back(line);
            }
            return false;

        default:
            // Idle or Complete state, shouldn't receive lines
            return false;
    }
}

ResponseBuilder::AtResponse ResponseBuilder::get_response() const {
    AtResponse response;

    if (state_ == State::Complete) {
        // Find status line (should be last line)
        if (!response_lines_.empty()) {
            const std::string& last_line = response_lines_.back();
            if (last_line == "OK") {
                response.success = true;
                response.status = "OK";
                response.data = std::vector<std::string>(
                    response_lines_.begin(),
                    response_lines_.end() - 1
                );
            } else if (last_line.find("ERROR") == 0) {
                response.success = false;
                response.status = "ERROR";
                response.error_code = last_line;
                response.data = std::vector<std::string>(
                    response_lines_.begin(),
                    response_lines_.end() - 1
                );
            } else {
                // No clear status, assume OK if we got here
                response.success = true;
                response.status = "OK";
                response.data = response_lines_;
            }
        } else {
            // No lines received, but marked as complete
            response.success = true;
            response.status = "OK";
        }
    } else {
        // Not complete yet
        response.success = false;
        response.status = "INCOMPLETE";
    }

    return response;
}

void ResponseBuilder::reset() {
    state_ = State::Idle;
    current_command_.clear();
    response_lines_.clear();
}

bool ResponseBuilder::is_complete_line(const std::string& line) const {
    // Check if line is a completion marker
    return line == "OK" || line.find("ERROR") == 0;
}

} // namespace smsrelay::at
