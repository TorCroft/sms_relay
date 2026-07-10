#include "http_client.h"
#include <iostream>
#include <memory>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdio>
#endif

namespace smsrelay::http {

/**
 * @brief HTTP client using system curl command
 *
 * Cross-platform solution that uses the system's curl binary.
 * Works on Linux (including Synology), macOS, and Windows.
 */
class CurlHttpClient : public HttpClient
{
public:
    HttpResponse post(const std::string &url, const std::string &body, const std::map<std::string, std::string> &headers, int timeout_ms) override
    {
        HttpResponse response;

        // Build curl command
        std::stringstream cmd;
        cmd << "curl -s -w \"\\n%{http_code}\" -X POST";

        // Add headers
        for (const auto &[key, value] : headers)
        {
            cmd << " -H \"" << key << ": " << value << "\"";
        }

        // Add body
        cmd << " -d \"" << escape_body(body) << "\"";

        // Add timeout
        cmd << " --max-time " << (timeout_ms / 1000);

        // Add URL
        cmd << " \"" << url << "\"";

        // Execute command
        std::string output = execute(cmd.str());

        // Parse response (last line is status code)
        size_t last_newline = output.find_last_of("\n");
        if (last_newline != std::string::npos && last_newline > 0)
        {
            std::string status_line = output.substr(last_newline + 1);
            response.body = output.substr(0, last_newline);

            try
            {
                response.status_code = std::stoi(status_line);
                response.success = (response.status_code >= 200 && response.status_code < 300);
            }
            catch (...)
            {
                response.error_message = "Failed to parse status code";
            }
        }
        else
        {
            response.error_message = "Invalid curl output";
        }

        return response;
    }

    HttpResponse get(const std::string &url, const std::map<std::string, std::string> &headers, int timeout_ms) override
    {

        HttpResponse response;

        // Build curl command
        std::stringstream cmd;
        cmd << "curl -s -w \"\\n%{http_code}\"";

        // Add headers
        for (const auto &[key, value] : headers)
        {
            cmd << " -H \"" << key << ": " << value << "\"";
        }

        // Add timeout
        cmd << " --max-time " << (timeout_ms / 1000);

        // Add URL
        cmd << " \"" << url << "\"";

        // Execute command
        std::string output = execute(cmd.str());

        // Parse response
        size_t last_newline = output.find_last_of("\n");
        if (last_newline != std::string::npos && last_newline > 0)
        {
            std::string status_line = output.substr(last_newline + 1);
            response.body = output.substr(0, last_newline);

            try
            {
                response.status_code = std::stoi(status_line);
                response.success =
                    (response.status_code >= 200 && response.status_code < 300);
            }
            catch (...)
            {
                response.error_message = "Failed to parse status code";
            }
        }
        else
        {
            response.error_message = "Invalid curl output";
        }

        return response;
    }

private:
#ifdef _WIN32
    /**
     * @brief Execute command on Windows
     */
    std::string execute(const std::string &cmd)
    {
        std::string output;
        HANDLE hReadPipe, hWritePipe;
        SECURITY_ATTRIBUTES sa = {sizeof(SECURITY_ATTRIBUTES), NULL, TRUE};

        if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0))
        {
            return "";
        }

        SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = hWritePipe;
        si.hStdError = hWritePipe;

        PROCESS_INFORMATION pi = {};
        std::string fullCmd = "cmd.exe /c " + cmd;

        if (CreateProcessA(NULL, const_cast<char *>(fullCmd.c_str()), NULL, NULL,
                           TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        {

            CloseHandle(hWritePipe);

            char buffer[4096];
            DWORD bytesRead;
            while (
                ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL) &&
                bytesRead > 0)
            {
                buffer[bytesRead] = '\0';
                output += buffer;
            }

            CloseHandle(hReadPipe);
            WaitForSingleObject(pi.hProcess, INFINITE);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
        else
        {
            CloseHandle(hReadPipe);
            CloseHandle(hWritePipe);
        }

        return output;
    }
#else
    /**
     * @brief Execute command on Linux/Unix
     */
    std::string execute(const std::string &cmd)
    {
        std::string output;
        FILE *pipe = popen(cmd.c_str(), "r");
        if (pipe)
        {
            char buffer[4096];
            while (fgets(buffer, sizeof(buffer), pipe))
            {
                output += buffer;
            }
            pclose(pipe);
        }
        return output;
    }
#endif

    /**
     * @brief Escape special characters for shell
     */
    std::string escape_body(const std::string &str)
    {
        std::string escaped;
        escaped.reserve(str.length() * 1.2);

        for (char c : str)
        {
            switch (c)
            {
                case '"':
                    escaped += "\\\"";
                    break;
                case '\\':
                    escaped += "\\\\";
                    break;
                case '\n':
                    escaped += "\\n";
                    break;
                case '\r':
                    escaped += "\\r";
                    break;
                case '\t':
                    escaped += "\\t";
                    break;
                case '$':
                    escaped += "\\$";
                    break;
                case '`':
                    escaped += "\\`";
                    break;
                default:
                    escaped += c;
                    break;
            }
        }

        return escaped;
    }
};

std::unique_ptr<HttpClient> create_default_client()
{
    return std::make_unique<CurlHttpClient>();
}

} // namespace smsrelay::http
