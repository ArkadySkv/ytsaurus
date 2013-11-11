﻿#include "stdafx.h"
#include "chunk_meta_extensions.h"

namespace NYT {
namespace NChunkClient {

using namespace NChunkClient::NProto;

///////////////////////////////////////////////////////////////////////////////

TChunkMeta FilterChunkMetaExtensions(const TChunkMeta& chunkMeta, const std::vector<int>& tags)
{
    // ToDo: use FilterProtoExtensions.
    TChunkMeta result;
    result.set_type(chunkMeta.type());
    result.set_version(chunkMeta.version());

    yhash_set<int> tagsSet(tags.begin(), tags.end());

    for (const auto& extension : chunkMeta.extensions().extensions()) {
        if (tagsSet.find(extension.tag()) != tagsSet.end()) {
            *result.mutable_extensions()->add_extensions() = extension;
        }
    }

    return result;
}

///////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
