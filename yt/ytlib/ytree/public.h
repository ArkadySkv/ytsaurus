#pragma once

#include <ytlib/actions/callback_forward.h>
#include <ytlib/misc/common.h>
#include <ytlib/misc/enum.h>

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

typedef Stroka TYPath;
typedef Stroka TYson;

//! A static node type.
DECLARE_ENUM(ENodeType,
    // Node contains a string (Stroka).
    (String)
    // Node contains an integer number (i64).
    (Int64)
    // Node contains an FP number (double).
    (Double)
    // Node contains a map from strings to other nodes.
    (Map)
    // Node contains a list (vector) of other nodes.
    (List)
    // Node is atomic, i.e. has no visible properties (aside from attributes).
    (Entity)
);
    
struct INode;
typedef TIntrusivePtr<INode> INodePtr;

struct ICompositeNode;
typedef TIntrusivePtr<ICompositeNode> ICompositeNodePtr;

struct IStringNode;
typedef TIntrusivePtr<IStringNode> IStringNodePtr;

struct IInt64Node;
typedef TIntrusivePtr<IInt64Node> IInt64NodePtr;

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

struct IYsonConsumer;

//! A callback capable of generating YSON by calling appropriate
//! methods for its IYsonConsumer argument.
typedef TCallback<void(IYsonConsumer*)> TYsonProducer;

struct IAttributeDictionary;

struct IAttributeProvider;

struct IYPathService;
typedef TIntrusivePtr<IYPathService> IYPathServicePtr;
typedef TCallback<IYPathServicePtr()> TYPathServiceProducer;

// TODO(roizner): Rename it and move somewhere.
template <class T, class = void>
struct TDeserializeTraits;

class TYPathRequest;
typedef TIntrusivePtr<TYPathRequest> TYPathRequestPtr;

template <class TRequestMessage, class TResponseMessage>
class TTypedYPathRequest;

class TYPathResponse;
typedef TIntrusivePtr<TYPathResponse> TYPathResponsePtr;

template <class TRequestMessage, class TResponseMessage>
class TTypedYPathResponse;

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree 
} // namespace NYT
