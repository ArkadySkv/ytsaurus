#pragma once

#include "command.h"

#include <ytlib/table_client/public.h>

#include <ytlib/ypath/rich.h>

#include <core/formats/format.h>

namespace NYT {
namespace NDriver {

////////////////////////////////////////////////////////////////////////////////

struct TReadRequest
    : public TTransactionalRequest
{
    NYPath::TRichYPath Path;
    NYTree::INodePtr TableReader;

    TReadRequest()
    {
        RegisterParameter("path", Path);
        RegisterParameter("table_reader", TableReader)
            .Default(nullptr);
    }

    virtual void OnLoaded() override
    {
        TTransactionalRequest::OnLoaded();
        
        Path = Path.Simplify();
    }
};

class TReadCommand
    : public TTypedCommand<TReadRequest>
{
private:
    virtual void DoExecute() override;

};

////////////////////////////////////////////////////////////////////////////////

struct TWriteRequest
    : public TTransactionalRequest
{
    NYPath::TRichYPath Path;
    TNullable<NTableClient::TKeyColumns> SortedBy;
    NYTree::INodePtr TableWriter;

    TWriteRequest()
    {
        RegisterParameter("path", Path);
        RegisterParameter("table_writer", TableWriter)
            .Default(nullptr);
    }

    virtual void OnLoaded() override
    {
        TTransactionalRequest::OnLoaded();
        
        Path = Path.Simplify();
    }
};

class TWriteCommand
    : public TTypedCommand<TWriteRequest>
{
private:
    virtual void DoExecute() override;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NDriver
} // namespace NYT
