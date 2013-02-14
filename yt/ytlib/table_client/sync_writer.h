﻿#pragma once

#include "public.h"
#include "key.h"

#include <ytlib/misc/ref_counted.h>
#include <ytlib/misc/nullable.h>
#include <ytlib/misc/sync.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

struct ISyncWriter
    : public virtual TRefCounted
{
    virtual void Open() = 0;
    virtual void Close() = 0;

    /*!
     *  \param key is used only if the table is sorted, e.g. GetKeyColumns
     *  returns not null.
     */
    virtual void WriteRow(const TRow& row) = 0;

    //! Returns all key columns seen so far.
    virtual const TNullable<TKeyColumns>& GetKeyColumns() const = 0;

    //! Returns the current row count (starting from 0).
    virtual i64 GetRowCount() const = 0;
};

////////////////////////////////////////////////////////////////////////////////

template <class TAsyncWriter>
class TSyncWriterAdapter
    : public ISyncWriter
{
public:
    explicit TSyncWriterAdapter(TIntrusivePtr<TAsyncWriter> writer)
        : Writer(writer)
    { }

    void Open()
    {
        Sync(~Writer, &TAsyncWriter::AsyncOpen);
    }


    void WriteRow(const TRow& row)
    {
        while (!Writer->TryWriteRow(row)) {
            Sync(~Writer, &TAsyncWriter::GetReadyEvent);
        }
    }

    void WriteRowUnsafe(const TRow& row)
    {
        while (!Writer->TryWriteRowUnsafe(row)) {
            Sync(~Writer, &TAsyncWriter::GetReadyEvent);
        }
    }

    void WriteRowUnsafe(const TRow& row, const TNonOwningKey& key)
    {
        while (!Writer->TryWriteRowUnsafe(row, key)) {
            Sync(~Writer, &TAsyncWriter::GetReadyEvent);
        }
    }

    void Close()
    {
        Sync(~Writer, &TAsyncWriter::AsyncClose);
    }

    const TNullable<TKeyColumns>& GetKeyColumns() const
    {
        return Writer->GetKeyColumns();
    }

    i64 GetRowCount() const
    {
        return Writer->GetRowCount();
    }

private:
    TIntrusivePtr<TAsyncWriter> Writer;

};

////////////////////////////////////////////////////////////////////////////////s

template <class TAsyncWriter>
TIntrusivePtr< TSyncWriterAdapter<TAsyncWriter> > CreateSyncWriter(TIntrusivePtr<TAsyncWriter> asyncWriter)
{
    return New< TSyncWriterAdapter<TAsyncWriter> >(asyncWriter);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
