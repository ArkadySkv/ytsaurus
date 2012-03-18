#pragma once

#include "common.h"
#include "message.h"

#include <ytlib/misc/delayed_invoker.h>
#include <ytlib/logging/tagged_logger.h>

namespace NYT {
namespace NBus {

////////////////////////////////////////////////////////////////////////////////

class TMessageRearranger
    : public TRefCounted
{
public:
    typedef TIntrusivePtr<TMessageRearranger> TPtr;

    TMessageRearranger(
        const TSessionId& sessionId,
        IParamAction<IMessage*>::TPtr onDequeuedMessage,
        TDuration timeout);

    void EnqueueMessage(
        IMessage* message,
        const TGuid& requestId,
        TSequenceId sequenceId);

private:
    struct TPostponedMessage
    {
        TPostponedMessage(const TGuid& requestId, IMessage* message)
            : RequestId(requestId)
            , Message(message)
        { }

        TGuid RequestId;
        IMessage::TPtr Message;
    };

    typedef ymap<TSequenceId, TPostponedMessage> TPostponedMessages;

    TSessionId SessionId;
    IParamAction<IMessage*>::TPtr OnMessageDequeued;
    TDuration Timeout;

    NLog::TTaggedLogger Logger;
    TSpinLock SpinLock;
    TDelayedInvoker::TCookie TimeoutCookie;
    TSequenceId ExpectedSequenceId;
    TPostponedMessages PostponedMessages;

    void RescheduleTimeout();
    void OnTimeout();
    void TryFlush();

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NBus
} // namespace NYT
