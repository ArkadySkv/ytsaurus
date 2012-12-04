#include "stdafx.h"
#include "private.h"

#include <ytlib/actions/bind.h>
#include <ytlib/actions/bind_helpers.h>

namespace NYT {
namespace NChunkClient {

////////////////////////////////////////////////////////////////////////////////

NLog::TLogger ChunkReaderLogger("ChunkReader");
NLog::TLogger ChunkWriterLogger("ChunkWriter");

TLazyHolder<NRpc::TChannelCache> NodeChannelCache;

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT

