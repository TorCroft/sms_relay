#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace smsrelay::ipc {

// Magic number for IPC protocol
constexpr uint32_t IPC_MAGIC = 0x534D534D;  // "SMSM" in ASCII

// Command types
enum class CommandType : uint32_t {
    LIST_SMS = 1,        // List all messages
    READ_SMS = 2,        // Read specific message(s)
    DELETE_SMS = 3,      // Delete specific message(s)
    SEND_SMS = 4,        // Send new SMS
    STATUS = 5,          // Get service status
    SHUTDOWN = 99        // Shutdown service
};

// Response status codes
enum class Status : uint32_t {
    SUCCESS = 0,
    FAILED = 1,    // Renamed from ERROR to avoid Windows macro conflict
    TIMEOUT = 2,
    NOT_FOUND = 3,
    INVALID_ARGS = 4
};

/**
 * @brief IPC Request header
 */
struct RequestHeader {
    uint32_t magic;
    uint32_t length;
    uint32_t command_type;
    uint32_t sequence_id;

    RequestHeader(uint32_t cmd, uint32_t seq = 0)
        : magic(IPC_MAGIC)
        , length(0)
        , command_type(static_cast<uint32_t>(cmd))
        , sequence_id(seq)
    {}
};

/**
 * @brief IPC Response header
 */
struct ResponseHeader {
    uint32_t magic;
    uint32_t length;
    uint32_t status;
    uint32_t sequence_id;

    ResponseHeader(uint32_t stat, uint32_t seq)
        : magic(IPC_MAGIC)
        , length(0)
        , status(static_cast<uint32_t>(stat))
        , sequence_id(seq)
    {}
};

/**
 * @brief List SMS request
 */
struct ListSmsRequest {
    std::string status;  // "ALL", "REC UNREAD", "REC READ", etc.
};

/**
 * @brief Read SMS request
 */
struct ReadSmsRequest {
    std::vector<uint8_t> indices;  // Message indices to read
};

/**
 * @brief Delete SMS request
 */
struct DeleteSmsRequest {
    std::vector<uint8_t> indices;  // Message indices to delete
};

/**
 * @brief Send SMS request
 */
struct SendSmsRequest {
    std::string recipient;  // Phone number
    std::string text;        // Message text
};

/**
 * @brief SMS part information (for multipart messages)
 */
struct SmsPartInfo {
    uint8_t index;
    uint8_t seq;
    uint8_t total;
};

/**
 * @brief SMS message info (for response)
 */
struct SmsInfo {
    uint8_t index;
    std::string sender;
    std::string text;
    std::string timestamp;
    bool has_udh;
    uint8_t concat_seq;
    uint8_t concat_total;
    uint8_t status; // Message status: 0=REC UNREAD, 1=REC READ, 2=STO UNSENT, 3=STO SENT

    // Multipart combination info
    bool is_combined = false;           // True if this is a combined multipart message
    std::vector<SmsPartInfo> parts;     // Part information (if combined)
};

/**
 * @brief List SMS response
 */
struct ListSmsResponse {
    std::vector<SmsInfo> messages;
    std::string error;
};

/**
 * @brief Read SMS response
 */
struct ReadSmsResponse {
    std::vector<SmsInfo> messages;  // One per requested index
    std::string error;
};

/**
 * @brief Delete SMS response
 */
struct DeleteSmsResponse {
    std::vector<uint8_t> deleted_indices;  // Successfully deleted
    std::vector<uint8_t> failed_indices;    // Failed to delete
    std::string error;
};

/**
 * @brief Send SMS response
 */
struct SendSmsResponse {
    bool success;
    uint8_t message_index;  // Index of sent message (if available)
    std::string error;
};

/**
 * @brief Status response
 */
struct StatusResponse {
    bool connected;
    std::string port;
    int message_count;
    std::string error;
};

} // namespace smsrelay::ipc
