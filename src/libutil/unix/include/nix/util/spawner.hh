#pragma once
///@file

#include "nix/util/processes.hh"
#include "nix/util/serialise.hh"

#include <span>

namespace nix {

/**
 * The spawner is a small, single-threaded helper process from which
 * child processes are forked, rather than from the (potentially big
 * and multithreaded) calling process. This makes creating a child
 * process cheap regardless of the size of the caller's address
 * space, and allows the child to run arbitrary code before calling
 * exec() (or without calling exec() at all), since it's forked from
 * a process that doesn't have any other threads.
 *
 * The spawner is started lazily by executing the current executable
 * with the environment variable `NIX_SPAWNER_FD` set. Executables
 * must therefore call `runSpawnerIfRequested()` at the start of
 * `main()`.
 *
 * What a child does is determined by an operation registered via
 * `RegisterSpawnOp`. Since the spawner runs the same executable as
 * the caller, it has the same operations. Arguments are passed to the
 * operation in serialised form; it's up to the operation to
 * explicitly pass any state of the caller that it needs.
 *
 * Children are children of the spawner, not of the caller. The
 * spawner keeps exited children as zombies until the caller has
 * received their exit status (via `Pid::wait()`), so it's safe for the
 * caller to send signals to the child or its process group as long as
 * it has a `Pid` for the child.
 */

MakeError(SpawnerError, Error);

struct SpawnOptions
{
    /**
     * Kill the child if the calling process goes away.
     */
    bool dieWithParent = true;

    /**
     * Put the child in a new session (and process group).
     */
    bool setSid = false;

    /**
     * Change to this directory in the child.
     */
    std::optional<std::filesystem::path> chdir;

    /**
     * Create the child using clone() with the specified flags (Linux
     * only).
     */
    int linuxCloneFlags = 0;

    /**
     * If set, create the child directly in the cgroup referred to by
     * this directory file descriptor (Linux only).
     */
    Descriptor cgroupFd = INVALID_DESCRIPTOR;

    /**
     * Drop all supplementary groups of the child before creating it.
     * This is done before entering any new namespaces, since it
     * can't be done afterwards inside a user namespace. If dropping
     * fails with `EPERM`, this is fatal only if
     * `requireDropSupplementaryGroups` is set.
     */
    bool dropSupplementaryGroups = false;
    bool requireDropSupplementaryGroups = false;

    /**
     * Prefix for errors printed by the child to stderr if the
     * operation throws an exception.
     */
    std::string errorPrefix;
};

/**
 * Register an operation that can be executed in a child process
 * created by the spawner. Typical usage:
 *
 *     static RegisterSpawnOp fooOp("foo", [](Source & args, std::vector<AutoCloseFD> fds) {
 *         auto x = readString(args);
 *         ...
 *     });
 *
 *     Pid pid = fooOp.spawn([&](Sink & args) { args << x; }, {fd});
 *
 * This must be done by a static object so that the operation is
 * registered in both the caller and the spawner.
 */
struct RegisterSpawnOp
{
    /**
     * The function executed in the child. It receives the arguments
     * written by the `writeArgs` callback of `spawn()`, and the file
     * descriptors passed to `spawn()`. The child exits if it returns.
     */
    using Main = fun<void(Source & args, std::vector<AutoCloseFD> fds)>;

    RegisterSpawnOp(std::string name, Main main);

    /**
     * Start a child process that executes this operation. Throws a
     * `SpawnerError` if the spawner is unavailable.
     */
    Pid spawn(fun<void(Sink & args)> writeArgs, std::span<const Descriptor> fds = {}, const SpawnOptions & options = {})
        const;

private:
    std::string name;
};

/**
 * If this process was started as a spawner, run the spawner and never
 * return. Must be called at the start of `main()` (i.e. after static
 * initialisers have registered all operations, but before anything
 * else). Calling this is also what enables `RegisterSpawnOp::spawn()`
 * in this process.
 */
void runSpawnerIfRequested();

/**
 * State of a child process created by the spawner, as seen by the
 * caller. Used by `Pid`.
 */
struct SpawnerChild;

/**
 * Wait for the child to exit and return its exit status.
 */
int waitSpawnerChild(SpawnerChild & child, bool allowInterrupts);

/**
 * Return whether the child is still running. If it has exited, it's
 * reaped.
 */
bool spawnerChildIsAlive(SpawnerChild & child);

/**
 * Let the spawner reap the child when it exits. The caller can no
 * longer wait for it.
 */
void detachSpawnerChild(SpawnerChild & child);

} // namespace nix
