#pragma once

#include "common.h"
#include "chunk.pb.h"

#include <ytlib/misc/common.h>
#include <ytlib/misc/enum.h>
#include <ytlib/misc/ref.h>
#include <ytlib/actions/future.h>
#include <ytlib/misc/error.h>
#include <ytlib/chunk_server/chunk_ypath_proxy.h>

namespace NYT {
namespace NChunkClient {

///////////////////////////////////////////////////////////////////////////////

//! Provides a basic interface for uploading chunks to holders.
/*!
 *  The client must feed the blocks one after another with #AddBlock method.
 *  It must call #Close to finish the session.
 *  An implementation may provide a buffering window (queue) to enable concurrent upload to
 *  multiple destinations using torrent or chaining strategies.
 */
struct IAsyncWriter
    : public virtual TRefCounted
{
    typedef TIntrusivePtr<IAsyncWriter> TPtr;

    //! Starts a new upload session.
    virtual void Open() = 0;

    //! Called when the client wants to upload a new block.
    /*!
     *  Subsequent calls to #AsyncWriteBlock or #AsyncClose are
     *  prohibited until the returned result is set.
     *  If the result indicates some error then the whole upload session is failed.
     *  (e.g. all chunk-holders are down).
     *  The client shouldn't retry writing the same block again.
     */
    virtual TAsyncError::TPtr AsyncWriteBlock(const TSharedRef& data) = 0;

    //! Called when the client has added all the blocks and is 
    //! willing to finalize the upload.
    /*!
     *  The call completes immediately but returns a result that gets
     *  set when the session is complete.
     *  
     *  Should be called only once.
     *  Calling #AsyncWriteBlock afterwards is an error.
     */
    virtual TAsyncError::TPtr AsyncClose(const NChunkHolder::NProto::TChunkAttributes& attributes) = 0;

    //! Returns the id of the chunk being written.
    virtual TChunkId GetChunkId() const = 0;

    //! Returns the confirmation request for the uploaded chunk.
    /*!
     *  This method call only be called when the writer is successfully closed.
     *  
     * \note Thread affinity: ClientThread.
     */
    virtual NChunkServer::TChunkYPathProxy::TReqConfirm::TPtr GetConfirmRequest()
    {
        // Implement in descendants that imply remote writer semantics.
        YUNIMPLEMENTED();
    }
};

///////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
