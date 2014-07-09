#pragma once

#include "public.h"

#include <core/rpc/public.h>

#include <ytlib/chunk_client/public.h>

#include <ytlib/transaction_client/public.h>

#include <ytlib/hive/public.h>

#include <ytlib/tablet_client/public.h>

#include <ytlib/query_client/public.h>

namespace NYT {
namespace NApi {

////////////////////////////////////////////////////////////////////////////////

struct TClientOptions
{
    TNullable<Stroka> User;
};

////////////////////////////////////////////////////////////////////////////////

//! Represents an established connection with a YT cluster.
/*
 *  IConnection instance caches most of the stuff needed for fast interaction
 *  with the cluster (e.g. connection channels, mount info etc).
 *  
 *  Thread affinity: any
 */
struct IConnection
    : public virtual TRefCounted
{
    virtual TConnectionConfigPtr GetConfig() = 0;
    virtual NRpc::IChannelPtr GetMasterChannel() = 0;
    virtual NRpc::IChannelPtr GetMasterCacheChannel() = 0;
    virtual NRpc::IChannelPtr GetSchedulerChannel() = 0;
    virtual NRpc::IChannelFactoryPtr GetNodeChannelFactory() = 0;
    virtual NChunkClient::IBlockCachePtr GetBlockCache() = 0;
    virtual NTabletClient::TTableMountCachePtr GetTableMountCache() = 0;
    virtual NTransactionClient::ITimestampProviderPtr GetTimestampProvider() = 0;
    virtual NHive::TCellDirectoryPtr GetCellDirectory() = 0;
    virtual NQueryClient::IPrepareCallbacks* GetQueryPrepareCallbacks() = 0;
    virtual NQueryClient::IExecutorPtr GetQueryExecutor() = 0;

    virtual IClientPtr CreateClient(const TClientOptions& options = TClientOptions()) = 0;

};

DEFINE_REFCOUNTED_TYPE(IConnection)

IConnectionPtr CreateConnection(TConnectionConfigPtr config);

////////////////////////////////////////////////////////////////////////////////

} // namespace NApi
} // namespace NYT

