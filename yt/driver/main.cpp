
#include <ytlib/logging/log_manager.h>

#include <ytlib/profiling/profiling_manager.h>

#include <ytlib/misc/delayed_invoker.h>

#include <ytlib/driver/driver.h>

#include <ytlib/rpc/rpc_manager.h>

#include <ytlib/ytree/serialize.h>
#include <ytlib/ytree/yson_reader.h>

#include <ytlib/ytree/yson_parser.h>

#include <ytlib/misc/home.h>
#include <ytlib/misc/fs.h>
#include <ytlib/misc/errortrace.h>

#include <util/config/last_getopt.h>
#include <util/stream/pipe.h>

#ifdef _win_
#include <io.h>
#else
#include <unistd.h>
#include <errno.h>
#endif

#include "arguments.h"

namespace NYT {

using namespace NDriver;
using namespace NYTree;

static NLog::TLogger& Logger = DriverLogger;
static const char* UserConfigFileName = ".ytdriver.conf";
static const char* SystemConfigFileName = "ytdriver.conf";

static const char* SystemConfigPath = "/etc/";

////////////////////////////////////////////////////////////////////////////////

class TSystemInput
    : public TInputStream
{
public:
    TSystemInput(int handle)
        : Handle(handle)
    { }

private:
    int Handle;

    virtual size_t DoRead(void* buf, size_t len)
    {
        int result;
        do {
            result = read(Handle, buf, len);
        } while (result < 0 && errno == EINTR);
        

        if (result < 0) {
            ythrow yexception() << Sprintf("Error reading from stream (Handle: %d, Error: %d)",
                Handle,
                errno);
        }

        return result;
    }
};

////////////////////////////////////////////////////////////////////////////////

class TSystemOutput
    : public TOutputStream
{
public:
    TSystemOutput(int handle)
        : Handle(handle)
    { }

private:
    int Handle;

    virtual void DoWrite(const void* buf, size_t len)
    {
        size_t totalWritten = 0;
        while (totalWritten < len) {
            int result;
            do {
                result = write(Handle, static_cast<const char*>(buf) + totalWritten, len - totalWritten);
            } while (result < 0 && errno == EINTR);

            if (result == 0) {
                ythrow yexception() << Sprintf("Error writing to stream (Handle: %d, Error: nothing written)",
                    Handle);
            }
            if (result < 0 ) {
                ythrow yexception() << Sprintf("Error writing to stream (Handle: %d, Error: %d)",
                    Handle,
                    errno);
            }
            
            totalWritten += result;
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

class TStreamProvider
    : public IDriverStreamProvider
{
public:
    virtual TAutoPtr<TInputStream> CreateInputStream()
    {
        return new TSystemInput(0);
    }

    virtual TAutoPtr<TOutputStream> CreateOutputStream()
    {
        return new TSystemOutput(1);
    }

    virtual TAutoPtr<TOutputStream> CreateErrorStream()
    {
        return new TSystemOutput(2);
    }
};

////////////////////////////////////////////////////////////////////////////////

class TDriverProgram
{
public:
    struct TConfig
        : public TDriver::TConfig
    {
        INodePtr Logging;

        TConfig()
        {
            Register("logging", Logging);
        }
    };

    TDriverProgram()
        : ExitCode(0)
    {
        RegisterParser("start_tx", ~New<TStartTxArgs>());
        RegisterParser("commit_tx", ~New<TCommitTxArgs>());
        RegisterParser("abort_tx", ~New<TAbortTxArgs>());

        RegisterParser("get", ~New<TGetArgs>());
        RegisterParser("set", ~New<TSetArgs>());
        RegisterParser("remove", ~New<TRemoveArgs>());
        RegisterParser("list", ~New<TListArgs>());
        RegisterParser("create", ~New<TCreateArgs>());
        RegisterParser("lock", ~New<TLockArgs>());

        RegisterParser("download", ~New<TDownloadArgs>());
        RegisterParser("upload", ~New<TUploadArgs>());

        RegisterParser("read", ~New<TReadArgs>());
        RegisterParser("write", ~New<TWriteArgs>());
    }

    int Main(int argc, const char* argv[])
    {
        NYT::SetupErrorHandler();

        try {
            if (argc < 2) {
                PrintAllCommands();
                ythrow yexception() << "Not enough arguments";
            }
            Stroka commandName = Stroka(argv[1]);

            if (commandName == "--help") {
                PrintAllCommands();
                exit(0);
            }

            auto argsParser = GetArgsParser(commandName);

            std::vector<std::string> args;
            for (int i = 1; i < argc; ++i) {
                args.push_back(std::string(argv[i]));
            }

            argsParser->Parse(args);

            Stroka configFromCmd = argsParser->GetConfigName();
            Stroka configFromEnv = Stroka(getenv("YT_CONFIG"));
            Stroka userConfig = NFS::CombinePaths(GetHomePath(), UserConfigFileName);
            Stroka systemConfig = NFS::CombinePaths(SystemConfigPath, SystemConfigFileName);


            auto configName = configFromCmd;
            if (configName.empty()) {
                configName = configFromEnv;
                if (configName.empty()) {
                    configName = userConfig;
                    if (!isexist(~configName)) {
                        configName = systemConfig;
                        if (!isexist(~configName)) {
                            ythrow yexception() <<
                                Sprintf("Config wasn't found. Please specify it using on of the following:\n"
                                "commandline option --config\n"
                                "env YT_CONFIG\n"
                                "user file: %s\n"
                                "system file: %s",
                                ~userConfig, ~systemConfig);
                        }
                    }
                }
            }

            auto config = New<TConfig>();
            INodePtr configNode;
            try {
                TIFStream configStream(configName);
                configNode = DeserializeFromYson(&configStream);
            } catch (const std::exception& ex) {
                ythrow yexception() << Sprintf("Error reading configuration\n%s", ex.what());
            }

            argsParser->ApplyConfigUpdates(~configNode);

            try {
                config->Load(~configNode);
            } catch (const std::exception& ex) {
                ythrow yexception() << Sprintf("Error parsing configuration\n%s", ex.what());
            }

            NLog::TLogManager::Get()->Configure(~config->Logging);

            auto outputFormatFromCmd = argsParser->GetOutputFormat();
            if (outputFormatFromCmd) {
                config->OutputFormat = outputFormatFromCmd.Get();
            }

            Cout << config->OutputFormat.ToString() << Endl;

            Driver = new TDriver(~config, &StreamProvider);

            auto command = argsParser->GetCommand();
            RunCommand(command);

        } catch (const std::exception& ex) {
            Cerr << "Error occured: " << ex.what() << Endl;
            ExitCode = 1;
        }

        // TODO: refactor system shutdown
        NLog::TLogManager::Get()->Shutdown();
        NRpc::TRpcManager::Get()->Shutdown();
        NProfiling::TProfilingManager::Get()->Shutdown();
        TDelayedInvoker::Shutdown();

        return ExitCode;
    }

    void PrintAllCommands()
    {
        Cout << "Available commands: " << Endl;
        FOREACH (auto parserPair, GetSortedIterators(ArgsParsers)) {
            Cout << "   " << parserPair->first << Endl;
        }
    }

private:
    int ExitCode;

    TStreamProvider StreamProvider;
    TAutoPtr<TDriver> Driver;

    yhash_map<Stroka, TArgsBase::TPtr> ArgsParsers;

    void RegisterParser(const Stroka& name, TArgsBase* command)
    {
        YVERIFY(ArgsParsers.insert(MakePair(name, command)).second);
    }

    TArgsBase::TPtr GetArgsParser(Stroka command) {
        auto parserIt = ArgsParsers.find(command);
        if (parserIt == ArgsParsers.end()) {
            ythrow yexception() << Sprintf("Unknown command %s", ~command.Quote());
        }
        return parserIt->second;
    }

    void RunCommand(INodePtr command)
    {
        auto error = Driver->Execute(command);
        if (!error.IsOK()) {
            ExitCode = 1;
        }
    }
};

////////////////////////////////////////////////////////////////////////////////


} // namespace NYT

int main(int argc, const char* argv[])
{
    NYT::TDriverProgram program;
    return program.Main(argc, argv);
}

