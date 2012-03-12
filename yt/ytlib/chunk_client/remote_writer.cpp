#include "stdafx.h"
#include "remote_writer.h"
#include "holder_channel_cache.h"
#include "writer_thread.h"
#include "chunk_holder_service.pb.h"

#include <ytlib/misc/serialize.h>
#include <ytlib/misc/metric.h>
#include <ytlib/misc/string.h>
#include <ytlib/actions/action_util.h>
#include <ytlib/actions/parallel_awaiter.h>
#include <ytlib/cypress/cypress_service_proxy.h>

#include <util/random/random.h>
#include <util/generic/yexception.h>
#include <util/datetime/base.h>
#include <util/datetime/cputimer.h>
#include <util/stream/str.h>

namespace NYT {
namespace NChunkClient {

using namespace NRpc;
using namespace NChunkHolder::NProto;
using namespace NChunkServer;
using namespace NCypress;

///////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = ChunkClientLogger;

///////////////////////////////////////////////////////////////////////////////

struct TRemoteWriter::THolder 
    : public TRefCounted
{
    int Index;
    bool IsAlive;
    const Stroka Address;
    TProxy Proxy;
    TDelayedInvoker::TCookie Cookie;

    THolder(int index, const Stroka& address, TDuration timeout)
        : Index(index)
        , IsAlive(true)
        , Address(address)
        , Proxy(~HolderChannelCache->GetChannel(address))
    {
        Proxy.SetDefaultTimeout(timeout);
    }
};

///////////////////////////////////////////////////////////////////////////////

class TRemoteWriter::TGroup 
    : public TRefCounted
{
public:
    TGroup(
        int holderCount, 
        int startBlockIndex, 
        TRemoteWriter* writer);

    void AddBlock(const TSharedRef& block);
    void Process();
    bool IsWritten() const;
    int GetSize() const;

    /*!
     * \note Thread affinity: any.
     */
    int GetStartBlockIndex() const;

    /*!
     * \note Thread affinity: any.
     */
    int GetEndBlockIndex() const;

    /*!
     * \note Thread affinity: WriterThread.
     */
    bool IsFlushing() const;

    /*!
     * \note Thread affinity: WriterThread.
     */
    void SetFlushing();

private:
    bool IsFlushing_;
    yvector<bool> IsSent;

    yvector<TSharedRef> Blocks;
    int StartBlockIndex;

    int Size;

    TRemoteWriter::TWeak Writer;

    NLog::TTaggedLogger& Logger;

    /*!
     * \note Thread affinity: WriterThread.
     */
    void PutGroup();

    /*!
     * \note Thread affinity: WriterThread.
     */
    TProxy::TInvPutBlocks::TPtr PutBlocks(THolderPtr holder);

    /*!
     * \note Thread affinity: WriterThread.
     */
    void OnPutBlocks(TProxy::TRspPutBlocks::TPtr rsp, THolderPtr holder);

    /*!
     * \note Thread affinity: WriterThread.
     */
    void SendGroup(THolderPtr srcHolder);

    /*!
     * \note Thread affinity: WriterThread.
     */
    TProxy::TInvSendBlocks::TPtr SendBlocks(THolderPtr srcHolder, THolderPtr dstHolder);

    /*!
     * \note Thread affinity: WriterThread.
     */
    void CheckSendResponse(
        TRemoteWriter::TProxy::TRspSendBlocks::TPtr rsp,
        THolderPtr srcHolder, 
        THolderPtr dstHolder);

    /*!
     * \note Thread affinity: WriterThread.
     */
    void OnSentBlocks(TProxy::TRspSendBlocks::TPtr rsp, THolderPtr srcHolder, THolderPtr dstHolder);
};

///////////////////////////////////////////////////////////////////////////////

TRemoteWriter::TGroup::TGroup(
    int holderCount, 
    int startBlockIndex, 
    TRemoteWriter* writer)
    : IsFlushing_(false)
    , IsSent(holderCount, false)
    , StartBlockIndex(startBlockIndex)
    , Size(0)
    , Writer(writer)
    , Logger(writer->Logger)
{ }

void TRemoteWriter::TGroup::AddBlock(const TSharedRef& block)
{
    Blocks.push_back(block);
    Size += block.Size();
}

int TRemoteWriter::TGroup::GetStartBlockIndex() const
{
    return StartBlockIndex;
}

int TRemoteWriter::TGroup::GetEndBlockIndex() const
{
    return StartBlockIndex + Blocks.ysize() - 1;
}

int TRemoteWriter::TGroup::GetSize() const
{
    return Size;
}

bool TRemoteWriter::TGroup::IsWritten() const
{
    auto writer = Writer.Lock();
    YASSERT(writer);

    VERIFY_THREAD_AFFINITY(writer->WriterThread);

    for (int holderIndex = 0; holderIndex < IsSent.ysize(); ++holderIndex) {
        if (writer->Holders[holderIndex]->IsAlive && !IsSent[holderIndex]) {
            return false;
        }
    }
    return true;
}

void TRemoteWriter::TGroup::PutGroup()
{
    auto writer = Writer.Lock();
    YASSERT(writer);

    VERIFY_THREAD_AFFINITY(writer->WriterThread);

    int holderIndex = 0;
    while (!writer->Holders[holderIndex]->IsAlive) {
        ++holderIndex;
        YASSERT(holderIndex < writer->Holders.ysize());
    }

    auto holder = writer->Holders[holderIndex];
    auto awaiter = New<TParallelAwaiter>(WriterThread->GetInvoker());
    auto onSuccess = FromMethod(
        &TGroup::OnPutBlocks, 
        TWeakPtr<TGroup>(this), 
        holder);
    auto onResponse = FromMethod(
        &TRemoteWriter::CheckResponse<TProxy::TRspPutBlocks>,
        Writer, // WeakPtr here
        holder, 
        onSuccess,
        &writer->PutBlocksTiming);
    awaiter->Await(PutBlocks(holder), onResponse);
    awaiter->Complete(FromMethod(
        &TRemoteWriter::TGroup::Process, 
        TWeakPtr<TGroup>(this)));
}

TRemoteWriter::TProxy::TInvPutBlocks::TPtr
TRemoteWriter::TGroup::PutBlocks(THolderPtr holder)
{
    auto writer = Writer.Lock();
    YASSERT(writer);

    VERIFY_THREAD_AFFINITY(writer->WriterThread);

    auto req = holder->Proxy.PutBlocks();
    req->set_chunk_id(writer->ChunkId.ToProto());
    req->set_start_block_index(StartBlockIndex);
    req->Attachments().insert(req->Attachments().begin(), Blocks.begin(), Blocks.end());

    LOG_DEBUG("Putting blocks (Blocks: %d-%d, Address: %s)",
        StartBlockIndex, 
        GetEndBlockIndex(),
        ~holder->Address);

    return req->Invoke();
}

void TRemoteWriter::TGroup::OnPutBlocks(TProxy::TRspPutBlocks::TPtr rsp, THolderPtr holder)
{
    auto writer = Writer.Lock();
    if (!writer)
        return;

    UNUSED(rsp);
    VERIFY_THREAD_AFFINITY(writer->WriterThread);

    IsSent[holder->Index] = true;

    LOG_DEBUG("Blocks are put (Blocks, %d-%d, Address: %s)",
        StartBlockIndex, 
        GetEndBlockIndex(),
        ~holder->Address);

    writer->SchedulePing(holder);
}

void TRemoteWriter::TGroup::SendGroup(THolderPtr srcHolder)
{
    auto writer = Writer.Lock();
    YASSERT(writer);

    VERIFY_THREAD_AFFINITY(writer->WriterThread);

    for (int dstHolderIndex = 0; dstHolderIndex < IsSent.ysize(); ++dstHolderIndex) {
        auto dstHolder = writer->Holders[dstHolderIndex];
        if (dstHolder->IsAlive && !IsSent[dstHolderIndex]) {
            auto awaiter = New<TParallelAwaiter>(WriterThread->GetInvoker());
            auto onResponse = FromMethod(
                &TGroup::CheckSendResponse,
                TWeakPtr<TGroup>(this),
                srcHolder,
                dstHolder);
            awaiter->Await(SendBlocks(srcHolder, dstHolder), onResponse);
            awaiter->Complete(FromMethod(&TGroup::Process, TWeakPtr<TGroup>(this)));
            break;
        }
    }
}

TRemoteWriter::TProxy::TInvSendBlocks::TPtr
TRemoteWriter::TGroup::SendBlocks(
    THolderPtr srcHolder, 
    THolderPtr dstHolder)
{
    auto writer = Writer.Lock();
    YASSERT(writer);

    VERIFY_THREAD_AFFINITY(writer->WriterThread);

    LOG_DEBUG("Sending blocks (Blocks: %d-%d, SrcAddress: %s, DstAddress: %s)",
        StartBlockIndex, 
        GetEndBlockIndex(),
        ~srcHolder->Address,
        ~dstHolder->Address);

    auto req = srcHolder->Proxy.SendBlocks();
    req->set_chunk_id(writer->ChunkId.ToProto());
    req->set_start_block_index(StartBlockIndex);
    req->set_block_count(Blocks.ysize());
    req->set_address(dstHolder->Address);
    return req->Invoke();
}

void TRemoteWriter::TGroup::CheckSendResponse(
    TRemoteWriter::TProxy::TRspSendBlocks::TPtr rsp,
    THolderPtr srcHolder, 
    THolderPtr dstHolder)
{
    auto writer = Writer.Lock();
    if (!writer)
        return;

    if (rsp->GetErrorCode() == EErrorCode::PutBlocksFailed) {
        writer->OnHolderDied(dstHolder);
        return;
    }

    auto onSuccess = FromMethod(
        &TGroup::OnSentBlocks, 
        this, // no need for smart pointer here - we're invoking action directly.
        srcHolder, 
        dstHolder);

    writer->CheckResponse<TRemoteWriter::TProxy::TRspSendBlocks>(
        rsp, 
        srcHolder, 
        onSuccess,
        &writer->SendBlocksTiming);
}

void TRemoteWriter::TGroup::OnSentBlocks(
    TProxy::TRspSendBlocks::TPtr rsp, 
    THolderPtr srcHolder, 
    THolderPtr dstHolder)
{
    auto writer = Writer.Lock();
    YASSERT(writer);

    UNUSED(rsp);
    VERIFY_THREAD_AFFINITY(writer->WriterThread);

    LOG_DEBUG("Blocks are sent (Blocks: %d-%d, SrcAddress: %s, DstAddress: %s)",
        StartBlockIndex, 
        GetEndBlockIndex(),
        ~srcHolder->Address,
        ~dstHolder->Address);

    IsSent[dstHolder->Index] = true;

    writer->SchedulePing(srcHolder);
    writer->SchedulePing(dstHolder);
}

bool TRemoteWriter::TGroup::IsFlushing() const
{
    auto writer = Writer.Lock();
    YASSERT(writer);

    VERIFY_THREAD_AFFINITY(writer->WriterThread);

    return IsFlushing_;
}

void TRemoteWriter::TGroup::SetFlushing()
{
    auto writer = Writer.Lock();
    YASSERT(writer);

    VERIFY_THREAD_AFFINITY(writer->WriterThread);

    IsFlushing_ = true;
}

void TRemoteWriter::TGroup::Process()
{
    auto writer = Writer.Lock();
    if (!writer)
        return;

    VERIFY_THREAD_AFFINITY(writer->WriterThread);

    if (!writer->State.IsActive()) {
        return;
    }

    YASSERT(writer->IsInitComplete);

    LOG_DEBUG("Processing group (Blocks: %d-%d)",
        StartBlockIndex, 
        GetEndBlockIndex());

    THolderPtr holderWithBlocks;
    bool emptyHolderFound = false;
    for (int holderIndex = 0; holderIndex < IsSent.ysize(); ++holderIndex) {
        auto holder = writer->Holders[holderIndex];
        if (holder->IsAlive) {
            if (IsSent[holderIndex]) {
                holderWithBlocks = holder;
            } else {
                emptyHolderFound = true;
            }
        }
    }

    if (!emptyHolderFound) {
        writer->ShiftWindow();
    } else if (!holderWithBlocks) {
        PutGroup();
    } else {
        SendGroup(holderWithBlocks);
    }
}

///////////////////////////////////////////////////////////////////////////////

TRemoteWriter::TRemoteWriter(
    TRemoteWriter::TConfig* config, 
    const TChunkId& chunkId,
    const yvector<Stroka>& addresses)
    : Config(config)
    , ChunkId(chunkId) 
    , Addresses(addresses)
    , IsOpen(false)
    , IsInitComplete(false)
    , IsCloseRequested(false)
    , WindowSlots(config->WindowSize)
    , AliveHolderCount(addresses.ysize())
    , CurrentGroup(New<TGroup>(AliveHolderCount, 0, this))
    , BlockCount(0)
    , ChunkSize(-1)
    , StartChunkTiming(0, 1000, 20)
    , PutBlocksTiming(0, 1000, 20)
    , SendBlocksTiming(0, 1000, 20)
    , FlushBlockTiming(0, 1000, 20)
    , FinishChunkTiming(0, 1000, 20)
    , Logger(ChunkClientLogger)
{
    YASSERT(AliveHolderCount > 0);

    Logger.AddTag(Sprintf("ChunkId: %s", ~ChunkId.ToString()));

    for (int index = 0; index < static_cast<int>(addresses.size()); ++index) {
        auto address = addresses[index];
        auto holder = New<THolder>(
            index,
            address,
            Config->HolderRpcTimeout);
        Holders.push_back(holder);
    }
}

TRemoteWriter::~TRemoteWriter()
{
    VERIFY_THREAD_AFFINITY_ANY();

    // Just a quick check.
    if (!State.IsActive())
        return;

    LOG_DEBUG("Writer canceled (ChunkId: %s)", ~ChunkId.ToString());

    State.Cancel(TError(TError::Fail, ""));
}

void TRemoteWriter::Open()
{
    LOG_DEBUG("Opening writer (Addresses: [%s])", ~JoinToString(Addresses));

    auto awaiter = New<TParallelAwaiter>(WriterThread->GetInvoker());
    FOREACH (auto holder, Holders) {
        auto onSuccess = FromMethod(
            &TRemoteWriter::OnChunkStarted, 
            TWeak(this), 
            holder);
        auto onResponse = FromMethod(
            &TRemoteWriter::CheckResponse<TProxy::TRspStartChunk>,
            TWeak(this),
            holder,
            onSuccess,
            &StartChunkTiming);
        awaiter->Await(StartChunk(holder), onResponse);
    }
    awaiter->Complete(FromMethod(&TRemoteWriter::OnSessionStarted, TWeak(this)));

    IsOpen = true;
}

void TRemoteWriter::ShiftWindow()
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    if (!State.IsActive()) {
        YASSERT(Window.empty());
        return;
    }

    int lastFlushableBlock = -1;
    for (auto it = Window.begin(); it != Window.end(); ++it) {
        auto group = *it;
        if (!group->IsFlushing()) {
            if (group->IsWritten()) {
                lastFlushableBlock = group->GetEndBlockIndex();
                group->SetFlushing();
            } else {
                break;
            }
        }
    }

    if (lastFlushableBlock < 0)
        return;

    auto awaiter = New<TParallelAwaiter>(WriterThread->GetInvoker());
    FOREACH (auto holder, Holders) {
        if (holder->IsAlive) {
            auto onSuccess = FromMethod(
                &TRemoteWriter::OnBlockFlushed, 
                TWeak(this), 
                holder,
                lastFlushableBlock);
            auto onResponse = FromMethod(
                &TRemoteWriter::CheckResponse<TProxy::TRspFlushBlock>,
                TWeak(this), 
                holder, 
                onSuccess,
                &FlushBlockTiming);
            awaiter->Await(FlushBlock(holder, lastFlushableBlock), onResponse);
        }
    }

    awaiter->Complete(FromMethod(
        &TRemoteWriter::OnWindowShifted, 
        TWeak(this),
        lastFlushableBlock));
}

TRemoteWriter::TProxy::TInvFlushBlock::TPtr
TRemoteWriter::FlushBlock(THolderPtr holder, int blockIndex)
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    LOG_DEBUG("Flushing blocks (BlockIndex: %d, Address: %s)",
        blockIndex,
        ~holder->Address);

    auto req = holder->Proxy.FlushBlock();
    req->set_chunk_id(ChunkId.ToProto());
    req->set_block_index(blockIndex);
    return req->Invoke();
}

void TRemoteWriter::OnBlockFlushed(TProxy::TRspFlushBlock::TPtr rsp, THolderPtr holder, int blockIndex)
{
    UNUSED(rsp);
    VERIFY_THREAD_AFFINITY(WriterThread);

    LOG_DEBUG("Blocks flushed (BlockIndex: %d, Address: %s)",
        blockIndex,
        ~holder->Address);

    SchedulePing(holder);
}

void TRemoteWriter::OnWindowShifted(int lastFlushedBlock)
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    if (Window.empty()) {
        // This happens when FlushBlocks responses are disordered
        // (i.e. a bigger BlockIndex is flushed before a smaller one)
        // and prevents repeated calling CloseSession
        return;
    }

    while (!Window.empty()) {
        auto group = Window.front();
        if (group->GetEndBlockIndex() > lastFlushedBlock)
            return;

        LOG_DEBUG("Window shifted (BlockIndex: %d, Size: %d)",
            group->GetEndBlockIndex(),
            group->GetSize());

        WindowSlots.Release(group->GetSize());
        Window.pop_front();
    }

    if (State.IsActive() && IsCloseRequested) {
        CloseSession();
    }
}

void TRemoteWriter::DoClose(const TChunkAttributes& attributes)
{
    VERIFY_THREAD_AFFINITY(WriterThread);
    YASSERT(!IsCloseRequested);

    if (!State.IsActive()) {
        State.FinishOperation();
        return;
    }

    LOG_DEBUG("Writer close requested");

    IsCloseRequested = true;
    Attributes.CopyFrom(attributes);

    if (Window.empty() && IsInitComplete) {
        CloseSession();
    }
}

void TRemoteWriter::AddGroup(TGroupPtr group)
{
    VERIFY_THREAD_AFFINITY(WriterThread);
    YASSERT(!IsCloseRequested);

    if (!State.IsActive())
        return;

    LOG_DEBUG("Group added (Blocks: %d-%d)",
        group->GetStartBlockIndex(),
        group->GetEndBlockIndex());

    Window.push_back(group);

    if (IsInitComplete) {
        group->Process();
    }
}

void TRemoteWriter::OnHolderDied(THolderPtr holder)
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    if (!holder->IsAlive)
        return;

    holder->IsAlive = false;
    --AliveHolderCount;

    LOG_INFO("Holder died (Address: %s, AliveCount: %d)",
        ~holder->Address,
        AliveHolderCount);

    if (State.IsActive() && AliveHolderCount == 0) {
        TError error("No alive holders left");
        LOG_WARNING("Chunk writing failed\n%s", ~error.ToString());
        State.Fail(error);
    }
}

template <class TResponse>
void TRemoteWriter::CheckResponse(
    TIntrusivePtr<TResponse> rsp,
    THolderPtr holder,
    typename IParamAction< TIntrusivePtr<TResponse> >::TPtr onSuccess, 
    TMetric* metric)
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    if (rsp->IsOK()) {
        metric->AddDelta(rsp->GetStartTime());
        onSuccess->Do(rsp);
    } else {
        // TODO: retry?
        LOG_ERROR("Error reported by holder (Address: %s)\n%s",
            ~holder->Address, 
            ~rsp->GetError().ToString());
        OnHolderDied(holder);
    }
}

TRemoteWriter::TProxy::TInvStartChunk::TPtr TRemoteWriter::StartChunk(THolderPtr holder)
{
    LOG_DEBUG("Starting chunk (Address: %s)", ~holder->Address);

    auto req = holder->Proxy.StartChunk();
    req->set_chunk_id(ChunkId.ToProto());
    return req->Invoke();
}

void TRemoteWriter::OnChunkStarted(TProxy::TRspStartChunk::TPtr rsp, THolderPtr holder)
{
    UNUSED(rsp);
    VERIFY_THREAD_AFFINITY(WriterThread);

    LOG_DEBUG("Chunk started (Address: %s)", ~holder->Address);

    SchedulePing(holder);
}

void TRemoteWriter::OnSessionStarted()
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    // Check if the session is not canceled yet.
    if (!State.IsActive()) {
        return;
    }

    LOG_DEBUG("Writer is ready");

    IsInitComplete = true;
    FOREACH(auto& group, Window) {
        group->Process();
    }

    // Possible for an empty chunk.
    if (Window.empty() && IsCloseRequested) {
        CloseSession();
    }
}

void TRemoteWriter::CloseSession()
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    YASSERT(IsCloseRequested);

    LOG_DEBUG("Closing writer");

    auto awaiter = New<TParallelAwaiter>(WriterThread->GetInvoker());
    FOREACH (auto holder, Holders) {
        if (holder->IsAlive) {
            auto onSuccess = FromMethod(
                &TRemoteWriter::OnChunkFinished, 
                TWeak(this), 
                holder);
            auto onResponse = FromMethod(
                &TRemoteWriter::CheckResponse<TProxy::TRspFinishChunk>,
                TWeak(this), 
                holder, 
                onSuccess, 
                &FinishChunkTiming);
            awaiter->Await(FinishChunk(holder), onResponse);
        }
    }
    awaiter->Complete(FromMethod(&TRemoteWriter::OnSessionFinished, TWeak(this)));
}

void TRemoteWriter::OnChunkFinished(TProxy::TRspFinishChunk::TPtr rsp, THolderPtr holder)
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    i64 size = rsp->size();
    LOG_DEBUG("Chunk is finished (Address: %s, Size: %" PRId64 ")",
        ~holder->Address,
        size);

    if (ChunkSize >= 0 && ChunkSize != size) {
        LOG_FATAL("Mismatched chunk size reported by holder (KnownSize: %" PRId64 ", NewSize: %" PRId64 ", Address: %s)",
            ChunkSize,
            size,
            ~holder->Address);
    }
    ChunkSize = size;
}

TRemoteWriter::TProxy::TInvFinishChunk::TPtr
TRemoteWriter::FinishChunk(THolderPtr holder)
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    LOG_DEBUG("Finishing chunk (Address: %s)", ~holder->Address);

    auto req = holder->Proxy.FinishChunk();
    req->set_chunk_id(ChunkId.ToProto());
    req->mutable_attributes()->CopyFrom(Attributes);
    return req->Invoke();
}

void TRemoteWriter::OnSessionFinished()
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    YASSERT(Window.empty());

    if (State.IsActive()) {
        State.Close();
    }

    CancelAllPings();

    LOG_DEBUG("Writer closed");

    State.FinishOperation();
}

void TRemoteWriter::PingSession(THolderPtr holder)
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    LOG_DEBUG("Pinging session (Address: %s)", ~holder->Address);

    auto req = holder->Proxy.PingSession();
    req->set_chunk_id(ChunkId.ToProto());
    req->Invoke();

    SchedulePing(holder);
}

void TRemoteWriter::SchedulePing(THolderPtr holder)
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    if (!State.IsActive()) {
        return;
    }

    TDelayedInvoker::CancelAndClear(holder->Cookie);
    holder->Cookie = TDelayedInvoker::Submit(
        ~FromMethod(
            &TRemoteWriter::PingSession,
            TWeak(this),
            holder)
        ->Via(WriterThread->GetInvoker()),
        Config->SessionPingInterval);
}

void TRemoteWriter::CancelPing(THolderPtr holder)
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    TDelayedInvoker::CancelAndClear(holder->Cookie);
}

void TRemoteWriter::CancelAllPings()
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    FOREACH (auto holder, Holders) {
        CancelPing(holder);
    }
}

TAsyncError::TPtr TRemoteWriter::AsyncWriteBlocks(std::vector<TSharedRef>&& blocks)
{
    VERIFY_THREAD_AFFINITY(ClientThread);
    YASSERT(IsOpen);
    YASSERT(!State.HasRunningOperation());
    YASSERT(!State.IsClosed());

    i64 sumSize = 0;
    FOREACH(auto& block, blocks) {
        sumSize += block.Size();
    }

    State.StartOperation();

    WindowSlots.AsyncAcquire(sumSize)->Subscribe(FromMethod(
        &TRemoteWriter::DoWriteBlocks,
        TWeak(this),
        blocks));

    return State.GetOperationError();
}

void TRemoteWriter::DoWriteBlocks(TVoid, std::vector<TSharedRef>& blocks)
{
    if (State.IsActive()) {
        AddBlocks(MoveRV(blocks));
    }

    State.FinishOperation();
}

void TRemoteWriter::AddBlocks(std::vector<TSharedRef>&& blocks)
{
    FOREACH(auto& block, blocks){
        LOG_DEBUG("Block added (BlockIndex: %d)",
            BlockCount);

        CurrentGroup->AddBlock(block);
        ++BlockCount;

        if (CurrentGroup->GetSize() >= Config->GroupSize) {
            WriterThread->GetInvoker()->Invoke(FromMethod(
                &TRemoteWriter::AddGroup,
                TWeak(this),
                CurrentGroup));
            // Construct a new (empty) group.
            CurrentGroup = New<TGroup>(Holders.ysize(), BlockCount, this);
        }
    }
}

TAsyncError::TPtr TRemoteWriter::AsyncClose(
    std::vector<TSharedRef>&& lastBlocks,
    const TChunkAttributes& attributes)
{
    YASSERT(IsOpen);
    YASSERT(!State.HasRunningOperation());
    YASSERT(!State.IsClosed());

    State.StartOperation();

    AddBlocks(MoveRV(lastBlocks));

    LOG_DEBUG("Requesting writer to close");

    if (CurrentGroup->GetSize() > 0) {
        WriterThread->GetInvoker()->Invoke(FromMethod(
            &TRemoteWriter::AddGroup,
            TWeak(this), 
            CurrentGroup));
    }

    // Set IsCloseRequested via queue to ensure proper serialization
    // (i.e. the flag will be set when all appended blocks are processed).
    WriterThread->GetInvoker()->Invoke(FromMethod(
        &TRemoteWriter::DoClose, 
        TWeak(this),
        attributes));

    return State.GetOperationError();
}

Stroka TRemoteWriter::GetDebugInfo()
{
    return Sprintf(
        "ChunkId: %s; "
        "StartChunk: (%s); "
        "FinishChunk timing: (%s); "
        "PutBlocks timing: (%s); "
        "SendBlocks timing: (%s); "
        "FlushBlocks timing: (%s); ",
        ~ChunkId.ToString(), 
        ~StartChunkTiming.GetDebugInfo(),
        ~FinishChunkTiming.GetDebugInfo(),
        ~PutBlocksTiming.GetDebugInfo(),
        ~SendBlocksTiming.GetDebugInfo(),
        ~FlushBlockTiming.GetDebugInfo());
}

TChunkId TRemoteWriter::GetChunkId() const
{
    VERIFY_THREAD_AFFINITY_ANY();
    return ChunkId;
}

TChunkYPathProxy::TReqConfirm::TPtr TRemoteWriter::GetConfirmRequest()
{
    VERIFY_THREAD_AFFINITY_ANY();
    YASSERT(State.IsClosed());

    auto req = TChunkYPathProxy::Confirm(FromObjectId(ChunkId));
    req->set_size(ChunkSize);
    *req->mutable_attributes() = Attributes;
    FOREACH (auto holder, Holders) {
        if (holder->IsAlive) {
            req->add_holder_addresses(holder->Address);
        }
    }

    return req;
}

///////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
