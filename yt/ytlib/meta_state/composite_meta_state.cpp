#include "stdafx.h"
#include "composite_meta_state.h"
#include "private.h"
#include "meta_state_manager.h"

#include <ytlib/misc/foreach.h>
#include <ytlib/actions/bind.h>

namespace NYT {
namespace NMetaState {

using namespace NProto;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = MetaStateLogger;

////////////////////////////////////////////////////////////////////////////////

TMetaStatePart::TMetaStatePart(
    IMetaStateManagerPtr metaStateManager,
    TCompositeMetaStatePtr metaState)
    : MetaStateManager(metaStateManager)
    , MetaState(metaState)
{
    YASSERT(metaStateManager);
    YASSERT(metaState);

    metaStateManager->SubscribeStartLeading(BIND(
        &TThis::OnStartLeading,
        MakeWeak(this)));
    metaStateManager->SubscribeStartLeading(BIND(
        &TThis::OnRecoveryStarted,
        MakeWeak(this)));
    metaStateManager->SubscribeLeaderRecoveryComplete(BIND(
        &TThis::OnRecoveryComplete,
        MakeWeak(this)));
    metaStateManager->SubscribeLeaderRecoveryComplete(BIND(
        &TThis::OnLeaderRecoveryComplete,
        MakeWeak(this)));
    metaStateManager->SubscribeActiveQuorumEstablished(BIND(
        &TThis::OnActiveQuorumEstablished,
        MakeWeak(this)));
    metaStateManager->SubscribeStopLeading(BIND(
        &TThis::OnStopLeading,
        MakeWeak(this)));

    metaStateManager->SubscribeStartFollowing(BIND(
        &TThis::OnStartFollowing,
        MakeWeak(this)));
    metaStateManager->SubscribeStartFollowing(BIND(
        &TThis::OnRecoveryStarted,
        MakeWeak(this)));
    metaStateManager->SubscribeFollowerRecoveryComplete(BIND(
        &TThis::OnRecoveryComplete,
        MakeWeak(this)));
    metaStateManager->SubscribeFollowerRecoveryComplete(BIND(
        &TThis::OnFollowerRecoveryComplete,
        MakeWeak(this)));
    metaStateManager->SubscribeStopFollowing(BIND(
        &TThis::OnStopFollowing,
        MakeWeak(this)));

    MetaState->RegisterPart(this);
}

void TMetaStatePart::RegisterSaver(
    int priority,
    const Stroka& name,
    i32 version,
    TSaver saver)
{
    TCompositeMetaState::TSaverInfo info(priority, name, version, saver);
    YCHECK(MetaState->Savers.insert(std::make_pair(name, info)).second);
}

void TMetaStatePart::RegisterLoader(
    const Stroka& name,
    TVersionValidator versionValidator,
    TLoader loader)
{
    TCompositeMetaState::TLoaderInfo info(name, versionValidator, loader);
    YCHECK(MetaState->Loaders.insert(std::make_pair(name, info)).second);
}

void TMetaStatePart::Clear()
{ }

bool TMetaStatePart::IsLeader() const
{
    return MetaStateManager->IsLeader();
}

bool TMetaStatePart::IsFolllower() const
{
    return MetaStateManager->IsFolllower();
}

bool TMetaStatePart::IsRecovery() const
{
    return MetaStateManager->IsRecovery();
}

void TMetaStatePart::OnStartLeading()
{ }

void TMetaStatePart::OnLeaderRecoveryComplete()
{ }

void TMetaStatePart::OnActiveQuorumEstablished()
{ }

void TMetaStatePart::OnStopLeading()
{ }

void TMetaStatePart::OnStartFollowing()
{ }

void TMetaStatePart::OnFollowerRecoveryComplete()
{ }

void TMetaStatePart::OnStopFollowing()
{ }

void TMetaStatePart::OnRecoveryStarted()
{ }

void TMetaStatePart::OnRecoveryComplete()
{ }

////////////////////////////////////////////////////////////////////////////////

TCompositeMetaState::TSaverInfo::TSaverInfo(
    int priority,
    const Stroka& name,
    i32 version,
    TSaver saver)
    : Priority(priority)
    , Name(name)
    , Version(version)
    , Saver(saver)
{ }

TCompositeMetaState::TLoaderInfo::TLoaderInfo(
    const Stroka& name,
    TVersionValidator versionValidator,
    TLoader loader)
    : Name(name)
    , VersionValidator(versionValidator)
    , Loader(loader)
{ }

////////////////////////////////////////////////////////////////////////////////

void TCompositeMetaState::RegisterPart(TMetaStatePartPtr part)
{
    YASSERT(part);

    Parts.push_back(part);
}

void TCompositeMetaState::Save(TOutputStream* output)
{
    std::vector<TSaverInfo> infos;
    FOREACH (const auto& pair, Savers) {
        infos.push_back(pair.second);
    }

    std::sort(
        infos.begin(),
        infos.end(),
        [] (const TSaverInfo& lhs, const TSaverInfo& rhs) {
            return lhs.Priority < rhs.Priority ||
                   lhs.Priority == rhs.Priority && lhs.Name < rhs.Name;
        });

    i32 partCount = infos.size();
    ::Save(output, partCount);

    TSaveContext context;
    context.SetOutput(output);

    FOREACH (const auto& info, infos) {
        ::Save(output, info.Name);
        ::Save(output, info.Version);
        info.Saver.Run(context);
    }
}

void TCompositeMetaState::Load(TInputStream* input)
{
    i32 partCount;
    ::Load(input, partCount);

    LOG_DEBUG("Started loading composite meta state (PartCount: %d)",
        partCount);

    for (i32 partIndex = 0; partIndex < partCount; ++partIndex) {
        Stroka name;
        i32 version;

        ::Load(input, name);
        ::Load(input, version);

        auto it = Loaders.find(name);
        LOG_FATAL_IF(it == Loaders.end(), "No appropriate loader is registered for part %s", ~name.Quote());

        TLoadContext context;
        context.SetInput(input);
        context.SetVersion(version);

        LOG_DEBUG("Loading meta state part (Name: %s, Version: %d)",
            ~name,
            version);

        const auto& info = it->second;
        info.VersionValidator.Run(version);
        info.Loader.Run(context);
    }

    LOG_DEBUG("Finished loading composite meta state");
}

void TCompositeMetaState::ApplyMutation(TMutationContext* context) throw()
{
    if (context->GetType().empty()) {
        // Empty mutation. Typically used as a tombstone in changelog editing.
        return;
    }
    auto it = Methods.find(context->GetType());
    YCHECK(it != Methods.end());
    it->second.Run(context);
}

void TCompositeMetaState::Clear()
{
    FOREACH (auto& part, Parts) {
        part->Clear();
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NMetaState
} // namespace NYT
