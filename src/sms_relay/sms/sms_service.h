#pragma once

#include "sms_relay/at/at_session.h"
#include "common/pdu/pdu.h"
#include "common/app_config.h"
#include <functional>
#include <string>
#include <memory>
#include <map>
#include <chrono>
#include <mutex>
#include <thread>
#include <queue>
#include <condition_variable>

namespace smsrelay {

/**
 * @brief Incoming SMS notification structure
 */
struct IncomingSms {
    uint8_t index;
    std::string storage;      // "ME", "SM", "MT", etc.
    pdu::PduMessage decoded;  // Decoded PDU message
    std::string pdu_hex;      // Raw PDU hex
    bool success = false;     // True if decoding was successful
    bool is_complete = true;  // For multipart: true if all parts received
    uint8_t status = 1;       // Message status: 0=REC UNREAD, 1=REC READ, 2=STO UNSENT, 3=STO SENT

    // For combined multipart messages: all original indices
    std::vector<uint8_t> original_indices;  // e.g., [0, 1, 2, 3]
};

/**
 * @brief Multipart SMS cache entry
 */
struct MultipartCacheEntry {
    std::string sender;              // Sender number
    std::map<uint8_t, std::string> parts;  // Sequence -> text
    std::map<uint8_t, uint8_t> part_indices;  // Sequence -> index
    uint8_t total_parts = 0;
    std::chrono::steady_clock::time_point first_received;
    std::string timestamp;           // Timestamp from first part
    uint8_t first_index = 0;         // Index of the first part

    /**
     * @brief Check if all parts have been received
     */
    bool is_complete() const {
        return parts.size() == total_parts && total_parts > 0;
    }

    /**
     * @brief Check if this entry has timed out (2 minutes)
     */
    bool is_timed_out() const {
        auto elapsed = std::chrono::steady_clock::now() - first_received;
        return elapsed > std::chrono::minutes(2);
    }

    /**
     * @brief Combine all parts into a single message
     */
    std::string combine() const {
        std::string result;
        for (const auto& [seq, text] : parts) {
            result += text;
        }
        return result;
    }

    /**
     * @brief Get all original indices (sorted)
     */
    std::vector<uint8_t> get_all_indices() const {
        std::vector<uint8_t> indices;
        for (const auto& [seq, idx] : part_indices) {
            indices.push_back(idx);
        }
        std::sort(indices.begin(), indices.end());
        return indices;
    }
};

/**
 * @brief SMS service for handling SMS operations
 *
 * Handles reading, decoding, concatenating, and processing SMS messages.
 * URC handling is done in a background thread to avoid blocking the IO thread.
 */
class SmsService {
public:
    using NewSmsCallback = std::function<void(const IncomingSms&)>;

    /**
     * @brief Constructor
     * @param at_session AT command session
     * @param config SMS configuration (optional, uses defaults if not provided)
     */
    explicit SmsService(std::shared_ptr<at::AtSession> at_session, const SmsConfig& config = SmsConfig{});

    /**
     * @brief Destructor - cleans up expired cache entries and stops background thread
     */
    ~SmsService();

    /**
     * @brief Set callback for new SMS notifications
     * @param callback Function to call when new SMS arrives (complete multipart or single)
     */
    void set_new_sms_callback(NewSmsCallback callback);

    /**
     * @brief Initialize message cache (call this after modem is ready)
     */
    void initialize_cache();

    /**
     * @brief Handle +CMTI URC (new message indication)
     * @param urc The URC type (always "+CMTI", unused)
     * @param args The URC arguments (e.g., ""ME",8")
     */
    void handle_cmti(const std::string& /* urc */, const std::string& args);

    /**
     * @brief Read and decode SMS from storage
     * @param storage Storage location ("ME", "SM", etc.)
     * @param index Message index
     * @return IncomingSms structure with decoded message
     */
    IncomingSms read_sms(const std::string& storage, uint8_t index);

    /**
     * @brief List and decode all SMS messages
     * @return Vector of decoded SMS messages (combined multipart)
     */
    std::vector<IncomingSms> list_and_decode_messages();

    /**
     * @brief Refresh the SMS cache (reload all messages from storage)
     * Call this after receiving new SMS or after deleting messages
     */
    void refresh_cache();

    /**
     * @brief Get all messages from cache (raw, not combined)
     * @return Vector of all cached messages
     */
    const std::vector<IncomingSms>& get_cached_messages() const;

    /**
     * @brief Get storage location from config
     * @return Storage location (e.g., "ME", "SM")
     */
    const std::string& get_storage() const { return config_.storage; }

    /**
     * @brief Add message to cache
     * @param sms Message to add
     */
    void add_to_cache(const IncomingSms& sms);

    /**
     * @brief Delete SMS from storage
     * @param storage Storage location ("ME", "SM", etc.)
     * @param index Message index
     * @return Empty string on success, error message on failure
     */
    std::string delete_message(const std::string& storage, uint8_t index);

    /**
     * @brief Remove message from cache by index
     * @param index Message index to remove
     */
    void remove_from_cache(uint8_t index);

private:
    /**
     * @brief Parse +CMTI arguments
     * @param args Arguments string (e.g., ""ME",8")
     * @return Pair of (storage, index)
     */
    std::pair<std::string, uint8_t> parse_cmti_args(const std::string& args);

    /**
     * @brief Extract PDU from AT+CMGR response
     * @param response AT response
     * @return PDU hex string
     */
    std::string extract_pdu_from_cmgr(const at::ResponseBuilder::AtResponse& response);

    /**
     * @brief Handle multipart SMS - cache and combine
     * @param sms The SMS part
     */
    void handle_multipart(IncomingSms sms);

    /**
     * @brief Get cache key for multipart SMS
     * @param sender Sender number
     * @param concat_ref Concatenation reference
     * @return Cache key string
     */
    std::string get_cache_key(const std::string& sender, uint16_t concat_ref) const;

    /**
     * @brief Clean up timed out cache entries
     */
    void cleanup_cache();

    /**
     * @brief Background thread worker for processing URC events
     */
    void background_worker();

    std::shared_ptr<at::AtSession> at_session_;
    NewSmsCallback new_sms_callback_;

    // Configuration
    SmsConfig config_;

    // Multipart SMS cache
    std::map<std::string, MultipartCacheEntry> multipart_cache_;
    std::mutex cache_mutex_;

    // SMS message cache (all messages, raw, not combined)
    std::vector<IncomingSms> message_cache_;
    std::mutex cache_mutex_messages_;
    bool cache_initialized_ = false;

    // Background thread for URC processing
    std::thread background_thread_;
    std::atomic<bool> background_running_{false};
    std::queue<std::function<void()>> task_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
};

} // namespace smsrelay
