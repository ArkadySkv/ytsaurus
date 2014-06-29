#pragma once

#include "public.h"

namespace NYT {
namespace NHydra {

////////////////////////////////////////////////////////////////////////////////
    
class TFileChangelogDispatcher
    : public TRefCounted
{
public:
    explicit TFileChangelogDispatcher(const Stroka& threadName);
    ~TFileChangelogDispatcher();

    void Shutdown();

    IInvokerPtr GetInvoker();

    IChangelogPtr CreateChangelog(
        const Stroka& path,
        const TSharedRef& meta,
        TFileChangelogConfigPtr config);

    IChangelogPtr OpenChangelog(
        const Stroka& path,
        TFileChangelogConfigPtr config);

    void RemoveChangelog(IChangelogPtr changelog);

private:
    class TImpl;
    typedef TIntrusivePtr<TImpl> TImplPtr;

    class TChangelogQueue;
    typedef TIntrusivePtr<TChangelogQueue> TChangelogQueuePtr;

    friend class TFileChangelog;

    TImplPtr Impl_;

};

DEFINE_REFCOUNTED_TYPE(TFileChangelogDispatcher)

////////////////////////////////////////////////////////////////////////////////

IChangelogStorePtr CreateFileChangelogStore(
    const Stroka& threadName,
    const TCellGuid& cellGuid,
    TFileChangelogStoreConfigPtr config);

////////////////////////////////////////////////////////////////////////////////

} // namespace NHydra
} // namespace NYT
