#include "ipc_server.h"
#include "common/ipc/ipc_protocol.h"
#include "common/ipc/ipc_serialization.h"
#include <cstring>
#include <iostream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace smsrelay::ipc {

IpcServer::IpcServer(int port, std::shared_ptr<SmsService> sms_service)
    : port_(port), sms_service_(sms_service), clients_(MAX_CLIENTS)
{
#ifdef _WIN32
    server_fd_ = INVALID_SOCKET;
#else
    server_fd_ = -1;
#endif
}

IpcServer::~IpcServer()
{
    // Just call stop - it handles all cleanup including WSACleanup
    stop();
}

bool IpcServer::start()
{
    if (running_.load())
    {
        std::cerr << "[IPC Server] Already running" << std::endl;
        return false;
    }

#ifdef _WIN32
    // Initialize Winsock
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
    {
        std::cerr << "[IPC Server] WSAStartup failed" << std::endl;
        return false;
    }
#endif

    // Create IPv6 socket (will also accept IPv4 if we set IPV6_V6ONLY=0)
#ifdef _WIN32
    server_fd_ = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (server_fd_ == INVALID_SOCKET)
#else
    server_fd_ = socket(AF_INET6, SOCK_STREAM, 0);
    if (server_fd_ < 0)
#endif
    {
        std::cerr << "[IPC Server] Failed to create socket" << std::endl;
        return false;
    }

    // Set socket options
    int reuse = 1;
    int v6only = 0;

#ifdef _WIN32
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));

    setsockopt(server_fd_, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char *>(&v6only), sizeof(v6only));
#else
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    setsockopt(server_fd_, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
#endif

    // Bind to IPv6 loopback address
    struct sockaddr_in6 address;
    std::memset(&address, 0, sizeof(address));
    address.sin6_family = AF_INET6;
    address.sin6_addr = in6addr_any;
    address.sin6_port = htons(port_);

    if (bind(server_fd_, reinterpret_cast<struct sockaddr *>(&address), sizeof(address)) < 0)
    {
        std::cerr << "[IPC Server] Failed to bind to port " << port_ << std::endl;
#ifdef _WIN32
        closesocket(server_fd_);
        WSACleanup();
#else
        close(server_fd_);
#endif
        server_fd_ = static_cast<decltype(server_fd_)>(-1);
        return false;
    }

    // Listen
    if (listen(server_fd_, 5) < 0)
    {
        std::cerr << "[IPC Server] Failed to listen" << std::endl;
#ifdef _WIN32
        closesocket(server_fd_);
        WSACleanup();
#else
        close(server_fd_);
#endif
        server_fd_ = static_cast<decltype(server_fd_)>(-1);
        return false;
    }

    running_.store(true);
    server_thread_ = std::thread([this]() { accept_loop(); });

    std::cout << "[IPC Server] Started on port " << port_ << std::endl;
    return true;
}

void IpcServer::stop()
{
    if (!running_.load())
    {
        return;
    }

    // First, stop accepting new connections
    running_.store(false);

    // Close server socket to unblock accept()
#ifdef _WIN32
    if (server_fd_ != INVALID_SOCKET)
    {
        closesocket(server_fd_);
        server_fd_ = INVALID_SOCKET;
    }
#else
    if (server_fd_ >= 0)
    {
        shutdown(server_fd_, SHUT_RDWR);
        close(server_fd_);
        server_fd_ = -1;
    }
#endif

    // Close all client connections to unblock recv() calls
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (int i = 0; i < MAX_CLIENTS; ++i)
        {
            if (clients_[i].active)
            {
                std::cout << "[IPC Server] Disconnecting client " << i << std::endl;

#ifdef _WIN32
                if (clients_[i].socket_fd != INVALID_SOCKET)
                {
                    closesocket(clients_[i].socket_fd);
                    clients_[i].socket_fd = INVALID_SOCKET;
                }
#else
                if (clients_[i].socket_fd >= 0)
                {
                    close(clients_[i].socket_fd);
                    clients_[i].socket_fd = -1;
                }
#endif
                clients_[i].active = false;
            }
        }
    }

    // Try to join server thread with timeout
    if (server_thread_.joinable())
    {
        server_thread_.join();
        std::cout << "[IPC Server] Accept thread finished" << std::endl;
    }

    // Try to join all client threads with timeout
    {
        for (int i = 0; i < MAX_CLIENTS; ++i)
        {
            if (clients_[i].handler_thread.joinable())
            {
                std::cout
                    << "[IPC Server] Waiting for client thread "
                    << i << " to finish..."
                    << std::endl;

                clients_[i].handler_thread.join();
            }
        }
    }

#ifdef _WIN32
    // Cleanup Winsock (only when server is completely stopped)
    // This should be called once per process, not per connection
    WSACleanup();
#endif

    std::cout << "[IPC Server] Stopped" << std::endl;
}

void IpcServer::accept_loop()
{
    while (running_.load())
    {
        struct sockaddr_in6 client_addr;
#ifdef _WIN32
        int addr_len = sizeof(client_addr);
#else
        socklen_t addr_len = sizeof(client_addr);
#endif

        int fd = server_fd_;

#ifdef _WIN32
        SOCKET client_fd = accept(fd, reinterpret_cast<struct sockaddr *>(&client_addr), &addr_len);
        if (client_fd == INVALID_SOCKET)
#else
        int client_fd = accept(fd, reinterpret_cast<struct sockaddr *>(&client_addr), &addr_len);
        if (client_fd < 0)
#endif
        {
            if (!running_.load())
            {
                break;
            }

            continue;
        }

        // Get client address for logging
        char client_ip[INET6_ADDRSTRLEN];
        if (client_addr.sin6_family == AF_INET6)
        {
            inet_ntop(AF_INET6, &client_addr.sin6_addr, client_ip, sizeof(client_ip));
        }
        else
        {
            // IPv4-mapped IPv6 address
            inet_ntop(AF_INET, &client_addr.sin6_addr, client_ip, sizeof(client_ip));
        }

        // Find available client slot
        int client_idx = find_available_client_slot();
        if (client_idx < 0)
        {
            std::cerr << "[IPC Server] Maximum connections reached, rejecting client from " << client_ip << std::endl;
#ifdef _WIN32
            closesocket(client_fd);
#else
            close(client_fd);
#endif
            continue;
        }

        // Join any existing thread for this slot before reusing it
        // Only join if we're not shutting down (to avoid deadlock during stop())
        if (clients_[client_idx].handler_thread.joinable() && running_.load())
        {
            clients_[client_idx].handler_thread.join();
        }
        // If we're shutting down, detach the old thread and let it finish independently
        else if (clients_[client_idx].handler_thread.joinable())
        {
            clients_[client_idx].handler_thread.detach();
        }

        // Add client connection
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            clients_[client_idx].socket_fd = client_fd;
            clients_[client_idx].address = std::string(client_ip);
            clients_[client_idx].active = true;
        }

        std::cout << "[IPC Server] Client connected from " << client_ip
                  << " (slot " << client_idx << ", active: " << get_active_connections() << ")"
                  << std::endl;

        // Start client handler thread
        clients_[client_idx].handler_thread = std::thread([this, client_idx]() {
            handle_client_thread(client_idx);
        });
    }
}

void IpcServer::handle_client_thread(int client_idx)
{
    ClientConnection &client = clients_[client_idx];

    while (running_.load() && client.active)
    {
        // Read header first (16 bytes)
        uint8_t header_buf[16];
        size_t header_received = 0;

        while (header_received < 16)
        {
#ifdef _WIN32
            int received = recv(client.socket_fd, reinterpret_cast<char *>(header_buf + header_received),
                                static_cast<int>(16 - header_received), 0);
#else
            ssize_t received = recv(client.socket_fd, header_buf + header_received, 16 - header_received, 0);
#endif
            if (received <= 0)
            {
                // Connection closed or error
                std::cout << "[IPC Server] Client " << client_idx << " disconnected" << std::endl;
                client.active = false;
                goto cleanup;
            }
            header_received += received;
        }

        // Parse header
        size_t offset = 0;
        uint32_t magic, length, command_type, sequence_id;

        if (!IpcSerializer::deserialize_u32(header_buf, 16, magic, offset))
        {
            std::cerr << "[IPC Server] Client " << client_idx << ": Failed to parse magic" << std::endl;
            client.active = false;
            goto cleanup;
        }
        if (!IpcSerializer::deserialize_u32(header_buf, 16, length, offset))
        {
            std::cerr << "[IPC Server] Client " << client_idx << ": Failed to parse length" << std::endl;
            client.active = false;
            goto cleanup;
        }
        if (!IpcSerializer::deserialize_u32(header_buf, 16, command_type, offset))
        {
            std::cerr << "[IPC Server] Client " << client_idx << ": Failed to parse command type" << std::endl;
            client.active = false;
            goto cleanup;
        }
        if (!IpcSerializer::deserialize_u32(header_buf, 16, sequence_id, offset))
        {
            std::cerr << "[IPC Server] Client " << client_idx << ": Failed to parse sequence ID" << std::endl;
            client.active = false;
            goto cleanup;
        }

        // Verify magic
        if (magic != IPC_MAGIC)
        {
            std::cerr << "[IPC Server] Client " << client_idx << ": Invalid magic number: 0x"
                      << std::hex << magic << std::endl;
            client.active = false;
            goto cleanup;
        }

        // Check payload length
        if (length > MAX_PAYLOAD_SIZE)
        {
            std::cerr << "[IPC Server] Client " << client_idx << ": Payload too large: " << length << std::endl;
            client.active = false;
            goto cleanup;
        }

        // Read payload if any
        std::vector<uint8_t> payload;
        if (length > 0)
        {
            payload.resize(length);
            size_t payload_received = 0;

            while (payload_received < length)
            {
#ifdef _WIN32
                int received = recv(client.socket_fd,
                                    reinterpret_cast<char *>(payload.data() + payload_received),
                                    static_cast<int>(length - payload_received), 0);
#else
                ssize_t received = recv(client.socket_fd, payload.data() + payload_received,
                                        length - payload_received, 0);
#endif
                if (received <= 0)
                {
                    std::cout << "[IPC Server] Client " << client_idx << " disconnected during payload read"
                              << std::endl;
                    client.active = false;
                    goto cleanup;
                }
                payload_received += received;
            }
        }

        // Build full request
        std::vector<uint8_t> request;
        request.insert(request.end(), header_buf, header_buf + 16);
        request.insert(request.end(), payload.begin(), payload.end());

        // Process command
        auto response = process_command(request);

        // Send response
        if (!response.empty())
        {
#ifdef _WIN32
            send(client.socket_fd, reinterpret_cast<const char *>(response.data()), static_cast<int>(response.size()), 0);
#else
            send(client.socket_fd, response.data(), response.size(), 0);
#endif
        }
    }

cleanup:
    // Properly close socket to prevent resource leaks
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        ClientConnection &client = clients_[client_idx];

#ifdef _WIN32
        if (client.socket_fd != INVALID_SOCKET)
        {
            closesocket(client.socket_fd);
            client.socket_fd = INVALID_SOCKET;
        }
#else
        if (client.socket_fd >= 0)
        {
            close(client.socket_fd);
            client.socket_fd = -1;
        }
#endif

        client.active = false;
    }
}

std::vector<uint8_t> IpcServer::process_command(const std::vector<uint8_t> &request)
{
    // Parse header
    size_t offset = 0;
    uint32_t magic, length, command_type, sequence_id;

    if (!IpcSerializer::deserialize_u32(request.data(), request.size(), magic, offset))
    {
        return IpcSerializer::serialize_response(Status::FAILED, 0, {});
    }
    if (!IpcSerializer::deserialize_u32(request.data(), request.size(), length, offset))
    {
        return IpcSerializer::serialize_response(Status::FAILED, 0, {});
    }
    if (!IpcSerializer::deserialize_u32(request.data(), request.size(), command_type, offset))
    {
        return IpcSerializer::serialize_response(Status::FAILED, 0, {});
    }
    if (!IpcSerializer::deserialize_u32(request.data(), request.size(), sequence_id, offset))
    {
        return IpcSerializer::serialize_response(Status::FAILED, 0, {});
    }

    // Extract payload
    std::vector<uint8_t> payload;
    if (offset < request.size())
    {
        payload.assign(request.begin() + offset, request.end());
    }

    // Dispatch command
    try
    {
        switch (static_cast<CommandType>(command_type))
        {
            case CommandType::LIST_SMS:
                return handle_list(payload);
            case CommandType::READ_SMS:
                return handle_read(payload);
            case CommandType::DELETE_SMS:
                return handle_delete(payload);
            case CommandType::SEND_SMS:
                return handle_send(payload);
            case CommandType::STATUS:
                return handle_status(payload);
            default:
                std::cerr << "[IPC Server] Unknown command: " << command_type << std::endl;
                return IpcSerializer::serialize_response(Status::INVALID_ARGS, sequence_id, {});
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "[IPC Server] Exception processing command: " << e.what() << std::endl;
        return IpcSerializer::serialize_response(Status::FAILED, sequence_id, {});
    }
}

std::vector<uint8_t>
IpcServer::handle_list(const std::vector<uint8_t> &payload)
{
    // Parse payload
    size_t offset = 0;
    std::string status;
    if (!IpcSerializer::deserialize_str(payload.data(), payload.size(), status, offset))
    {
        return IpcSerializer::serialize_response(Status::INVALID_ARGS, 0, {});
    }

    // List messages from SMS service (always returns all messages)
    auto messages = sms_service_->list_and_decode_messages();

    // Build response
    std::vector<uint8_t> response_payload;
    auto count_bytes = IpcSerializer::serialize_u32(static_cast<uint32_t>(messages.size()));
    response_payload.insert(response_payload.end(), count_bytes.begin(), count_bytes.end());

    for (const auto &msg : messages)
    {
        SmsInfo info;
        info.index = msg.index;
        info.sender = msg.decoded.number;
        info.text = msg.decoded.text;
        info.timestamp = msg.decoded.timestamp;
        info.has_udh = msg.decoded.has_udh;
        info.concat_seq = msg.decoded.concat_seq;
        info.concat_total = msg.decoded.concat_total;
        info.status = msg.status;

        auto sms_bytes = PayloadSerializer::serialize_sms_info(info);
        response_payload.insert(response_payload.end(), sms_bytes.begin(), sms_bytes.end());
    }

    return IpcSerializer::serialize_response(Status::SUCCESS, 0, response_payload);
}

std::vector<uint8_t>
IpcServer::handle_read(const std::vector<uint8_t> &payload)
{
    // Parse payload
    size_t offset = 0;
    std::vector<uint8_t> indices;
    if (!IpcSerializer::deserialize_vec(payload.data(), payload.size(), indices, offset))
    {
        return IpcSerializer::serialize_response(Status::INVALID_ARGS, 0, {});
    }

    if (indices.empty())
    {
        return IpcSerializer::serialize_response(Status::INVALID_ARGS, 0, {});
    }

    // Read messages directly from cache (cache contains already combined
    // messages)
    std::vector<uint8_t> response_payload;
    std::vector<SmsInfo> results;

    const auto &cached_messages = sms_service_->get_cached_messages();

    for (uint8_t index : indices)
    {
        bool found = false;
        for (const auto &cached : cached_messages)
        {
            if (cached.index == index && cached.success)
            {
                SmsInfo info;
                info.index = cached.index;
                info.sender = cached.decoded.number;
                info.text = cached.decoded.text;
                info.timestamp = cached.decoded.timestamp;
                info.status = cached.status;
                info.has_udh = cached.decoded.has_udh;
                info.concat_seq = cached.decoded.concat_seq;
                info.concat_total = cached.decoded.concat_total;
                info.is_combined = true; // Mark as combined (from cache)
                // Since cache has combined messages, show as single message
                info.parts = {{index, 1, 1}};

                results.push_back(info);
                found = true;
                break;
            }
        }

        if (!found)
        {
            // Message not found in cache (probably deleted)
            SmsInfo info;
            info.index = index;
            info.text = "<Message not found or deleted>";
            results.push_back(info);
        }
    }

    // Serialize results
    auto count_bytes =
        IpcSerializer::serialize_u32(static_cast<uint32_t>(results.size()));
    response_payload.insert(response_payload.end(), count_bytes.begin(),
                            count_bytes.end());

    for (const auto &info : results)
    {
        auto sms_bytes = PayloadSerializer::serialize_sms_info(info);
        response_payload.insert(response_payload.end(), sms_bytes.begin(), sms_bytes.end());
    }

    return IpcSerializer::serialize_response(Status::SUCCESS, 0,
                                             response_payload);
}

std::vector<uint8_t>
IpcServer::handle_delete(const std::vector<uint8_t> &payload)
{
    // Parse payload
    size_t offset = 0;
    std::vector<uint8_t> indices;
    if (!IpcSerializer::deserialize_vec(payload.data(), payload.size(), indices,
                                        offset))
    {
        return IpcSerializer::serialize_response(Status::INVALID_ARGS, 0, {});
    }

    if (indices.empty())
    {
        return IpcSerializer::serialize_response(Status::INVALID_ARGS, 0, {});
    }

    // Delete messages from SMS service
    std::vector<uint8_t> deleted_indices;
    std::vector<uint8_t> failed_indices;

    for (uint8_t index : indices)
    {
        std::string error =
            sms_service_->delete_message(sms_service_->get_storage(), index);
        if (error.empty())
        {
            // Success - remove from cache (indices don't reorder in GSM)
            deleted_indices.push_back(index);
            sms_service_->remove_from_cache(index);
        }
        else
        {
            // Failed
            failed_indices.push_back(index);
            std::cerr << "[IPC Server] Failed to delete index " << static_cast<int>(index) << ": " << error << std::endl;
        }
    }

    // Build response
    std::vector<uint8_t> response_payload;
    auto deleted_bytes = IpcSerializer::serialize_vec(deleted_indices);
    auto failed_bytes = IpcSerializer::serialize_vec(failed_indices);

    response_payload.insert(response_payload.end(), deleted_bytes.begin(),
                            deleted_bytes.end());
    response_payload.insert(response_payload.end(), failed_bytes.begin(),
                            failed_bytes.end());

    return IpcSerializer::serialize_response(Status::SUCCESS, 0,
                                             response_payload);
}

std::vector<uint8_t>
IpcServer::handle_send(const std::vector<uint8_t> &payload)
{
    // Parse payload
    size_t offset = 0;
    std::string recipient;
    std::string text;

    if (!IpcSerializer::deserialize_str(payload.data(), payload.size(), recipient,
                                        offset))
    {
        return IpcSerializer::serialize_response(Status::INVALID_ARGS, 0, {});
    }
    if (!IpcSerializer::deserialize_str(payload.data(), payload.size(), text,
                                        offset))
    {
        return IpcSerializer::serialize_response(Status::INVALID_ARGS, 0, {});
    }

    // Send message via SMS service
    // TODO: Implement send_message in SmsService
    bool success = false; // Placeholder
    std::string error_msg = "Send SMS not yet implemented";

    // Build response
    std::vector<uint8_t> response_payload;
    if (success)
    {
        response_payload.push_back(0); // No index available
        return IpcSerializer::serialize_response(Status::SUCCESS, 0,
                                                 response_payload);
    }
    else
    {
        auto error_bytes = IpcSerializer::serialize_str(error_msg);
        response_payload.insert(response_payload.end(), error_bytes.begin(),
                                error_bytes.end());
        return IpcSerializer::serialize_response(Status::FAILED, 0,
                                                 response_payload);
    }
}

std::vector<uint8_t>
IpcServer::handle_status(const std::vector<uint8_t> &payload)
{
    (void)payload;
    // Build response with basic status
    std::vector<uint8_t> response_payload;
    response_payload.push_back(1); // Connected (placeholder)

    std::string port_str = "[::1]:" + std::to_string(port_);
    auto port_bytes = IpcSerializer::serialize_str(port_str);
    response_payload.insert(response_payload.end(), port_bytes.begin(), port_bytes.end());

    auto count_bytes = IpcSerializer::serialize_u32(0); // Message count (placeholder)
    response_payload.insert(response_payload.end(), count_bytes.begin(), count_bytes.end());

    return IpcSerializer::serialize_response(Status::SUCCESS, 0, response_payload);
}

int IpcServer::get_active_connections() const
{
    int count = 0;
    for (const auto &client : clients_)
    {
        if (client.active)
        {
            ++count;
        }
    }
    return count;
}

int IpcServer::find_available_client_slot()
{
    std::lock_guard<std::mutex> lock(clients_mutex_);

    for (int i = 0; i < MAX_CLIENTS; ++i)
    {
        if (!clients_[i].active)
        {
            return i;
        }
    }
    return -1; // No available slot
}

void IpcServer::remove_client(int client_idx)
{
    std::lock_guard<std::mutex> lock(clients_mutex_);

    if (client_idx < 0 || client_idx >= MAX_CLIENTS)
    {
        return;
    }

    ClientConnection &client = clients_[client_idx];

    if (!client.active)
    {
        return; // Already removed
    }

    // Close socket to trigger thread exit
#ifdef _WIN32
    if (client.socket_fd != INVALID_SOCKET)
    {
        closesocket(client.socket_fd);
        client.socket_fd = INVALID_SOCKET;
    }
#else
    if (client.socket_fd >= 0)
    {
        close(client.socket_fd);
        client.socket_fd = -1;
    }
#endif

    // Mark as inactive (thread will exit on its own)
    client.active = false;
}

} // namespace smsrelay::ipc
