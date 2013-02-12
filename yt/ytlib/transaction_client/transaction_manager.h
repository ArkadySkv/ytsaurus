#pragma once

#include "public.h"

#include <ytlib/ytree/public.h>

#include <ytlib/object_client/object_service_proxy.h>

#include <ytlib/transaction_client//transaction_ypath_proxy.h>

namespace NYT {
namespace NTransactionClient {

////////////////////////////////////////////////////////////////////////////////

//! Describes settings for a newly created transaction.
struct TTransactionStartOptions
    : private TNonCopyable
{
    TTransactionStartOptions();

    TNullable<TDuration> Timeout;
    TTransactionId ParentId;
    bool Ping;
    bool PingAncestors;
    bool EnableUncommittedAccounting;
    bool EnableStagedAccounting;
    TAutoPtr<NYTree::IAttributeDictionary> Attributes;
};

//! Describes settings used for attaching to existing transactions.
struct TTransactionAttachOptions
    : private TNonCopyable
{
    explicit TTransactionAttachOptions(const TTransactionId& id);

    TTransactionId Id;
    bool AutoAbort;
    bool Ping;
    bool PingAncestors;
};

//! Controls transactions at client-side.
/*!
 *  Provides a factory for all client-side transactions.
 *  Keeps track of all active transactions and sends pings to master servers periodically.
 */
class TTransactionManager
    : public virtual TRefCounted
{
public:
    //! Initializes an instance.
    /*!
     * \param config A configuration.
     * \param channel A channel used for communicating with masters.
     */
    TTransactionManager(
        TTransactionManagerConfigPtr config,
        NRpc::IChannelPtr channel);

    //! Starts a new transaction.
    /*!
     *
     *  If |options.Ping| is True then Transaction Manager will be renewing
     *  the lease of this transaction.
     *
     *  If |options.PingAncestors| is True then Transaction Manager will be renewing
     *  the leases of all ancestors of this transaction.
     *
     *  \note
     *  This call does not block.
     *  Thread affinity: any.
     */
    ITransactionPtr Start(const TTransactionStartOptions& options);

    //! Attaches to an existing transaction.
    /*!
     *  If |options.AutoAbort| is True then the transaction will be aborted
     *  (if not already committed) at the end of its lifetime.
     *
     *  If |options.Ping| is True then Transaction Manager will be renewing
     *  the lease of this transaction.
     *
     *  If |options.PingAncestors| is True then Transaction Manager will be renewing
     *  the leases of all ancestors of this transaction.
     *  
     *  \note
     *  This call may block.
     *  Thread affinity: any.
     */
    ITransactionPtr Attach(const TTransactionAttachOptions& options);

private:
    class TTransaction;
    typedef TIntrusivePtr<TTransaction> TTransactionPtr;

    typedef TTransactionManager TThis;

    TTransactionManagerConfigPtr Config;
    NRpc::IChannelPtr Channel;
    NObjectClient::TObjectServiceProxy ObjectProxy;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTransactionClient
} // namespace NYT
