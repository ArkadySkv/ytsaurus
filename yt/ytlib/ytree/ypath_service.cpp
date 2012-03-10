#include "stdafx.h"
#include "ypath_service.h"
#include "tree_builder.h"
#include "ephemeral.h"
#include "ypath_client.h"
#include "ypath_detail.h"

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

IYPathServicePtr IYPathService::FromProducer(TYsonProducer producer)
{
    auto builder = CreateBuilderFromFactory(GetEphemeralNodeFactory());
    builder->BeginTree();
    producer->Do(~builder);
    return builder->EndTree();
}

////////////////////////////////////////////////////////////////////////////////

namespace {

class TViaYPathService
	: public TYPathServiceBase
{
public:
	TViaYPathService(IYPathService* underlyingService, IInvoker* invoker)
		: UnderlyingService(underlyingService)
		, Invoker(invoker)
	{ }

	virtual TResolveResult Resolve(const TYPath& path, const Stroka& verb)
	{
		return TResolveResult::Here(path);
	}

private:
	IYPathServicePtr UnderlyingService;
	IInvoker::TPtr Invoker;

	virtual void DoInvoke(NRpc::IServiceContext* context)
	{
		Invoker->Invoke(FromMethod(
			&TViaYPathService::ExecuteRequest,
			MakeStrong(this),
			context));
	}

	void ExecuteRequest(NRpc::IServiceContext::TPtr context)
	{
		ExecuteVerb(~UnderlyingService, ~context);
	}
};

} // namespace <anonymous>

IYPathServicePtr IYPathService::Via(IInvoker* invoker)
{
	return New<TViaYPathService>(this, invoker);
}

////////////////////////////////////////////////////////////////////////////////

namespace {

class TFromProducerPathService
	: public TYPathServiceBase
{
public:
	TFromProducerPathService(TYPathServiceProducer producer)
		: Producer(producer)
	{ }

	virtual TResolveResult Resolve(const TYPath& path, const Stroka& verb)
	{
		return TResolveResult::Here(path);
	}

private:
	TYPathServiceProducer Producer;

	virtual void DoInvoke(NRpc::IServiceContext* context)
	{
		auto service = Producer->Do();
		ExecuteVerb(~service, context);
	}
};

} // namespace <anonymous>

IYPathServicePtr IYPathService::FromProducer(TYPathServiceProducer producer)
{
	return New<TFromProducerPathService>(producer);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT
