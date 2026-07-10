#pragma once

#include "sms_relay/sms/sms_service.h"
#include <memory>

namespace smsrelay::forward {

/**
 * @brief Abstract base class for forwarding targets
 *
 * Each forwarding target (Bark, etc.) implements this interface
 * to send notifications when new SMS arrives.
 */
class ForwardTarget
{
public:
    virtual ~ForwardTarget() = default;

    /**
     * @brief Send a notification for the incoming SMS
     * @param sms The incoming SMS to forward
     * @return true if sending was successful
     */
    virtual bool send(const IncomingSms &sms) = 0;

    /**
     * @brief Get the name of this target type
     */
    virtual const char *name() const = 0;
};

} // namespace smsrelay::forward
