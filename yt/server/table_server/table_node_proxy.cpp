#include "stdafx.h"
#include "table_node_proxy.h"
#include "table_node.h"
#include "private.h"

#include <core/misc/string.h>
#include <core/misc/serialize.h>

#include <core/erasure/codec.h>

#include <core/ytree/tree_builder.h>
#include <core/ytree/ephemeral_node_factory.h>

#include <core/ypath/token.h>

#include <ytlib/table_client/table_ypath_proxy.h>

#include <ytlib/new_table_client/schema.h>

#include <ytlib/chunk_client/read_limit.h>

#include <server/node_tracker_server/node_directory_builder.h>

#include <server/chunk_server/chunk.h>
#include <server/chunk_server/chunk_list.h>
#include <server/chunk_server/chunk_owner_node_proxy.h>

#include <server/tablet_server/tablet_manager.h>
#include <server/tablet_server/tablet.h>
#include <server/tablet_server/tablet_cell.h>

#include <server/cell_master/bootstrap.h>

namespace NYT {
namespace NTableServer {

using namespace NChunkServer;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NCypressServer;
using namespace NRpc;
using namespace NYTree;
using namespace NYson;
using namespace NTableClient;
using namespace NVersionedTableClient;
using namespace NTransactionServer;
using namespace NTabletServer;
using namespace NNodeTrackerServer;

using NChunkClient::TChannel;
using NChunkClient::TReadLimit;

////////////////////////////////////////////////////////////////////////////////

class TTableNodeProxy
    : public TCypressNodeProxyBase<TChunkOwnerNodeProxy, IEntityNode, TTableNode>
{
public:
    TTableNodeProxy(
        INodeTypeHandlerPtr typeHandler,
        NCellMaster::TBootstrap* bootstrap,
        TTransaction* transaction,
        TTableNode* trunkNode)
    : TBase(
        typeHandler,
        bootstrap,
        transaction,
        trunkNode)
    { }

private:
    typedef TCypressNodeProxyBase<TChunkOwnerNodeProxy, IEntityNode, TTableNode> TBase;

    virtual NLog::TLogger CreateLogger() const override
    {
        return TableServerLogger;
    }


    virtual void ListSystemAttributes(std::vector<TAttributeInfo>* attributes) override
    {
        const auto* node = GetThisTypedImpl();

        attributes->push_back("row_count");
        attributes->push_back("sorted");
        attributes->push_back("key_columns");
        attributes->push_back(TAttributeInfo("sorted_by", node->GetSorted()));
        attributes->push_back(TAttributeInfo("tablets", true, true));
        TBase::ListSystemAttributes(attributes);
    }

    virtual bool GetSystemAttribute(const Stroka& key, IYsonConsumer* consumer) override
    {
        const auto* node = GetThisTypedImpl();
        const auto* chunkList = node->GetChunkList();
        const auto& statistics = chunkList->Statistics();

        if (key == "row_count") {
            BuildYsonFluently(consumer)
                .Value(statistics.RowCount);
            return true;
        }

        if (key == "sorted") {
            BuildYsonFluently(consumer)
                .Value(node->GetSorted());
            return true;
        }

        if (key == "key_columns") {
            BuildYsonFluently(consumer)
                .Value(node->KeyColumns());
            return true;
        }

        if (node->GetSorted()) {
            if (key == "sorted_by") {
                BuildYsonFluently(consumer)
                    .Value(node->KeyColumns());
                return true;
            }
        }

        if (key == "tablets") {
            BuildYsonFluently(consumer)
                .DoListFor(node->Tablets(), [] (TFluentList fluent, TTablet* tablet) {
                auto* cell = tablet->GetCell();
                fluent
                    .Item().BeginMap()
                    .Item("tablet_id").Value(tablet->GetId())
                    .Item("state").Value(tablet->GetState())
                    .Item("pivot_key").Value(tablet->GetPivotKey())
                    .DoIf(cell, [&] (TFluentMap fluent) {
                    fluent
                        .Item("cell_id").Value(cell->GetId());
                })
                    .EndMap();
            });
            return true;
        }

        return TBase::GetSystemAttribute(key, consumer);
    }

    bool SetSystemAttribute(const Stroka& key, const TYsonString& value) override
    {
        if (key == "key_columns") {
            ValidateNoTransaction();

            auto* node = LockThisTypedImpl();
            auto* chunkList = node->GetChunkList();
            if (!chunkList->Children().empty() ||
                !chunkList->Parents().empty() ||
                !node->Tablets().empty())
            {
                THROW_ERROR_EXCEPTION("Operation is not supported");
            }

            node->KeyColumns() = ConvertTo<TKeyColumns>(value);
            node->SetSorted(!node->KeyColumns().empty());
            return true;
        }

        return TBase::SetSystemAttribute(key, value);
    }
    
    virtual void ValidateUserAttributeUpdate(
        const Stroka& key,
        const TNullable<TYsonString>& oldValue,
        const TNullable<TYsonString>& newValue) override
    {
        UNUSED(oldValue);

        if (key == "channels") {
            if (!newValue) {
                ThrowCannotRemoveAttribute(key);
            }
            ConvertTo<TChannels>(newValue.Get());
            return;
        }

        if (key == "schema") {
            if (!newValue) {
                ThrowCannotRemoveAttribute(key);
            }
            ConvertTo<TTableSchema>(newValue.Get());
            return;
        }

        TBase::ValidateUserAttributeUpdate(key, oldValue, newValue);
    }

    virtual void ValidateFetchParameters(
        const TChannel& channel,
        const TReadLimit& upperLimit,
        const TReadLimit& lowerLimit) override
    {
        TChunkOwnerNodeProxy::ValidateFetchParameters(
            channel,
            upperLimit,
            lowerLimit);

        const auto* node = GetThisTypedImpl();
        if ((upperLimit.HasKey() || lowerLimit.HasKey()) && !node->GetSorted()) {
            THROW_ERROR_EXCEPTION("Cannot fetch a range of an unsorted table");
        }

        if (upperLimit.HasOffset() || lowerLimit.HasOffset()) {
            THROW_ERROR_EXCEPTION("Offset selectors are not supported for tables");
        }
    }


    virtual void Clear() override
    {
        TChunkOwnerNodeProxy::Clear();

        auto* node = GetThisTypedImpl();
        node->KeyColumns().clear();
        node->SetSorted(false);
    }

    virtual NCypressClient::ELockMode GetLockMode(EUpdateMode updateMode) override
    {
        return updateMode == EUpdateMode::Append
            ? ELockMode::Shared
            : ELockMode::Exclusive;
    }


    virtual bool DoInvoke(IServiceContextPtr context) override
    {
        DISPATCH_YPATH_SERVICE_METHOD(SetSorted);
        DISPATCH_YPATH_SERVICE_METHOD(Mount);
        DISPATCH_YPATH_SERVICE_METHOD(Unmount);
        DISPATCH_YPATH_SERVICE_METHOD(Reshard);
        DISPATCH_YPATH_SERVICE_METHOD(GetMountInfo);
        return TBase::DoInvoke(context);
    }


    virtual void ValidateFetch() override
    {
        TBase::ValidateFetch();

        const auto* node = GetThisTypedImpl();
        if (!node->Tablets().empty()) {
            THROW_ERROR_EXCEPTION("Cannot fetch a table with tablets");
        }
    }

    virtual void ValidatePrepareForUpdate() override
    {
        TBase::ValidatePrepareForUpdate();

        const auto* trunkNode = GetThisTypedImpl()->GetTrunkNode();
        if (!trunkNode->Tablets().empty()) {
            THROW_ERROR_EXCEPTION("Cannot write into a table with tablets");
        }
    }


    DECLARE_YPATH_SERVICE_METHOD(NTableClient::NProto, SetSorted)
    {
        DeclareMutating();

        auto keyColumns = FromProto<Stroka>(request->key_columns());
        context->SetRequestInfo("KeyColumns: %s",
            ~ConvertToYsonString(keyColumns, EYsonFormat::Text).Data());

        ValidatePermission(EPermissionCheckScope::This, EPermission::Write);

        auto* node = LockThisTypedImpl();

        if (node->GetUpdateMode() != EUpdateMode::Overwrite) {
            THROW_ERROR_EXCEPTION("Table must be in \"overwrite\" mode");
        }

        node->KeyColumns() = keyColumns;
        node->SetSorted(true);

        SetModified();

        context->Reply();
    }

    DECLARE_YPATH_SERVICE_METHOD(NTableClient::NProto, Mount)
    {
        DeclareMutating();

        int firstTabletIndex = request->first_tablet_index();
        int lastTabletIndex = request->first_tablet_index();
        context->SetRequestInfo("FirstTabletIndex: %d, LastTabletIndex: %d",
            firstTabletIndex,
            lastTabletIndex);

        ValidateNoTransaction();

        auto* impl = LockThisTypedImpl();

        auto tabletManager = Bootstrap->GetTabletManager();
        tabletManager->MountTable(
            impl,
            firstTabletIndex,
            lastTabletIndex);

        context->Reply();
    }

    DECLARE_YPATH_SERVICE_METHOD(NTableClient::NProto, Unmount)
    {
        DeclareMutating();

        int firstTabletIndex = request->first_tablet_index();
        int lastTabletIndex = request->first_tablet_index();
        bool force = request->force();
        context->SetRequestInfo("FirstTabletIndex: %d, LastTabletIndex: %d, Force: %s",
            firstTabletIndex,
            lastTabletIndex,
            ~FormatBool(force));

        ValidateNoTransaction();

        auto* impl = LockThisTypedImpl();

        auto tabletManager = Bootstrap->GetTabletManager();
        tabletManager->UnmountTable(
            impl,
            force,
            firstTabletIndex,
            lastTabletIndex);

        context->Reply();
    }

    DECLARE_YPATH_SERVICE_METHOD(NTableClient::NProto, Reshard)
    {
        DeclareMutating();

        int firstTabletIndex = request->first_tablet_index();
        int lastTabletIndex = request->last_tablet_index();
        auto pivotKeys = FromProto<NVersionedTableClient::TOwningKey>(request->pivot_keys());
        context->SetRequestInfo("FirstTabletIndex: %d, LastTabletIndex: %d, PivotKeyCount: %d",
            firstTabletIndex,
            lastTabletIndex,
            static_cast<int>(pivotKeys.size()));

        ValidateNoTransaction();

        auto* impl = LockThisTypedImpl();

        auto tabletManager = Bootstrap->GetTabletManager();
        tabletManager->ReshardTable(
            impl,
            firstTabletIndex,
            lastTabletIndex,
            pivotKeys);

        context->Reply();
    }

    DECLARE_YPATH_SERVICE_METHOD(NTableClient::NProto, GetMountInfo)
    {
        DeclareNonMutating();

        context->SetRequestInfo("");

        ValidateNoTransaction();

        auto* node = GetThisTypedImpl();

        ToProto(response->mutable_table_id(), node->GetId());
        ToProto(response->mutable_key_columns()->mutable_names(), node->KeyColumns());
        response->set_sorted(node->GetSorted());

        auto tabletManager = Bootstrap->GetTabletManager();
        auto schema = tabletManager->GetTableSchema(node);
        ToProto(response->mutable_schema(), schema);

        TNodeDirectoryBuilder builder(response->mutable_node_directory());

        for (auto* tablet : node->Tablets()) {
            auto* cell = tablet->GetCell();
            auto* protoTablet = response->add_tablets();
            ToProto(protoTablet->mutable_tablet_id(), tablet->GetId());
            protoTablet->set_state(tablet->GetState());
            ToProto(protoTablet->mutable_pivot_key(), tablet->GetPivotKey());
            if (cell) {
                ToProto(protoTablet->mutable_cell_id(), cell->GetId());
                protoTablet->mutable_cell_config()->CopyFrom(cell->Config());

                for (const auto& peer : cell->Peers()) {
                    if (peer.Node) {
                        const auto& slot = peer.Node->TabletSlots()[peer.SlotIndex];
                        if (slot.PeerState == EPeerState::Leading) {
                            builder.Add(peer.Node);
                            protoTablet->add_replica_node_ids(peer.Node->GetId());
                        }
                    }
                }
            }
        }

        context->Reply();
    }

};

////////////////////////////////////////////////////////////////////////////////

ICypressNodeProxyPtr CreateTableNodeProxy(
    INodeTypeHandlerPtr typeHandler,
    NCellMaster::TBootstrap* bootstrap,
    TTransaction* transaction,
    TTableNode* trunkNode)
{
    return New<TTableNodeProxy>(
        typeHandler,
        bootstrap,
        transaction,
        trunkNode);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableServer
} // namespace NYT

