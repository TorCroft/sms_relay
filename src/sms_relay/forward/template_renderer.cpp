#include "template_renderer.h"
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace smsrelay::forward {

std::string TemplateRenderer::render(const std::string &tmpl,
                                     const IncomingSms &sms)
{
    std::string result = tmpl;

    // Replace variables in order of longest first to avoid partial replacements
    // ${sender}
    replace_all(result, "${sender}", sms.decoded.number);

    // ${text}
    replace_all(result, "${text}", sms.decoded.text);

    // ${timestamp}
    replace_all(result, "${timestamp}", sms.decoded.timestamp);

    // ${index}
    replace_all(result, "${index}", std::to_string(static_cast<int>(sms.index)));

    return result;
}

std::string TemplateRenderer::url_encode(const std::string &str)
{
    std::ostringstream encoded;
    encoded.fill('0');
    encoded << std::hex;

    for (char c : str)
    {
        // Keep alphanumeric and other accepted characters intact
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ||
            c == '.' || c == '~' || c == ' ')
        {
            encoded << c;
        }
        else
        {
            // Any other characters are percent-encoded
            encoded << std::uppercase;
            encoded << '%' << std::setw(2)
                    << static_cast<int>(static_cast<unsigned char>(c));
            encoded << std::nouppercase;
        }
    }

    return encoded.str();
}

void TemplateRenderer::replace_all(std::string &str, const std::string &from,
                                   const std::string &to)
{
    if (from.empty())
    {
        return;
    }

    size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != std::string::npos)
    {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
}

std::string TemplateRenderer::truncate(const std::string &str, size_t max_len)
{
    if (str.length() <= max_len)
    {
        return str;
    }
    return str.substr(0, max_len);
}

} // namespace smsrelay::forward
