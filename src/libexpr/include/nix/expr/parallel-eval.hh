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

/**
 * The two kinds of speculative work, each with its own budget of
 * outstanding items: the speculative evaluation of substantial thunks
 * (`EvalState::speculate()`), and the speculative instantiation of the
 * dependencies of a derivation (the `derivationStrict` hook). The
 * latter recurses through the dependency graph, so it is not subject
 * to the brake that keeps the former from cascading.
 */
enum class SpeculationKind { Thunk, Instantiation };

struct Executor
{
    using work_t = MoveOnlyFunction<void()>;

    struct Item
    {
        std::promise<void> promise;
        work_t work;

        /**
         * Whether this is speculative work (see
         * `EvalState::speculate()` and the `derivationStrict` hook):
         * work that runs at the lowest priority, is not counted as
         * backlog, may not start thunk speculation of its own, and is
         * subject to the fiber reserve for demand work.
         */
        bool speculative = false;

        /**
         * For speculative work: which budget it is charged to (see
         * `SpeculationKind`).
         */
        bool instantiation = false;
    };

    /**
     * The priority of speculative work items, i.e. the lowest one.
     */
    static constexpr uint8_t speculativePriority = 255;

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

    /**
     * The number of queued, non-speculative work items, and the
     * number of ready fibers. Maintained outside the state lock so
     * that `hasBacklog()` is lock-free.
     */
    std::atomic<uint32_t> nrQueuedDemand{0};
    std::atomic<uint32_t> nrReadyFibers{0};

    /**
     * The number of speculative work items of each kind that have
     * been submitted but not yet finished (or dropped), and their
     * high-water marks.
     */
    std::atomic<uint32_t> nrSpeculativeOutstanding{0};
    std::atomic<uint32_t> maxSpeculativeOutstanding{0};
    std::atomic<uint32_t> nrInstantiationsOutstanding{0};
    std::atomic<uint32_t> maxInstantiationsOutstanding{0};

    std::atomic<uint32_t> & outstanding(SpeculationKind kind)
    {
        return kind == SpeculationKind::Thunk ? nrSpeculativeOutstanding : nrInstantiationsOutstanding;
    }

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
     *
     * Speculative work may not use the worker's last fiber slot,
     * which is reserved for demand work: otherwise suspended
     * speculative fibers could occupy all slots and keep demand items
     * (which always sort before speculative ones) from starting. When
     * the reserve is exhausted, the worker sleeps until one of its
     * fibers finishes.
     */
    bool canStartFiber(const Worker & worker, bool speculative = false) const
    {
        return worker.nrLiveFibers + (speculative ? std::min(1U, maxFibersPerWorker / 2) : 0) < maxFibersPerWorker;
    }

    /**
     * Put a previously suspended fiber back onto its owner's ready
     * queue, and wake up the owner if necessary. Called when the thunk
     * it was waiting on has been finished, or on shutdown/interrupt.
     */
    void enqueueFiber(FiberPtr fiber);

    /**
     * Fail work items that haven't started with an `Interrupted`
     * exception, so we get a nicer error than "std::future_error:
     * Broken promise". Note: a fresh exception per item, not a shared
     * one.
     */
    void failItems(std::multimap<uint64_t, Item> && items);

    /**
     * Fail all queued work items that haven't started with an
     * `Interrupted` exception on their promise. Called on
     * shutdown/interrupt.
     */
    void failQueuedItems();

    using WorkItems = std::vector<std::pair<Executor::work_t, uint8_t>>;

    std::vector<std::future<void>> spawn(WorkItems && items);

    /**
     * Submit a speculative work item (see `Item::speculative`) of the
     * given kind. Its result is not awaited by anyone, so exceptions
     * other than `Interrupted` are discarded. The caller is responsible
     * for checking the kind's budget (`outstanding(kind)`) beforehand.
     */
    void spawnSpeculative(work_t && work, SpeculationKind kind);

    /**
     * Whether there is already at least one queued non-speculative
     * work item or ready fiber per worker thread. Optional background
     * work (such as the speculative instantiation of dependencies in
     * `derivationStrict`) should be skipped in that case: it cannot
     * add parallelism, only scheduling overhead.
     */
    bool hasBacklog() const
    {
        return nrQueuedDemand.load(std::memory_order_relaxed) + nrReadyFibers.load(std::memory_order_relaxed)
               >= evalCores;
    }

    /**
     * Whether the current execution context is running speculative
     * work. Speculative work must not spawn speculation of its own
     * (to bound the cascade), and later this will also be the
     * predicate for bailing out of side effects.
     */
    static bool speculating()
    {
        return inSpeculation;
    }

    [[gnu::tls_model("initial-exec")]] static thread_local bool amWorkerThread;

    /**
     * Whether the fiber running on this thread is speculative.
     * Swapped by `runFiber()` on every switch-in/out.
     */
    [[gnu::tls_model("initial-exec")]] static thread_local bool inSpeculation;
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
