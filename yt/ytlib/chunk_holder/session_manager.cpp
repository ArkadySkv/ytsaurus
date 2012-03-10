#include "stdafx.h"
#include "session_manager.h"
#include "common.h"
#include "config.h"
#include "location.h"
#include "block_store.h"
#include "chunk.h"
#include "chunk_store.h"
#include "chunk.pb.h"

#include <ytlib/misc/fs.h>
#include <ytlib/misc/sync.h>

namespace NYT {
namespace NChunkHolder {

using namespace NRpc;
using namespace NChunkClient;
using namespace NProto;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = ChunkHolderLogger;

////////////////////////////////////////////////////////////////////////////////

TSession::TSession(
    TSessionManager* sessionManager,
    const TChunkId& chunkId,
    TLocation* location)
    : SessionManager(sessionManager)
    , ChunkId(chunkId)
    , Location(location)
    , WindowStart(0)
    , FirstUnwritten(0)
    , Size(0)
    , Logger(ChunkHolderLogger)
{
    YASSERT(sessionManager);
    YASSERT(location);

    Logger.AddTag(Sprintf("ChunkId: %s", ~ChunkId.ToString()));

    Location->UpdateSessionCount(+1);
    FileName = Location->GetChunkFileName(ChunkId);
}

TSession::~TSession()
{
    Location->UpdateSessionCount(-1);
}

void TSession::Start()
{
    GetIOInvoker()->Invoke(FromMethod(
        &TSession::DoOpenFile,
        TPtr(this)));
}

void TSession::DoOpenFile()
{
    try {
        NFS::ForcePath(NFS::GetDirectoryName(FileName));
        Writer = New<TChunkFileWriter>(ChunkId, FileName);
        Writer->Open();
    }
    catch (const std::exception& ex) {
        LOG_FATAL("Error opening chunk file\n%s", ex.what());
    }
    LOG_DEBUG("Chunk file opened");
}

void TSession::SetLease(TLeaseManager::TLease lease)
{
    Lease = lease;
}

void TSession::RenewLease()
{
    TLeaseManager::RenewLease(Lease);
}

void TSession::CloseLease()
{
    TLeaseManager::CloseLease(Lease);
}

IInvoker::TPtr TSession::GetIOInvoker()
{
    return Location->GetInvoker();
}

TChunkId TSession::GetChunkId() const
{
    return ChunkId;
}

TLocationPtr TSession::GetLocation() const
{
    return Location;
}

i64 TSession::GetSize() const
{
    return Size;
}

TChunkInfo TSession::GetChunkInfo() const
{
    return Writer->GetChunkInfo();
}

TCachedBlockPtr TSession::GetBlock(i32 blockIndex)
{
    VerifyInWindow(blockIndex);

    RenewLease();

    const auto& slot = GetSlot(blockIndex);
    if (slot.State == ESlotState::Empty) {
        ythrow TServiceException(EErrorCode::WindowError) <<
            Sprintf("Trying to retrieve a block that is not received yet (WindowStart: %d, BlockIndex: %d)",
            WindowStart,
            blockIndex);
    }

    LOG_DEBUG("Chunk block retrieved (BlockIndex: %d)",
        blockIndex);

    return slot.Block;
}

void TSession::PutBlock(i32 blockIndex, const TSharedRef& data)
{
    TBlockId blockId(ChunkId, blockIndex);

    VerifyInWindow(blockIndex);

    RenewLease();

    if (!Location->HasEnoughSpace(data.Size())) {
        ythrow TServiceException(EErrorCode::OutOfSpace) <<
            Sprintf("Not enough space to put block (BlockId: %s)",
            ~blockId.ToString());
    }

    auto& slot = GetSlot(blockIndex);
    if (slot.State != ESlotState::Empty) {
        if (TRef::CompareContent(slot.Block->GetData(), data)) {
            LOG_WARNING("Block has been already received (BlockId: %s)", ~blockId.ToString());
            return;
        }

        ythrow TServiceException(EErrorCode::BlockContentMismatch) <<
            Sprintf("Block with the same id but different content already received (BlockId: %s, WindowStart: %d)",
            ~blockId.ToString(),
            WindowStart);
    }

    slot.State = ESlotState::Received;
    slot.Block = SessionManager->BlockStore->PutBlock(blockId, data, Stroka());

    Location->UpdateUsedSpace(data.Size());
    Size += data.Size();

    LOG_DEBUG("Chunk block received (BlockId: %s)", ~blockId.ToString());

    EnqueueWrites();
}

void TSession::EnqueueWrites()
{
    while (IsInWindow(FirstUnwritten)) {
        i32 blockIndex = FirstUnwritten;

        const TSlot& slot = GetSlot(blockIndex);
        if (slot.State != ESlotState::Received)
            break;

        FromMethod(
            &TSession::DoWrite,
            TPtr(this),
            slot.Block,
            blockIndex)
        ->AsyncVia(GetIOInvoker())
        ->Do()
        ->Subscribe(FromMethod(
            &TSession::OnBlockWritten,
            TPtr(this),
            blockIndex)
        ->Via(SessionManager->ServiceInvoker));

        ++FirstUnwritten;
    }
}

TVoid TSession::DoWrite(TCachedBlockPtr block, i32 blockIndex)
{
    LOG_DEBUG("Start writing chunk block (BlockIndex: %d)",
        blockIndex);

    try {
        Sync(~Writer, &TChunkFileWriter::AsyncWriteBlock, block->GetData());
    } catch (const std::exception& ex) {
        LOG_FATAL("Error writing chunk block (BlockIndex: %d)\n%s",
            blockIndex,
            ex.what());
    }

    LOG_DEBUG("Chunk block written (BlockIndex: %d)",
        blockIndex);

    return TVoid();
}

void TSession::OnBlockWritten(TVoid, i32 blockIndex)
{
    auto& slot = GetSlot(blockIndex);
    YASSERT(slot.State == ESlotState::Received);
    slot.State = ESlotState::Written;
    slot.IsWritten->Set(TVoid());
}

TFuture<TVoid>::TPtr TSession::FlushBlock(i32 blockIndex)
{
    // TODO: verify monotonicity of blockIndex

    VerifyInWindow(blockIndex);

    RenewLease();

    const TSlot& slot = GetSlot(blockIndex);
    if (slot.State == ESlotState::Empty) {
        ythrow TServiceException(EErrorCode::WindowError) <<
            Sprintf("Flushing an empty block (WindowStart: %d, WindowSize: %d, BlockIndex: %d)",
            WindowStart,
            Window.ysize(),
            blockIndex);
    }

    // IsWritten is set in ServiceInvoker, hence no need for AsyncVia.
    return slot.IsWritten->Apply(FromMethod(
        &TSession::OnBlockFlushed,
        TPtr(this),
        blockIndex));
}

TVoid TSession::OnBlockFlushed(TVoid, i32 blockIndex)
{
    ReleaseBlocks(blockIndex);
    return TVoid();
}

TFuture<TChunkPtr>::TPtr TSession::Finish(const TChunkAttributes& attributes)
{
    CloseLease();

    for (i32 blockIndex = WindowStart; blockIndex < Window.ysize(); ++blockIndex) {
        const TSlot& slot = GetSlot(blockIndex);
        if (slot.State != ESlotState::Empty) {
            ythrow TServiceException(EErrorCode::WindowError) <<
                Sprintf("Finishing a session with an unflushed block (WindowStart: %d, WindowSize: %d, BlockIndex: %d)",
                WindowStart,
                Window.ysize(),
                blockIndex);
        }
    }

    return CloseFile(attributes)->Apply(
        FromMethod(&TSession::OnFileClosed, TPtr(this))
        ->AsyncVia(SessionManager->ServiceInvoker));
}

void TSession::Cancel(const TError& error)
{
    CloseLease();
    DeleteFile(error)->Apply(
        FromMethod(&TSession::OnFileDeleted, TPtr(this))
        ->AsyncVia(SessionManager->ServiceInvoker));
}

TFuture<TVoid>::TPtr TSession::DeleteFile(const TError& error)
{
    return
        FromMethod(
            &TSession::DoDeleteFile,
            TPtr(this),
            error)
        ->AsyncVia(GetIOInvoker())
        ->Do();
}

TVoid TSession::DoDeleteFile(const TError& error)
{
    Writer.Reset();

    LOG_DEBUG("Chunk file deleted\n%s",
        ~error.ToString());

    return TVoid();
}

TVoid TSession::OnFileDeleted(TVoid)
{
    ReleaseSpaceOccupiedByBlocks();
    return TVoid();
}

TFuture<TVoid>::TPtr TSession::CloseFile(const TChunkAttributes& attributes)
{
    return
        FromMethod(
            &TSession::DoCloseFile,
            TPtr(this),
            attributes)
        ->AsyncVia(GetIOInvoker())
        ->Do();
}

TVoid TSession::DoCloseFile(const TChunkAttributes& attributes)
{
    try {
        Sync(~Writer, &TChunkFileWriter::AsyncClose, attributes);
    } catch (const std::exception& ex) {
        LOG_FATAL("Error closing chunk file\n%s",
            ex.what());
    }

    LOG_DEBUG("Chunk file closed");

    return TVoid();
}

TChunkPtr TSession::OnFileClosed(TVoid)
{
    ReleaseSpaceOccupiedByBlocks();
    auto chunk = New<TStoredChunk>(~Location, GetChunkInfo());
    SessionManager->ChunkStore->RegisterChunk(~chunk);
    return chunk;
}

void TSession::ReleaseBlocks(i32 flushedBlockIndex)
{
    YASSERT(WindowStart <= flushedBlockIndex);

    while (WindowStart <= flushedBlockIndex) {
        auto& slot = GetSlot(WindowStart);
        slot.Block.Reset();
        slot.IsWritten.Reset();
        ++WindowStart;
    }

    LOG_DEBUG("Released blocks (WindowStart: %d)",
        WindowStart);
}

bool TSession::IsInWindow(i32 blockIndex)
{
    return blockIndex >= WindowStart;
}

void TSession::VerifyInWindow(i32 blockIndex)
{
    if (!IsInWindow(blockIndex)) {
        ythrow TServiceException(EErrorCode::WindowError) <<
            Sprintf("Accessing a block out of the window (WindowStart: %d, WindowSize: %d, BlockIndex: %d)",
            WindowStart,
            Window.ysize(),
            blockIndex);
    }
}

TSession::TSlot& TSession::GetSlot(i32 blockIndex)
{
    YASSERT(IsInWindow(blockIndex));
    while (Window.size() <= blockIndex) {
        Window.resize(blockIndex + 1);
    }

    return Window[blockIndex];
}

void TSession::ReleaseSpaceOccupiedByBlocks()
{
    Location->UpdateUsedSpace(-Size);
}

////////////////////////////////////////////////////////////////////////////////

TSessionManager::TSessionManager(
    TChunkHolderConfig* config,
    TBlockStore* blockStore,
    TChunkStore* chunkStore,
    IInvoker* serviceInvoker)
    : Config(config)
    , BlockStore(blockStore)
    , ChunkStore(chunkStore)
    , ServiceInvoker(serviceInvoker)
{
    YASSERT(blockStore);
    YASSERT(chunkStore);
    YASSERT(serviceInvoker);
}

TSessionPtr TSessionManager::FindSession(const TChunkId& chunkId) const
{
    auto it = SessionMap.find(chunkId);
    if (it == SessionMap.end())
        return NULL;
    
    auto session = it->second;
    session->RenewLease();
    return session;
}

TSessionPtr TSessionManager::StartSession(
    const TChunkId& chunkId)
{
    auto location = ChunkStore->GetNewChunkLocation();

    auto session = New<TSession>(this, chunkId, ~location);
    session->Start();

    auto lease = TLeaseManager::CreateLease(
        Config->SessionTimeout,
        ~FromMethod(
            &TSessionManager::OnLeaseExpired,
            TPtr(this),
            session)
        ->Via(ServiceInvoker));
    session->SetLease(lease);

    YVERIFY(SessionMap.insert(MakePair(chunkId, session)).second);

    LOG_INFO("Session started (ChunkId: %s, Location: %s)",
        ~chunkId.ToString(),
        ~location->GetPath());

    return session;
}

void TSessionManager::CancelSession(TSession* session, const TError& error)
{
    auto chunkId = session->GetChunkId();

    YVERIFY(SessionMap.erase(chunkId) == 1);

    session->Cancel(error);

    LOG_INFO("Session canceled (ChunkId: %s)\n%s",
        ~chunkId.ToString(),
        ~error.ToString());
}

TFuture<TChunkPtr>::TPtr TSessionManager::FinishSession(
    TSession* session, 
    const TChunkAttributes& attributes)
{
    auto chunkId = session->GetChunkId();

    YVERIFY(SessionMap.erase(chunkId) == 1);

    return session->Finish(attributes)->Apply(FromMethod(
        &TSessionManager::OnSessionFinished,
        TPtr(this),
        session));
}

TChunkPtr TSessionManager::OnSessionFinished(TChunkPtr chunk, TSessionPtr session)
{
    LOG_INFO("Session finished (ChunkId: %s)", ~session->GetChunkId().ToString());
    return chunk;
}

void TSessionManager::OnLeaseExpired(TSessionPtr session)
{
    if (SessionMap.find(session->GetChunkId()) != SessionMap.end()) {
        LOG_INFO("Session lease expired (ChunkId: %s)", ~session->GetChunkId().ToString());
        CancelSession(~session, TError("Session lease expired"));
    }
}

int TSessionManager::GetSessionCount() const
{
    return SessionMap.ysize();
}

TSessionManager::TSessions TSessionManager::GetSessions() const
{
    TSessions result;
    result.reserve(SessionMap.ysize());
    FOREACH(const auto& pair, SessionMap) {
        result.push_back(pair.second);
    }
    return result;
}

////////////////////////////////////////////////////////////////////////////////


} // namespace NChunkHolder
} // namespace NYT
