#pragma once

#include "ref.h"

#include <util/stream/input.h>
#include <util/stream/output.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

typedef ui64 TChecksum;

////////////////////////////////////////////////////////////////////////////////

TChecksum GetChecksum(TRef data);

////////////////////////////////////////////////////////////////////////////////

class TChecksummableInput
    : public TInputStream
{
public:
    TChecksummableInput(TInputStream& input);
    TChecksum GetChecksum() const;

protected:
    virtual size_t DoRead(void* buf, size_t len);
    
private:
    TInputStream& Input;
    TChecksum Checksum;
};

////////////////////////////////////////////////////////////////////////////////

class TChecksummableOutput
    : public TOutputStream
{
public:
    TChecksummableOutput(TOutputStream& output);
    TChecksum GetChecksum() const;

protected:
    virtual void DoWrite(const void* buf, size_t len);
    virtual void DoFlush();
    virtual void DoFinish();

private:
    TOutputStream& Output;
    TChecksum Checksum;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
