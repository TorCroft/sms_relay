#include "command_queue.h"

namespace smsrelay::at {

std::future<ResponseBuilder::AtResponse>
CommandQueue::enqueue(std::string cmd)
{
    auto promise = std::make_shared<std::promise<ResponseBuilder::AtResponse>>();
    auto future = promise->get_future();

    QueuedCommand queued_cmd;
    queued_cmd.text = std::move(cmd);
    queued_cmd.promise = promise;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(queued_cmd));
    }

    return future;
}

CommandQueue::QueuedCommand CommandQueue::dequeue()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty())
    {
        return {};
    }

    auto cmd = std::move(queue_.front());
    queue_.pop();
    return cmd;
}

} // namespace smsrelay::at
