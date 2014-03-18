#include "stdafx.h"
#include "framework.h"

#include <core/misc/process.h>

namespace NYT {
namespace {

////////////////////////////////////////////////////////////////////////////////

#ifndef _win_

TEST(TProcessTest, Basic)
{
    TProcess p("/bin/ls");
    auto error = p.Spawn();
    ASSERT_TRUE(error.IsOK());
    p.Wait();
}

TEST(TProcessTest, InvalidPath)
{
    TProcess p("/some/bad/path/binary");
    auto error = p.Spawn();

    ASSERT_TRUE(error.IsOK());
    error = p.Wait();

    EXPECT_FALSE(error.IsOK());
}

TEST(TProcessTest, ProcessReturnCode0)
{
    TProcess p("/bin/true");

    auto error = p.Spawn();
    ASSERT_TRUE(error.IsOK());

    error = p.Wait();
    ASSERT_TRUE(error.IsOK()) << ToString(error);
}

TEST(TProcessTest, ProcessReturnCode1)
{
    TProcess p("/bin/false");

    auto error = p.Spawn();
    ASSERT_TRUE(error.IsOK());

    error = p.Wait();
    ASSERT_FALSE(error.IsOK()) << ToString(error);
}

TEST(TProcessTest, Params1)
{
    TProcess p("/bin/bash");
    p.AddArgument("-c");
    p.AddArgument("if test 3 -gt 1; then exit 7; fi");

    auto error = p.Spawn();
    ASSERT_TRUE(error.IsOK());

    error = p.Wait();
    EXPECT_FALSE(error.IsOK());
}

TEST(TProcessTest, Params2)
{
    TProcess p("/bin/bash");
    p.AddArgument("-c");
    p.AddArgument("if test 1 -gt 3; then exit 7; fi");

    auto error = p.Spawn();
    ASSERT_TRUE(error.IsOK());

    error = p.Wait();
    EXPECT_TRUE(error.IsOK()) << ToString(error);
}

TEST(TProcessTest, InheritEnvironment)
{
    const char* name = "SPAWN_TEST_ENV_VAR";
    const char* value = "42";
    setenv(name, value, 1);

    TProcess p("/bin/bash");
    p.AddArgument("-c");
    p.AddArgument("if test $SPAWN_TEST_ENV_VAR = 42; then exit 7; fi");

    auto error = p.Spawn();
    ASSERT_TRUE(error.IsOK());

    error = p.Wait();
    EXPECT_FALSE(error.IsOK());

    unsetenv(name);
}

#endif

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYT
