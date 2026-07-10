#include "dispatcher.h"

namespace smsrelay::at {

bool Dispatcher::dispatch(const std::string &line)
{
    if (line.empty())
    {
        return false;
    }

    // Special handling for data input mode (AT+CMGS sending PDU)
    if (response_builder_.state() == ResponseBuilder::State::WaitingPrompt ||
        response_builder_.state() == ResponseBuilder::State::WaitingFinal)
    {
        // In PDU input mode, everything goes to response builder
        response_builder_.add_line(line);
        return true;
    }

    // Check if line could be response to current command
    if (could_be_response_to_current(line))
    {
        // This is a response line
        bool complete = response_builder_.add_line(line);

        if (complete)
        {
            // Command finished, response is ready
            return true;
        }
        return true;
    }

    // Not a response, check if it's a URC
    if (urc_dispatcher_.dispatch(line))
    {
        // Was handled as URC
        return true;
    }

    // Unknown line, ignore
    return false;
}

bool Dispatcher::looks_like_urc(const std::string &line) const
{
    if (line.empty())
    {
        return false;
    }

    // URCs typically start with +
    if (line[0] != '+')
    {
        return false;
    }

    // Check if it's a known URC
    if (urc_dispatcher_.is_urc(line))
    {
        return true;
    }

    // Could be a URC if it doesn't match current command
    return !could_be_response_to_current(line);
}

bool Dispatcher::could_be_response_to_current(const std::string &line) const
{
    if (line.empty() ||
        response_builder_.state() == ResponseBuilder::State::Idle)
    {
        return false;
    }

    const std::string &current_cmd = response_builder_.current_command();

    // Check if current command is a query (contains ? or =?)
    if (current_cmd.find('?') != std::string::npos ||
        current_cmd.find("=?") != std::string::npos)
    {

        // Extract the command prefix (e.g., +CREG from AT+CREG?)
        std::string prefix = extract_command_prefix(current_cmd);

        // Check if line starts with same prefix
        if (line.find(prefix) == 0)
        {
            return true; // This is a response to the query
        }

        // Different prefix, must be URC
        return false;
    }

    // For non-query commands, check if line is a known response format
    // Response lines typically don't start with + (except the query result above)
    // However, some commands like AT+CMGL, AT+CMGR have responses that start with
    // + Check if the line matches the current command prefix
    if (line[0] == '+')
    {
        // Extract command prefix from current command
        std::string prefix = extract_command_prefix(current_cmd);
        // Check if line starts with this prefix (e.g., +CMGL: matches AT+CMGL)
        if (line.find(prefix) == 0)
        {
            return true; // This is a response to our command
        }
        return false; // Likely a URC (different prefix)
    }

    // Otherwise, assume it's a response line
    return true;
}

std::string Dispatcher::extract_command_prefix(const std::string &cmd) const
{
    // Extract prefix from commands like:
    // "AT+CREG?" → "+CREG"
    // "AT+CSQ" → "+CSQ"
    // "ATD12345;" → "ATD" (dialing command)

    // Find + or AT
    size_t start = cmd.find('+');
    if (start == std::string::npos)
    {
        // No +, might be basic command like ATZ
        start = cmd.find("AT");
        if (start != std::string::npos)
        {
            start += 2; // Skip "AT"
        }
        else
        {
            return "";
        }
    }

    // Find end of prefix (at =, ?, or space)
    size_t end = cmd.find_first_of("= ?*", start);
    if (end == std::string::npos)
    {
        return cmd.substr(start);
    }

    return cmd.substr(start, end - start);
}

} // namespace smsrelay::at
