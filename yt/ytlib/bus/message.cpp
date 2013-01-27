#include "stdafx.h"
#include "message.h"

#include <ytlib/misc/serialize.h>

namespace NYT {
namespace NBus {

////////////////////////////////////////////////////////////////////////////////

class TMessage
    : public IMessage
{
public:
    explicit TMessage(const std::vector<TSharedRef>& parts)
        : Parts(parts)
    { }

    explicit TMessage(std::vector<TSharedRef>&& parts)
        : Parts(ForwardRV< std::vector<TSharedRef> >(parts))
    { }

    virtual const std::vector<TSharedRef>& GetParts() override
    {
        return Parts;
    }

private:
    std::vector<TSharedRef> Parts;

};

////////////////////////////////////////////////////////////////////////////////

IMessagePtr CreateMessageFromParts(const std::vector<TSharedRef>& parts)
{
    return New<TMessage>(parts);
}

IMessagePtr CreateMessageFromParts(std::vector<TSharedRef>&& parts)
{
    return New<TMessage>(parts);
}

IMessagePtr CreateMessageFromPart(const TSharedRef& part)
{
    std::vector<TSharedRef> parts;
    parts.push_back(part);
    return New<TMessage>(parts);
}

IMessagePtr CreateMessageFromParts(TBlob&& blob, const std::vector<TRef>& refs)
{
    auto sharedBlob = TSharedRef::FromBlob(MoveRV(blob));
    std::vector<TSharedRef> parts(refs.size());
    for (int i = 0; i < static_cast<int>(refs.size()); ++i) {
        parts[i] = sharedBlob.Slice(refs[i]);
    }
    return New<TMessage>(MoveRV(parts));
}

TSharedRef PackMessage(IMessagePtr message)
{
    return PackRefs(message->GetParts());
}

IMessagePtr UnpackMessage(const TSharedRef& packedBlob)
{
    std::vector<TSharedRef> parts;
    UnpackRefs(packedBlob, &parts);
    return CreateMessageFromParts(parts);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NBus
} // namespace NYT
