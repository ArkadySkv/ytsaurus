#include "stdafx.h"
#include "decorated_automaton.h"
#include "config.h"
#include "snapshot.h"
#include "changelog.h"
#include "automaton.h"
#include "serialize.h"
#include "mutation_context.h"
#include "snapshot_discovery.h"

#include <core/concurrency/scheduler.h>

#include <core/rpc/response_keeper.h>

#include <ytlib/election/cell_manager.h>

#include <ytlib/hydra/hydra_service.pb.h>
#include <ytlib/hydra/hydra_manager.pb.h>

#include <server/misc/snapshot_builder_detail.h>

#include <util/random/random.h>

namespace NYT {
namespace NHydra {

using namespace NConcurrency;
using namespace NElection;
using namespace NRpc;

////////////////////////////////////////////////////////////////////////////////

class TDecoratedAutomaton::TUserLockGuard
{
public:
    TUserLockGuard(TUserLockGuard&& other)
        : Automaton_(std::move(other.Automaton_))
    { }

    ~TUserLockGuard()
    {
        if (Automaton_) {
            Automaton_->ReleaseUserLock();
        }
    }

    explicit operator bool()
    {
        return static_cast<bool>(Automaton_);
    }

    static TUserLockGuard TryAcquire(TDecoratedAutomatonPtr automaton)
    {
        return automaton->TryAcquireUserLock()
            ? TUserLockGuard(std::move(automaton))
            : TUserLockGuard();
    }

private:
    TUserLockGuard()
    { }

    explicit TUserLockGuard(TDecoratedAutomatonPtr automaton)
        : Automaton_(std::move(automaton))
    { }


    TDecoratedAutomatonPtr Automaton_;

};

////////////////////////////////////////////////////////////////////////////////

class TDecoratedAutomaton::TSystemLockGuard
{
public:
    TSystemLockGuard(TSystemLockGuard&& other)
        : Automaton_(std::move(other.Automaton_))
    { }

    ~TSystemLockGuard()
    {
        if (Automaton_) {
            Automaton_->ReleaseSystemLock();
        }
    }

    static TSystemLockGuard Acquire(TDecoratedAutomatonPtr automaton)
    {
        automaton->AcquireSystemLock();
        return TSystemLockGuard(std::move(automaton));
    }

private:
    explicit TSystemLockGuard(TDecoratedAutomatonPtr automaton)
        : Automaton_(std::move(automaton))
    { }


    TDecoratedAutomatonPtr Automaton_;

};

////////////////////////////////////////////////////////////////////////////////

class TDecoratedAutomaton::TGuardedUserInvoker
    : public IInvoker
{
public:
    TGuardedUserInvoker(
        TDecoratedAutomatonPtr decoratedAutomaton,
        IInvokerPtr underlyingInvoker)
        : Owner_(decoratedAutomaton)
        , UnderlyingInvoker_(underlyingInvoker)
    { }

    virtual void Invoke(const TClosure& callback) override
    {
        auto guard = TUserLockGuard::TryAcquire(Owner_);
        if (!guard)
            return;

        if (Owner_->GetState() != EPeerState::Leading &&
            Owner_->GetState() != EPeerState::Following)
            return;

        auto doInvoke = [] (IInvokerPtr invoker, const TClosure& callback) {
            TCurrentInvokerGuard guard(std::move(invoker));
            callback.Run();
        };

        UnderlyingInvoker_->Invoke(BIND(
            doInvoke,
            MakeStrong(this),
            callback));
    }

    virtual NConcurrency::TThreadId GetThreadId() const override
    {
        return UnderlyingInvoker_->GetThreadId();
    }

private:
    TDecoratedAutomatonPtr Owner_;
    IInvokerPtr UnderlyingInvoker_;

};

////////////////////////////////////////////////////////////////////////////////

class TDecoratedAutomaton::TSystemInvoker
    : public IInvoker
{
public:
    explicit TSystemInvoker(TDecoratedAutomaton* decoratedAutomaton)
        : Owner_(decoratedAutomaton)
    { }

    virtual void Invoke(const TClosure& callback) override
    {
        auto guard = TSystemLockGuard::Acquire(Owner_);

        auto doInvoke = [] (IInvokerPtr invoker, const TClosure& callback, TSystemLockGuard /*guard*/) {
            TCurrentInvokerGuard guard(std::move(invoker));
            callback.Run();
        };

        Owner_->AutomatonInvoker_->Invoke(BIND(
            doInvoke,
            MakeStrong(this),
            callback,
            Passed(std::move(guard))));
    }

    virtual NConcurrency::TThreadId GetThreadId() const override
    {
        return Owner_->AutomatonInvoker_->GetThreadId();
    }

private:
    TDecoratedAutomaton* Owner_;

};

////////////////////////////////////////////////////////////////////////////////

class TDecoratedAutomaton::TSnapshotBuilder
    : public TSnapshotBuilderBase
{
public:
    TSnapshotBuilder(
        TDecoratedAutomatonPtr owner,
        TPromise<TErrorOr<TRemoteSnapshotParams>> promise)
        : Owner_(owner)
        , Promise_(promise)
    {
        Logger = HydraLogger;
    }

    void Run()
    {
        VERIFY_THREAD_AFFINITY(Owner_->AutomatonThread);

        SnapshotId_ = Owner_->AutomatonVersion_.SegmentId + 1;
        SnapshotParams_.PrevRecordCount = Owner_->AutomatonVersion_.RecordId;

        TSnapshotBuilderBase::Run().Subscribe(
            BIND(&TSnapshotBuilder::OnFinished, MakeStrong(this))
                .Via(Owner_->ControlInvoker_));
    }

private:
    TDecoratedAutomatonPtr Owner_;
    TPromise<TErrorOr<TRemoteSnapshotParams>> Promise_;

    int SnapshotId_;
    TSnapshotCreateParams SnapshotParams_;


    virtual TDuration GetTimeout() const override
    {
        return Owner_->Config_->SnapshotTimeout;
    }

    virtual void Build() override
    {
        auto writer = Owner_->SnapshotStore_->CreateWriter(SnapshotId_, SnapshotParams_);
        Owner_->SaveSnapshot(writer->GetStream());
        writer->Close();
    }

    void OnFinished(TError error)
    {
        if (!error.IsOK()) {
            Promise_.Set(error);
            return;
        }

        auto paramsOrError = WaitFor(Owner_->SnapshotStore_->ConfirmSnapshot(SnapshotId_));
        if (!paramsOrError.IsOK()) {
            Promise_.Set(TError("Error confirming snapshot")
                << paramsOrError);
            return;
        }
        
        TRemoteSnapshotParams remoteParams;
        remoteParams.PeerId = Owner_->CellManager_->GetSelfId();
        remoteParams.SnapshotId = SnapshotId_;
        static_cast<TSnapshotParams&>(remoteParams) = paramsOrError.Value();
        Promise_.Set(remoteParams);
    }

};

////////////////////////////////////////////////////////////////////////////////

TDecoratedAutomaton::TDecoratedAutomaton(
    TDistributedHydraManagerConfigPtr config,
    TCellManagerPtr cellManager,
    IAutomatonPtr automaton,
    IInvokerPtr automatonInvoker,
    IInvokerPtr controlInvoker,
    ISnapshotStorePtr snapshotStore,
    IChangelogStorePtr changelogStore,
    NProfiling::TProfiler profiler)
    : State_(EPeerState::Stopped)
    , Config_(config)
    , CellManager_(cellManager)
    , Automaton_(automaton)
    , AutomatonInvoker_(automatonInvoker)
    , ControlInvoker_(controlInvoker)
    , UserLock_(0)
    , SystemLock_(0)
    , SystemInvoker_(New<TSystemInvoker>(this))
    , SnapshotStore_(snapshotStore)
    , ChangelogStore_(changelogStore)
    , MutationContext_(nullptr)
    , LoggedVersion_(InvalidVersion)
    , BatchCommitTimeCounter_("/batch_commit_time")
    , Logger(HydraLogger)
    , Profiler(profiler)
{
    YCHECK(Config_);
    YCHECK(CellManager_);
    YCHECK(Automaton_);
    YCHECK(ControlInvoker_);
    YCHECK(SnapshotStore_);
    YCHECK(ChangelogStore_);

    VERIFY_INVOKER_AFFINITY(AutomatonInvoker_, AutomatonThread);
    VERIFY_INVOKER_AFFINITY(ControlInvoker_, ControlThread);

    Logger.AddTag(Sprintf("CellGuid: %s",
        ~ToString(CellManager_->GetCellGuid())));

    ResponseKeeper_ = New<TResponseKeeper>(
        Config_->ResponseKeeper,
        Profiler);

    Reset();
}

void TDecoratedAutomaton::OnStartLeading()
{
    YCHECK(State_ == EPeerState::Stopped);
    State_ = EPeerState::LeaderRecovery;
}

void TDecoratedAutomaton::OnLeaderRecoveryComplete()
{
    YCHECK(State_ == EPeerState::LeaderRecovery);
    State_ = EPeerState::Leading;
    LastSnapshotTime_ = TInstant::Now();
}

void TDecoratedAutomaton::OnStopLeading()
{
    YCHECK(State_ == EPeerState::Leading || State_ == EPeerState::LeaderRecovery);
    State_ = EPeerState::Stopped;
    Reset();
}

void TDecoratedAutomaton::OnStartFollowing()
{
    YCHECK(State_ == EPeerState::Stopped);
    State_ = EPeerState::FollowerRecovery;
}

void TDecoratedAutomaton::OnFollowerRecoveryComplete()
{
    YCHECK(State_ == EPeerState::FollowerRecovery);
    State_ = EPeerState::Following;
    LastSnapshotTime_ = TInstant::Now();
}

void TDecoratedAutomaton::OnStopFollowing()
{
    YCHECK(State_ == EPeerState::Following || State_ == EPeerState::FollowerRecovery);
    State_ = EPeerState::Stopped;
    Reset();
}

IInvokerPtr TDecoratedAutomaton::CreateGuardedUserInvoker(IInvokerPtr underlyingInvoker)
{
    VERIFY_THREAD_AFFINITY_ANY();

    return New<TGuardedUserInvoker>(this, underlyingInvoker);
}

IInvokerPtr TDecoratedAutomaton::GetSystemInvoker()
{
    VERIFY_THREAD_AFFINITY_ANY();

    return SystemInvoker_;
}

IAutomatonPtr TDecoratedAutomaton::GetAutomaton()
{
    VERIFY_THREAD_AFFINITY_ANY();

    return Automaton_;
}

void TDecoratedAutomaton::Clear()
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    Automaton_->Clear();
    ResponseKeeper_->Clear();
    Reset();

    {
        TGuard<TSpinLock> guard(VersionSpinLock_);
        AutomatonVersion_ = TVersion();
    }
}

void TDecoratedAutomaton::SaveSnapshot(TOutputStream* output)
{
    YCHECK(output);
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    Automaton_->SaveSnapshot(output);
}

void TDecoratedAutomaton::LoadSnapshot(TVersion version, TInputStream* input)
{
    YCHECK(input);
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    LOG_INFO("Started loading snapshot %v to reach version %v",
        version.SegmentId + 1,
        version);

    Changelog_.Reset();

    PROFILE_TIMING ("/snapshot_load_time") {
        Automaton_->Clear();
        Automaton_->LoadSnapshot(input);
    }

    LOG_INFO("Finished loading snapshot");

    {
        TGuard<TSpinLock> guard(VersionSpinLock_);
        AutomatonVersion_ = version;
    }
}

void TDecoratedAutomaton::ApplyMutationDuringRecovery(const TSharedRef& recordData)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    NProto::TMutationHeader header;
    TSharedRef requestData;
    DeserializeMutationRecord(recordData, &header, &requestData);

    auto mutationVersion = TVersion(header.segment_id(), header.record_id());
    RotateAutomatonVersionIfNeeded(mutationVersion);

    TMutationRequest request(header.mutation_type(), requestData);

    TMutationContext context(
        AutomatonVersion_,
        request,
        TInstant(header.timestamp()),
        header.random_seed());

    DoApplyMutation(&context);
}

void TDecoratedAutomaton::LogLeaderMutation(
    const TMutationRequest& request,
    TSharedRef* recordData,
    TAsyncError* logResult,
    TPromise<TErrorOr<TMutationResponse>> commitPromise)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);
    YASSERT(recordData);
    YASSERT(logResult);
    YASSERT(commitPromise);

    TPendingMutation pendingMutation;
    pendingMutation.Version = LoggedVersion_;
    pendingMutation.Request = request;
    pendingMutation.Timestamp = TInstant::Now();
    pendingMutation.RandomSeed  = RandomNumber<ui64>();
    pendingMutation.CommitPromise = std::move(commitPromise);
    PendingMutations_.push(pendingMutation);

    MutationHeader_.Clear(); // don't forget to cleanup the pooled instance
    MutationHeader_.set_mutation_type(request.Type);
    if (request.Id != NullMutationId) {
        ToProto(MutationHeader_.mutable_mutation_id(), request.Id);
    }
    MutationHeader_.set_timestamp(pendingMutation.Timestamp.GetValue());
    MutationHeader_.set_random_seed(pendingMutation.RandomSeed);
    MutationHeader_.set_segment_id(LoggedVersion_.SegmentId);
    MutationHeader_.set_record_id(LoggedVersion_.RecordId);
    
    *recordData = SerializeMutationRecord(MutationHeader_, request.Data);

    LOG_DEBUG("Logging mutation at version %v",
        LoggedVersion_);

    *logResult = Changelog_->Append(*recordData);
    
    {
        TGuard<TSpinLock> guard(VersionSpinLock_);
        LoggedVersion_.Advance();
    }
}

void TDecoratedAutomaton::CancelPendingLeaderMutations(const TError& error)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    while (!PendingMutations_.empty()) {
        auto& pendingMutation = PendingMutations_.front();
        pendingMutation.CommitPromise.Set(error);
        PendingMutations_.pop();
    }
}

void TDecoratedAutomaton::LogFollowerMutation(
    const TSharedRef& recordData,
    TAsyncError* logResult)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    TSharedRef mutationData;
    DeserializeMutationRecord(recordData, &MutationHeader_, &mutationData);

    TPendingMutation pendingMutation;
    pendingMutation.Version = LoggedVersion_;
    pendingMutation.Request.Type = MutationHeader_.mutation_type();
    pendingMutation.Request.Data = mutationData;
    pendingMutation.Request.Id =
        MutationHeader_.has_mutation_id()
        ? FromProto<TMutationId>(MutationHeader_.mutation_id())
        : NullMutationId;
    pendingMutation.Timestamp = TInstant(MutationHeader_.timestamp());
    pendingMutation.RandomSeed  = MutationHeader_.random_seed();
    PendingMutations_.push(pendingMutation);

    LOG_DEBUG("Logging mutation at version %v",
        LoggedVersion_);

    auto actualLogResult = Changelog_->Append(recordData);
    if (logResult) {
        *logResult = std::move(actualLogResult);
    }

    {
        TGuard<TSpinLock> guard(VersionSpinLock_);
        LoggedVersion_.Advance();
    }
}

TFuture<TErrorOr<TRemoteSnapshotParams>> TDecoratedAutomaton::BuildSnapshot()
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    LastSnapshotTime_ = TInstant::Now();
    SnapshotVersion_ = LoggedVersion_;
    auto promise = SnapshotParamsPromise_ = NewPromise<TErrorOr<TRemoteSnapshotParams>>();

    LOG_INFO("Scheduled snapshot at version %v",
        LoggedVersion_);

    MaybeStartSnapshotBuilder();

    return promise;
}

TAsyncError TDecoratedAutomaton::RotateChangelog(TEpochContextPtr epochContext)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    LOG_INFO("Rotating changelog at version %v",
        LoggedVersion_);

    return BIND(&TDecoratedAutomaton::DoRotateChangelog, MakeStrong(this))
        .Guarded()
        .AsyncVia(epochContext->EpochUserAutomatonInvoker)
        .Run();
}

void TDecoratedAutomaton::DoRotateChangelog()
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    {
        auto result = WaitFor(Changelog_->Flush());
        THROW_ERROR_EXCEPTION_IF_FAILED(result);
    }
    
    if (Changelog_->IsSealed()) {
        LOG_WARNING("Changelog %v is already sealed",
            LoggedVersion_.SegmentId);
    } else {
        auto result = WaitFor(Changelog_->Seal(Changelog_->GetRecordCount()));
        THROW_ERROR_EXCEPTION_IF_FAILED(result);
    }

    NProto::TChangelogMeta meta;
    meta.set_prev_record_count(Changelog_->GetRecordCount());

    TSharedRef metaBlob;
    YCHECK(SerializeToProto(meta, &metaBlob));

    auto newChangelogOrError = WaitFor(ChangelogStore_->CreateChangelog(
        LoggedVersion_.SegmentId + 1,
        metaBlob));
    THROW_ERROR_EXCEPTION_IF_FAILED(newChangelogOrError);

    Changelog_ = newChangelogOrError.Value();

    {
        TGuard<TSpinLock> guard(VersionSpinLock_);
        LoggedVersion_.Rotate();
    }

    LOG_INFO("Changelog rotated");
}

void TDecoratedAutomaton::CommitMutations(TVersion version)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    LOG_DEBUG("Applying mutations upto version %v",
        version);

    PROFILE_AGGREGATED_TIMING (BatchCommitTimeCounter_) {
        while (!PendingMutations_.empty()) {
            auto& pendingMutation = PendingMutations_.front();
            if (pendingMutation.Version >= version)
                break;

            LOG_DEBUG("Applying mutation at version %v",
                pendingMutation.Version);

            RotateAutomatonVersionIfNeeded(pendingMutation.Version);

            TMutationContext context(
                AutomatonVersion_,
                pendingMutation.Request,
                pendingMutation.Timestamp,
                pendingMutation.RandomSeed);

            DoApplyMutation(&context);

            if (pendingMutation.CommitPromise) {
                pendingMutation.CommitPromise.Set(context.Response());
            }

            PendingMutations_.pop();

            MaybeStartSnapshotBuilder();
        }
    }
}

void TDecoratedAutomaton::RotateAutomatonVersionIfNeeded(TVersion mutationVersion)
{
    if (mutationVersion.SegmentId == AutomatonVersion_.SegmentId) {
        YCHECK(mutationVersion.RecordId == AutomatonVersion_.RecordId);
    } else {
        YCHECK(mutationVersion.SegmentId > AutomatonVersion_.SegmentId);
        YCHECK(mutationVersion.RecordId == 0);
        RotateAutomatonVersion(mutationVersion.SegmentId);
    }
}

void TDecoratedAutomaton::DoApplyMutation(TMutationContext* context)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    YASSERT(!MutationContext_);
    MutationContext_ = context;

    const auto& request = context->Request();
    const auto& response = context->Response();

    if (request.Action) {
        request.Action.Run(context);
    } else {
        Automaton_->ApplyMutation(context);
    }

    {
        TGuard<TSpinLock> guard(VersionSpinLock_);
        AutomatonVersion_.Advance();
    }

    if (context->Request().Id == NullMutationId || context->IsMutationSuppressed()) {
        ResponseKeeper_->RemoveExpiredResponses(context->GetTimestamp());
    } else {
        ResponseKeeper_->RegisterResponse(
            request.Id,
            response.Data,
            context->GetTimestamp());
    }

    MutationContext_ = nullptr;
}

void TDecoratedAutomaton::RegisterKeptResponse(
    const TMutationId& mutationId,
    const TMutationResponse& response)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);
    YASSERT(MutationContext_);

    ResponseKeeper_->RegisterResponse(
        mutationId,
        response.Data,
        MutationContext_->GetTimestamp());
}

TNullable<TMutationResponse> TDecoratedAutomaton::FindKeptResponse(const TMutationId& mutationId)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    auto data = ResponseKeeper_->FindResponse(mutationId);
    if (!data) {
        return Null;
    }

    return TMutationResponse(std::move(data), true);
}

TVersion TDecoratedAutomaton::GetLoggedVersion() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    TGuard<TSpinLock> guard(VersionSpinLock_);
    return LoggedVersion_;
}

void TDecoratedAutomaton::SetChangelog(IChangelogPtr changelog)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    Changelog_ = changelog;
}

void TDecoratedAutomaton::SetLoggedVersion(TVersion version)
{
    VERIFY_THREAD_AFFINITY_ANY();

    TGuard<TSpinLock> guard(VersionSpinLock_);
    LoggedVersion_ = version;
}

i64 TDecoratedAutomaton::GetLoggedDataSize() const
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    return Changelog_->GetDataSize();
}

TInstant TDecoratedAutomaton::GetLastSnapshotTime() const
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    return LastSnapshotTime_;
}

TVersion TDecoratedAutomaton::GetAutomatonVersion() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    TGuard<TSpinLock> guard(VersionSpinLock_);
    return AutomatonVersion_;
}


void TDecoratedAutomaton::RotateAutomatonVersion(int segmentId)
{
    VERIFY_THREAD_AFFINITY_ANY();

    {
        TGuard<TSpinLock> guard(VersionSpinLock_);
        YCHECK(AutomatonVersion_.SegmentId < segmentId);
        AutomatonVersion_ = TVersion(segmentId, 0);
    }

    LOG_INFO("Automaton version is rotated to %v",
        AutomatonVersion_);
}

TMutationContext* TDecoratedAutomaton::GetMutationContext()
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    return MutationContext_;
}

bool TDecoratedAutomaton::TryAcquireUserLock()
{
    if (SystemLock_.load() != 0) {
        return false;
    }
    ++UserLock_;
    if (SystemLock_.load() != 0) {
        --UserLock_;
        return false;
    }
    return true;
}

void TDecoratedAutomaton::ReleaseUserLock()
{
    --UserLock_;
}

void TDecoratedAutomaton::AcquireSystemLock()
{
    int result = ++SystemLock_;
    while (UserLock_.load() != 0) {
        SpinLockPause();
    }
    LOG_DEBUG("System lock acquired (Lock: %v)",
        result);
}

void TDecoratedAutomaton::ReleaseSystemLock()
{
    int result = --SystemLock_;
    LOG_DEBUG("System lock released (Lock: %v)",
        result);
}

void TDecoratedAutomaton::Reset()
{
    PendingMutations_.clear();
    Changelog_.Reset();
    SnapshotVersion_ = TVersion();
    SnapshotParamsPromise_.Reset();
}

void TDecoratedAutomaton::MaybeStartSnapshotBuilder()
{
    if (AutomatonVersion_ != SnapshotVersion_)
        return;

    auto builder = New<TSnapshotBuilder>(this, SnapshotParamsPromise_);
    builder->Run();

    SnapshotParamsPromise_.Reset();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NHydra
} // namespace NYT
