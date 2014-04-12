#include "stdafx.h"
#include "store_flusher.h"
#include "private.h"
#include "config.h"
#include "dynamic_memory_store.h"
#include "chunk_store.h"
#include "tablet.h"
#include "tablet_slot.h"
#include "tablet_manager.h"
#include "tablet_slot_manager.h"
#include "store_manager.h"

#include <core/misc/address.h>

#include <core/concurrency/action_queue.h>
#include <core/concurrency/scheduler.h>
#include <core/concurrency/async_semaphore.h>

#include <core/ytree/attribute_helpers.h>

#include <core/logging/tagged_logger.h>

#include <ytlib/transaction_client/transaction_manager.h>

#include <ytlib/object_client/object_service_proxy.h>
#include <ytlib/object_client/master_ypath_proxy.h>
#include <ytlib/object_client/helpers.h>

#include <ytlib/chunk_client/async_writer.h>
#include <ytlib/chunk_client/replication_writer.h>
#include <ytlib/chunk_client/chunk_ypath_proxy.h>

#include <ytlib/new_table_client/unversioned_row.h>
#include <ytlib/new_table_client/versioned_row.h>
#include <ytlib/new_table_client/versioned_reader.h>
#include <ytlib/new_table_client/versioned_writer.h>
#include <ytlib/new_table_client/versioned_chunk_writer.h>

#include <ytlib/node_tracker_client/node_directory.h>

#include <ytlib/api/client.h>
#include <ytlib/api/transaction.h>

#include <server/tablet_server/tablet_manager.pb.h>

#include <server/tablet_node/tablet_manager.pb.h>

#include <server/hydra/mutation.h>

#include <server/hive/hive_manager.h>

#include <server/cell_node/bootstrap.h>
#include <server/cell_node/config.h>

namespace NYT {
namespace NTabletNode {

using namespace NConcurrency;
using namespace NYTree;
using namespace NApi;
using namespace NVersionedTableClient;
using namespace NVersionedTableClient::NProto;
using namespace NNodeTrackerClient;
using namespace NObjectClient;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NHydra;
using namespace NTabletServer::NProto;
using namespace NTabletNode::NProto;

////////////////////////////////////////////////////////////////////////////////

static auto& Logger = TabletNodeLogger;
static const size_t MaxRowsPerRead = 1024;

////////////////////////////////////////////////////////////////////////////////

class TStoreFlusher;
typedef TIntrusivePtr<TStoreFlusher> TStoreFlusherPtr;

class TStoreFlusher
    : public TRefCounted
{
public:
    TStoreFlusher(
        TStoreFlusherConfigPtr config,
        NCellNode::TBootstrap* bootstrap)
        : Config_(config)
        , Bootstrap_(bootstrap)
        , ThreadPool_(New<TThreadPool>(Config_->ThreadPoolSize, "StoreFlush"))
        , Semaphore_(Config_->MaxConcurrentFlushes)
    { }

    void Start()
    {
        auto tabletSlotManager = Bootstrap_->GetTabletSlotManager();
        tabletSlotManager->SubscribeBeginSlotScan(BIND(&TStoreFlusher::BeginSlotScan, MakeStrong(this)));
        tabletSlotManager->SubscribeScanSlot(BIND(&TStoreFlusher::ScanSlot, MakeStrong(this)));
        tabletSlotManager->SubscribeEndSlotScan(BIND(&TStoreFlusher::EndSlotScan, MakeStrong(this)));
    }

private:
    TStoreFlusherConfigPtr Config_;
    NCellNode::TBootstrap* Bootstrap_;

    TThreadPoolPtr ThreadPool_;
    TAsyncSemaphore Semaphore_;

    struct TForcedRotationCandidate
    {
        i64 MemoryUsage;
        TTabletId TabletId;
    };

    TSpinLock SpinLock_;
    i64 PassiveMemoryUsage_;
    std::vector<TForcedRotationCandidate> ForcedRotationCandidates_;


    void BeginSlotScan()
    {
        // NB: No locking is needed.
        PassiveMemoryUsage_ = 0;
        ForcedRotationCandidates_.clear();
    }

    void ScanSlot(TTabletSlotPtr slot)
    {
        if (slot->GetAutomatonState() != EPeerState::Leading)
            return;

        auto tabletManager = slot->GetTabletManager();
        auto tablets = tabletManager->Tablets().GetValues();
        for (auto* tablet : tablets) {
            ScanTablet(tablet);
        }
    }

    void EndSlotScan()
    {
        // NB: No locking is needed.
        // Order candidates by increasing memory usage.
        std::sort(
            ForcedRotationCandidates_. begin(),
            ForcedRotationCandidates_.end(),
            [] (const TForcedRotationCandidate& lhs, const TForcedRotationCandidate& rhs) {
                return lhs.MemoryUsage < rhs.MemoryUsage;
            });
        
        // Pick the heaviest candidates until no more rotations are needed.
        auto tabletSlotManager = Bootstrap_->GetTabletSlotManager();
        while (tabletSlotManager->IsRotationForced(PassiveMemoryUsage_) &&
               !ForcedRotationCandidates_.empty())
        {
            auto candidate = ForcedRotationCandidates_.back();
            ForcedRotationCandidates_.pop_back();

            auto tabletId = candidate.TabletId;
            auto tabletDesriptor = tabletSlotManager->FindTabletDescriptor(tabletId);
            if (!tabletDesriptor)
                continue;

            auto slot = tabletDesriptor->Slot;
            auto invoker = slot->GetGuardedAutomatonInvoker(EAutomatonThreadQueue::Read);
            if (!invoker)
                continue;

            LOG_INFO("Scheduling store rotation due to memory pressure condition (TabletId: %s, "
                "TotalMemoryUsage: %" PRId64 ", TabletMemoryUsage: %" PRId64 ", "
                "MemoryLimit: %" PRId64 ")",
                ~ToString(candidate.TabletId),
                Bootstrap_->GetMemoryUsageTracker().GetUsed(NCellNode::EMemoryConsumer::Tablet),
                candidate.MemoryUsage,
                Bootstrap_->GetConfig()->TabletNode->MemoryLimit);

            invoker->Invoke(BIND([slot, tabletId] () {
                auto tabletManager = slot->GetTabletManager();
                auto* tablet = tabletManager->FindTablet(tabletId);
                if (!tablet)
                    return;
                tabletManager->ScheduleStoreRotation(tablet);
            }));

            PassiveMemoryUsage_ += candidate.MemoryUsage;
        }
    }

    void ScanTablet(TTablet* tablet)
    {
        auto slot = tablet->GetSlot();
        auto tabletManager = slot->GetTabletManager();
        auto storeManager = tablet->GetStoreManager();

        if (storeManager->IsPeriodicRotationNeeded()) {
            LOG_INFO("Scheduling periodic store rotation (TabletId: %s)",
                ~ToString(tablet->GetId()));
            tabletManager->ScheduleStoreRotation(tablet);
        }

        if (storeManager->IsOverflowRotationNeeded()) {
            LOG_INFO("Scheduling store rotation due to overflow (TabletId: %s)",
                ~ToString(tablet->GetId()));
            tabletManager->ScheduleStoreRotation(tablet);
        }

        for (const auto& pair : tablet->Stores()) {
            const auto& store = pair.second;
            ScanStore(tablet, store);
            if (store->GetState() == EStoreState::PassiveDynamic) {
                TGuard<TSpinLock> guard(SpinLock_);
                auto dynamicStore = store->AsDynamicMemory();
                PassiveMemoryUsage_ += dynamicStore->GetMemoryUsage();
            }
        }

        {
            TGuard<TSpinLock> guard(SpinLock_);
            auto storeManager = tablet->GetStoreManager();
            if (storeManager->IsForcedRotationPossible()) {
                const auto& store = tablet->GetActiveStore();
                i64 memoryUsage = store->GetMemoryUsage();
                if (storeManager->IsRotationScheduled()) {
                    PassiveMemoryUsage_ += memoryUsage;
                } else {
                    TForcedRotationCandidate candidate;
                    candidate.TabletId = tablet->GetId();
                    candidate.MemoryUsage = memoryUsage;
                    ForcedRotationCandidates_.push_back(candidate);
                }
            }
        }
    }

    void ScanStore(TTablet* tablet, const IStorePtr& store)
    {
        if (store->GetState() != EStoreState::PassiveDynamic)
            return;

        auto guard = TAsyncSemaphoreGuard::TryAcquire(&Semaphore_);
        if (!guard)
            return;

        store->SetState(EStoreState::Flushing);

        tablet->GetEpochAutomatonInvoker(EAutomatonThreadQueue::Write)->Invoke(BIND(
            &TStoreFlusher::FlushStore,
            MakeStrong(this),
            Passed(std::move(guard)),
            tablet,
            store));
    }


    void FlushStore(
        TAsyncSemaphoreGuard /*guard*/,
        TTablet* tablet,
        IStorePtr store)
    {
        YCHECK(store->GetState() == EStoreState::Flushing);

        NLog::TTaggedLogger Logger(TabletNodeLogger);
        Logger.AddTag(Sprintf("TabletId: %s, StoreId: %s",
            ~ToString(tablet->GetId()),
            ~ToString(store->GetId())));

        auto* slot = tablet->GetSlot();
        auto hydraManager = slot->GetHydraManager();
        auto tabletManager = slot->GetTabletManager();

        auto automatonInvoker = tablet->GetEpochAutomatonInvoker(EAutomatonThreadQueue::Write);
        auto poolInvoker = ThreadPool_->GetInvoker();

        TObjectServiceProxy proxy(Bootstrap_->GetMasterClient()->GetMasterChannel());

        try {
            LOG_INFO("Store flush started");

            TReqCommitTabletStoresUpdate updateStoresRequest;
            ToProto(updateStoresRequest.mutable_tablet_id(), tablet->GetId());
            {
                auto* descriptor = updateStoresRequest.add_stores_to_remove();
                ToProto(descriptor->mutable_store_id(), store->GetId());
            }

            auto reader = store->CreateReader(
                MinKey(),
                MaxKey(),
                AllCommittedTimestamp,
                TColumnFilter());
        
            // NB: Memory store reader is always synchronous.
            YCHECK(reader->Open().Get().IsOK());

            SwitchTo(poolInvoker);

            ITransactionPtr transaction;
            {
                LOG_INFO("Creating store flush transaction");
                TTransactionStartOptions options;
                options.AutoAbort = false;
                auto attributes = CreateEphemeralAttributes();
                attributes->Set("title", Sprintf("Flushing store %s, tablet %s",
                    ~ToString(store->GetId()),
                    ~ToString(tablet->GetId())));
                options.Attributes = attributes.get();
                auto transactionOrError = WaitFor(Bootstrap_->GetMasterClient()->StartTransaction(
                    NTransactionClient::ETransactionType::Master,
                    options));
                THROW_ERROR_EXCEPTION_IF_FAILED(transactionOrError);
                transaction = transactionOrError.Value();
            }

            auto writer = CreateVersionedMultiChunkWriter(
                Config_->Writer,
                tablet->GetWriterOptions(),
                tablet->Schema(),
                tablet->KeyColumns(),
                Bootstrap_->GetMasterClient()->GetMasterChannel(),
                transaction->GetId());

            {
                auto result = WaitFor(writer->Open());
                THROW_ERROR_EXCEPTION_IF_FAILED(result);
            }
        
            std::vector<TVersionedRow> rows;
            rows.reserve(MaxRowsPerRead);

            while (true) {
                // NB: Memory store reader is always synchronous.
                reader->Read(&rows);
                if (rows.empty())
                    break;
                if (!writer->Write(rows)) {
                    auto result = WaitFor(writer->GetReadyEvent());
                    THROW_ERROR_EXCEPTION_IF_FAILED(result);
                }
            }

            {
                auto result = WaitFor(writer->Close());
                THROW_ERROR_EXCEPTION_IF_FAILED(result);
            }

            for (const auto& chunkSpec : writer->GetWrittenChunks()) {
                auto* descriptor = updateStoresRequest.add_stores_to_add();
                descriptor->mutable_store_id()->CopyFrom(chunkSpec.chunk_id());
                descriptor->mutable_chunk_meta()->CopyFrom(chunkSpec.chunk_meta());
            }

            SwitchTo(automatonInvoker);

            CreateMutation(slot->GetHydraManager(), updateStoresRequest)
                ->Commit();

            LOG_INFO("Store flush completed");

            // Just abandon the transaction, hopefully it won't expire before the chunk is attached.
        } catch (const std::exception& ex) {
            LOG_ERROR(ex, "Error flushing tablet store, backing off");
        
            SwitchTo(automatonInvoker);

            YCHECK(store->GetState() == EStoreState::Flushing);
            tabletManager->BackoffStore(store, EStoreState::FlushFailed);
        }
    }

};

////////////////////////////////////////////////////////////////////////////////

void StartStoreFlusher(
    TStoreFlusherConfigPtr config,
    NCellNode::TBootstrap* bootstrap)
{
    New<TStoreFlusher>(config, bootstrap)->Start();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
