#pragma once

#include "common.h"
#include "pattern.h"

#include <ytlib/misc/ref_counted.h>
#include <ytlib/ytree/yson_serializable.h>

#include <util/system/file.h>
#include <util/stream/file.h>

namespace NYT {
namespace NLog {

////////////////////////////////////////////////////////////////////////////////

extern const char* const SystemLoggingCategory;

////////////////////////////////////////////////////////////////////////////////

struct ILogWriter
    : public virtual TRefCounted
{
    virtual void Write(const TLogEvent& event) = 0;
    virtual void Flush() = 0;
    virtual void Reload() = 0;

    DECLARE_ENUM(EType,
        (File)
        (StdOut)
        (StdErr)
        (Raw)
    );

    struct TConfig
        : public TYsonSerializable
    {
        typedef TIntrusivePtr<TConfig> TPtr;

        EType Type;
        Stroka Pattern;
        Stroka FileName;

        TConfig()
        {
            Register("type", Type);
            Register("pattern", Pattern)
                .Default()
                .CheckThat(BIND([] (const Stroka& pattern) {
                    auto error = ValidatePattern(pattern);
                    if (!error.IsOK()) {
                        THROW_ERROR error;
                    }
                }));
            Register("file_name", FileName).Default();
        }

        virtual void DoValidate() const override
        {
            if ((Type == EType::File || Type == EType::Raw) && FileName.empty()) {
                THROW_ERROR_EXCEPTION("FileName is empty while type is File");
            } else if (Type != EType::File && Type != EType::Raw && !FileName.empty()) {
                THROW_ERROR_EXCEPTION("FileName is not empty while type is not File");
            }
            if (Type != EType::Raw && Pattern.empty()) {
                THROW_ERROR_EXCEPTION("Pattern is empty while type is not Raw");
            }
        }
    };
};

////////////////////////////////////////////////////////////////////////////////

class TStreamLogWriter
    : public ILogWriter
{
public:
    TStreamLogWriter(
        TOutputStream* stream,
        Stroka pattern);

    virtual void Write(const TLogEvent& event);
    virtual void Flush();
    virtual void Reload();

private:
    TOutputStream* Stream;
    Stroka Pattern;

};

////////////////////////////////////////////////////////////////////////////////

class TStdErrLogWriter
    : public TStreamLogWriter
{
public:
    explicit TStdErrLogWriter(const Stroka& pattern);

};

////////////////////////////////////////////////////////////////////////////////

class TStdOutLogWriter
    : public TStreamLogWriter
{
public:
    explicit TStdOutLogWriter(const Stroka& pattern);

};

////////////////////////////////////////////////////////////////////////////////

class TFileLogWriter
    : public ILogWriter
{
public:
    TFileLogWriter(
        Stroka fileName,
        Stroka pattern);

    virtual void Write(const TLogEvent& event);
    virtual void Flush();
    virtual void Reload();

private:
    static const size_t BufferSize = 1 << 16;

    void EnsureInitialized();

    Stroka FileName;
    Stroka Pattern;
    bool Initialized;
    THolder<TFile> File;
    THolder<TBufferedFileOutput> FileOutput;
    ILogWriterPtr LogWriter;

};

////////////////////////////////////////////////////////////////////////////////

class TRawFileLogWriter
    : public ILogWriter
{
public:
    explicit TRawFileLogWriter(const Stroka& fileName);

    virtual void Write(const TLogEvent& event);
    virtual void Flush();
    virtual void Reload();

private:
    static const size_t BufferSize = 1 << 16;

    void EnsureInitialized();

    Stroka FileName;
    bool Initialized;
    THolder<TMessageBuffer> Buffer;
    THolder<TFile> File;
    THolder<TBufferedFileOutput> FileOutput;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NLog
} // namespace NYT
