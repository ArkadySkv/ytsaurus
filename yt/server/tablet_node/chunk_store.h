#pragma once

#include "public.h"
#include "store_detail.h"

#include <core/misc/nullable.h>

#include <core/rpc/public.h>

#include <ytlib/new_table_client/unversioned_row.h>
#include <ytlib/new_table_client/versioned_row.h>

#include <ytlib/chunk_client/public.h>
#include <ytlib/chunk_client/chunk.pb.h>

#include <ytlib/node_tracker_client/node_directory.h>

#include <server/cell_node/public.h>

namespace NYT {
namespace NTabletNode {

////////////////////////////////////////////////////////////////////////////////

class TChunkStore
    : public TStoreBase
{
public:
    TChunkStore(
        TTabletManagerConfigPtr config,
        const TStoreId& id,
        TTablet* tablet,
        const NChunkClient::NProto::TChunkMeta* chunkMeta,
        NCellNode::TBootstrap* bootstrap);
    ~TChunkStore();

    const NChunkClient::NProto::TChunkMeta& GetChunkMeta() const;

    // IStore implementation.
    virtual i64 GetDataSize() const override;

    virtual TOwningKey GetMinKey() const override;
    virtual TOwningKey GetMaxKey() const override;

    virtual TTimestamp GetMinTimestamp() const override;
    virtual TTimestamp GetMaxTimestamp() const override;

    virtual NVersionedTableClient::IVersionedReaderPtr CreateReader(
        TOwningKey lowerKey,
        TOwningKey upperKey,
        TTimestamp timestamp,
        const TColumnFilter& columnFilter) override;

    virtual TTimestamp GetLatestCommitTimestamp(TKey key) override;

    virtual void Save(TSaveContext& context) const override;
    virtual void Load(TLoadContext& context) override;

    virtual void BuildOrchidYson(NYson::IYsonConsumer* consumer) override;

private:
    TTabletManagerConfigPtr Config_;
    NCellNode::TBootstrap* Bootstrap_;

    // Cached for fast retrieval from ChunkMeta_.
    TOwningKey MinKey_;
    TOwningKey MaxKey_;
    TTimestamp MinTimestamp_;
    TTimestamp MaxTimestamp_;
    i64 DataSize_;

    NChunkClient::NProto::TChunkMeta ChunkMeta_;

    NVersionedTableClient::TCachedVersionedChunkMetaPtr CachedMeta_;

    std::vector<TVersionedRow> PooledRows_;


    void PrecacheProperties();

};

DEFINE_REFCOUNTED_TYPE(TChunkStore)

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
