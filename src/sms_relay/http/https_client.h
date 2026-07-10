#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace smsrelay::http {

/**
 * @brief HTTP response structure
 */
struct HttpResponse
{
    int status_code = 0;
    std::string body;
    std::string error_message;
    bool success = false;
};

/**
 * @brief Simple HTTPS client using asio + OpenSSL
 *
 * Lightweight alternative to libcurl for basic HTTPS POST requests.
 * Only supports what's needed for Bark API.
 */
class HttpsClient
{
public:
    /**
     * @brief Constructor
     */
    HttpsClient();

    /**
     * @brief Destructor
     */
    ~HttpsClient();

    /**
     * @brief Send HTTPS POST request
     * @param url Target URL (https://...)
     * @param body Request body
     * @param headers Request headers
     * @param timeout_ms Timeout in milliseconds
     * @return HTTP response
     */
    HttpResponse post(const std::string &url, const std::string &body,
                      const std::map<std::string, std::string> &headers = {},
                      int timeout_ms = 5000);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * @brief Create HTTPS client
 */
std::unique_ptr<HttpsClient> create_https_client();

} // namespace smsrelay::http
