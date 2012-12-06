#include "output_stack.h"

#include <util/stream/zlib.h>
#include <util/stream/lz.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

TNodeJSOutputStack::TNodeJSOutputStack(TNodeJSOutputStream* base)
    : TGrowingStreamStack(base)
    , HasAnyData_(false)
{
    THREAD_AFFINITY_IS_V8();
    YASSERT(base == Bottom());
    GetBaseStream()->AsyncRef(true);
}

TNodeJSOutputStack::~TNodeJSOutputStack() throw()
{
    GetBaseStream()->AsyncUnref();
}

TNodeJSOutputStream* TNodeJSOutputStack::GetBaseStream()
{
    return static_cast<TNodeJSOutputStream*>(Bottom());
}

void TNodeJSOutputStack::AddCompression(ECompression compression)
{
    switch (compression) {
        case ECompression::None:
            break;
        case ECompression::Gzip:
            Add<TZLibCompress>(ZLib::GZip, 4, DefaultStreamBufferSize);
            break;
        case ECompression::Deflate:
            Add<TZLibCompress>(ZLib::ZLib, 4, DefaultStreamBufferSize);
            break;
        case ECompression::LZO:
            Add<TLzoCompress>(DefaultStreamBufferSize);
            break;
        case ECompression::LZF:
            Add<TLzfCompress>(DefaultStreamBufferSize);
            break;
        case ECompression::Snappy:
            Add<TSnappyCompress>(DefaultStreamBufferSize);
            break;
        default:
            YUNREACHABLE();
    }

    Add<TBufferedOutput>()->SetPropagateMode(true);
}

bool TNodeJSOutputStack::HasAnyData()
{
    return HasAnyData_;
}

void TNodeJSOutputStack::DoWrite(const void* buffer, size_t length)
{
    HasAnyData_ = true;
    return Top()->Write(buffer, length);
}

void TNodeJSOutputStack::DoWriteV(const TPart* parts, size_t count)
{
    return Top()->Write(parts, count);
}

void TNodeJSOutputStack::DoFlush()
{
    return Top()->Flush();
}

void TNodeJSOutputStack::DoFinish()
{
    return Top()->Finish();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
