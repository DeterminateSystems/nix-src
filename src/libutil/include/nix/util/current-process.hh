#pragma once
///@file

#include <filesystem>
#include <optional>
#include <chrono>

#ifndef _WIN32
#  include <sys/resource.h>
#endif

#include "nix/util/types.hh"
#include "nix/util/fun.hh"

namespace nix {

/**
 * Get the current process's user space CPU time.
 */
std::chrono::microseconds getCpuUserTime();

/**
 * If cgroups are active, attempt to calculate the number of CPUs available.
 * If cgroups are unavailable or if cpu.max is set to "max", return 0.
 */
unsigned int getMaxCPU();

// It does not seem possible to dynamically change stack size on Windows.
#ifndef _WIN32
/**
 * Increase the RLIMIT_STACK rlimit if it is currently smaller than `stackSize`.
 * @note Not thread safe. Calls to this should be wrapped in a std::call_once.
 */
void ensureStackSizeAtLeast(size_t stackSize);

/**
 * Make the calling thread use an alternate signal stack (see
 * `sigaltstack(2)`) allocated by Nix, replacing any alternate signal
 * stack set up by someone else. Signal handlers installed with
 * `SA_ONSTACK` (such as the stack overflow handler installed by
 * `detectStackOverflow()`, or Sentry's crash handler) can only run on
 * a thread that has run out of stack if the thread has an alternate
 * signal stack. Threads don't inherit the alternate signal stack of
 * the thread that created them, so every thread that can overflow its
 * stack (e.g. the evaluator's worker threads) has to call this. The
 * stack is freed when the thread exits.
 */
void ensureSignalStack();
#endif

/**
 * Restore the original inherited Unix process context (such as signal
 * masks, stack size).

 * See unix::startSignalHandlerThread(), unix::saveSignalMask().
 */
void restoreProcessContext(bool restoreMounts = true);

/**
 * @return the path of the current executable.
 */
std::optional<std::filesystem::path> getSelfExe();

/**
 * Register a function to be called by quickExit(). Functions are
 * called in reverse order of registration. This is intended for
 * releasing static objects that own resources that need cleaning up
 * (e.g. child processes).
 */
void atQuickExit(fun<void()> f);

/**
 * Run the functions registered with atQuickExit(), flush the standard
 * C and C++ output streams and exit with `status`, without running
 * static destructors or `atexit()` handlers.
 *
 * Static destructors run while other threads (e.g. the curl worker
 * thread) may still be using the objects they destroy, which causes
 * crashes on exit. Resources that need cleaning up (child processes,
 * SQLite databases, temporary GC roots) are owned by objects on the
 * stack, so they have already been released by the time this is
 * called, or by static objects that release them via atQuickExit().
 */
[[noreturn]] void quickExit(int status);

} // namespace nix
