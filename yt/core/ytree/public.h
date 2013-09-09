#pragma once

#include <core/misc/common.h>

#include <core/actions/callback_forward.h>

#include <core/ypath/public.h>

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

class TYsonString;

struct INode;
typedef TIntrusivePtr<INode> INodePtr;
typedef TIntrusivePtr<const INode> IConstNodePtr;

struct ICompositeNode;
typedef TIntrusivePtr<ICompositeNode> ICompositeNodePtr;

struct IStringNode;
typedef TIntrusivePtr<IStringNode> IStringNodePtr;

struct IIntegerNode;
typedef TIntrusivePtr<IIntegerNode> IIntegerNodePtr;

struct IDoubleNode;
typedef TIntrusivePtr<IDoubleNode> IDoubleNodePtr;

struct IListNode;
typedef TIntrusivePtr<IListNode> IListNodePtr;

struct IMapNode;
typedef TIntrusivePtr<IMapNode> IMapNodePtr;

struct IEntityNode;
typedef TIntrusivePtr<IEntityNode> IEntityNodePtr;

struct INodeFactory;
typedef TIntrusivePtr<INodeFactory> INodeFactoryPtr;

struct INodeResolver;
typedef TIntrusivePtr<INodeResolver> INodeResolverPtr;

class TYsonProducer;

class TYsonInput;
class TYsonOutput;

struct IAttributeDictionary;

struct IAttributeOwner;

struct ISystemAttributeProvider;

struct IYPathService;
typedef TIntrusivePtr<IYPathService> IYPathServicePtr;

class TYPathRequest;
typedef TIntrusivePtr<TYPathRequest> TYPathRequestPtr;

template <class TRequestMessage, class TResponseMessage>
class TTypedYPathRequest;

class TYPathResponse;
typedef TIntrusivePtr<TYPathResponse> TYPathResponsePtr;

template <class TRequestMessage, class TResponseMessage>
class TTypedYPathResponse;

using NYPath::TYPath;

////////////////////////////////////////////////////////////////////////////////

//! A static node type.
DECLARE_ENUM(ENodeType,
    // Node contains a string (Stroka).
    (String)
    // Node contains an integer number (i64).
    (Integer)
    // Node contains an FP number (double).
    (Double)
    // Node contains a map from strings to other nodes.
    (Map)
    // Node contains a list (vector) of other nodes.
    (List)
    // Node is atomic, i.e. has no visible properties (aside from attributes).
    (Entity)
    // Either List or Map.
    (Composite)
);

DECLARE_ENUM(EErrorCode,
    ((ResolveError)    (500))
    ((AlreadyExists)   (501))
);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT
