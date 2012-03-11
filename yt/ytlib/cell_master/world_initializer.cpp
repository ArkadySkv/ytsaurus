#include "stdafx.h"
#include "world_initializer.h"
#include "config.h"

#include <ytlib/actions/action_util.h>
#include <ytlib/misc/periodic_invoker.h>
#include <ytlib/ytree/ypath_proxy.h>
#include <ytlib/ytree/ypath_client.h>
#include <ytlib/cypress/cypress_manager.h>
#include <ytlib/cypress/cypress_ypath_proxy.h>
#include <ytlib/cypress/cypress_service_proxy.h>
#include <ytlib/transaction_server/transaction_ypath_proxy.h>
#include <ytlib/cell_master/bootstrap.h>
#include <ytlib/logging/log.h>

namespace NYT {
namespace NCellMaster {

using namespace NMetaState;
using namespace NYTree;
using namespace NCypress;
using namespace NTransactionServer;
using namespace NObjectServer;

////////////////////////////////////////////////////////////////////////////////

static TDuration CheckPeriod = TDuration::Seconds(1);
static NLog::TLogger Logger("Cypress");

////////////////////////////////////////////////////////////////////////////////

class TWorldInitializer::TImpl
    : public TRefCounted
{
public:
    TImpl(TBootstrap* bootstrap)
        : Bootstrap(bootstrap)
    {
        YASSERT(bootstrap);

        PeriodicInvoker = New<TPeriodicInvoker>(
            FromMethod(&TImpl::OnCheck, MakeWeak(this))
            ->Via(bootstrap->GetStateInvoker()),
            CheckPeriod);
        PeriodicInvoker->Start();
    }

    bool IsInitialized() const
    {
        // 1 means just the root.
        // TODO(babenko): fixme
        return Bootstrap->GetCypressManager()->GetNodeCount() > 1;
    }

private:
    TBootstrap* Bootstrap;
    TPeriodicInvoker::TPtr PeriodicInvoker;

    void OnCheck()
    {
        if (IsInitialized()) {
            PeriodicInvoker->Stop();
        } else if (CanInitialize()) {
            Initialize();
            PeriodicInvoker->Stop();
        }
    }

    bool CanInitialize() const
    {
        auto metaStateManager = Bootstrap->GetMetaStateManager();
        return
            metaStateManager->GetStateStatus() == EPeerStatus::Leading &&
            metaStateManager->HasActiveQuorum();
    }

    void Initialize()
    {
        LOG_INFO("World initialization started");

        try {
            auto service = Bootstrap->GetObjectManager()->GetRootService();

            auto transactionId = NullTransactionId; //StartTransaction();

            SyncYPathSet(
                service,
                WithTransaction("/sys/scheduler", transactionId),
                "{}");

            SyncYPathCreate(
                service,
                WithTransaction("/sys/holders", transactionId),
                EObjectType::HolderMap);

            FOREACH (const auto& address, Bootstrap->GetConfig()->MetaState->Cell->Addresses) {
                SyncYPathSet(
                    service,
                    WithTransaction(CombineYPaths("/sys/masters", address), transactionId),
                    "{}");

                SyncYPathCreate(
                    service,
                    WithTransaction(CombineYPaths("/sys/masters", address, "orchid"), transactionId),
                    EObjectType::Orchid,
                    BuildYsonFluently()
                        .BeginMap()
                            .Item("remote_address").Scalar(address)
                        .EndMap());
            }

            SyncYPathCreate(
                service,
                WithTransaction("/sys/chunks", transactionId),
                EObjectType::ChunkMap);

            SyncYPathCreate(
                service,
                WithTransaction("/sys/lost_chunks", transactionId),
                EObjectType::LostChunkMap);

            SyncYPathCreate(
                service,
                WithTransaction("/sys/overreplicated_chunks", transactionId),
                EObjectType::OverreplicatedChunkMap);

            SyncYPathCreate(
                service,
                WithTransaction("/sys/underreplicated_chunks", transactionId),
                EObjectType::UnderreplicatedChunkMap);

            SyncYPathCreate(
                service,
                WithTransaction("/sys/chunk_lists", transactionId),
                EObjectType::ChunkListMap);

            SyncYPathCreate(
                service,
                WithTransaction("/sys/nodes", transactionId),
                EObjectType::NodeMap);

            SyncYPathCreate(
                service,
                WithTransaction("/sys/locks", transactionId),
                EObjectType::LockMap);

            SyncYPathCreate(
                service,
                WithTransaction("/sys/transactions", transactionId),
                EObjectType::TransactionMap);

            //CommitTransaction(transactionId);
        } catch (const std::exception& ex) {
            LOG_FATAL("World initialization failed\n%s", ex.what());
        }

        LOG_INFO("World initialization completed");
    }

    TTransactionId StartTransaction()
    {
        auto service = Bootstrap->GetObjectManager()->GetRootService();
        auto req = TTransactionYPathProxy::CreateObject(RootTransactionPath);
        req->set_type(EObjectType::Transaction);
        auto rsp = SyncExecuteVerb(service, ~req);
        return TTransactionId::FromProto(rsp->object_id());
    }

    void CommitTransaction(const TTransactionId& transactionId)
    {
        auto service = Bootstrap->GetObjectManager()->GetRootService();
        auto req = TTransactionYPathProxy::Commit(FromObjectId(transactionId));
        SyncExecuteVerb(service, ~req);
    }

    // TODO(babenko): consider moving somewhere
    static TObjectId SyncYPathCreate(IYPathService* service, const TYPath& path, EObjectType type, const TYson& manifest = "{}")
    {
        auto req = TCypressYPathProxy::Create(path);
        req->set_type(type);
        req->set_manifest(manifest);
        auto rsp = SyncExecuteVerb(service, ~req);
        return TObjectId::FromProto(rsp->object_id());
    }
};

////////////////////////////////////////////////////////////////////////////////

TWorldInitializer::TWorldInitializer(TBootstrap* bootstrap)
    : Impl(New<TImpl>(bootstrap))
{ }

bool TWorldInitializer::IsInitialized() const
{
    return Impl->IsInitialized();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NCellMaster
} // namespace NYT

