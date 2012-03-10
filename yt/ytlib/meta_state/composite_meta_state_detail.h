#pragma once

#include "public.h"
#include "meta_state_manager.pb.h"

#include <ytlib/misc/ref.h>

namespace NYT {
namespace NMetaState {

////////////////////////////////////////////////////////////////////////////////
    
#pragma pack(push, 4)

struct TFixedChangeHeader
{
    i32 HeaderSize;
    i32 MessageSize;
};

#pragma pack(pop)

template <class TMessage>
TBlob SerializeChange(
    const NProto::TMsgChangeHeader& header,
    const TMessage& message);

void DeserializeChangeHeader(
    TRef changeData,
    NProto::TMsgChangeHeader* header);

void DeserializeChange(
    TRef changeData,
    NMetaState::NProto::TMsgChangeHeader* header,
    TRef* messageData);

////////////////////////////////////////////////////////////////////////////////

} // namespace NMetaState
} // namespace NYT

#define COMPOSITE_META_STATE_DETAIL_INL_H_
#include "composite_meta_state_detail-inl.h"
#undef COMPOSITE_META_STATE_DETAIL_INL_H_
