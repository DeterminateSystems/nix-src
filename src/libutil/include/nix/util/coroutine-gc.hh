#pragma once
///@file

#include <cstddef>

namespace nix {

/**
 * Hooks that make the boost::coroutines2 coroutines in libutil (see
 * `sourceToSink`/`sinkToSource` in `serialise.hh`) visible to a
 * conservative garbage collector. libutil itself does not depend on
 * any GC; these default to null and are installed by the evaluator
 * (`libexpr`), which implements them in terms of bdwgc's registered
 * stacks (`GC_register_stack` etc.).
 *
 * The contract:
 *
 * - `coroStackRegister(base, size)` is called when a coroutine stack
 *   is allocated, with `base` its cold (hi) end. It returns an opaque
 *   cookie identifying the stack. `coroStackUnregister(cookie)` is
 *   called when the stack is deallocated.
 *
 * - `coroSwitchTo(cookie)` is called just before the current thread
 *   switches onto the coroutine stack identified by `cookie`. It
 *   records the stack pointer of the stack being left (so that
 *   everything from there up to that stack's base is scannable) and
 *   returns an opaque handle for it, to be passed to `coroSwitchBack`
 *   when the switch returns.
 *
 * - `coroMarkSuspended(cookie)` is called on the coroutine stack just
 *   before yielding back to the caller, and records the coroutine's
 *   stack pointer; `coroMarkActive(cookie)` is called when the
 *   coroutine is resumed again.
 *
 * All hooks are invoked with the corresponding stack switches
 * strictly balanced.
 */
extern void * (*coroStackRegister)(void * base, size_t size);
extern void (*coroStackUnregister)(void * cookie);
extern void * (*coroSwitchTo)(void * cookie);
extern void (*coroSwitchBack)(void * prevHandle);
extern void (*coroMarkSuspended)(void * cookie);
extern void (*coroMarkActive)(void * cookie);

} // namespace nix
