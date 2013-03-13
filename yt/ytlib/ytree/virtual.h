#pragma once

#include "ypath_detail.h"
#include "system_attribute_provider.h"

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

class TVirtualMapBase
    : public TSupportsAttributes
    , public ISystemAttributeProvider
{
protected:
    virtual std::vector<Stroka> GetKeys(size_t sizeLimit = Max<size_t>()) const = 0;
    virtual size_t GetSize() const = 0;
    virtual IYPathServicePtr FindItemService(const TStringBuf& key) const = 0;

private:
    virtual bool DoInvoke(NRpc::IServiceContextPtr context) override;

    virtual TResolveResult ResolveRecursive(const TYPath& path, NRpc::IServiceContextPtr context) override;
    virtual void GetSelf(TReqGet* request, TRspGet* response, TCtxGetPtr context) override;
    virtual void ListSelf(TReqList* request, TRspList* response, TCtxListPtr context) override;

    // TSupportsAttributes overrides
    virtual ISystemAttributeProvider* GetSystemAttributeProvider() override;

    // ISystemAttributeProvider overrides
    virtual void ListSystemAttributes(std::vector<TAttributeInfo>* attributes) override;
    virtual bool GetSystemAttribute(const Stroka& key, NYson::IYsonConsumer* consumer) override;
    virtual TAsyncError GetSystemAttributeAsync(const Stroka& key, NYson::IYsonConsumer* consumer) override;
    virtual bool SetSystemAttribute(const Stroka& key, const TYsonString& value) override;

};

////////////////////////////////////////////////////////////////////////////////

INodePtr CreateVirtualNode(IYPathServicePtr service);
INodePtr CreateVirtualNode(TYPathServiceProducer producer);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT
