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
 * The GC side keeps track of the "current stack" of each thread (the
 * registered stack the thread is executing on) and, for every stack
 * that is not being executed on, a saved stack pointer from which it
 * is scanned. The hooks keep that bookkeeping in sync with the
 * coroutine switches.
 *
 * Note that a coroutine's continuation does not necessarily live on
 * its own stack: when coroutine B's body resumes coroutine C, and C's
 * body in turn writes to B's sink, B yields *from C's stack*, and the
 * next resumption of B lands on C's stack as well. (This happens
 * e.g. when `copyStorePath()`'s `sinkToSource` coroutine streams a
 * NAR through a `sourceToSink` decompression coroutine.) Hence the
 * yield-side hooks operate on the current stack rather than on the
 * coroutine's own stack, and the resume-side hooks only provisionally
 * set the current stack, which the resumed code corrects.
 *
 * The contract:
 *
 * - `coroStackRegister(base, size)` is called when a coroutine stack
 *   is allocated, with `base` its cold (hi) end. It returns an opaque
 *   cookie identifying the stack. `coroStackUnregister(cookie)` is
 *   called when the stack is deallocated.
 *
 * - `coroSwitchTo(cookie)` is called just before the current thread
 *   resumes the coroutine identified by `cookie` (including the
 *   initial start of its body and the forced unwinding of a suspended
 *   one). It records the stack pointer of the stack being left (so
 *   that everything from there up to that stack's base is scannable),
 *   provisionally makes the coroutine's own stack the current one,
 *   and returns an opaque handle for the stack being left, to be
 *   passed to `coroSwitchBack` when the switch returns (because the
 *   coroutine yielded or finished).
 *
 * - `coroEnter(cookie)` is called at the start of the coroutine body
 *   (which does run on the coroutine's own stack) and makes that stack
 *   the current one. This is needed because the cookie is only known
 *   once the stack has been allocated, which happens after the
 *   `coroSwitchTo` for the initial start.
 *
 * - `coroYield()` is called just before a coroutine yields back to its
 *   resumer. It records the stack pointer of the current stack
 *   (whichever registered stack the yield is executed on) and returns
 *   a handle for it; `coroResume(handle)` is called when the yield
 *   returns (normally or by exception) and makes that stack the
 *   current one again.
 *
 * All hooks are invoked with the corresponding stack switches
 * strictly balanced.
 *
 * The hooks may be reset to null at any time, in particular in a
 * forked child process (which inherits the parent's bookkeeping but
 * doesn't execute on the stack it describes and never runs the
 * collector), so every use must check for null, including the
 * "closing" half of a pair whose "opening" half did run.
 */
extern void * (*coroStackRegister)(void * base, size_t size);
extern void (*coroStackUnregister)(void * cookie);
extern void * (*coroSwitchTo)(void * cookie);
extern void (*coroSwitchBack)(void * prevHandle);
extern void (*coroEnter)(void * cookie);
extern void * (*coroYield)();
extern void (*coroResume)(void * handle);

} // namespace nix
