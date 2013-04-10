﻿#include "stdafx.h"
#include "stderr_output.h"
#include "private.h"

#include <ytlib/file_client/config.h>
#include <ytlib/file_client/file_chunk_output.h>

#include <ytlib/chunk_client/chunk_list_ypath_proxy.h>

#include <ytlib/transaction_client/transaction_ypath_proxy.h>

#include <ytlib/security_client/public.h>

#include <ytlib/rpc/channel.h>

namespace NYT {
namespace NJobProxy {

using namespace NFileClient;
using namespace NRpc;
using namespace NTransactionClient;
using namespace NChunkServer;

////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = JobProxyLogger;

////////////////////////////////////////////////////////////////////

TErrorOutput::TErrorOutput(
    TFileWriterConfigPtr config,
    IChannelPtr masterChannel,
    const TTransactionId& transactionId,
    i64 maxSize)
    : Config(config)
    , MasterChannel(masterChannel)
    , TransactionId(transactionId)
    , MaxSize(maxSize)
    , IsClosed(false)
{ }

TErrorOutput::~TErrorOutput() throw()
{ }

void TErrorOutput::DoWrite(const void* buf, size_t len)
{
    if (!FileWriter) {
        LOG_DEBUG("Opening stderr stream");

        FileWriter = new TFileChunkOutput(
            Config,
            MasterChannel,
            TransactionId,
            NSecurityClient::SysAccountName,
            MaxSize);
        FileWriter->Open();

        LOG_DEBUG("Stderr stream opened");
    }

    FileWriter->Write(buf, len);
}

void TErrorOutput::DoFinish()
{
    if (!FileWriter)
        return;

    FileWriter->Finish();
    IsClosed = true;;
}

TChunkId TErrorOutput::GetChunkId() const
{
    return IsClosed ? FileWriter->GetChunkId() : NullChunkId;
}

////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
