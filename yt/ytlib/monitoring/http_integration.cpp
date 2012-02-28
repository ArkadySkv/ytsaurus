#include "stdafx.h"
#include "http_integration.h"

#include <ytlib/misc/foreach.h>
#include <ytlib/ytree/json_adapter.h>
#include <ytlib/ytree/ypath_proxy.h>
#include <ytlib/ytree/yson_reader.h>
#include <ytlib/ytree/ypath_detail.h>
#include <ytlib/ytree/virtual.h>
#include <ytlib/ytree/serialize.h>

namespace NYT {
namespace NMonitoring {
    
////////////////////////////////////////////////////////////////////////////////

using namespace NYTree;
using namespace NHttp;

namespace {

Stroka OnResponse(TYPathProxy::TRspGet::TPtr rsp)
{
    if (!rsp->IsOK()) {
        // TODO(sandello): Proper JSON escaping here.
        return FormatInternalServerErrorResponse(rsp->GetError().GetMessage().Quote());
    }

    // TODO(babenko): maybe extract method
    TStringStream output;
    TJsonAdapter adapter(&output);
    TStringInput input(rsp->value());
    TYsonReader reader(&adapter, &input);
    reader.Read();
    adapter.Flush();

    return FormatOKResponse(output.Str());
}

void ParseQuery(IAttributeDictionary* attributes, const Stroka& query)
{
	yvector<Stroka> params;
	Split(query, "&", params);
	FOREACH (const auto& param, params) {
		auto eqIndex = param.find_first_of('=');
		if (eqIndex == Stroka::npos) {
			ythrow yexception() << "Malformed query";
		}
		if (eqIndex == 0) {
			ythrow yexception() << "Empty query parameter name";
		}

		Stroka key = param.substr(0, eqIndex);
		TYson value = param.substr(eqIndex + 1);

		// Just a check, IAttributeDictionary takes raw YSON anyway.
		try {
			ValidateYson(value);
		} catch (const std::exception& ex) {
			ythrow yexception() << Sprintf("Error parsing value of query parameter %s\n%s",
				~key,
				ex.what());
		}

		attributes->SetYson(key, value);
	}
}

// TOOD(babenko): use const&
TFuture<Stroka>::TPtr HandleRequest(Stroka url, IYPathServicePtr service)
{
	try {
		// TODO(babenko): rewrite using some standard URL parser
		auto queryIndex = url.find_first_of('?');
		auto req = TYPathProxy::Get();
		TYPath path;
		if (queryIndex == Stroka::npos) {
			path = url;
		} else {
			path = url.substr(0, queryIndex);
			ParseQuery(&req->Attributes(), url.substr(queryIndex + 1));
		}
		req->SetPath(path);
		return ExecuteVerb(~service, ~req)->Apply(FromMethod(&OnResponse));
	} catch (const std::exception& ex) {
		// TODO(sandello): Proper JSON escaping here.
		return MakeFuture(FormatInternalServerErrorResponse(Stroka(ex.what()).Quote()));
	}
}

} // namespace <anonymous>

TServer::TAsyncHandler::TPtr GetYPathHttpHandler(IYPathService* service)
{
	// TODO(babenko): use AsStrong
    return FromMethod(&HandleRequest, IYPathServicePtr(service));
}

TServer::TAsyncHandler::TPtr GetYPathHttpHandler(TYPathServiceProducer producer)
{
	return GetYPathHttpHandler(~IYPathService::FromProducer(producer));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NMonitoring
} // namespace NYT
