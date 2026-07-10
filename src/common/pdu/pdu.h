#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pdu {

// =============================================================================
// Public API - Simple Encode/Decode Interface
// =============================================================================

/**
 * @brief PDU message structure
 */
struct PduMessage
{
    std::string smsc;         // Service center address
    std::string number;       // Sender/recipient number
    std::string text;         // Message text
    uint8_t reference = 0;    // Message reference (for SUBMIT/STATUS-REPORT)
    uint8_t status = 0;       // Status (for STATUS-REPORT)
    std::string timestamp;    // Timestamp (for DELIVER/STATUS-REPORT)
    bool has_udh = false;     // User Data Header present
    uint16_t concat_ref = 0;  // Concatenation reference (if applicable)
    uint8_t concat_seq = 0;   // Concatenation sequence (if applicable)
    uint8_t concat_total = 0; // Concatenation total (if applicable)
};

/**
 * @brief Encode SMS-SUBMIT PDU
 *
 * @param number    Destination phone number (international format: +1234567890)
 * @param text      Message text to send
 * @param reference Message reference number (default: 0)
 * @param smsc      SMSC phone number (empty = use default)
 *
 * @return Vector of PDU hex strings (may be multiple for concatenated messages)
 *
 * @throws std::invalid_argument if parameters are invalid
 * @throws std::runtime_error if encoding fails
 */
[[nodiscard]]
std::vector<std::string>
encode_submit(const std::string &number, const std::string &text,
              uint8_t reference = 0, const std::string &smsc = "");

/**
 * @brief Decode SMS PDU
 *
 * @param pdu_hex   PDU data as hex string
 *
 * @return Decoded PDU message structure
 *
 * @throws std::invalid_argument if PDU is invalid
 * @throws std::runtime_error if decoding fails
 */
[[nodiscard]]
PduMessage decode(const std::string &pdu_hex);

/**
 * @brief Decode SMS PDU from bytes
 *
 * @param pdu_bytes PDU data as byte vector
 *
 * @return Decoded PDU message structure
 *
 * @throws std::invalid_argument if PDU is invalid
 * @throws std::runtime_error if decoding fails
 */
[[nodiscard]]
PduMessage decode(const std::vector<uint8_t> &pdu_bytes);

} // namespace pdu
