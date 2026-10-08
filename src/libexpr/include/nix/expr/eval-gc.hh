#pragma once
///@file

#include <cstddef>

// For `NIX_USE_BOEHMGC`
#include "nix/expr/config.hh"

#if NIX_USE_BOEHMGC

#  define GC_INCLUDE_NEW
#  define GC_THREADS 1

#  include <gc/gc.h>
#  include <gc/gc_cpp.h>
#  include <gc/gc_allocator.h>

#else

#  include <memory>

/* Some dummy aliases for Boehm GC definitions to reduce the number of
   #ifdefs. */

template<typename T>
using traceable_allocator = std::allocator<T>;

template<typename T>
using gc_allocator = std::allocator<T>;

#  define GC_MALLOC_ATOMIC std::malloc

struct gc
{};

struct gc_cleanup
{};

#endif

namespace nix {

/**
 * Initialise the Boehm GC, if applicable.
 */
void initGC();

/**
 * Make sure `initGC` has already been called.
 */
void assertGCInitialized();

#if NIX_USE_BOEHMGC
/**
 * The number of GC cycles since initGC().
 */
size_t getGCCycles();

/**
 * Slack to subtract from `GC_get_approx_sp()` when recording the stack
 * pointer of a stack that is about to be switched away from (see
 * `GC_stack::saved_sp`). The approximation is taken in a callee of the
 * function performing the switch, so it already lies below that
 * function's own frame; the slack additionally covers what the switch
 * itself pushes below the call site: the frames of the Boost.Context
 * resume path and the block of callee-saved registers stored by its
 * trampoline (72 bytes on x86-64, 176 bytes on aarch64). The latter is
 * essential, since those spilled registers may hold GC pointers of the
 * callers. Erring on the low side merely makes the GC scan a few bytes
 * of (mapped) stack below the true stack pointer.
 */
constexpr size_t gcStackSwitchSlack = 512;

/**
 * Record the current stack pointer (lowered by `gcStackSwitchSlack`)
 * as the saved stack pointer of `stk`, which must be the registered
 * stack that the current thread is executing on and about to switch
 * away from. Call this from the function performing the switch.
 *
 * Panics if the current stack pointer does not lie within `stk`:
 * recording it anyway would make the collector scan from one stack up
 * to the base of another (crashing on the guard page in between, or
 * silently dropping roots), so it's better to fail right here, where
 * the backtrace shows which stack switch went wrong. See
 * `coroutine-gc.hh` for how such a mismatch can come about.
 */
void gcSaveStackPointer(struct GC_stack * stk);
#endif

} // namespace nix
