#pragma once

#include "public.h"

#include <ytlib/meta_state/config.h>
#include <server/transaction_server/config.h>
#include <server/chunk_server/config.h>
#include <server/object_server/config.h>
#include <server/bootstrap/config.h>

namespace NYT {
namespace NCellMaster {

////////////////////////////////////////////////////////////////////////////////

//! Describes a configuration of TCellMaster.
struct TCellMasterConfig
    : public TServerConfig
{
    //! Meta state configuration.
    NMetaState::TPersistentStateManagerConfigPtr MetaState;

    NTransactionServer::TTransactionManagerConfigPtr Transactions;

    NChunkServer::TChunkManagerConfigPtr Chunks;

    NObjectServer::TObjectManagerConfigPtr Objects;

    //! HTTP monitoring interface port number.
    int MonitoringPort;

    TCellMasterConfig()
    {
        Register("meta_state", MetaState)
            .DefaultNew();
        Register("transactions", Transactions)
            .DefaultNew();
        Register("chunks", Chunks)
            .DefaultNew();
        Register("objects", Objects)
            .DefaultNew();
        Register("monitoring_port", MonitoringPort)
            .Default(10000);
    }
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NCellMaster
} // namespace NYT
