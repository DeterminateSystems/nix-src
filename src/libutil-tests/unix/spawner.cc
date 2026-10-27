#include <gtest/gtest.h>

#include "nix/util/file-descriptor.hh"
#include "nix/util/processes.hh"
#include "nix/util/spawner.hh"

#include <thread>

#include <sys/wait.h>

#ifdef __linux__
#  include <sched.h>
#endif

namespace nix {

static RegisterSpawnOp testExitOp("test-exit", [](Source & args, std::vector<AutoCloseFD> fds) {
    _exit(readNum<int>(args));
});

static RegisterSpawnOp testWriteOp("test-write", [](Source & args, std::vector<AutoCloseFD> fds) {
    auto s = readString(args);
    for (auto & fd : fds)
        writeFull(fd.get(), s);
});

static RegisterSpawnOp testVerbosityOp("test-verbosity", [](Source & args, std::vector<AutoCloseFD> fds) {
    writeFull(fds.at(0).get(), std::to_string((int) verbosity));
});

static RegisterSpawnOp testSleepOp("test-sleep", [](Source & args, std::vector<AutoCloseFD> fds) {
    while (true)
        pause();
});

static RegisterSpawnOp testThrowOp("test-throw", [](Source & args, std::vector<AutoCloseFD> fds) {
    throw Error("this is an error");
});

TEST(Spawner, exitStatus)
{
    for (int code : {0, 1, 42}) {
        auto pid = testExitOp.spawn([&](Sink & args) { args << (uint64_t) code; });
        auto status = pid.wait();
        ASSERT_TRUE(WIFEXITED(status));
        ASSERT_EQ(WEXITSTATUS(status), code);
    }
}

TEST(Spawner, passFds)
{
    Pipe pipe1, pipe2;
    pipe1.create();
    pipe2.create();

    std::array<Descriptor, 2> fds{pipe1.writeSide.get(), pipe2.writeSide.get()};
    auto pid = testWriteOp.spawn([&](Sink & args) { args << "hello"; }, fds);

    pipe1.writeSide.close();
    pipe2.writeSide.close();

    ASSERT_EQ(drainFD(pipe1.readSide.get()), "hello");
    ASSERT_EQ(drainFD(pipe2.readSide.get()), "hello");
    ASSERT_TRUE(statusOk(pid.wait()));
}

TEST(Spawner, verbosity)
{
    auto oldVerbosity = verbosity;
    verbosity = lvlDebug;

    Pipe pipe;
    pipe.create();
    std::array<Descriptor, 1> fds{pipe.writeSide.get()};
    auto pid = testVerbosityOp.spawn([&](Sink & args) {}, fds);
    pipe.writeSide.close();

    verbosity = oldVerbosity;

    ASSERT_EQ(drainFD(pipe.readSide.get()), std::to_string((int) lvlDebug));
    ASSERT_TRUE(statusOk(pid.wait()));
}

TEST(Spawner, kill)
{
    auto pid = testSleepOp.spawn([&](Sink & args) {});
    ASSERT_TRUE(pid.isAlive());
    auto status = pid.kill();
    ASSERT_TRUE(WIFSIGNALED(status));
    ASSERT_EQ(WTERMSIG(status), SIGKILL);
}

TEST(Spawner, killProcessGroup)
{
    auto pid = testSleepOp.spawn([&](Sink & args) {}, {}, {.setSid = true});
    pid.setSeparatePG(true);
    pid.setKillSignal(SIGTERM);
    auto status = pid.kill();
    ASSERT_TRUE(WIFSIGNALED(status));
    ASSERT_EQ(WTERMSIG(status), SIGTERM);
}

TEST(Spawner, isAlive)
{
    auto pid = testExitOp.spawn([&](Sink & args) { args << (uint64_t) 0; });
    while (pid.isAlive())
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_EQ((pid_t) pid, -1);
}

TEST(Spawner, throwingOp)
{
    auto pid = testThrowOp.spawn([&](Sink & args) {});
    auto status = pid.wait();
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 1);
}

TEST(Spawner, chdir)
{
    auto pid = testExitOp.spawn(
        [&](Sink & args) { args << (uint64_t) 0; }, {}, {.chdir = std::filesystem::path("/no-such-directory")});
    auto status = pid.wait();
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 1);
}

#ifdef __linux__
TEST(Spawner, spawnFailure)
{
    /* CLONE_NEWUSER and CLONE_FS can't be combined, so clone() fails
       in the spawner. */
    ASSERT_THROW(
        testExitOp.spawn([&](Sink & args) { args << (uint64_t) 0; }, {}, {.linuxCloneFlags = CLONE_NEWUSER | CLONE_FS}),
        Error);

    /* The spawner should still work afterwards. */
    auto pid = testExitOp.spawn([&](Sink & args) { args << (uint64_t) 3; });
    ASSERT_EQ(WEXITSTATUS(pid.wait()), 3);
}
#endif

TEST(Spawner, detach)
{
    {
        auto pid = testExitOp.spawn([&](Sink & args) { args << (uint64_t) 0; });
        pid.release();
    }

    {
        auto pid = testSleepOp.spawn([&](Sink & args) {});
        auto p = pid.release();
        ::kill(p, SIGKILL);
    }

    auto pid = testExitOp.spawn([&](Sink & args) { args << (uint64_t) 5; });
    ASSERT_EQ(WEXITSTATUS(pid.wait()), 5);
}

TEST(Spawner, concurrent)
{
    std::vector<std::thread> threads;
    std::atomic<int> failures{0};

    for (int t = 0; t < 8; ++t)
        threads.emplace_back([&, t]() {
            try {
                for (int i = 0; i < 20; ++i) {
                    int code = (t * 20 + i) % 200;
                    auto pid = testExitOp.spawn([&](Sink & args) { args << (uint64_t) code; });
                    auto status = pid.wait();
                    if (!WIFEXITED(status) || WEXITSTATUS(status) != code)
                        failures++;
                }
            } catch (...) {
                failures++;
            }
        });

    for (auto & thread : threads)
        thread.join();

    ASSERT_EQ(failures, 0);
}

} // namespace nix
