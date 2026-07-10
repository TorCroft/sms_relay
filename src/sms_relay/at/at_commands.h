#pragma once

#include <cstdint>
#include <string>

namespace smsrelay::at::commands {

// =============================================================================
// AT Command Constants
// =============================================================================

// Initialization commands
constexpr const char *INIT = "AT";   // Simple connection test
constexpr const char *RESET = "ATZ"; // Reset modem to factory defaults
constexpr const char *ECHO_OFF = "ATE0";
constexpr const char *VERBOSE_ERROR = "AT+CMEE=1";

// SMS mode settings
constexpr const char *SET_SMS_PDU_MODE = "AT+CMGF=0";  // PDU mode
constexpr const char *SET_SMS_TEXT_MODE = "AT+CMGF=1"; // Text mode

// New message indication
// Format: AT+CNMI=<mode>,<mt>,<bm>,<ds>,<bfr>
// <mode>=0: Store to TA/TE, 1: Store to ME, 2: Direct to TA/TE
// <mt>=0: No indication, 1: Store + indication, 2: Direct + indication
constexpr const char *SET_NEW_MSG_IND = "AT+CNMI=2,1,0,1,0";

// SMS operations
// For PDU mode (AT+CMGF=0), status is integer:
// 0 = REC UNREAD (received but unread)
// 1 = REC READ (received and read)
// 2 = STO UNSENT (stored but not sent)
// 3 = STO SENT (stored and sent)
// 4 = ALL (all messages)
inline std::string list_messages(uint8_t status)
{
    return "AT+CMGL=" + std::to_string(status);
}

// For Text mode (AT+CMGF=1), status is string:
constexpr const char *LIST_MESSAGES_ALL_TEXT = "AT+CMGL=\"ALL\"";
constexpr const char *LIST_MESSAGES_REC_UNREAD_TEXT = "AT+CMGL=\"REC UNREAD\"";
constexpr const char *LIST_MESSAGES_REC_READ_TEXT = "AT+CMGL=\"REC READ\"";
constexpr const char *LIST_MESSAGES_STO_UNSENT_TEXT = "AT+CMGL=\"STO UNSENT\"";
constexpr const char *LIST_MESSAGES_STO_SENT_TEXT = "AT+CMGL=\"STO SENT\"";

constexpr const char *READ_MESSAGE_PREFIX = "AT+CMGR=";   // + <index>
constexpr const char *SEND_MESSAGE_PREFIX = "AT+CMGS=";   // + <length>
constexpr const char *DELETE_MESSAGE_PREFIX = "AT+CMGD="; // + <index>

// Status queries
constexpr const char *SIGNAL_QUALITY = "AT+CSQ";
constexpr const char *REGISTRATION = "AT+CREG?";
constexpr const char *OPERATOR = "AT+COPS?";
constexpr const char *MANUFACTURER = "AT+GMI";
constexpr const char *MODEL = "AT+GMM";
constexpr const char *REVISION = "AT+GMR";
constexpr const char *IMEI = "AT+GSN";

// Character set configuration
constexpr const char *GET_CHARACTER_SET = "AT+CSCS?";
constexpr const char *SET_CHARACTER_SET_PREFIX =
    "AT+CSCS="; // + "GSM"/"IRA"/"UCS2"

// URC (Unsolicited Result Code) configuration - Quectel specific
constexpr const char *GET_URC_PORT = "AT+QURCCFG=\"urcport\"";
constexpr const char *SET_URC_PORT_PREFIX =
    "AT+QURCCFG=\"urcport\",\""; // + port + "

// Helper functions to build commands
inline std::string read_message(uint8_t index)
{
    return std::string(READ_MESSAGE_PREFIX) + std::to_string(index);
}

inline std::string delete_message(uint8_t index)
{
    return std::string(DELETE_MESSAGE_PREFIX) + std::to_string(index);
}

inline std::string send_message(uint16_t length)
{
    return std::string(SEND_MESSAGE_PREFIX) + std::to_string(length);
}

inline std::string set_character_set(const std::string &charset)
{
    return std::string(SET_CHARACTER_SET_PREFIX) + charset + "\"";
}

inline std::string set_urc_port(const std::string &port)
{
    return std::string(SET_URC_PORT_PREFIX) + port + "\"";
}

// URC port values supported by Quectel modules
// Check supported values with: AT+QURCCFG=?
constexpr const char *URC_PORT_UART1 = "uart1";
constexpr const char *URC_PORT_USB_AT = "usbat";
constexpr const char *URC_PORT_USB_MODEM = "usbmodem";

} // namespace smsrelay::at::commands
