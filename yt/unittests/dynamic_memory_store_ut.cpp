#include "stdafx.h"
#include "memory_store_ut.h"

#include <yt/core/concurrency/fiber.h>

#include <yt/ytlib/new_table_client/versioned_row.h>

#include <yt/server/tablet_node/public.h>
#include <yt/server/tablet_node/config.h>
#include <yt/server/tablet_node/tablet_manager.h>

namespace NYT {
namespace NTabletNode {
namespace {

using namespace NTransactionClient;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

class TDynamicMemoryStoreTest
    : public TMemoryStoreTestBase
{
public:
    TDynamicMemoryStoreTest()
    {
        auto config = New<TTabletManagerConfig>();
        Store = New<TDynamicMemoryStore>(config, Tablet.get());
    }



    void ConfirmRow(TDynamicRow row)
    {
        Store->ConfirmRow(row);
    }

    void PrepareRow(TDynamicRow row)
    {
        Store->PrepareRow(row);
    }

    void CommitRow(TDynamicRow row)
    {
        Store->CommitRow(row);
    }

    void AbortRow(TDynamicRow row)
    {
        Store->AbortRow(row);
    }


    TDynamicRow WriteRow(
        TTransaction* transaction,
        TUnversionedRow row,
        bool prewrite)
    {
        return Store->WriteRow(
            NameTable,
            transaction,
            row,
            prewrite);
    }

    TTimestamp WriteRow(TUnversionedRow row)
    {
        auto transaction = StartTransaction();
        auto dynamicRow = WriteRow(transaction.get(), row, false);
        PrepareTransaction(transaction.get());
        PrepareRow(dynamicRow);
        CommitTransaction(transaction.get());
        CommitRow(dynamicRow);
        return transaction->GetCommitTimestamp();
    }


    TDynamicRow DeleteRow(
        TTransaction* transaction,
        NVersionedTableClient::TKey key,
        bool prewrite)
    {
        return Store->DeleteRow(
            transaction,
            key,
            prewrite);
    }

    TTimestamp DeleteRow(NVersionedTableClient::TKey key)
    {
        auto transaction = StartTransaction();
        auto row = DeleteRow(transaction.get(), key, false);
        PrepareTransaction(transaction.get());
        PrepareRow(row);
        CommitTransaction(transaction.get());
        CommitRow(row);
        return transaction->GetCommitTimestamp();
    }


    TUnversionedOwningRow LookupRow(
        NVersionedTableClient::TKey key,
        TTimestamp timestamp)
    {
        auto scanner = Store->CreateScanner();
        auto scannerTimestamp = scanner->Find(key, timestamp);

        if (scannerTimestamp == NullTimestamp) {
            return TUnversionedOwningRow();
        }

        if (scannerTimestamp & TombstoneTimestampMask) {
            return TUnversionedOwningRow();
        }

        TUnversionedOwningRowBuilder builder;
        
        int keyCount = static_cast<int>(Tablet->KeyColumns().size());
        int schemaColumnCount = static_cast<int>(Tablet->Schema().Columns().size());

        // Keys
        const auto* keys = scanner->GetKeys();
        for (int index = 0; index < keyCount; ++index) {
            builder.AddValue(keys[index]);
        }

        // Fixed values
        for (int index = 0; index < schemaColumnCount - keyCount; ++index) {
            const auto* value = scanner->GetFixedValue(index);
            builder.AddValue(
                value
                ? *static_cast<const TUnversionedValue*>(value)
                : MakeUnversionedSentinelValue(EValueType::Null, index + keyCount));
        }

        return builder.Finish();
    }

    void CompareRows(TUnversionedRow row, const TNullable<Stroka>& yson)
    {
        if (!row && !yson)
            return;

        ASSERT_TRUE(static_cast<bool>(row));
        ASSERT_TRUE(yson.HasValue());

        auto expectedRowParts = ConvertTo<yhash_map<Stroka, INodePtr>>(TYsonString(*yson, EYsonType::MapFragment));

        for (int index = 0; index < row.GetValueCount(); ++index) {
            const auto& value = row[index];
            const auto& name = NameTable->GetName(value.Id);
            auto it = expectedRowParts.find(name);
            switch (value.Type) {
                case EValueType::Integer:
                    ASSERT_EQ(it->second->GetValue<i64>(), value.Data.Integer);
                    break;
                
                case EValueType::Double:
                    ASSERT_EQ(it->second->GetValue<double>(), value.Data.Double);
                    break;
                
                case EValueType::String:
                    ASSERT_EQ(it->second->GetValue<Stroka>(), Stroka(value.Data.String, value.Length));
                    break;

                case EValueType::Null:
                    ASSERT_TRUE(it == expectedRowParts.end());
                    break;

                default:
                    YUNREACHABLE();
            }
        }
    }


    TDynamicMemoryStorePtr Store;

};

///////////////////////////////////////////////////////////////////////////////

TEST_F(TDynamicMemoryStoreTest, Empty)
{
    auto key = BuildKey("1");
    CompareRows(LookupRow(key, 0), Null);
    CompareRows(LookupRow(key, LastCommittedTimestamp), Null);
}

TEST_F(TDynamicMemoryStoreTest, PrewriteAndCommit)
{
    auto transaction = StartTransaction();

    auto key = BuildKey("1");

    Stroka rowString("key=1;a=1");
    
    CompareRows(LookupRow(key, LastCommittedTimestamp), Null);

    auto row = WriteRow(transaction.get(), BuildRow(rowString), true);
    ASSERT_EQ(row.GetTransaction(), transaction.get());
    ASSERT_EQ(row.GetLockMode(), ERowLockMode::Write);
    ASSERT_EQ(row.GetLockIndex(), -1);
    ASSERT_TRUE(transaction->LockedRows().empty());

    ConfirmRow(row);
    ASSERT_EQ(row.GetLockIndex(), 0);
    ASSERT_EQ(transaction->LockedRows().size(), 1);
    ASSERT_TRUE(transaction->LockedRows()[0].Row == row);

    CompareRows(LookupRow(key, LastCommittedTimestamp), Null);

    PrepareTransaction(transaction.get());
    PrepareRow(row);

    CommitTransaction(transaction.get());
    CommitRow(row);

    ASSERT_EQ(row.GetTransaction(), nullptr);
    ASSERT_EQ(row.GetLockMode(), ERowLockMode::None);
    ASSERT_EQ(row.GetLockIndex(), -1);

    CompareRows(LookupRow(key, MinTimestamp), Null);
    CompareRows(LookupRow(key, LastCommittedTimestamp), rowString);
    CompareRows(LookupRow(key, MaxTimestamp), rowString);
    CompareRows(LookupRow(key, transaction->GetCommitTimestamp()), rowString);
    CompareRows(LookupRow(key, transaction->GetCommitTimestamp() - 1), Null);
}

TEST_F(TDynamicMemoryStoreTest, PrewriteManyAndCommit)
{
    auto key = BuildKey("1");

    std::vector<TTimestamp> timestamps;

    for (int i = 0; i < 100; ++i) {
        auto transaction = StartTransaction();

        if (i == 0) {
            CompareRows(LookupRow(key, transaction->GetStartTimestamp()), Null);
        } else {
            CompareRows(LookupRow(key, transaction->GetStartTimestamp()), "key=1;a=" + ToString(i - 1));
        }

        auto row = WriteRow(transaction.get(), BuildRow("key=1;a=" + ToString(i)), false);

        PrepareTransaction(transaction.get());
        PrepareRow(row);

        CommitTransaction(transaction.get());
        CommitRow(row);

        timestamps.push_back(transaction->GetCommitTimestamp());
    }


    CompareRows(LookupRow(key, MinTimestamp), Null);
    CompareRows(LookupRow(key, MaxTimestamp), Stroka("key=1;a=99"));
    CompareRows(LookupRow(key, LastCommittedTimestamp), Stroka("key=1;a=99"));

    for (int i = 0; i < 100; ++i) {
        CompareRows(LookupRow(key, timestamps[i]), Stroka("key=1;a=" + ToString(i)));
    }
}

TEST_F(TDynamicMemoryStoreTest, WriteSameRow)
{
    auto key = BuildKey("1");

    auto transaction = StartTransaction();

    auto row = WriteRow(transaction.get(), BuildRow("key=1;b=3.14"), false);
    ASSERT_TRUE(WriteRow(transaction.get(), BuildRow("key=1;b=2.71"), false) == TDynamicRow());

    ASSERT_EQ(row.GetLockIndex(), 0);
    ASSERT_EQ(transaction->LockedRows().size(), 1);
    ASSERT_TRUE(transaction->LockedRows()[0].Row == row);

    PrepareTransaction(transaction.get());
    PrepareRow(row);

    CommitTransaction(transaction.get());
    CommitRow(row);

    CompareRows(LookupRow(key, LastCommittedTimestamp), Stroka("key=1;b=2.71"));
}

TEST_F(TDynamicMemoryStoreTest, WriteAndAbort)
{
    auto key = BuildKey("1");

    auto transaction = StartTransaction();

    auto row = WriteRow(transaction.get(), BuildRow("key=1;b=3.14"), false);

    PrepareTransaction(transaction.get());
    PrepareRow(row);

    AbortTransaction(transaction.get());
    AbortRow(row);

    ASSERT_EQ(row.GetTransaction(), nullptr);
    ASSERT_EQ(row.GetLockMode(), ERowLockMode::None);
    ASSERT_EQ(row.GetLockIndex(), -1);
}

TEST_F(TDynamicMemoryStoreTest, Delete)
{
    auto key = BuildKey("1");

    auto transaction = StartTransaction();
    DeleteRow(transaction.get(), key, false);

    CompareRows(LookupRow(key, MinTimestamp), Null);
    CompareRows(LookupRow(key, LastCommittedTimestamp), Null);
}

TEST_F(TDynamicMemoryStoreTest, WriteDelete)
{
    auto key = BuildKey("1");

    auto ts1 = WriteRow(BuildRow("key=1;c=value"));
    auto ts2 = DeleteRow(key);

    CompareRows(LookupRow(key, MinTimestamp), Null);
    CompareRows(LookupRow(key, ts1), Stroka("key=1;c=value"));
    CompareRows(LookupRow(key, ts2), Null);
}

TEST_F(TDynamicMemoryStoreTest, DeleteSameRow)
{
    auto key = BuildKey("1");

    auto transaction = StartTransaction();

    auto row = DeleteRow(transaction.get(), key, false);
    ASSERT_TRUE(DeleteRow(transaction.get(), key, false) == TDynamicRow());

    PrepareTransaction(transaction.get());
    PrepareRow(row);

    CommitTransaction(transaction.get());
    CommitRow(row);

    CompareRows(LookupRow(key, LastCommittedTimestamp), Null);
}

TEST_F(TDynamicMemoryStoreTest, Update1)
{
    auto key = BuildKey("1");
    
    auto ts = WriteRow(BuildRow("key=1", false));
    
    CompareRows(LookupRow(key, MinTimestamp), Null);
    CompareRows(LookupRow(key, ts), Stroka("key=1"));
}

TEST_F(TDynamicMemoryStoreTest, Update2)
{
    auto key = BuildKey("1");
    
    auto ts1 = WriteRow(BuildRow("key=1;a=1", false));
    auto ts2 = WriteRow(BuildRow("key=1;b=3.0", false));
    auto ts3 = WriteRow(BuildRow("key=1;c=test", false));
    
    CompareRows(LookupRow(key, MinTimestamp), Null);
    CompareRows(LookupRow(key, ts1), Stroka("key=1;a=1"));
    CompareRows(LookupRow(key, ts2), Stroka("key=1;a=1;b=3.0"));
    CompareRows(LookupRow(key, ts3), Stroka("key=1;a=1;b=3.0;c=test"));
}

TEST_F(TDynamicMemoryStoreTest, Update3)
{
    auto key = BuildKey("1");
    
    auto ts1 = WriteRow(BuildRow("key=1;a=1", false));
    auto ts2 = WriteRow(BuildRow("key=1;a=2", false));
    auto ts3 = WriteRow(BuildRow("key=1;a=3", false));
    
    CompareRows(LookupRow(key, MinTimestamp), Null);
    CompareRows(LookupRow(key, ts1), Stroka("key=1;a=1"));
    CompareRows(LookupRow(key, ts2), Stroka("key=1;a=2"));
    CompareRows(LookupRow(key, ts3), Stroka("key=1;a=3"));
}

TEST_F(TDynamicMemoryStoreTest, UpdateDelete1)
{
    auto key = BuildKey("1");
    
    auto ts1 = WriteRow(BuildRow("key=1;a=1", false));
    auto ts2 = DeleteRow(key);
    auto ts3 = WriteRow(BuildRow("key=1;b=2.0", false));
    auto ts4 = DeleteRow(key);
    auto ts5 = WriteRow(BuildRow("key=1;c=test", false));
    auto ts6 = DeleteRow(key);
    
    CompareRows(LookupRow(key, MinTimestamp), Null);
    CompareRows(LookupRow(key, ts1), Stroka("key=1;a=1"));
    CompareRows(LookupRow(key, ts2), Null);
    CompareRows(LookupRow(key, ts3), Stroka("key=1;b=2.0"));
    CompareRows(LookupRow(key, ts4), Null);
    CompareRows(LookupRow(key, ts5), Stroka("key=1;c=test"));
    CompareRows(LookupRow(key, ts6), Null);
}

TEST_F(TDynamicMemoryStoreTest, UpdateDelete2)
{
    auto key = BuildKey("1");
    
    auto ts1 = DeleteRow(key);
    auto ts2 = DeleteRow(key);
    auto ts3 = WriteRow(BuildRow("key=1;a=1", false));
    auto ts4 = DeleteRow(key);
    auto ts5 = DeleteRow(key);
    
    CompareRows(LookupRow(key, MinTimestamp), Null);
    CompareRows(LookupRow(key, ts1), Null);
    CompareRows(LookupRow(key, ts2), Null);
    CompareRows(LookupRow(key, ts3), Stroka("key=1;a=1"));
    CompareRows(LookupRow(key, ts4), Null);
    CompareRows(LookupRow(key, ts5), Null);
}

TEST_F(TDynamicMemoryStoreTest, DeleteAfterWriteFailure1)
{
    auto transaction = StartTransaction();
    WriteRow(transaction.get(), BuildRow("key=1"), true);
    ASSERT_ANY_THROW({
        DeleteRow(transaction.get(), BuildKey("1"), true);
    });
}

TEST_F(TDynamicMemoryStoreTest, DeleteAfterWriteFailure2)
{
    WriteRow(BuildRow("key=1"));

    {
        auto transaction = StartTransaction();
        WriteRow(transaction.get(), BuildRow("key=1"), true);
        ASSERT_ANY_THROW({
            DeleteRow(transaction.get(), BuildKey("1"), true);
        });
    }
}

TEST_F(TDynamicMemoryStoreTest, WriteAfterDeleteFailure1)
{
    auto transaction = StartTransaction();
    DeleteRow(transaction.get(), BuildKey("1"), true);
    ASSERT_ANY_THROW({
        WriteRow(transaction.get(), BuildRow("key=1"), true);
    });
}

TEST_F(TDynamicMemoryStoreTest, WriteAfterDeleteFailure2)
{
    WriteRow(BuildRow("key=1"));

    {
        auto transaction = StartTransaction();
        DeleteRow(transaction.get(), BuildKey("1"), true);
        ASSERT_ANY_THROW({
            WriteRow(transaction.get(), BuildRow("key=1"), true);
        });
    }
}

TEST_F(TDynamicMemoryStoreTest, WriteWriteConflict1)
{
    auto key = BuildKey("1");

    auto transaction1 = StartTransaction();
    auto transaction2 = StartTransaction();
    WriteRow(transaction1.get(), BuildRow("key=1;c=test1"), true);
    ASSERT_ANY_THROW({
        WriteRow(transaction2.get(), BuildRow("key=1;c=test2"), true);
    });
}

TEST_F(TDynamicMemoryStoreTest, WriteWriteConflict2)
{
    auto key = BuildKey("1");

    auto transaction1 = StartTransaction();
    auto transaction2 = StartTransaction();

    auto row = WriteRow(transaction1.get(), BuildRow("key=1;a=1"), true);

    PrepareTransaction(transaction1.get());
    PrepareRow(row);

    CommitTransaction(transaction1.get());
    CommitRow(row);

    ASSERT_ANY_THROW({
        WriteRow(transaction2.get(), BuildRow("key=1;a=2"), true);
    });
}

TEST_F(TDynamicMemoryStoreTest, ReadNotPostponed)
{
    auto key = BuildKey("1");

    auto transaction = StartTransaction();

    auto row = WriteRow(transaction.get(), BuildRow("key=1;a=1"), false);
    
    PrepareTransaction(transaction.get());
    PrepareRow(row);

    auto fiber = New<TFiber>(BIND([&] () {
        // Not postponed because of timestamp.
        CompareRows(LookupRow(key, LastCommittedTimestamp), Null);
        CompareRows(LookupRow(key, transaction->GetPrepareTimestamp()), Null);
    }));

    fiber->Run();
    ASSERT_EQ(fiber->GetState(), EFiberState::Terminated);
}

TEST_F(TDynamicMemoryStoreTest, ReadPostpostedAbort)
{
    auto key = BuildKey("1");

    auto transaction = StartTransaction();
    
    auto row = WriteRow(transaction.get(), BuildRow("key=1;a=1"), false);
    
    PrepareTransaction(transaction.get());
    PrepareRow(row);

    auto fiber = New<TFiber>(BIND([&] () {
        // Postponed, old value is read.
        CompareRows(LookupRow(key, MaxTimestamp), Null);
    }));

    fiber->Run();
    ASSERT_EQ(fiber->GetState(), EFiberState::Suspended);

    AbortTransaction(transaction.get());
    AbortRow(row);

    fiber->Run();
    ASSERT_EQ(fiber->GetState(), EFiberState::Terminated);
}

TEST_F(TDynamicMemoryStoreTest, ReadPostponedCommit)
{
    auto key = BuildKey("1");

    auto transaction = StartTransaction();
    
    auto row = WriteRow(transaction.get(), BuildRow("key=1;a=1"), false);
    
    PrepareTransaction(transaction.get());
    PrepareRow(row);

    auto fiber = New<TFiber>(BIND([&] () {
        // Postponed, new value is read.
        CompareRows(LookupRow(key, MaxTimestamp), Stroka("key=1;a=1"));
    }));

    fiber->Run();
    ASSERT_EQ(fiber->GetState(), EFiberState::Suspended);

    CommitTransaction(transaction.get());
    CommitRow(row);

    fiber->Run();
    ASSERT_EQ(fiber->GetState(), EFiberState::Terminated);
}

///////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NTabletNode
} // namespace NYT
