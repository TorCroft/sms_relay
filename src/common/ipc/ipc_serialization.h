#pragma once

#include "ipc_protocol.h"
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>

namespace smsrelay::ipc {

/**
 * @brief IPC serialization utilities
 */
class IpcSerializer {
public:
    // Serialize primitive types
    static std::vector<uint8_t> serialize_u32(uint32_t value) {
        return {
            static_cast<uint8_t>((value >> 24) & 0xFF),
            static_cast<uint8_t>((value >> 16) & 0xFF),
            static_cast<uint8_t>((value >> 8) & 0xFF),
            static_cast<uint8_t>(value & 0xFF)
        };
    }

    static std::vector<uint8_t> serialize_u8(uint8_t value) {
        return {value};
    }

    static std::vector<uint8_t> serialize_str(const std::string& str) {
        std::vector<uint8_t> result;
        uint32_t len = static_cast<uint32_t>(str.size());
        auto len_bytes = serialize_u32(len);
        result.insert(result.end(), len_bytes.begin(), len_bytes.end());
        result.insert(result.end(), str.begin(), str.end());
        return result;
    }

    static std::vector<uint8_t> serialize_vec(const std::vector<uint8_t>& vec) {
        std::vector<uint8_t> result;
        uint32_t len = static_cast<uint32_t>(vec.size());
        auto len_bytes = serialize_u32(len);
        result.insert(result.end(), len_bytes.begin(), len_bytes.end());
        result.insert(result.end(), vec.begin(), vec.end());
        return result;
    }

    // Deserialize primitive types
    static bool deserialize_u32(const uint8_t* data, size_t size, uint32_t& value, size_t& offset) {
        if (offset + 4 > size) return false;
        value = (static_cast<uint32_t>(data[offset]) << 24) |
                (static_cast<uint32_t>(data[offset + 1]) << 16) |
                (static_cast<uint32_t>(data[offset + 2]) << 8) |
                (static_cast<uint32_t>(data[offset + 3]));
        offset += 4;
        return true;
    }

    static bool deserialize_u8(const uint8_t* data, size_t size, uint8_t& value, size_t& offset) {
        if (offset >= size) return false;
        value = data[offset];
        offset += 1;
        return true;
    }

    static bool deserialize_str(const uint8_t* data, size_t size, std::string& value, size_t& offset) {
        uint32_t len = 0;
        if (!deserialize_u32(data, size, len, offset)) return false;
        if (offset + len > size) return false;
        value.assign(reinterpret_cast<const char*>(data + offset), len);
        offset += len;
        return true;
    }

    static bool deserialize_vec(const uint8_t* data, size_t size, std::vector<uint8_t>& value, size_t& offset) {
        uint32_t len = 0;
        if (!deserialize_u32(data, size, len, offset)) return false;
        if (offset + len > size) return false;
        value.assign(data + offset, data + offset + len);
        offset += len;
        return true;
    }

    // Serialize complete request
    static std::vector<uint8_t> serialize_request(CommandType cmd, uint32_t seq, const std::vector<uint8_t>& payload) {
        std::vector<uint8_t> result;

        // Header
        auto magic = serialize_u32(IPC_MAGIC);
        auto length = serialize_u32(static_cast<uint32_t>(payload.size()));
        auto command = serialize_u32(static_cast<uint32_t>(cmd));
        auto sequence = serialize_u32(seq);

        result.insert(result.end(), magic.begin(), magic.end());
        result.insert(result.end(), length.begin(), length.end());
        result.insert(result.end(), command.begin(), command.end());
        result.insert(result.end(), sequence.begin(), sequence.end());

        // Payload
        result.insert(result.end(), payload.begin(), payload.end());

        return result;
    }

    // Serialize response
    static std::vector<uint8_t> serialize_response(Status status, uint32_t seq, const std::vector<uint8_t>& payload) {
        std::vector<uint8_t> result;

        // Header
        auto magic = serialize_u32(IPC_MAGIC);
        auto length = serialize_u32(static_cast<uint32_t>(payload.size()));
        auto status_code = serialize_u32(static_cast<uint32_t>(status));
        auto sequence = serialize_u32(seq);

        result.insert(result.end(), magic.begin(), magic.end());
        result.insert(result.end(), length.begin(), length.end());
        result.insert(result.end(), status_code.begin(), status_code.end());
        result.insert(result.end(), sequence.begin(), sequence.end());

        // Payload
        result.insert(result.end(), payload.begin(), payload.end());

        return result;
    }
};

/**
 * @brief Payload serializers for specific commands
 */
class PayloadSerializer {
public:
    // List SMS
    static std::vector<uint8_t> serialize_list(const ListSmsRequest& req) {
        std::vector<uint8_t> result;
        auto status_bytes = IpcSerializer::serialize_str(req.status);
        result.insert(result.end(), status_bytes.begin(), status_bytes.end());
        return result;
    }

    // Read SMS
    static std::vector<uint8_t> serialize_read(const ReadSmsRequest& req) {
        std::vector<uint8_t> result;
        auto indices_bytes = IpcSerializer::serialize_vec(req.indices);
        result.insert(result.end(), indices_bytes.begin(), indices_bytes.end());
        return result;
    }

    // Delete SMS
    static std::vector<uint8_t> serialize_delete(const DeleteSmsRequest& req) {
        std::vector<uint8_t> result;
        auto indices_bytes = IpcSerializer::serialize_vec(req.indices);
        result.insert(result.end(), indices_bytes.begin(), indices_bytes.end());
        return result;
    }

    // Send SMS
    static std::vector<uint8_t> serialize_send(const SendSmsRequest& req) {
        std::vector<uint8_t> result;
        auto recipient_bytes = IpcSerializer::serialize_str(req.recipient);
        auto text_bytes = IpcSerializer::serialize_str(req.text);
        result.insert(result.end(), recipient_bytes.begin(), recipient_bytes.end());
        result.insert(result.end(), text_bytes.begin(), text_bytes.end());
        return result;
    }

    // Deserialize SMS info
    static bool deserialize_sms_info(const uint8_t* data, size_t size, SmsInfo& info, size_t& offset) {
        uint8_t has_udh_val = 0;
        uint8_t concat_seq_val = 0;
        uint8_t concat_total_val = 0;
        uint8_t is_combined_val = 0;

        if (!IpcSerializer::deserialize_u8(data, size, info.index, offset)) return false;
        if (!IpcSerializer::deserialize_str(data, size, info.sender, offset)) return false;
        if (!IpcSerializer::deserialize_str(data, size, info.text, offset)) return false;
        if (!IpcSerializer::deserialize_str(data, size, info.timestamp, offset)) return false;
        if (!IpcSerializer::deserialize_u8(data, size, has_udh_val, offset)) return false;
        if (!IpcSerializer::deserialize_u8(data, size, concat_seq_val, offset)) return false;
        if (!IpcSerializer::deserialize_u8(data, size, concat_total_val, offset)) return false;
        if (!IpcSerializer::deserialize_u8(data, size, info.status, offset)) return false;
        if (!IpcSerializer::deserialize_u8(data, size, is_combined_val, offset)) return false;

        info.has_udh = has_udh_val != 0;
        info.concat_seq = concat_seq_val;
        info.concat_total = concat_total_val;
        info.is_combined = is_combined_val != 0;

        // Deserialize parts if combined
        if (info.is_combined) {
            uint32_t part_count = 0;
            if (!IpcSerializer::deserialize_u32(data, size, part_count, offset)) return false;

            for (uint32_t i = 0; i < part_count; ++i) {
                SmsPartInfo part;
                uint8_t index, seq, total;
                if (!IpcSerializer::deserialize_u8(data, size, index, offset)) return false;
                if (!IpcSerializer::deserialize_u8(data, size, seq, offset)) return false;
                if (!IpcSerializer::deserialize_u8(data, size, total, offset)) return false;
                part.index = index;
                part.seq = seq;
                part.total = total;
                info.parts.push_back(part);
            }
        }

        return true;
    }

    // Serialize SMS info
    static std::vector<uint8_t> serialize_sms_info(const SmsInfo& info) {
        std::vector<uint8_t> result;
        auto index_bytes = IpcSerializer::serialize_u8(info.index);
        auto sender_bytes = IpcSerializer::serialize_str(info.sender);
        auto text_bytes = IpcSerializer::serialize_str(info.text);
        auto time_bytes = IpcSerializer::serialize_str(info.timestamp);
        auto has_udh_bytes = IpcSerializer::serialize_u8(info.has_udh);
        auto seq_bytes = IpcSerializer::serialize_u8(info.concat_seq);
        auto total_bytes = IpcSerializer::serialize_u8(info.concat_total);
        auto status_bytes = IpcSerializer::serialize_u8(info.status);
        auto is_combined_bytes = IpcSerializer::serialize_u8(info.is_combined);

        result.insert(result.end(), index_bytes.begin(), index_bytes.end());
        result.insert(result.end(), sender_bytes.begin(), sender_bytes.end());
        result.insert(result.end(), text_bytes.begin(), text_bytes.end());
        result.insert(result.end(), time_bytes.begin(), time_bytes.end());
        result.insert(result.end(), has_udh_bytes.begin(), has_udh_bytes.end());
        result.insert(result.end(), seq_bytes.begin(), seq_bytes.end());
        result.insert(result.end(), total_bytes.begin(), total_bytes.end());
        result.insert(result.end(), status_bytes.begin(), status_bytes.end());
        result.insert(result.end(), is_combined_bytes.begin(), is_combined_bytes.end());

        // Serialize parts if combined
        if (info.is_combined) {
            auto part_count_bytes = IpcSerializer::serialize_u32(static_cast<uint32_t>(info.parts.size()));
            result.insert(result.end(), part_count_bytes.begin(), part_count_bytes.end());

            for (const auto& part : info.parts) {
                result.push_back(part.index);
                result.push_back(part.seq);
                result.push_back(part.total);
            }
        }

        return result;
    }
};

} // namespace smsrelay::ipc
