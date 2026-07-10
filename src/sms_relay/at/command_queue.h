#pragma once

#include "sms_relay/at/response_builder.h"
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>

namespace smsrelay::at {

/**
 * @brief Thread-safe queue for AT commands
 *
 * Commands are executed in FIFO order.
 * Returns std::future for async result retrieval.
 */
class CommandQueue
{
public:
    CommandQueue() = default;

    /**
     * @brief Add command to queue
     * @return future for the response
     */
    std::future<ResponseBuilder::AtResponse> enqueue(std::string cmd);

    /**
     * @brief Get next command structure
     */
    struct QueuedCommand
    {
        std::string text;
        std::shared_ptr<std::promise<ResponseBuilder::AtResponse>> promise;
    };

    QueuedCommand dequeue();

    /**
     * @brief Check if queue is empty
     */
    bool empty() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    /**
     * @brief Get queue size
     */
    size_t size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    mutable std::mutex mutex_;
    std::queue<QueuedCommand> queue_;
};

} // namespace smsrelay::at
