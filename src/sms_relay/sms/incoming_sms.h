#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace smsrelay::sms {

struct IncomingSms
{
    uint8_t index = 0;
    std::string sender;
    std::string text;
    std::string timestamp;
    bool has_udh = false;
    uint16_t concat_ref = 0;
    uint8_t concat_seq = 0;
    uint8_t concat_total = 0;
    std::chrono::system_clock::time_point received_time;

    // Check if this is part of a multipart SMS
    bool is_multipart() const { return has_udh && concat_total > 1; }

    // Check if this is the last part of a multipart SMS
    bool is_last_part() const
    {
        return is_multipart() && concat_seq == concat_total;
    }

    // Get a unique identifier for this multipart conversation
    std::string get_conversation_id() const
    {
        if (!is_multipart())
            return std::to_string(index);
        return sender + "_" + std::to_string(concat_ref);
    }
};

// Multipart SMS cache entry
struct MultipartSmsCache
{
    std::string conversation_id;
    std::vector<IncomingSms> parts;
    std::chrono::system_clock::time_point first_received;
    std::chrono::system_clock::time_point last_received;

    // Check if all parts are received
    bool is_complete() const
    {
        if (parts.empty())
            return false;
        return parts[0].concat_total == parts.size();
    }

    // Check if this part is already in cache
    bool has_part(uint8_t seq) const
    {
        for (const auto &p : parts)
        {
            if (p.concat_seq == seq)
                return true;
        }
        return false;
    }

    // Add a part to the cache
    void add_part(const IncomingSms &sms)
    {
        parts.push_back(sms);
        last_received = std::chrono::system_clock::now();
        if (parts.size() == 1)
        {
            first_received = last_received;
        }
    }

    // Get timeout duration (default 5 minutes)
    static constexpr std::chrono::minutes TIMEOUT{5};

    // Check if cache has timed out
    bool is_timed_out() const
    {
        auto now = std::chrono::system_clock::now();
        return (now - last_received) > TIMEOUT;
    }

    // Combine all parts into a single SMS
    IncomingSms combine() const
    {
        if (parts.empty())
            return IncomingSms{};

        // Sort by sequence number
        std::vector<IncomingSms> sorted_parts = parts;
        std::sort(sorted_parts.begin(), sorted_parts.end(),
                  [](const IncomingSms &a, const IncomingSms &b) {
                      return a.concat_seq < b.concat_seq;
                  });

        // Create combined SMS
        IncomingSms combined = sorted_parts[0];
        combined.concat_seq = sorted_parts.size();
        combined.concat_total = sorted_parts[0].concat_total;

        // Combine text
        std::string combined_text;
        for (const auto &part : sorted_parts)
        {
            combined_text += part.text;
        }
        combined.text = combined_text;

        return combined;
    }
};

} // namespace smsrelay::sms
