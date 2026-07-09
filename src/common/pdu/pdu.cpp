#include "pdu.h"

#include <array>
#include <cstring>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <string_view>
#include <iostream>

// =============================================================================
// Internal Implementation Details
// =============================================================================

namespace pdu::detail {

// -----------------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------------

namespace constants {
    // Encoding indices from DCS bits 3-2
    constexpr uint8_t DCS_GSM7 = 0x00;   // bits 3-2 = 00
    constexpr uint8_t DCS_8BIT = 0x01;   // bits 3-2 = 01
    constexpr uint8_t DCS_UCS2 = 0x02;   // bits 3-2 = 10
    constexpr uint8_t DCS_RESERVED = 0x03;  // bits 3-2 = 11

    constexpr uint8_t MT_SMS_DELIVER = 0x00;
    constexpr uint8_t MT_SMS_SUBMIT = 0x01;
    constexpr uint8_t MT_SMS_STATUS_REPORT = 0x02;

    constexpr size_t MAX_GSM7_CHARS = 160;
    constexpr size_t MAX_UCS2_CHARS = 70;

    constexpr size_t MAX_GSM7_WITH_UDH = 153;
    constexpr size_t MAX_UCS2_WITH_UDH = 67;
}

// -----------------------------------------------------------------------------
// Optimized GSM-7 Lookup Table (constexpr array for O(1) access)
// -----------------------------------------------------------------------------

struct Gsm7Char {
    uint32_t code_point;
    uint8_t position;
};

constexpr std::array<Gsm7Char, 128> GSM7_LOOKUP = {{
    // Positions 0-15: Special symbols
    {0x40, 0},   // @
    {0xA3, 1},   // £
    {0x24, 2},   // $
    {0xA5, 3},   // ¥
    {0xE8, 4},   // è
    {0xE9, 5},   // é
    {0xF9, 6},   // ù
    {0xEC, 7},   // ì
    {0xF2, 8},   // ò
    {0xC7, 9},   // Ç
    {0x0A, 10},  // \n
    {0xD8, 11},  // Ø
    {0xF8, 12},  // ø
    {0x0D, 13},  // \r
    {0xC5, 14},  // Å
    {0xE5, 15},  // å
    // Position 16: Δ (Greek delta)
    {0x0394, 16},
    // Position 17: _
    {0x5F, 17},
    // Positions 18-27: Greek letters
    {0x03A6, 18}, // Φ
    {0x0393, 19}, // Γ
    {0x039B, 20}, // Λ
    {0x03A9, 21}, // Ω
    {0x03A0, 22}, // Π
    {0x03A8, 23}, // Ψ
    {0x03A3, 24}, // Σ
    {0x0398, 25}, // Θ
    {0x039E, 26}, // Ξ
    {0x001B, 27}, // ESC
    // Positions 28-31
    {0xC6, 28},  // Æ
    {0xAE, 29},  // æ
    {0xDF, 30},  // ß
    {0xC9, 31},  // É
    // Positions 32-64: ASCII range (space through ¡)
    {0x20, 32},  // space
    {0x21, 33},  // !
    {0x22, 34},  // "
    {0x23, 35},  // #
    {0xA4, 36},  // ¤
    {0x25, 37},  // %
    {0x26, 38},  // &
    {0x27, 39},  // '
    {0x28, 40},  // (
    {0x29, 41},  // )
    {0x2A, 42},  // *
    {0x2B, 43},  // +
    {0x2C, 44},  // ,
    {0x2D, 45},  // -
    {0x2E, 46},  // .
    {0x2F, 47},  // /
    {0x30, 48},  // 0
    {0x31, 49},  // 1
    {0x32, 50},  // 2
    {0x33, 51},  // 3
    {0x34, 52},  // 4
    {0x35, 53},  // 5
    {0x36, 54},  // 6
    {0x37, 55},  // 7
    {0x38, 56},  // 8
    {0x39, 57},  // 9
    {0x3A, 58},  // :
    {0x3B, 59},  // ;
    {0x3C, 60},  // <
    {0x3D, 61},  // =
    {0x3E, 62},  // >
    {0x3F, 63},  // ?
    {0xA1, 64},  // ¡
    // Positions 65-90: A-Z
    {0x41, 65},  // A
    {0x42, 66},  // B
    {0x43, 67},  // C
    {0x44, 68},  // D
    {0x45, 69},  // E
    {0x46, 70},  // F
    {0x47, 71},  // G
    {0x48, 72},  // H
    {0x49, 73},  // I
    {0x4A, 74},  // J
    {0x4B, 75},  // K
    {0x4C, 76},  // L
    {0x4D, 77},  // M
    {0x4E, 78},  // N
    {0x4F, 79},  // O
    {0x50, 80},  // P
    {0x51, 81},  // Q
    {0x52, 82},  // R
    {0x53, 83},  // S
    {0x54, 84},  // T
    {0x55, 85},  // U
    {0x56, 86},  // V
    {0x57, 87},  // W
    {0x58, 88},  // X
    {0x59, 89},  // Y
    {0x5A, 90},  // Z
    // Positions 91-96
    {0xC4, 91},  // Ä
    {0xD6, 92},  // Ö
    {0xD1, 93},  // Ñ
    {0xDC, 94},  // Ü
    {0x60, 95},  // `
    {0xBF, 96},  // ¿
    // Positions 97-122: a-z
    {0x61, 97},  // a
    {0x62, 98},  // b
    {0x63, 99},  // c
    {0x64, 100}, // d
    {0x65, 101}, // e
    {0x66, 102}, // f
    {0x67, 103}, // g
    {0x68, 104}, // h
    {0x69, 105}, // i
    {0x6A, 106}, // j
    {0x6B, 107}, // k
    {0x6C, 108}, // l
    {0x6D, 109}, // m
    {0x6E, 110}, // n
    {0x6F, 111}, // o
    {0x70, 112}, // p
    {0x71, 113}, // q
    {0x72, 114}, // r
    {0x73, 115}, // s
    {0x74, 116}, // t
    {0x75, 117}, // u
    {0x76, 118}, // v
    {0x77, 119}, // w
    {0x78, 120}, // x
    {0x79, 121}, // y
    {0x7A, 122}, // z
    // Positions 123-127
    {0xE4, 123}, // ä
    {0xF6, 124}, // ö
    {0xF1, 125}, // ñ
    {0xFC, 126}, // ü
    {0xE0, 127}  // à
}};

// GSM-7 extended table
constexpr std::array<std::pair<char, uint8_t>, 9> GSM7_EXTENDED = {{
    {'\f', 0x0A},  // Form feed
    {'^', 0x14},
    {'{', 0x28},
    {'}', 0x29},
    {'\\', 0x2F},
    {'[', 0x3C},
    {'~', 0x3D},
    {']', 0x3E},
    {'|', 0x40}
}};

// Helper to get decoded character from position
[[nodiscard]] inline std::string gsm7_get_char(uint8_t position) {
    // Return UTF-8 string for given GSM-7 position
    switch (position) {
        // Positions 0-15: Special symbols
        case 0: return "\x40";     // @
        case 1: return "\xC2\xA3"; // £ (UTF-8)
        case 2: return "\x24";     // $
        case 3: return "\xC2\xA5"; // ¥ (UTF-8)
        case 4: return "\xC3\xA8"; // è (UTF-8)
        case 5: return "\xC3\xA9"; // é (UTF-8)
        case 6: return "\xC3\xB9"; // ù (UTF-8)
        case 7: return "\xC3\xAC"; // ì (UTF-8)
        case 8: return "\xC3\xB2"; // ò (UTF-8)
        case 9: return "\xC3\x87"; // Ç (UTF-8)
        case 10: return "\x0A";    // \n
        case 11: return "\xC3\x98"; // Ø (UTF-8)
        case 12: return "\xC3\xB8"; // ø (UTF-8)
        case 13: return "\x0D";    // \r
        case 14: return "\xC3\x85"; // Å (UTF-8)
        case 15: return "\xC3\xA5"; // å (UTF-8)
        // Positions 16-27: Greek letters
        case 16: return "\xCE\x94"; // Δ (UTF-8)
        case 17: return "\x5F";     // _
        case 18: return "\xCE\xA6"; // Φ (UTF-8)
        case 19: return "\xCE\x93"; // Γ (UTF-8)
        case 20: return "\xCE\x9B"; // Λ (UTF-8)
        case 21: return "\xCE\xA9"; // Ω (UTF-8)
        case 22: return "\xCE\xA0"; // Π (UTF-8)
        case 23: return "\xCE\xA8"; // Ψ (UTF-8)
        case 24: return "\xCE\xA3"; // Σ (UTF-8)
        case 25: return "\xCE\x98"; // Θ (UTF-8)
        case 26: return "\xCE\x9E"; // Ξ (UTF-8)
        case 27: return "\x1B";     // ESC
        // Positions 28-31
        case 28: return "\xC3\x86"; // Æ (UTF-8)
        case 29: return "\xC3\xAE"; // æ (UTF-8)
        case 30: return "\xC3\x9F"; // ß (UTF-8)
        case 31: return "\xC3\x89"; // É (UTF-8)
        // Positions 32-126: ASCII compatible
        default:
            if (position >= 32 && position <= 126) {
                return std::string(1, static_cast<char>(position));
            }
            return "?";  // Unknown position
    }
}

// -----------------------------------------------------------------------------
// Utilities
// -----------------------------------------------------------------------------

[[nodiscard]] inline std::vector<uint8_t> hex_to_bytes(std::string_view hex) {
    std::vector<uint8_t> result;
    result.reserve(hex.length() / 2);

    for (size_t i = 0; i + 1 < hex.length(); i += 2) {
        uint8_t high = 0, low = 0;
        char h = hex[i];
        char l = hex[i + 1];

        if (h >= '0' && h <= '9') high = h - '0';
        else if (h >= 'A' && h <= 'F') high = h - 'A' + 10;
        else if (h >= 'a' && h <= 'f') high = h - 'a' + 10;

        if (l >= '0' && l <= '9') low = l - '0';
        else if (l >= 'A' && l <= 'F') low = l - 'A' + 10;
        else if (l >= 'a' && l <= 'f') low = l - 'a' + 10;

        result.push_back((high << 4) | low);
    }
    return result;
}

[[nodiscard]] inline std::string bytes_to_hex(const std::vector<uint8_t>& bytes) {
    std::ostringstream oss;
    oss << std::uppercase << std::hex << std::setfill('0');
    for (uint8_t b : bytes) {
        oss << std::setw(2) << static_cast<int>(b);
    }
    return oss.str();
}

// -----------------------------------------------------------------------------
// UTF-8 Helper Functions
// -----------------------------------------------------------------------------

[[nodiscard]] inline uint32_t utf8_to_codepoint(std::string_view str, size_t& pos) {
    unsigned char c = static_cast<unsigned char>(str[pos]);
    uint32_t code_point = 0;
    size_t char_len = 1;

    if ((c & 0x80) == 0) {
        code_point = c;
    } else if ((c & 0xE0) == 0xC0 && pos + 1 < str.length()) {
        code_point = ((c & 0x1F) << 6) | (str[pos + 1] & 0x3F);
        char_len = 2;
    } else if ((c & 0xF0) == 0xE0 && pos + 2 < str.length()) {
        code_point = ((c & 0x0F) << 12) | ((str[pos + 1] & 0x3F) << 6) | (str[pos + 2] & 0x3F);
        char_len = 3;
    } else if ((c & 0xF8) == 0xF0 && pos + 3 < str.length()) {
        code_point = ((c & 0x07) << 18) | ((str[pos + 1] & 0x3F) << 12) |
                     ((str[pos + 2] & 0x3F) << 6) | (str[pos + 3] & 0x3F);
        char_len = 4;
    }

    pos += char_len;
    return code_point;
}

// -----------------------------------------------------------------------------
// GSM-7 Encoding/Decoding
// -----------------------------------------------------------------------------

[[nodiscard]] inline bool gsm7_find_position(uint32_t code_point, uint8_t& position) {
    for (const auto& entry : GSM7_LOOKUP) {
        if (entry.code_point == code_point) {
            position = entry.position;
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline std::vector<uint8_t> gsm7_encode(std::string_view text, bool discard_invalid = false) {
    std::vector<uint8_t> result;
    result.reserve(text.length());

    for (size_t i = 0; i < text.length(); ) {
        uint32_t code_point = utf8_to_codepoint(text, i);

        uint8_t position;
        if (gsm7_find_position(code_point, position)) {
            result.push_back(position);
        } else {
            // Check extended table
            bool found = false;
            for (const auto& ext : GSM7_EXTENDED) {
                if (static_cast<uint32_t>(static_cast<unsigned char>(ext.first)) == code_point) {
                    result.push_back(0x1B);  // ESC
                    result.push_back(ext.second);
                    found = true;
                    break;
                }
            }

            if (!found && !discard_invalid) {
                throw std::invalid_argument("Cannot encode character in GSM-7: code point 0x" +
                    std::to_string(code_point));
            }
        }
    }

    return result;
}

[[nodiscard]] inline std::string gsm7_decode(const std::vector<uint8_t>& encoded) {
    std::string result;
    result.reserve(encoded.size() * 2);  // Reserve more space for UTF-8

    for (size_t i = 0; i < encoded.size(); ++i) {
        uint8_t byte_val = encoded[i];

        if (byte_val == 0x1B && i + 1 < encoded.size()) {  // Escape
            uint8_t next_val = encoded[++i];
            bool found = false;
            for (const auto& ext : GSM7_EXTENDED) {
                if (ext.second == next_val) {
                    result += ext.first;
                    found = true;
                    break;
                }
            }
            if (!found) {
                // Invalid extended sequence, skip
            }
        } else if (byte_val < 128) {
            result += gsm7_get_char(byte_val);
        }
    }

    return result;
}

// -----------------------------------------------------------------------------
// Septet Packing/Unpacking
// -----------------------------------------------------------------------------

[[nodiscard]] inline std::vector<uint8_t> septets_pack(const std::vector<uint8_t>& septets, size_t pad_bits = 0) {
    if (septets.empty()) return {};

    std::vector<uint8_t> result;
    result.reserve(septets.size() * 7 / 8 + 2);

    int shift = static_cast<int>(pad_bits);
    uint8_t prev_septet = (pad_bits == 0) ? septets[0] : 0;
    size_t start_idx = (pad_bits == 0) ? 1 : 0;

    for (size_t i = start_idx; i < septets.size(); ++i) {
        uint8_t septet = septets[i] & 0x7F;

        if (shift == 7) {
            shift = 0;
            prev_septet = septet;
            continue;
        }

        uint8_t b = ((septet << (7 - shift)) & 0xFF) | (prev_septet >> shift);
        prev_septet = septet;
        shift++;
        result.push_back(b);
    }

    if (shift != 7) {
        result.push_back(prev_septet >> shift);
    }

    return result;
}

[[nodiscard]] inline std::vector<uint8_t> septets_unpack(const std::vector<uint8_t>& packed,
                                                          size_t num_septets = 0,
                                                          uint8_t prev_octet = 0,
                                                          int shift = 7) {
    std::vector<uint8_t> result;

    if (num_septets == 0) num_septets = packed.size() * 8 / 7 + 1;

    size_t count = 0;
    for (uint8_t octet : packed) {
        if (count >= num_septets) break;
        count++;

        if (shift == 7) {
            shift = 1;
            if (prev_octet != 0) {
                result.push_back(prev_octet >> 1);
            }
            if (count <= num_septets) {
                result.push_back(octet & 0x7F);
                prev_octet = octet;
            }
            if (count == num_septets) break;
            continue;
        }

        uint8_t b = ((octet << shift) & 0x7F) | (prev_octet >> (8 - shift));
        prev_octet = octet;
        result.push_back(b);
        shift++;

        if (count == num_septets) break;
    }

    if (shift == 7 && prev_octet != 0) {
        uint8_t b = prev_octet >> (8 - shift);
        if (b) result.push_back(b);
    }

    return result;
}

// -----------------------------------------------------------------------------
// UCS-2 Encoding/Decoding
// -----------------------------------------------------------------------------

[[nodiscard]] inline std::vector<uint8_t> ucs2_encode(std::string_view text) {
    std::vector<uint8_t> result;
    result.reserve(text.length() * 2);

    for (size_t i = 0; i < text.length(); ) {
        uint32_t code_point = utf8_to_codepoint(text, i);
        uint16_t value = static_cast<uint16_t>(code_point & 0xFFFF);
        result.push_back(value >> 8);
        result.push_back(value & 0xFF);
    }

    return result;
}

[[nodiscard]] inline std::string ucs2_decode(const std::vector<uint8_t>& encoded) {
    std::string result;
    result.reserve(encoded.size());  // UTF-8 may be up to 3 bytes per UCS2 char

    for (size_t i = 0; i + 1 < encoded.size(); i += 2) {
        // UCS-2 is big-endian: first byte is high byte
        uint16_t ucs2_char = (encoded[i] << 8) | encoded[i + 1];

        // Convert UCS-2/UTF-16 to UTF-8
        if (ucs2_char < 0x80) {
            // 1 byte: 0xxxxxxx
            result += static_cast<char>(ucs2_char);
        } else if (ucs2_char < 0x800) {
            // 2 bytes: 110xxxxx 10xxxxxx
            result += static_cast<char>(0xC0 | (ucs2_char >> 6));
            result += static_cast<char>(0x80 | (ucs2_char & 0x3F));
        } else {
            // 3 bytes: 1110xxxx 10xxxxxx 10xxxxxx
            result += static_cast<char>(0xE0 | (ucs2_char >> 12));
            result += static_cast<char>(0x80 | ((ucs2_char >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (ucs2_char & 0x3F));
        }
    }

    return result;
}

// -----------------------------------------------------------------------------
// Semi-Octet Encoding/Decoding
// -----------------------------------------------------------------------------

[[nodiscard]] inline std::vector<uint8_t> semi_octets_encode(std::string_view number) {
    std::string num(number);  // Convert to string for manipulation
    if (num.length() % 2 == 1) {
        num += 'F';
    }

    std::vector<uint8_t> result;
    result.reserve(num.length() / 2);

    for (size_t i = 0; i < num.length(); i += 2) {
        uint8_t high = 0, low = 0;
        char h = num[i + 1];
        char l = num[i];

        if (h >= '0' && h <= '9') high = h - '0';
        else if (h >= 'A' && h <= 'F') high = h - 'A' + 10;
        else if (h >= 'a' && h <= 'f') high = h - 'a' + 10;

        if (l >= '0' && l <= '9') low = l - '0';
        else if (l >= 'A' && l <= 'F') low = l - 'A' + 10;
        else if (l >= 'a' && l <= 'f') low = l - 'a' + 10;

        result.push_back((high << 4) | low);
    }

    return result;
}

[[nodiscard]] inline std::string semi_octets_decode(const std::vector<uint8_t>& encoded, size_t num_octets = 0) {
    std::string result;
    result.reserve(encoded.size() * 2);

    size_t limit = (num_octets == 0) ? encoded.size() : num_octets;

    for (size_t i = 0; i < limit && i < encoded.size(); ++i) {
        uint8_t octet = encoded[i];
        uint8_t low = octet & 0x0F;
        uint8_t high = (octet >> 4) & 0x0F;

        result += static_cast<char>('0' + low);
        if (high == 0x0F) break;
        result += static_cast<char>('0' + high);
    }

    return result;
}

// -----------------------------------------------------------------------------
// Address Field Encoding/Decoding
// -----------------------------------------------------------------------------

struct AddressField {
    std::string number;
    uint8_t length = 0;
    uint8_t type = 0;
    std::vector<uint8_t> encoded;
};

[[nodiscard]] inline AddressField build_address_field(std::string_view address, bool smsc_field = false) {
    AddressField result;

    // Build Type-of-Address
    uint8_t toa = 0x80 | 0x01;  // Unknown/International | ISDN
    std::string addr_value;
    bool is_alphanumeric = false;

    // Parse address
    if (!address.empty()) {
        if (address[0] == '+' && address.length() > 1) {
            toa |= 0x10;  // International
            addr_value = address.substr(1);
        } else if (address.find_first_not_of("0123456789") == std::string::npos) {
            toa |= 0x20;  // National
            addr_value = address;
        } else {
            toa |= 0x50;  // Alphanumeric
            is_alphanumeric = true;
            addr_value = address;
        }
    }

    result.type = toa;

    // Encode address value
    if (is_alphanumeric) {
        auto gsm7 = gsm7_encode(addr_value, true);
        result.encoded = septets_pack(gsm7);
        result.length = static_cast<uint8_t>(result.encoded.size() * 2);
    } else {
        result.encoded = semi_octets_encode(addr_value);
        result.length = smsc_field ?
            static_cast<uint8_t>(result.encoded.size() + 1) :
            static_cast<uint8_t>(addr_value.length());
    }

    result.number = std::string(address);
    return result;
}

[[nodiscard]] inline AddressField parse_address_field(std::vector<uint8_t>::const_iterator& it, bool smsc_field = false) {
    AddressField result;
    result.length = *it++;

    if (result.length == 0) {
        return result;
    }

    result.type = *it++;
    uint8_t ton = result.type & 0x70;

    // Alphanumeric address
    if (ton == 0x50) {
        size_t octet_count = (result.length + 1) / 2;
        std::vector<uint8_t> encoded(it, it + octet_count);
        it += octet_count;

        auto septets = septets_unpack(encoded, octet_count);
        result.number = gsm7_decode(septets);
        return result;
    }

    // Numeric address
    size_t octet_count = smsc_field ? (result.length - 1) : ((result.length + 1) / 2);
    std::string decoded = semi_octets_decode(std::vector<uint8_t>(it, it + octet_count), octet_count);
    it += octet_count;

    // Add international prefix
    if (ton == 0x10) {
        result.number = "+" + decoded;
    } else {
        result.number = decoded;
    }

    return result;
}

// -----------------------------------------------------------------------------
// Timestamp Encoding/Decoding
// -----------------------------------------------------------------------------

[[nodiscard]] inline std::string parse_timestamp(std::vector<uint8_t>::const_iterator& it) {
    std::vector<uint8_t> ts_bytes(it, it + 7);
    it += 7;

    std::string decoded = semi_octets_decode(ts_bytes, 7);

    if (decoded.length() >= 14) {
        std::string ts = decoded.substr(0, 12);
        std::string tz_str = decoded.substr(12, 2);

        // Format: YYMMDDHHMMSS + timezone
        std::string formatted = ts.substr(0, 2) + "/" + ts.substr(2, 2) + "/" + ts.substr(4, 2) + " " +
                               ts.substr(6, 2) + ":" + ts.substr(8, 2) + ":" + ts.substr(10, 2);

        // Parse timezone
        int tz_hex = std::stoi(tz_str, nullptr, 16);
        if (tz_hex & 0x80) {
            int magnitude = (tz_hex & 0x7F);
            formatted += " -" + std::to_string(magnitude * 15) + "min";
        } else {
            formatted += " +" + std::to_string(tz_hex * 15) + "min";
        }

        return formatted;
    }

    return decoded;
}

// -----------------------------------------------------------------------------
// Data Coding Scheme
// -----------------------------------------------------------------------------

[[nodiscard]] inline uint8_t parse_data_coding_scheme(uint8_t octet) {
    if ((octet & 0xC0) == 0) {  // General data coding group
        return (octet & 0x0C) >> 2;
    }
    return constants::DCS_GSM7;  // Default
}

// -----------------------------------------------------------------------------
// User Data Header (UDH) Parsing
// -----------------------------------------------------------------------------

struct UdhInfo {
    bool present = false;
    uint16_t concat_ref = 0;
    uint8_t concat_seq = 0;
    uint8_t concat_total = 0;
};

[[nodiscard]] inline UdhInfo parse_udh(std::vector<uint8_t>::const_iterator& it, uint8_t udh_len) {
    UdhInfo info;
    info.present = true;

    size_t ie_len_read = 0;
    while (ie_len_read < udh_len) {
        uint8_t iei = *it++;
        uint8_t ie_len = *it++;

        // Check for concatenation IE
        if (iei == 0x00 && ie_len >= 3) {  // 8-bit concatenation
            info.concat_ref = *it++;
            info.concat_total = *it++;
            info.concat_seq = *it++;
            it += ie_len - 3;
        } else if (iei == 0x08 && ie_len >= 4) {  // 16-bit concatenation
            info.concat_ref = (*it++ << 8) | *it++;
            info.concat_total = *it++;
            info.concat_seq = *it++;
            it += ie_len - 4;
        } else {
            it += ie_len;
        }
        ie_len_read += ie_len + 2;
    }

    return info;
}

// -----------------------------------------------------------------------------
// SMS-SUBMIT Encoding
// -----------------------------------------------------------------------------

[[nodiscard]] inline std::vector<uint8_t> encode_submit_pdu_internal(
    std::string_view number,
    std::string_view text,
    uint8_t reference,
    std::string_view smsc,
    uint8_t dcs,
    bool with_udh = false,
    uint16_t concat_ref = 0,
    uint8_t concat_seq = 0,
    uint8_t concat_total = 0
) {
    std::vector<uint8_t> pdu;

    // SMSC
    if (smsc.empty()) {
        pdu.push_back(0x00);
    } else {
        auto smsc_field = build_address_field(smsc, true);
        pdu.push_back(smsc_field.length);
        pdu.push_back(smsc_field.type);
        pdu.insert(pdu.end(), smsc_field.encoded.begin(), smsc_field.encoded.end());
    }

    // TPDU header
    uint8_t tpdu_first = constants::MT_SMS_SUBMIT;
    if (with_udh) tpdu_first |= 0x40;
    pdu.push_back(tpdu_first);
    pdu.push_back(reference);

    // Destination address
    auto dest_addr = build_address_field(number, false);
    pdu.push_back(dest_addr.length);
    pdu.push_back(dest_addr.type);
    pdu.insert(pdu.end(), dest_addr.encoded.begin(), dest_addr.encoded.end());

    pdu.push_back(0x00);  // Protocol identifier
    pdu.push_back(dcs);   // Data coding scheme

    // User data
    std::vector<uint8_t> user_data;
    size_t user_data_length;

    if (dcs == constants::DCS_GSM7) {
        auto septets = gsm7_encode(text);
        user_data_length = septets.size();

        std::vector<uint8_t> udh;
        if (with_udh) {
            udh.push_back(0x00);  // Concatenation IEI (8-bit ref)
            udh.push_back(0x03);  // IE length
            udh.push_back(static_cast<uint8_t>(concat_ref));
            udh.push_back(concat_total);
            udh.push_back(concat_seq);

            int shift = static_cast<int>((udh.size() + 1) * 8) % 7;
            user_data = septets_pack(septets, shift);
            if (shift > 0) user_data_length += 1;
        } else {
            user_data = septets_pack(septets);
        }
    } else {
        user_data = ucs2_encode(text);
        user_data_length = user_data.size();

        std::vector<uint8_t> udh;
        if (with_udh) {
            udh.push_back(0x00);
            udh.push_back(0x03);
            udh.push_back(static_cast<uint8_t>(concat_ref));
            udh.push_back(concat_total);
            udh.push_back(concat_seq);
        }

        user_data_length += udh.size();
        if (with_udh) {
            user_data.insert(user_data.begin(), udh.begin(), udh.end());
            user_data_length += 1;  // UDH length byte
        }
    }

    // Add user data length and data
    if (dcs == constants::DCS_GSM7 && with_udh) {
        std::vector<uint8_t> udh;
        udh.push_back(0x00);
        udh.push_back(0x03);
        udh.push_back(static_cast<uint8_t>(concat_ref));
        udh.push_back(concat_total);
        udh.push_back(concat_seq);

        user_data_length += udh.size() + 1;
        pdu.push_back(static_cast<uint8_t>(user_data_length));
        pdu.push_back(static_cast<uint8_t>(udh.size()));
        pdu.insert(pdu.end(), udh.begin(), udh.end());
        pdu.insert(pdu.end(), user_data.begin(), user_data.end());
    } else {
        pdu.push_back(static_cast<uint8_t>(user_data_length));
        pdu.insert(pdu.end(), user_data.begin(), user_data.end());
    }

    return pdu;
}

} // namespace detail

// =============================================================================
// Public API Implementation
// =============================================================================

namespace pdu {

std::vector<std::string> encode_submit(
    const std::string& number,
    const std::string& text,
    uint8_t reference,
    const std::string& smsc
) {
    using namespace detail::constants;

    // Validate inputs
    if (number.empty()) {
        throw std::invalid_argument("Number cannot be empty");
    }
    if (text.empty()) {
        throw std::invalid_argument("Text cannot be empty");
    }

    // Determine encoding
    uint8_t dcs = DCS_GSM7;
    size_t max_len = MAX_GSM7_CHARS;

    try {
        [[maybe_unused]] auto test = detail::gsm7_encode(text);
        (void)test;
    } catch (const std::invalid_argument&) {
        dcs = DCS_UCS2;
        max_len = MAX_UCS2_CHARS;
    }

    // Check if concatenation needed
    if (text.length() > max_len) {
        size_t with_udh_max = (dcs == DCS_GSM7) ? MAX_GSM7_WITH_UDH : MAX_UCS2_WITH_UDH;
        size_t total_parts = (text.length() + with_udh_max - 1) / with_udh_max;

        std::vector<std::string> pdus;
        for (size_t part = 0; part < total_parts; ++part) {
            size_t start = part * with_udh_max;
            size_t len = std::min(with_udh_max, text.length() - start);
            std::string part_text = text.substr(start, len);

            auto pdu_bytes = detail::encode_submit_pdu_internal(
                number, part_text, reference, smsc, dcs,
                true, reference, static_cast<uint8_t>(part + 1), static_cast<uint8_t>(total_parts)
            );
            pdus.push_back(detail::bytes_to_hex(pdu_bytes));
        }
        return pdus;
    }

    // Single PDU
    auto pdu_bytes = detail::encode_submit_pdu_internal(number, text, reference, smsc, dcs);
    return {detail::bytes_to_hex(pdu_bytes)};
}

PduMessage decode(const std::string& pdu_hex) {
    auto pdu_bytes = detail::hex_to_bytes(pdu_hex);
    return decode(pdu_bytes);
}

PduMessage decode(const std::vector<uint8_t>& pdu_bytes) {
    using namespace detail;

    if (pdu_bytes.empty()) {
        throw std::invalid_argument("PDU bytes cannot be empty");
    }

    PduMessage msg;
    auto it = pdu_bytes.begin();

    // Parse SMSC
    auto smsc_addr = parse_address_field(it, true);
    msg.smsc = smsc_addr.number;

    // Get TPDU first octet
    uint8_t tpdu_first = *it++;
    uint8_t pdu_type = tpdu_first & 0x03;

    if (pdu_type == constants::MT_SMS_DELIVER) {
        msg.number = parse_address_field(it, false).number;
        ++it;  // Skip PID
        uint8_t dcs_octet = *it++;
        uint8_t dcs = parse_data_coding_scheme(dcs_octet);
        msg.timestamp = parse_timestamp(it);
        uint8_t user_data_len = *it++;

        bool udh_present = (tpdu_first & 0x40) != 0;
        if (udh_present) {
            // Save position of UDH length byte before parsing UDH
            auto udh_start_it = it;
            uint8_t udh_len = *it++;
            auto udh_info = parse_udh(it, udh_len);
            msg.has_udh = true;
            msg.concat_ref = udh_info.concat_ref;
            msg.concat_seq = udh_info.concat_seq;
            msg.concat_total = udh_info.concat_total;

            // Create remaining from UDH length byte position (include UDH in septet unpacking)
            std::vector<uint8_t> remaining(udh_start_it, pdu_bytes.end());
            if (dcs == constants::DCS_GSM7) {
                auto septets = septets_unpack(remaining, user_data_len);
                // Skip UDH septets: ceil((udh_len + 1) * 8 / 7)
                size_t udh_septets = ((udh_len + 1) * 8 + 6) / 7;
                // Create a vector of only text septets
                std::vector<uint8_t> text_septets;
                if (septets.size() > udh_septets) {
                    text_septets.assign(septets.begin() + udh_septets, septets.end());
                }
                msg.text = gsm7_decode(text_septets);
            } else if (dcs == constants::DCS_UCS2) {
                // For UCS-2, skip UDH bytes (udh_len + 1 for length byte)
                std::vector<uint8_t> text_data(it, pdu_bytes.end());
                msg.text = ucs2_decode(text_data);
            } else {
                for (uint8_t b : remaining) msg.text += static_cast<char>(b);
            }
        } else {
            // No UDH present
            std::vector<uint8_t> remaining(it, pdu_bytes.end());
            if (dcs == constants::DCS_GSM7) {
                auto septets = septets_unpack(remaining, user_data_len);
                msg.text = gsm7_decode(septets);
            } else if (dcs == constants::DCS_UCS2) {
                msg.text = ucs2_decode(remaining);
            } else {
                for (uint8_t b : remaining) msg.text += static_cast<char>(b);
            }
        }
    } else if (pdu_type == constants::MT_SMS_SUBMIT) {
        msg.reference = *it++;
        msg.number = parse_address_field(it, false).number;
        ++it;  // Skip PID
        uint8_t dcs = parse_data_coding_scheme(*it++);

        // Skip validity period if present
        uint8_t vp_format = (tpdu_first & 0x18) >> 3;
        if (vp_format == 0x02) {
            it++;  // Relative VP
        } else if (vp_format == 0x03) {
            it += 7;  // Absolute VP
        }

        uint8_t user_data_len = *it++;

        bool udh_present = (tpdu_first & 0x40) != 0;
        if (udh_present) {
            // Save position of UDH length byte before parsing UDH
            auto udh_start_it = it;
            uint8_t udh_len = *it++;
            auto udh_info = parse_udh(it, udh_len);
            msg.has_udh = true;
            msg.concat_ref = udh_info.concat_ref;
            msg.concat_seq = udh_info.concat_seq;
            msg.concat_total = udh_info.concat_total;

            // Create remaining from UDH length byte position (include UDH in septet unpacking)
            std::vector<uint8_t> remaining(udh_start_it, pdu_bytes.end());
            if (dcs == constants::DCS_GSM7) {
                auto septets = septets_unpack(remaining, user_data_len);
                // Skip UDH septets: ceil((udh_len + 1) * 8 / 7)
                size_t udh_septets = ((udh_len + 1) * 8 + 6) / 7;
                // Create a vector of only text septets
                std::vector<uint8_t> text_septets;
                if (septets.size() > udh_septets) {
                    text_septets.assign(septets.begin() + udh_septets, septets.end());
                }
                msg.text = gsm7_decode(text_septets);
            } else if (dcs == constants::DCS_UCS2) {
                // For UCS-2, skip UDH bytes (udh_len + 1 for length byte)
                std::vector<uint8_t> text_data(it, pdu_bytes.end());
                msg.text = ucs2_decode(text_data);
            } else {
                for (uint8_t b : remaining) msg.text += static_cast<char>(b);
            }
        } else {
            // No UDH present
            std::vector<uint8_t> remaining(it, pdu_bytes.end());
            if (dcs == constants::DCS_GSM7) {
                auto septets = septets_unpack(remaining, user_data_len);
                msg.text = gsm7_decode(septets);
            } else if (dcs == constants::DCS_UCS2) {
                msg.text = ucs2_decode(remaining);
            } else {
                for (uint8_t b : remaining) msg.text += static_cast<char>(b);
            }
        }
    } else if (pdu_type == constants::MT_SMS_STATUS_REPORT) {
        msg.reference = *it++;
        msg.number = parse_address_field(it, false).number;
        msg.timestamp = parse_timestamp(it);
        std::string discharge = parse_timestamp(it);  // Discharge time
        (void)discharge;  // Suppress unused warning
        msg.status = *it++;
    } else {
        throw std::invalid_argument("Unknown PDU type: " + std::to_string(pdu_type));
    }

    return msg;
}

} // namespace pdu
