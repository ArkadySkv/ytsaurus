#pragma once

#include "public.h"

#include <ytlib/misc/property.h>

#include <ytlib/meta_state/composite_meta_state.h>

#include <server/object_server/public.h>

#include <server/transaction_server/public.h>

#include <server/chunk_server/public.h>

#include <server/cypress_server/public.h>

#include <server/security_server/public.h>

namespace NYT {
namespace NCellMaster {

////////////////////////////////////////////////////////////////////////////////

DECLARE_ENUM(ESavePriority,
    (Keys)
    (Values)
);

const int CurrentSnapshotVersion = 7;
NMetaState::TVersionValidator SnapshotVersionValidator();

struct TLoadContext
    : public NMetaState::TLoadContext
{
    DEFINE_BYVAL_RW_PROPERTY(TBootstrap*, Bootstrap);

    template <class T>
    T* Get(const NObjectClient::TObjectId& id) const;

    template <class T>
    T* Get(const NObjectClient::TVersionedObjectId& id) const;
};

struct TSaveContext
    : public NMetaState::TSaveContext
{ };

template <>
NObjectServer::TObjectBase* TLoadContext::Get(const NObjectClient::TObjectId& id) const;

template <>
NTransactionServer::TTransaction* TLoadContext::Get(const NObjectClient::TObjectId& id) const;

template <>
NChunkServer::TChunkList* TLoadContext::Get(const NObjectClient::TObjectId& id) const;

template <>
NChunkServer::TChunk* TLoadContext::Get(const NObjectClient::TObjectId& id) const;

template <>
NChunkServer::TJob* TLoadContext::Get(const NObjectClient::TObjectId& id) const;

template <>
NCypressServer::TCypressNodeBase* TLoadContext::Get(const NCypressClient::TNodeId& id) const;

template <>
NCypressServer::TCypressNodeBase* TLoadContext::Get(const NCypressClient::TVersionedNodeId& id) const;

template <>
NSecurityServer::TAccount* TLoadContext::Get(const NObjectClient::TObjectId& id) const;

////////////////////////////////////////////////////////////////////////////////

template <class T>
void SaveObjectRef(TOutputStream* output, T object);

template <class T>
void LoadObjectRef(TInputStream* input, T& object, const TLoadContext& context);

////////////////////////////////////////////////////////////////////////////////

template <class T>
void SaveObjectRefs(TOutputStream* output, const T& object);

template <class T>
void LoadObjectRefs(TInputStream* input, T& object, const TLoadContext& context);

////////////////////////////////////////////////////////////////////////////////

template <class T>
T Load(const TLoadContext& context);

template <class T>
void Load(const TLoadContext& context, T& value);

template <class T>
void Save(const TSaveContext& context, const T& value);

////////////////////////////////////////////////////////////////////////////////

} // namespace NCellMaster
} // namespace NYT

#define SERIALIZATION_CONTEXT_INL_H_
#include "serialization_context-inl.h"
#undef SERIALIZATION_CONTEXT_INL_H_
