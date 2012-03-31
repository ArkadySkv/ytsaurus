#include "stdafx.h"
#include "file_commands.h"

#include <ytlib/file_client/file_reader.h>
#include <ytlib/file_client/file_writer.h>

namespace NYT {
namespace NDriver {

using namespace NFileClient;

////////////////////////////////////////////////////////////////////////////////

void TDownloadCommand::DoExecute(TDownloadRequestPtr request)
{
    auto config = Host->GetConfig()->FileReader;

    auto reader = New<TFileReader>(
        ~config,
        ~Host->GetMasterChannel(),
        ~Host->GetTransaction(request),
        ~Host->GetBlockCache(),
        Host->PreprocessYPath(request->Path));
    reader->Open();

    // TODO(babenko): use FileName and Executable values

    auto output = Host->CreateOutputStream();

    while (true) {
        auto block = reader->Read();
        if (!block) {
            break;
        }
        output->Write(block.Begin(), block.Size());
    }
}

////////////////////////////////////////////////////////////////////////////////

void TUploadCommand::DoExecute(TUploadRequestPtr request)
{
    auto config = Host->GetConfig()->FileWriter;

    auto writer = New<TFileWriter>(
        ~config,
        ~Host->GetMasterChannel(),
        ~Host->GetTransaction(request),
        ~Host->GetTransactionManager(),
        Host->PreprocessYPath(request->Path));
    writer->Open();

    auto input = Host->CreateInputStream();
    
    TBlob buffer(config->BlockSize);
    while (true) {
        size_t bytesRead = input->Read(buffer.begin(), buffer.size());
        if (bytesRead == 0)
            break;
        TRef block(buffer.begin(), bytesRead);
        writer->Write(block);
    }

    writer->Close();

    auto id = writer->GetNodeId();
    BuildYsonFluently(~Host->CreateOutputConsumer())
        .BeginMap()
            .Item("object_id").Scalar(id.ToString())
        .EndMap();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDriver
} // namespace NYT
