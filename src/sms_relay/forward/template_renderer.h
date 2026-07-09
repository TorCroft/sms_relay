#pragma once

#include "sms_relay/sms/sms_service.h"
#include <string>
#include <functional>

namespace smsrelay::forward {

/**
 * @brief Template renderer for forwarding notifications
 *
 * Supports variable substitution in templates:
 * - ${sender}    - Sender phone number
 * - ${text}      - SMS text content
 * - ${timestamp} - SMS timestamp
 * - ${index}     - SMS index in storage
 */
class TemplateRenderer {
public:
    /**
     * @brief Render a template string with SMS data
     * @param tmpl Template string with ${var} placeholders
     * @param sms Incoming SMS data
     * @return Rendered string with variables replaced
     */
    static std::string render(const std::string& tmpl, const IncomingSms& sms);

    /**
     * @brief URL-encode a string (for use in query parameters)
     * @param str String to encode
     * @return URL-encoded string
     */
    static std::string url_encode(const std::string& str);

private:
    /**
     * @brief Replace all occurrences of a substring
     */
    static void replace_all(std::string& str, const std::string& from, const std::string& to);

    /**
     * @brief Truncate text to maximum length
     */
    static std::string truncate(const std::string& str, size_t max_len);
};

} // namespace smsrelay::forward
