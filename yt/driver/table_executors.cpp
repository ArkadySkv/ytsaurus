#include "table_executors.h"
#include "preprocess.h"

#include <ytlib/driver/driver.h>

namespace NYT {
namespace NDriver {

using namespace NYTree;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

TReadExecutor::TReadExecutor()
    : PathArg("path", "table path to read", true, "", "YPATH")
{
    CmdLine.add(PathArg);
}

void TReadExecutor::BuildArgs(IYsonConsumer* consumer)
{
    auto path = PreprocessYPath(PathArg.getValue());

    BuildYsonMapFluently(consumer)
        .Item("do").Scalar("read")
        .Item("path").Scalar(path);

    TTransactedExecutor::BuildArgs(consumer);
}

Stroka TReadExecutor::GetCommandName() const
{
    return "read";
}

//////////////////////////////////////////////////////////////////////////////////

TWriteExecutor::TWriteExecutor()
    : PathArg("path", "table path to write", true, "", "YPATH")
    , ValueArg("value", "row(s) to write", false, "", "YSON")
    , SortedBy("", "sorted_by", "key columns names (for sorted write)", false, "", "YSON_LIST_FRAGMENT")
    , UseStdIn(true)
{
    CmdLine.add(PathArg);
    CmdLine.add(ValueArg);
    CmdLine.add(SortedBy);
}

void TWriteExecutor::BuildArgs(IYsonConsumer* consumer)
{
    auto path = PreprocessYPath(PathArg.getValue());
    auto sortedBy = ConvertTo< std::vector<Stroka> >(TYsonString(SortedBy.getValue(), EYsonType::ListFragment));

    const auto& value = ValueArg.getValue();
    if (!value.empty()) {
        Stream.Write(value);
        UseStdIn = false;
    }

    if (!sortedBy.empty()) {
        path.Attributes().Set("sorted_by", sortedBy);
    }

    BuildYsonMapFluently(consumer)
        .Item("do").Scalar("write")
        .Item("path").Scalar(path);

    TTransactedExecutor::BuildArgs(consumer);
}

TInputStream* TWriteExecutor::GetInputStream()
{
    return UseStdIn ? &StdInStream() : &Stream;
}

Stroka TWriteExecutor::GetCommandName() const
{
    return "write";
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDriver
} // namespace NYT
