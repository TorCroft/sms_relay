#pragma once

#include "sms_relay/at/response_builder.h"
#include "sms_relay/transport/urc_dispatcher.h"
#include <string>

namespace smsrelay::at {

/**
 * @brief Dispatches lines to URC handler or ResponseBuilder
 *
 * Intelligently determines whether a line is:
 * - A URC (unsolicited result code)
 * - Part of current command response
 *
 * Key insight: Some formats like "+CREG:" can be either:
 * - Response to "AT+CREG?" (query)
 * - URC when network registration changes
 *
 * Must examine CurrentCommand to distinguish.
 */
class Dispatcher {
public:
    Dispatcher(ResponseBuilder& builder, transport::URCDispatcher& urc)
        : response_builder_(builder)
        , urc_dispatcher_(urc)
    {}

    /**
     * @brief Dispatch line to appropriate handler
     * @param line The line to dispatch
     * @return true if line was dispatched, false otherwise
     */
    bool dispatch(const std::string& line);

    /**
     * @brief Check if line appears to be a URC (heuristic)
     */
    bool looks_like_urc(const std::string& line) const;

private:
    /**
     * @brief Check if this could be a response to current command
     *
     * For example:
     * - Current: "AT+CREG?" → "+CREG:0,1" is a response
     * - Current: "AT+CSQ" → "+CREG:1" is a URC
     */
    bool could_be_response_to_current(const std::string& line) const;

    /**
     * @brief Extract command prefix (e.g., "+CREG" from "AT+CREG?")
     */
    std::string extract_command_prefix(const std::string& cmd) const;

    ResponseBuilder& response_builder_;
    transport::URCDispatcher& urc_dispatcher_;
};

} // namespace smsrelay::at
