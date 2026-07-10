#include "sms_service.h"
#include "common/pdu/pdu.h"
#include <algorithm>
#include <iostream>
#include <regex>

namespace smsrelay {

SmsService::SmsService(std::shared_ptr<at::AtSession> at_session,
                       const SmsConfig &config)
    : at_session_(at_session), config_(config)
{
    // Start background thread for URC processing
    background_running_ = true;
    background_thread_ = std::thread([this]() { background_worker(); });

    // Don't initialize cache here - wait until modem is ready
    // Cache will be initialized on first list operation
}

SmsService::~SmsService()
{
    // Step 1: Stop background thread and ensure all tasks are complete
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        background_running_ = false;

        // Clear any remaining tasks to prevent them from executing
        while (!task_queue_.empty())
        {
            task_queue_.pop();
        }
    }

    // Wake up the background thread so it can exit
    queue_cv_.notify_one();

    // Step 2: Wait for background thread to finish
    // This ensures no tasks are running when we clean up
    if (background_thread_.joinable())
    {
        background_thread_.join();
    }

    // Step 3: Now it's safe to clean up caches
    // No other threads should be accessing these at this point
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        multipart_cache_.clear();
    }

    {
        // Use unique_lock for write access during cleanup
        std::unique_lock<std::shared_mutex> lock(cache_mutex_messages_);
        message_cache_.clear();
    }

    std::cout << "[SMS] Service cleaned up successfully" << std::endl;
}

void SmsService::set_new_sms_callback(NewSmsCallback callback)
{
    new_sms_callback_ = std::move(callback);
}

void SmsService::background_worker()
{
    while (background_running_)
    {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this]() {
                return !background_running_ || !task_queue_.empty();
            });

            if (!background_running_)
            {
                break;
            }

            if (!task_queue_.empty())
            {
                task = std::move(task_queue_.front());
                task_queue_.pop();
            }
        }

        if (task)
        {
            try
            {
                task();
            }
            catch (const std::exception &e)
            {
                std::cerr << "[SMS] Background task error: " << e.what() << std::endl;
            }
        }
    }
}

void SmsService::handle_cmti(const std::string & /* urc */,
                             const std::string &args)
{
    // Post task to background thread to avoid blocking IO thread
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        task_queue_.push([this, args]() {
            try
            {
                auto [storage, index] = parse_cmti_args(args);

                // Clean up timed out entries periodically
                cleanup_cache();

                // Read and decode the SMS (blocking is OK in background thread)
                auto sms = read_sms(storage, index);

                if (!sms.success)
                    return;

                // Check if this is a multipart SMS
                if (sms.decoded.has_udh && sms.decoded.concat_total > 1)
                {
                    // Handle multipart - cache and wait for all parts
                    handle_multipart(std::move(sms));
                }
                else
                {
                    // Single part SMS - notify immediately
                    if (new_sms_callback_)
                    {
                        new_sms_callback_(sms);
                    }
                }
            }
            catch (const std::exception &e)
            {
                std::cerr << "[SMS] Error: " << e.what() << std::endl;
            }
        });
    }
    queue_cv_.notify_one();
}

IncomingSms SmsService::read_sms(const std::string &storage, uint8_t index)
{
    IncomingSms sms;
    sms.index = index;
    sms.storage = storage;

    try
    {
        // Send AT+CMGR=<index> command and wait for response
        auto response_future = at_session_->read_message(index);
        auto response = response_future.get();

        if (!response.success)
        {
            std::cerr << "[SMS] Failed to read message (status: " << response.status
                      << ")" << std::endl;
            return sms;
        }

        // Extract PDU from response
        sms.pdu_hex = extract_pdu_from_cmgr(response);

        if (!sms.pdu_hex.empty())
        {
            // Decode PDU
            sms.decoded = pdu::decode(sms.pdu_hex);
            sms.success = true;
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "[SMS] Error reading message: " << e.what() << std::endl;
    }

    return sms;
}

std::pair<std::string, uint8_t>
SmsService::parse_cmti_args(const std::string &args)
{
    // Format: "ME",8 or "SM",5
    std::regex regex(R"(\"([^\"]+)\",(\d+))");
    std::smatch match;

    if (std::regex_search(args, match, regex) && match.size() >= 3)
    {
        std::string storage = match[1].str();
        uint8_t index = static_cast<uint8_t>(std::stoi(match[2].str()));
        return {storage, index};
    }

    throw std::runtime_error("Invalid +CMTI format: " + args);
}

std::string SmsService::extract_pdu_from_cmgr(
    const at::ResponseBuilder::AtResponse &response)
{
    // In PDU mode, AT+CMGR response format:
    // +CMGR: <stat>,<oa>[,<alpha>],<scts>[,<tos>]<CR><LF>
    // <pdu_hex><CR><LF>
    // OK

    // The PDU is typically the second line or a line with hex characters
    for (const auto &line : response.data)
    {
        // Check if line looks like hex PDU (should be fairly long and only hex
        // chars)
        if (line.length() > 20)
        {
            bool is_hex = true;
            for (char c : line)
            {
                if (!std::isxdigit(c))
                {
                    is_hex = false;
                    break;
                }
            }
            if (is_hex)
            {
                return line;
            }
        }
    }

    return "";
}

void SmsService::handle_multipart(IncomingSms sms)
{
    const auto &decoded = sms.decoded;
    std::string cache_key = get_cache_key(decoded.number, decoded.concat_ref);

    std::lock_guard<std::mutex> lock(cache_mutex_);

    auto &entry = multipart_cache_[cache_key];

    // Initialize entry if new
    if (entry.parts.empty())
    {
        entry.sender = decoded.number;
        entry.total_parts = decoded.concat_total;
        entry.first_received = std::chrono::steady_clock::now();
        entry.timestamp = decoded.timestamp;
    }

    // Add this part
    entry.parts[decoded.concat_seq] = decoded.text;

    // Check if all parts received
    if (entry.is_complete())
    {

        // Combine all parts
        IncomingSms combined;
        combined.index = sms.index;
        combined.storage = sms.storage;
        combined.decoded = sms.decoded;          // Copy metadata
        combined.decoded.text = entry.combine(); // Replace with combined text
        combined.success = true;
        combined.is_complete = true;

        // Remove from cache
        multipart_cache_.erase(cache_key);

        // Notify callback with combined message
        if (new_sms_callback_)
        {
            new_sms_callback_(combined);
        }
    }
}

std::string SmsService::get_cache_key(const std::string &sender,
                                      uint16_t concat_ref) const
{
    // Key format: "sender_ref" to uniquely identify a multipart SMS
    return sender + "_" + std::to_string(concat_ref);
}

void SmsService::cleanup_cache()
{
    std::lock_guard<std::mutex> lock(cache_mutex_);

    for (auto it = multipart_cache_.begin(); it != multipart_cache_.end();)
    {
        if (it->second.is_timed_out())
        {
            std::cerr << "[SMS] Multipart timeout for " << it->first << ", got "
                      << it->second.parts.size() << "/" << it->second.total_parts
                      << " parts" << std::endl;

            // Could notify with partial message, but for now just discard
            it = multipart_cache_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

std::vector<IncomingSms> SmsService::list_and_decode_messages()
{
    // Refresh cache (this reads, decodes, combines ALL messages)
    refresh_cache();

    // Return all messages from cache
    std::vector<IncomingSms> result;
    {
        // Use shared_lock for read access when copying cache
        std::shared_lock<std::shared_mutex> lock(cache_mutex_messages_);
        result = message_cache_; // Copy all (already decoded and combined)
    }

    return result;
}

std::string SmsService::delete_message(const std::string &storage,
                                       uint8_t index)
{
    try
    {
        // Note: storage parameter is kept for API consistency but internally we use
        // config_.storage This allows for future expansion where different storage
        // locations might be supported
        (void)storage; // Suppress unused parameter warning

        // Check if this index is part of a combined multipart message in cache
        std::vector<uint8_t> indices_to_delete = {index};

        {
            // Use shared_lock for read access when checking cache
            std::shared_lock<std::shared_mutex> lock(cache_mutex_messages_);
            for (const auto &sms : message_cache_)
            {
                if (sms.index == index && !sms.original_indices.empty())
                {
                    // This is a combined message, delete all its parts
                    indices_to_delete = sms.original_indices;
                    break;
                }
            }
        }

        // Delete all indices
        std::string last_error;
        for (uint8_t idx : indices_to_delete)
        {
            auto response_future = at_session_->delete_message(idx);
            auto response = response_future.get();

            if (!response.success)
            {
                std::string error =
                    "Failed to delete index " + std::to_string(static_cast<int>(idx));
                if (!response.data.empty())
                {
                    error += ": " + response.data[0];
                }
                last_error = error;
            }

            std::this_thread::sleep_for(
                std::chrono::milliseconds(config_.read_delay_ms));
        }

        return last_error.empty()
                   ? ""
                   : last_error; // Return last error, or empty if all succeeded
    }
    catch (const std::exception &e)
    {
        return std::string("Exception: ") + e.what();
    }
}

void SmsService::initialize_cache() { refresh_cache(); }

void SmsService::refresh_cache()
{
    try
    {
        // Step 1: Get list of all message indices
        auto response_future = at_session_->send_command("AT+CMGL=4"); // 4 = ALL
        auto response = response_future.get();

        if (!response.success)
        {
            std::cerr << "[SMS] Failed to list messages for cache" << std::endl;
            return;
        }

        // Step 2: Parse response to extract message indices
        auto index_status_pairs = parse_cmgl_response(response);

        // Step 3: Read and decode all messages
        auto all_messages = read_all_messages(index_status_pairs);

        // Step 4: Combine multipart messages
        auto combined_messages = combine_multipart_messages(all_messages);

        // Step 5: Update cache
        update_message_cache(std::move(combined_messages));
    }
    catch (const std::exception &e)
    {
        std::cerr << "[SMS] Error: " << e.what() << std::endl;
    }
}

// ============================================================================
// Cache Refresh Helper Methods (Extracted for better readability)
// ============================================================================

std::vector<std::pair<uint8_t, uint8_t>> SmsService::parse_cmgl_response(
    const at::ResponseBuilder::AtResponse &response)
{
    std::vector<std::pair<uint8_t, uint8_t>> index_status_pairs;

    for (const auto &line : response.data)
    {
        if (line.find("+CMGL:") == 0)
        {
            size_t colon_pos = line.find(':');
            size_t first_comma = line.find(',', colon_pos + 1);
            size_t second_comma = line.find(',', first_comma + 1);

            if (first_comma != std::string::npos &&
                second_comma != std::string::npos)
            {
                std::string index_str =
                    line.substr(colon_pos + 1, first_comma - colon_pos - 1);
                std::string status_str =
                    line.substr(first_comma + 1, second_comma - first_comma - 1);
                try
                {
                    uint8_t index = static_cast<uint8_t>(std::stoi(index_str));
                    uint8_t stat = static_cast<uint8_t>(std::stoi(status_str));
                    index_status_pairs.push_back({index, stat});
                }
                catch (const std::exception &e)
                {
                    std::cerr << "[SMS] Failed to parse index/status: " << e.what()
                              << std::endl;
                }
            }
        }
    }

    return index_status_pairs;
}

std::vector<IncomingSms> SmsService::read_all_messages(
    const std::vector<std::pair<uint8_t, uint8_t>> &index_status_pairs)
{
    std::vector<IncomingSms> all_messages;

    for (const auto &[index, stat] : index_status_pairs)
    {
        auto sms = read_sms(config_.storage, index);
        if (sms.success)
        {
            sms.status = stat;
            all_messages.push_back(sms);
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(config_.read_delay_ms));
    }

    return all_messages;
}

std::vector<IncomingSms>
SmsService::combine_multipart_messages(std::vector<IncomingSms> &raw_messages)
{
    std::map<std::string, MultipartCacheEntry> multipart_map;
    std::vector<IncomingSms> combined_messages;

    // Separate single and multipart messages
    for (auto &sms : raw_messages)
    {
        if (sms.decoded.has_udh && sms.decoded.concat_total > 1)
        {
            // Multipart - collect parts
            std::string cache_key =
                get_cache_key(sms.decoded.number, sms.decoded.concat_ref);
            auto &entry = multipart_map[cache_key];
            if (entry.parts.empty())
            {
                entry.sender = sms.decoded.number;
                entry.total_parts = sms.decoded.concat_total;
                entry.timestamp = sms.decoded.timestamp;
                entry.first_index = sms.index;
                entry.first_received = std::chrono::steady_clock::now();
            }
            entry.parts[sms.decoded.concat_seq] = sms.decoded.text;
            entry.part_indices[sms.decoded.concat_seq] = sms.index;
        }
        else
        {
            // Single part - add directly
            combined_messages.push_back(sms);
        }
    }

    // Combine complete multipart messages
    for (auto &[key, entry] : multipart_map)
    {
        if (entry.is_complete())
        {
            // Create combined message
            IncomingSms combined;
            combined.index = entry.first_index;
            combined.storage = "ME";
            combined.decoded.number = entry.sender;
            combined.decoded.text = entry.combine();
            combined.decoded.timestamp = entry.timestamp;
            combined.decoded.has_udh = false; // Already combined
            combined.decoded.concat_total = 1;
            combined.success = true;
            combined.is_complete = true;

            // Store all original indices for deletion
            combined.original_indices = entry.get_all_indices();

            // Find status from original parts
            for (const auto &sms : raw_messages)
            {
                if (sms.index == entry.first_index)
                {
                    combined.status = sms.status;
                    break;
                }
            }

            combined_messages.push_back(combined);
        }
        else
        {
            // Incomplete multipart - add individual parts
            for (auto &sms : raw_messages)
            {
                std::string sms_key =
                    get_cache_key(sms.decoded.number, sms.decoded.concat_ref);
                if (sms_key == key)
                {
                    combined_messages.push_back(sms);
                }
            }
        }
    }

    return combined_messages;
}

void SmsService::update_message_cache(
    std::vector<IncomingSms> &&combined_messages)
{
    // Use unique_lock for write access - exclusive access for cache updates
    std::unique_lock<std::shared_mutex> lock(cache_mutex_messages_);
    message_cache_ = std::move(combined_messages);
    cache_initialized_.store(true, std::memory_order_release);
}

const std::vector<IncomingSms> &SmsService::get_cached_messages() const
{
    // Fast path: use atomic flag to avoid lock if cache not initialized
    if (!cache_initialized_.load(std::memory_order_acquire))
    {
        static const std::vector<IncomingSms> empty_cache;
        return empty_cache;
    }

    // Use shared_lock for concurrent read access - multiple readers can access
    // simultaneously
    std::shared_lock<std::shared_mutex> lock(cache_mutex_messages_);
    return message_cache_;
}

void SmsService::remove_from_cache(uint8_t index)
{
    // Use unique_lock for write access
    std::unique_lock<std::shared_mutex> lock(cache_mutex_messages_);

    // Remove message with matching index from cache
    auto it = std::remove_if(
        message_cache_.begin(), message_cache_.end(),
        [index](const IncomingSms &sms) { return sms.index == index; });

    if (it != message_cache_.end())
    {
        message_cache_.erase(it, message_cache_.end());
    }
}

void SmsService::add_to_cache(const IncomingSms &sms)
{
    // Use unique_lock for write access
    std::unique_lock<std::shared_mutex> lock(cache_mutex_messages_);

    // Check if message with this index already exists
    auto it = std::find_if(
        message_cache_.begin(), message_cache_.end(),
        [&sms](const IncomingSms &cached) { return cached.index == sms.index; });

    if (it != message_cache_.end())
    {
        // Update existing entry
        *it = sms;
    }
    else
    {
        // Add new entry
        message_cache_.push_back(sms);
    }
}

} // namespace smsrelay
