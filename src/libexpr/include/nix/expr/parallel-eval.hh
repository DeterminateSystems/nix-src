#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <queue>
#include <future>
#include <random>
#include <thread>

#include "nix/util/move-only-function.hh"
#include "nix/util/sync.hh"
#include "nix/util/logging.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/util.hh"
#include "nix/util/signals.hh"

namespace nix {

struct Executor
{
    using work_t = MoveOnlyFunction<void()>;

    struct Item
    {
        std::promise<void> promise;
        work_t work;
    };

    /**
     * A work item running on its own stack. Defined in
     * `parallel-eval.cc`; opaque here to keep the Boost.Context
     * dependency out of this header.
     */
    struct Fiber;

    using FiberPtr = std::unique_ptr<Fiber>;

    /**
     * A pool of reusable fiber stacks, to avoid the cost of
     * allocating and faulting in a fresh stack for every work
     * item. Defined in `parallel-eval.cc`.
     */
    struct StackPool;

    const std::unique_ptr<StackPool> stackPool;

    struct State
    {
        /**
         * Fresh work items, ordered by priority. Each gets a new fiber
         * when it's picked up by a worker.
         */
        std::multimap<uint64_t, Item> queue;

        /**
         * The number of fibers in existence, i.e. running, suspended
         * or ready to be resumed, across all workers. Each has its own
         * stack. Only used for statistics; the fiber limit is
         * per-worker (see `Worker::nrLiveFibers`).
         */
        size_t nrLiveFibers = 0;
    };

    /**
     * A worker thread. Fibers are pinned to the worker that created
     * them: a suspended fiber is only ever resumed by its owner. This
     * is essential, since compilers assume that the address of a
     * thread-local variable is invariant for the duration of a
     * function (e.g. on AArch64 the thread pointer is read once in the
     * prologue and kept in a callee-saved register), so a fiber
     * resumed on another thread would keep using the thread-locals of
     * its previous thread. Fresh work items, on the other hand, are
     * taken from the shared `State::queue` by any worker.
     *
     * All fields except `thread` are protected by `Executor::state_`.
     */
    struct Worker
    {
        std::thread thread;

        /**
         * Suspended fibers owned by this worker whose awaited value
         * has been finished, in FIFO order. The worker resumes these
         * before starting fresh work items, so that existing work is
         * drained first.
         */
        std::deque<FiberPtr> readyFibers;

        /**
         * Signalled when a fiber is pushed onto `readyFibers`, when a
         * fresh work item is queued, or on shutdown.
         */
        std::condition_variable wakeup;

        /**
         * Whether the worker is blocked on `wakeup`. Producers only
         * notify sleeping workers, to avoid pointless futex traffic.
         */
        bool sleeping = false;

        /**
         * The number of fibers owned by this worker, i.e. running,
         * suspended or ready to be resumed. The worker doesn't exit
         * while this is non-zero.
         */
        size_t nrLiveFibers = 0;
    };

    std::atomic_bool quit{false};

    const unsigned int evalCores;

    /**
     * The maximum number of live fibers per worker (derived from
     * `eval-max-fibers`). See `canStartFiber()`.
     */
    const unsigned int maxFibersPerWorker;

    const bool enabled;

    Sync<State> state_;

    /**
     * The worker threads. Only mutated in the constructor; the
     * `Worker` objects have stable addresses (fibers refer to their
     * owner by reference).
     */
    std::vector<std::unique_ptr<Worker>> workers;

    /**
     * Note: declared last so that it is destroyed first, i.e. an
     * interrupt arriving during destruction cannot touch the members
     * above after they have been destroyed.
     */
    const std::unique_ptr<InterruptCallback> interruptCallback;

    std::atomic<uint64_t> nrFibersSpawned{0};
    std::atomic<uint64_t> nrFiberWakeups{0};
    std::atomic<uint64_t> currentSuspendedFibers{0};
    std::atomic<uint64_t> maxSuspendedFibers{0};
    std::atomic<uint64_t> maxLiveFibers{0};
    std::atomic<uint64_t> nrFiberStacksAllocated{0};

    static unsigned int getEvalCores(const EvalSettings & evalSettings);

    static unsigned int getMaxFibersPerWorker(const EvalSettings & evalSettings, unsigned int evalCores);

    Executor(const EvalSettings & evalSettings);

    ~Executor();

    void createWorker();

    void worker(Worker & self);

    /**
     * Wake up all sleeping workers, e.g. so that they can observe
     * `quit` or an interrupt.
     */
    void wakeupAllWorkers();

    /**
     * Create a fiber for a fresh work item, owned by `owner`. If fiber
     * creation fails (e.g. stack allocation failure), the item's
     * promise receives the exception and a null pointer is returned.
     */
    FiberPtr makeFiber(Worker & owner, Item && item);

    /**
     * Start or resume a fiber on the current thread, which must be
     * its owner. On return, the fiber has either finished (its
     * promise is fulfilled and the fiber is destroyed) or suspended
     * itself waiting on a thunk (in which case it has been registered
     * with the thunk's waiter domain). Returns whether the fiber
     * finished.
     */
    bool runFiber(FiberPtr fiber);

    /**
     * Whether a worker may start a fresh work item on a new fiber,
     * i.e. its fiber limit has not been reached.
     *
     * Waiting for a fiber slot cannot deadlock: the limit only applies
     * to fresh work items, never to resuming ready fibers. A suspended
     * fiber is waiting on a value that is being evaluated by some
     * fiber or thread, so whenever fibers are suspended, either some
     * fiber is runnable (and its owner, which doesn't exit while it
     * owns fibers, will resume it), or a non-fiber thread (e.g. the
     * main thread) is evaluating and will eventually finish the
     * value. Fresh work items that haven't started hold no values
     * pending, so they are never what a suspended fiber is waiting
     * for. (This relies on non-fiber threads never blocking on the
     * futures of queued work items while they have a value pending.)
     */
    bool canStartFiber(const Worker & worker) const
    {
        return worker.nrLiveFibers < maxFibersPerWorker;
    }

    /**
     * Put a previously suspended fiber back onto its owner's ready
     * queue, and wake up the owner if necessary. Called when the thunk
     * it was waiting on has been finished, or on shutdown/interrupt.
     */
    void enqueueFiber(FiberPtr fiber);

    /**
     * Fail all queued work items that haven't started with an
     * `Interrupted` exception on their promise. Called on
     * shutdown/interrupt.
     */
    void failQueuedItems();

    using WorkItems = std::vector<std::pair<Executor::work_t, uint8_t>>;

    std::vector<std::future<void>> spawn(WorkItems && items);

    [[gnu::tls_model("initial-exec")]] static thread_local bool amWorkerThread;
};

/**
 * RAII guard that prevents the current fiber (if any) from being
 * suspended while the guard is alive: if it has to wait for a value
 * that is being evaluated by another thread, it blocks its worker
 * thread instead of yielding it to other fibers. This is needed around
 * code that keeps per-thread state which must not be interleaved with
 * or migrated to other threads, such as wasmtime's stack of active
 * Wasm calls. Nesting is allowed.
 */
struct FiberNoSuspend
{
    FiberNoSuspend();
    ~FiberNoSuspend();
    FiberNoSuspend(const FiberNoSuspend &) = delete;
    FiberNoSuspend & operator=(const FiberNoSuspend &) = delete;

private:
    Executor::Fiber * fiber;
};

struct FutureVector
{
    Executor & executor;

    struct State
    {
        std::vector<std::future<void>> futures;
    };

    Sync<State> state_;

    ~FutureVector();

    // FIXME: add a destructor that cancels/waits for all futures.

    void spawn(Executor::WorkItems && work);

    void spawn(uint8_t prioPrefix, Executor::work_t && work)
    {
        Executor::WorkItems items;
        items.emplace_back(std::move(work), prioPrefix);
        spawn(std::move(items));
    }

    void finishAll();
};

} // namespace nix
