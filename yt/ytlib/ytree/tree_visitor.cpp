#include "stdafx.h"
#include "tree_visitor.h"
#include "attributes.h"
#include "yson_producer.h"
#include "attribute_helpers.h"

#include <ytlib/misc/serialize.h>
#include <ytlib/misc/assert.h>

#include <ytlib/ytree/node.h>
#include <ytlib/ytree/convert.h>

namespace NYT {
namespace NYTree {

using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

//! Traverses a YTree and invokes appropriate methods of IYsonConsumer.
class TTreeVisitor
    : private TNonCopyable
{
public:
    TTreeVisitor(
        IYsonConsumer* consumer,
        const TAttributeFilter& attributeFilter,
        bool sortKeys)
        : Consumer(consumer)
        , AttributeFilter(attributeFilter)
        , SortKeys(sortKeys)
    { }

    void Visit(const INodePtr& root)
    {
        VisitAny(root, true);
    }

private:
    IYsonConsumer* Consumer;
    TAttributeFilter AttributeFilter;
    bool SortKeys;

    void VisitAny(const INodePtr& node, bool isRoot = false)
    {
        node->SerializeAttributes(Consumer, AttributeFilter, SortKeys);

        if (!isRoot && node->Attributes().Get<bool>("opaque", false)) {
            // This node is opaque, i.e. replaced by entity during tree traversal.
            Consumer->OnEntity();
            return;
        }

        switch (node->GetType()) {
            case ENodeType::String:
            case ENodeType::Integer:
            case ENodeType::Double:
                VisitScalar(node);
                break;

            case ENodeType::Entity:
                VisitEntity(node);
                break;

            case ENodeType::List:
                VisitList(node->AsList());
                break;

            case ENodeType::Map:
                VisitMap(node->AsMap());
                break;

            default:
                YUNREACHABLE();
        }
    }

    void VisitScalar(const INodePtr& node)
    {
        switch (node->GetType()) {
            case ENodeType::String:
                Consumer->OnStringScalar(node->GetValue<Stroka>());
                break;

            case ENodeType::Integer:
                Consumer->OnIntegerScalar(node->GetValue<i64>());
                break;

            case ENodeType::Double:
                Consumer->OnDoubleScalar(node->GetValue<double>());
                break;

            default:
                YUNREACHABLE();
        }
    }

    void VisitEntity(const INodePtr& node)
    {
        UNUSED(node);
        Consumer->OnEntity();
    }

    void VisitList(const IListNodePtr& node)
    {
        Consumer->OnBeginList();
        for (int i = 0; i < node->GetChildCount(); ++i) {
            Consumer->OnListItem();
            VisitAny(node->GetChild(i));
        }
        Consumer->OnEndList();
    }

    void VisitMap(const IMapNodePtr& node)
    {
        Consumer->OnBeginMap();
        auto children = node->GetChildren();
        if (SortKeys) {
            typedef std::pair<Stroka, INodePtr> TPair;
            std::sort(
                children.begin(),
                children.end(),
                [] (const TPair& lhs, const TPair& rhs) {
                    return lhs.first < rhs.first;
                });
        }
        FOREACH (const auto& pair, children) {
            Consumer->OnKeyedItem(pair.first);
            VisitAny(pair.second);
        }
        Consumer->OnEndMap();
    }
};

////////////////////////////////////////////////////////////////////////////////

void VisitTree(
    INodePtr root,
    IYsonConsumer* consumer,
    const TAttributeFilter& attributeFilter,
    bool sortKeys)
{
    TTreeVisitor treeVisitor(
        consumer,
        attributeFilter,
        sortKeys);
    treeVisitor.Visit(root);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT
