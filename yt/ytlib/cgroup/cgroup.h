#pragma once

#include <ytlib/cgroup/statistics.pb.h>

#include <util/generic/stroka.h>

#include <vector>
#include <chrono>

namespace NYT {
namespace NCGroup {

////////////////////////////////////////////////////////////////////////////////

class TMemory;

class TEvent
    : private TNonCopyable
{
public:
    TEvent();
    ~TEvent();

    TEvent(TEvent&& other);

    bool Fired();

    void Clear();
    void Destroy();

    TEvent& operator=(TEvent&& other);

protected:
    TEvent(int eventFd, int fd = -1);

private:
    void Swap(TEvent& other);

    int EventFd_;
    int Fd_;
    bool Fired_;

    friend TMemory;
};

////////////////////////////////////////////////////////////////////////////////

std::vector<Stroka> GetSupportedCGroups();

void RemoveAllSubcgroups(const Stroka& path);

void RunKiller(const Stroka& processGroupPath);

void KillProcessGroup(const Stroka& processGroupPath);

////////////////////////////////////////////////////////////////////////////////

class TNonOwningCGroup
    : private TNonCopyable
{
public:
    explicit TNonOwningCGroup(const Stroka& fullPath);
    TNonOwningCGroup(const Stroka& type, const Stroka& name);

    void AddCurrentTask();

    void Set(const Stroka& name, const Stroka& value) const;

    std::vector<int> GetTasks() const;
    const Stroka& GetFullPath() const;

    void EnsureExistance();

protected:
    Stroka FullPath_;
};

////////////////////////////////////////////////////////////////////////////////

class TCGroup
    : public TNonOwningCGroup
{
protected:
    TCGroup(const Stroka& type, const Stroka& name);

public:
    ~TCGroup();

    void Create();
    void Destroy();

    bool IsCreated() const;

private:
    bool Created_;
};

////////////////////////////////////////////////////////////////////////////////

class TCpuAccounting
    : public TCGroup
{
public:
    struct TStatistics
    {
        TStatistics();

        TDuration UserTime;
        TDuration SystemTime;
    };

    explicit TCpuAccounting(const Stroka& name);
    TStatistics GetStatistics();
};

void ToProto(NProto::TCpuAccountingStatistics* protoStats, const TCpuAccounting::TStatistics& stats);

////////////////////////////////////////////////////////////////////////////////

class TBlockIO
    : public TCGroup
{
public:
    struct TStatistics
    {
        TStatistics();

        i64 TotalSectors;
        i64 BytesRead;
        i64 BytesWritten;
    };

    explicit TBlockIO(const Stroka& name);
    TStatistics GetStatistics();
};

void ToProto(NProto::TBlockIOStatistics* protoStats, const TBlockIO::TStatistics& stats);

////////////////////////////////////////////////////////////////////////////////

class TMemory
    : public TCGroup
{
public:
    struct TStatistics
    {
        TStatistics();

        i64 UsageInBytes;
    };

    explicit TMemory(const Stroka& name);
    TStatistics GetStatistics();

    void SetLimitInBytes(i64 bytes) const;
    void DisableOom() const;
    TEvent GetOomEvent() const;
};

////////////////////////////////////////////////////////////////////////////////

std::map<Stroka, Stroka> ParseCurrentProcessCGroups(TStringBuf str);

////////////////////////////////////////////////////////////////////////////////

} // namespace NCGroup
} // namespace NYT
