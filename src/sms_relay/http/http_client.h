#pragma once

#include <map>
#include <memory>
#include <string>

namespace smsrelay::http {

/**
 * @brief HTTP response structure
 */
struct HttpResponse
{
    int status_code = 0;       // HTTP status code (200, 404, etc.)
    std::string body;          // Response body
    std::string error_message; // Error message if request failed
    bool success = false;      // True if request succeeded
};

/**
 * @brief HTTP request method
 */
enum class HttpMethod
{
    GET,
    POST,
    PUT,
    DELETE_REQ
};

/**
 * @brief HTTP client interface
 *
 * Abstract interface for HTTP requests.
 * Implementations can use different backends (libcurl, cpr, asio+ssl, etc.)
 */
class HttpClient
{
public:
    virtual ~HttpClient() = default;

    /**
     * @brief Send HTTP POST request
     * @param url Target URL
     * @param body Request body
     * @param headers Request headers
     * @param timeout_ms Timeout in milliseconds
     * @return HTTP response
     */
    virtual HttpResponse
    post(const std::string &url, const std::string &body,
         const std::map<std::string, std::string> &headers = {},
         int timeout_ms = 5000) = 0;

    /**
     * @brief Send HTTP GET request
     * @param url Target URL
     * @param headers Request headers
     * @param timeout_ms Timeout in milliseconds
     * @return HTTP response
     */
    virtual HttpResponse
    get(const std::string &url,
        const std::map<std::string, std::string> &headers = {},
        int timeout_ms = 5000) = 0;
};

/**
 * @brief Create default HTTP client implementation
 *
 * Returns a platform-appropriate HTTP client.
 * On Windows/Unix, uses libcurl if available, otherwise uses a basic
 * implementation.
 */
std::unique_ptr<HttpClient> create_default_client();

} // namespace smsrelay::http
