#include "stdafx.h"

#include "framework.h"

#include <ytlib/cgroup/cgroup.h>

#ifdef _linux_
  #include <sys/wait.h>
  #include <unistd.h>
  #include <sys/eventfd.h>
#endif

namespace NYT {
namespace NCGroup {
namespace {

////////////////////////////////////////////////////////////////////////////////

TEST(CGroup, CreateDestroy)
{
    for (int i = 0; i < 2; ++i) {
        TBlockIO group("some");
        group.Create();
        group.Destroy();
    }
}

TEST(CGroup, NotExistingGroupGetTasks)
{
    TBlockIO group("wierd_name");
    EXPECT_THROW(group.GetTasks(), std::exception);
}

TEST(CGroup, DoubleCreate)
{
    TBlockIO group("wierd_name");
    group.Create();
    group.Create();
    group.Destroy();
}

TEST(CGroup, EmptyHasNoTasks)
{
    TBlockIO group("some2");
    group.Create();
    auto tasks = group.GetTasks();
    EXPECT_EQ(0, tasks.size());
    group.Destroy();
}

#ifdef _linux_

TEST(CGroup, AddCurrentProcess)
{
    TBlockIO group("some");
    group.Create();

    auto pid = fork();
    ASSERT_TRUE(pid >= 0);

    if (pid == 0) {
        group.AddCurrentProcess();
        auto tasks = group.GetTasks();
        ASSERT_EQ(1, tasks.size());
        EXPECT_EQ(getpid(), tasks[0]);
        exit(0);
    }

    auto waitedpid = waitpid(pid, nullptr, 0);

    group.Destroy();

    ASSERT_EQ(pid, waitedpid);
}

TEST(CGroup, GetCpuAccStat)
{
    TCpuAccounting group("some");
    group.Create();

    auto stats = group.GetStatistics();
    EXPECT_EQ(0, stats.UserTime.MilliSeconds());
    EXPECT_EQ(0, stats.SystemTime.MilliSeconds());

    group.Destroy();
}

TEST(CGroup, GetBlockIOStat)
{
    TBlockIO group("some");
    group.Create();

    auto stats = group.GetStatistics();
    EXPECT_EQ(0, stats.BytesRead);
    EXPECT_EQ(0, stats.BytesWritten);
    EXPECT_EQ(0, stats.TotalSectors);

    group.Destroy();
}

TEST(CGroup, GetMemoryStats)
{
    TMemory group("some");
    group.Create();

    auto stats = group.GetStatistics();
    EXPECT_EQ(0, stats.TotalUsageInBytes);

    group.Destroy();
}

TEST(CurrentProcessCGroup, Empty)
{
    std::vector<char> empty;
    auto result = ParseCurrentProcessCGroups(TStringBuf(empty.data(), empty.size()));
    EXPECT_TRUE(result.empty());
}

TEST(CurrentProcessCGroup, Basic)
{
    auto basic = STRINGBUF("4:blkio:/\n3:cpuacct:/\n2:freezer:/some\n1:memory:/\n");
    auto result = ParseCurrentProcessCGroups(TStringBuf(basic.data(), basic.length()));
    EXPECT_EQ("", result["blkio"]);
    EXPECT_EQ("", result["cpuacct"]);
    EXPECT_EQ("some", result["freezer"]);
    EXPECT_EQ("", result["memory"]);
    EXPECT_EQ(4, result.size());
}

TEST(CurrentProcessCGroup, Multiple)
{
    auto basic = STRINGBUF("5:cpuacct,cpu,cpuset:/daemons\n");
    auto result = ParseCurrentProcessCGroups(TStringBuf(basic.data(), basic.length()));
    EXPECT_EQ("daemons", result["cpu"]);
    EXPECT_EQ("daemons", result["cpuset"]);
    EXPECT_EQ("daemons", result["cpuacct"]);
    EXPECT_EQ(3, result.size());
}

TEST(CurrentProcessCGroup, BadInput)
{
    auto basic = STRINGBUF("xxx:cpuacct,cpu,cpuset:/daemons\n");
    EXPECT_THROW(ParseCurrentProcessCGroups(TStringBuf(basic.data(), basic.length())), std::exception);
}

class TEvent : public NCGroup::TEvent
{
public:
    TEvent(int eventFd, int fd = -1)
        : NCGroup::TEvent(eventFd, fd)
    { }
};

TEST(TEvent, Fired)
{
    auto eventFd = eventfd(0, EFD_NONBLOCK);
    TEvent event(eventFd, -1);

    EXPECT_FALSE(event.Fired());

    i64 value = 1;
    write(eventFd, &value, sizeof(value));

    EXPECT_TRUE(event.Fired());
}

TEST(TEvent, Stiky)
{
    auto eventFd = eventfd(0, EFD_NONBLOCK);
    TEvent event(eventFd, -1);

    i64 value = 1;
    write(eventFd, &value, sizeof(value));

    EXPECT_TRUE(event.Fired());
    EXPECT_TRUE(event.Fired());
}

TEST(TEvent, Clear)
{
    auto eventFd = eventfd(0, EFD_NONBLOCK);
    TEvent event(eventFd, -1);

    i64 value = 1;
    write(eventFd, &value, sizeof(value));

    EXPECT_TRUE(event.Fired());
    event.Clear();
    EXPECT_FALSE(event.Fired());
}

#endif // _linux_

////////////////////////////////////////////////////////////////////////////////

}
} // namespace NCGroup
} // namespace NYT
