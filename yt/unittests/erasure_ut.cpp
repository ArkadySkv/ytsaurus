#include <ytlib/misc/foreach.h>

#include <ytlib/erasure/codec.h>

#include <ytlib/chunk_client/file_reader.h>
#include <ytlib/chunk_client/file_writer.h>
#include <ytlib/chunk_client/erasure_writer.h>
#include <ytlib/chunk_client/erasure_reader.h>
#include <ytlib/chunk_client/config.h>

#include <contrib/testing/framework.h>

#include <util/random/randcpp.h>
#include <util/system/fs.h>
#include <util/stream/file.h>

////////////////////////////////////////////////////////////////////////////////

using NYT::TRef;
using NYT::TSharedRef;

using namespace NYT::NChunkClient;
using namespace NYT::NErasure;

Stroka ToString(TSharedRef ref)
{
    return NYT::ToString(TRef(ref));
}

////////////////////////////////////////////////////////////////////////////////

class TErasureCodingTest
    : public ::testing::Test
{ };

TEST_F(TErasureCodingTest, RandomText)
{
    TRand rand;

    std::map<ECodec::EDomain, int> guaranteedRecoveryCount;
    guaranteedRecoveryCount[ECodec::ReedSolomon] = 3;
    guaranteedRecoveryCount[ECodec::Lrc] = 3;

    std::vector<char> data;
    for (int i = 0; i < 16 * 64; ++i) {
        data.push_back(static_cast<char>('a' + (std::abs(rand.random()) % 26)));
    }

    FOREACH (auto codecId, ECodec::GetDomainValues()) {
        if (codecId == ECodec::None) {
            continue;
        }

        auto codec = GetCodec(codecId);

        int blocksCount = codec->GetDataBlockCount() + codec->GetParityBlockCount();
        YCHECK(blocksCount <= 16);

        std::vector<TSharedRef> dataBlocks;
        for (int i = 0; i < codec->GetDataBlockCount(); ++i) {
            char* begin = data.data() + i * 64;
            NYT::TBlob blob(begin, 64);
            dataBlocks.push_back(TSharedRef::FromBlob(std::move(blob)));
        }


        auto parityBlocks = codec->Encode(dataBlocks);

        std::vector<TSharedRef> allBlocks(dataBlocks);
        std::copy(parityBlocks.begin(), parityBlocks.end(), std::back_inserter(allBlocks));

        for (int mask = 0; mask < (1 << blocksCount); ++mask) {
            TBlockIndexList erasedIndices;
            for (int i = 0; i < blocksCount; ++i) {
                if ((mask & (1 << i)) > 0) {
                    erasedIndices.push_back(i);
                }
            }
            if (erasedIndices.size() == 1) continue;

            auto recoveryIndices = codec->GetRepairIndices(erasedIndices);
            ASSERT_EQ(static_cast<bool>(recoveryIndices), codec->CanRepair(erasedIndices));
            if (erasedIndices.size() <= guaranteedRecoveryCount[codecId]) {
                EXPECT_TRUE(recoveryIndices);
            }


            if (recoveryIndices) {
                std::vector<TSharedRef> aliveBlocks;
                for (int i = 0; i < recoveryIndices->size(); ++i) {
                    aliveBlocks.push_back(allBlocks[(*recoveryIndices)[i]]);
                }
                std::vector<TSharedRef> recoveredBlocks = codec->Decode(aliveBlocks, erasedIndices);
                EXPECT_TRUE(recoveredBlocks.size() == erasedIndices.size());
                for (int i = 0; i < erasedIndices.size(); ++i) {
                    EXPECT_EQ(
                        ToString(allBlocks[erasedIndices[i]]),
                        ToString(recoveredBlocks[i]));
                }
            }
        }

    }
}

class TErasureMixture
    : public ::testing::Test
{
public:
    static std::vector<TSharedRef> ToSharedRefs(const std::vector<Stroka>& strings)
    {
        std::vector<TSharedRef> refs;
        FOREACH (const auto& str, strings) {
            refs.push_back(TSharedRef::FromString(str));
        }
        return refs;
    }

    static void WriteErasureChunk(ICodec* codec, std::vector<TSharedRef> data)
    {
        auto config = NYT::New<TErasureWriterConfig>();
        config->ErasureWindowSize = 64;

        std::vector<IAsyncWriterPtr> writers;
        for (int i = 0; i < codec->GetTotalBlockCount(); ++i) {
            Stroka filename = "block" + ToString(i + 1);
            writers.push_back(NYT::New<TFileWriter>(filename));
        }

        FOREACH (auto writer, writers) {
            writer->Open();
        }

        NProto::TChunkMeta meta;
        meta.set_type(1);
        meta.set_version(1);

        auto erasureWriter = CreateErasureWriter(config, codec, writers);
        FOREACH (const auto& ref, data) {
            erasureWriter->TryWriteBlock(ref);
        }
        erasureWriter->AsyncClose(meta).Get();

        FOREACH (auto writer, writers) {
            EXPECT_TRUE(writer->AsyncClose(meta).Get().IsOK());
        }
    }

    static IAsyncReaderPtr CreateErasureReader(ICodec* codec)
    {
        std::vector<IAsyncReaderPtr> readers;
        for (int i = 0; i < codec->GetDataBlockCount(); ++i) {
            Stroka filename = "block" + ToString(i + 1);
            auto reader = NYT::New<TFileReader>(filename);
            reader->Open();
            readers.push_back(reader);
        }
        return CreateNonReparingErasureReader(readers);
    }

    static void Cleanup(ICodec* codec)
    {
        for (int i = 0; i < codec->GetTotalBlockCount(); ++i) {
            Stroka filename = "block" + ToString(i + 1);
            NFs::Remove(filename.c_str());
            NFs::Remove((filename + ".meta").c_str());
        }
    }
};

TEST_F(TErasureMixture, WriterTest)
{
    auto codec = GetCodec(ECodec::Lrc);

    // Prepare data
    Stroka data[] = {
        "a",
        "b",
        "",
        "Hello world"};
    std::vector<Stroka> dataStrings(data, data + sizeof(data) / sizeof(Stroka));
    auto dataRefs = ToSharedRefs(dataStrings);

    WriteErasureChunk(codec, dataRefs);

    // Manually check that data in files is correct
    for (int i = 0; i < codec->GetTotalBlockCount(); ++i) {
        Stroka filename = "block" + ToString(i + 1);
        if (i == 0) {
            EXPECT_EQ(Stroka("ab"), TFileInput("block" + ToString(i + 1)).ReadAll());
        } else if (i == 1) {
            EXPECT_EQ(Stroka("Hello world"), TFileInput("block" + ToString(i + 1)).ReadAll());
        } else if (i < 12) {
            EXPECT_EQ("", TFileInput("block" + ToString(i + 1)).ReadAll());
        } else {
            EXPECT_EQ(64, TFileInput("block" + ToString(i + 1)).ReadAll().Size());
        }
    }

    Cleanup(codec);
}

TEST_F(TErasureMixture, ReaderTest)
{
    auto codec = GetCodec(ECodec::Lrc);

    // Prepare data
    Stroka data[] = {
        "a",
        "b",
        "",
        "Hello world"};
    std::vector<Stroka> dataStrings(data, data + sizeof(data) / sizeof(Stroka));
    auto dataRefs = ToSharedRefs(dataStrings);

    WriteErasureChunk(codec, dataRefs);

    auto erasureReader = CreateErasureReader(codec);

    {
        // Check blocks separately
        int index = 0;
        FOREACH (const auto& ref, dataRefs) {
            auto result = erasureReader->AsyncReadBlocks(std::vector<int>(1, index++)).Get();
            EXPECT_TRUE(result.IsOK());
            auto resultRef = result.GetOrThrow().front();

            EXPECT_EQ(ToString(ref), ToString(resultRef));
        }
    }

    {
        // Check some non-trivial read request
        std::vector<int> indices;
        indices.push_back(1);
        indices.push_back(3);
        auto result = erasureReader->AsyncReadBlocks(indices).Get();
        EXPECT_TRUE(result.IsOK());
        auto resultRef = result.GetOrThrow();
        EXPECT_EQ(ToString(dataRefs[1]), ToString(resultRef[0]));
        EXPECT_EQ(ToString(dataRefs[3]), ToString(resultRef[1]));
    }

    Cleanup(codec);
}

TEST_F(TErasureMixture, RepairTest)
{
    auto codec = GetCodec(ECodec::Lrc);

    // Prepare data
    Stroka data[] = {
        "a",
        "b",
        "",
        "Hello world"};
    std::vector<Stroka> dataStrings(data, data + sizeof(data) / sizeof(Stroka));
    auto dataRefs = ToSharedRefs(dataStrings);

    WriteErasureChunk(codec, dataRefs);

    TBlockIndexList erasedIndices;
    erasedIndices.push_back(0);
    erasedIndices.push_back(13);

    std::set<int> erasedIndicesSet(erasedIndices.begin(), erasedIndices.end());

    auto repairIndices = *codec->GetRepairIndices(erasedIndices);
    std::set<int> repairIndicesSet(repairIndices.begin(), repairIndices.end());

    for (int i = 0; i < erasedIndices.size(); ++i) {
        Stroka filename = "block" + ToString(erasedIndices[i] + 1);
        NFs::Remove(filename.c_str());
        NFs::Remove((filename + ".meta").c_str());
    }

    std::vector<IAsyncReaderPtr> readers;
    std::vector<IAsyncWriterPtr> writers;
    for (int i = 0; i < codec->GetTotalBlockCount(); ++i) {
        Stroka filename = "block" + ToString(i + 1);
        if (erasedIndicesSet.find(i) != erasedIndicesSet.end()) {
            writers.push_back(NYT::New<TFileWriter>(filename));
        }
        if (repairIndicesSet.find(i) != repairIndicesSet.end()) {
            auto reader = NYT::New<TFileReader>(filename);
            reader->Open();
            readers.push_back(reader);
        }
    }

    RepairErasedBlocks(codec, erasedIndices, readers, writers).Get();

    auto erasureReader = CreateErasureReader(codec);

    int index = 0;
    FOREACH (const auto& ref, dataRefs) {
        auto result = erasureReader->AsyncReadBlocks(std::vector<int>(1, index++)).Get();
        EXPECT_TRUE(result.IsOK());
        auto resultRef = result.GetOrThrow().front();

        EXPECT_EQ(ToString(ref), ToString(resultRef));
    }

    Cleanup(codec);
}

TEST_F(TErasureMixture, RepairTestWithSeveralWindows)
{
    TRand rand;

    auto codec = GetCodec(ECodec::Lrc);

    // Prepare data
    std::vector<TSharedRef> dataRefs;
    for (int i = 0; i < 20; ++i) {
        NYT::TBlob data(100);
        for (int i = 0; i < 100; ++i) {
            data[i] = static_cast<char>('a' + (std::abs(rand.random()) % 26));
        }
        dataRefs.push_back(TSharedRef::FromBlob(std::move(data)));
    }
    WriteErasureChunk(codec, dataRefs);

    { // Check reader
        auto erasureReader = CreateErasureReader(codec);
        for (int i = 0; i < dataRefs.size(); ++i ) {
            auto result = erasureReader->AsyncReadBlocks(std::vector<int>(1, i)).Get();
            EXPECT_TRUE(result.IsOK());

            auto resultRef = result.Value().front();
            EXPECT_EQ(ToString(dataRefs[i]), ToString(resultRef));
        }
    }

    TBlockIndexList erasedIndices;
    erasedIndices.push_back(1);
    erasedIndices.push_back(8);
    erasedIndices.push_back(13);
    erasedIndices.push_back(15);
    std::set<int> erasedIndicesSet(erasedIndices.begin(), erasedIndices.end());

    auto repairIndices = *codec->GetRepairIndices(erasedIndices);
    std::set<int> repairIndicesSet(repairIndices.begin(), repairIndices.end());

    for (int i = 0; i < erasedIndices.size(); ++i) {
        Stroka filename = "block" + ToString(erasedIndices[i] + 1);
        NFs::Remove(filename.c_str());
        NFs::Remove((filename + ".meta").c_str());
    }

    std::vector<IAsyncReaderPtr> readers;
    std::vector<IAsyncWriterPtr> writers;
    for (int i = 0; i < codec->GetTotalBlockCount(); ++i) {
        Stroka filename = "block" + ToString(i + 1);
        if (erasedIndicesSet.find(i) != erasedIndicesSet.end()) {
            writers.push_back(NYT::New<TFileWriter>(filename));
        }
        if (repairIndicesSet.find(i) != repairIndicesSet.end()) {
            auto reader = NYT::New<TFileReader>(filename);
            reader->Open();
            readers.push_back(reader);
        }
    }

    RepairErasedBlocks(codec, erasedIndices, readers, writers).Get();

    { // Check reader
        auto erasureReader = CreateErasureReader(codec);
        for (int i = 0; i < dataRefs.size(); ++i ) {
            auto result = erasureReader->AsyncReadBlocks(std::vector<int>(1, i)).Get();
            EXPECT_TRUE(result.IsOK());

            auto resultRef = result.Value().front();
            EXPECT_EQ(dataRefs[i].Size(), resultRef.Size());
            EXPECT_EQ(ToString(dataRefs[i]), ToString(resultRef));
        }
    }

    Cleanup(codec);
}
