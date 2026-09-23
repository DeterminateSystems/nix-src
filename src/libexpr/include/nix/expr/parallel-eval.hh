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
         * Suspended fibers whose awaited value has been finished, in
         * FIFO order. Workers resume these before starting fresh work
         * items, so that existing work is drained first.
         */
        std::deque<FiberPtr> readyFibers;

        std::vector<std::thread> threads;

        /**
         * The number of workers currently blocked on `wakeup`.
         * Producers wake up at most this many workers (and none when
         * all workers are busy), to avoid pointless futex traffic and
         * thundering herds.
         */
        size_t nrSleeping = 0;

        /**
         * The number of fibers in existence, i.e. running, suspended
         * or ready to be resumed. Each has its own stack.
         */
        size_t nrLiveFibers = 0;
    };

    std::atomic_bool quit{false};

    const unsigned int evalCores;

    /**
     * The maximum number of live fibers (`eval-max-fibers`). See
     * `canStartFiber()`.
     */
    const unsigned int maxFibers;

    const bool enabled;

    const std::unique_ptr<InterruptCallback> interruptCallback;

    Sync<State> state_;

    std::condition_variable wakeup;

    std::atomic<uint64_t> nrFibersSpawned{0};
    std::atomic<uint64_t> nrFiberWakeups{0};
    std::atomic<uint64_t> currentSuspendedFibers{0};
    std::atomic<uint64_t> maxSuspendedFibers{0};
    std::atomic<uint64_t> maxLiveFibers{0};
    std::atomic<uint64_t> nrFiberStacksAllocated{0};

    static unsigned int getEvalCores(const EvalSettings & evalSettings);

    static unsigned int getMaxFibers(const EvalSettings & evalSettings, unsigned int evalCores);

    Executor(const EvalSettings & evalSettings);

    ~Executor();

    void createWorker(State & state);

    void worker();

    /**
     * Create a fiber for a fresh work item. If fiber creation fails
     * (e.g. stack allocation failure), the item's promise receives the
     * exception and a null pointer is returned.
     */
    FiberPtr makeFiber(Item && item);

    /**
     * Start or resume a fiber on the current thread. On return, the
     * fiber has either finished (its promise is fulfilled and the
     * fiber is destroyed) or suspended itself waiting on a thunk (in
     * which case it has been registered with the thunk's waiter
     * domain). Returns whether the fiber finished.
     */
    bool runFiber(FiberPtr fiber);

    /**
     * Whether a worker may start a fresh work item on a new fiber,
     * i.e. the fiber limit has not been reached.
     *
     * Waiting for a fiber slot cannot deadlock: a suspended fiber is
     * waiting on a value that is being evaluated by some fiber or
     * thread, so whenever fibers are suspended, either some fiber is
     * runnable, or a non-fiber thread (e.g. the main thread) is
     * evaluating and will eventually finish the value. Fresh work
     * items that haven't started hold no values pending, so they are
     * never what a suspended fiber is waiting for. (This relies on
     * non-fiber threads never blocking on the futures of queued work
     * items while they have a value pending.)
     */
    bool canStartFiber(const State & state) const
    {
        return state.nrLiveFibers < maxFibers;
    }

    /**
     * Put a previously suspended fiber back onto the ready queue.
     * Called when the thunk it was waiting on has been finished.
     */
    void enqueueFiber(FiberPtr fiber);

    /**
     * Drain the queue on shutdown/interrupt: work items that haven't
     * started get an `Interrupted` exception on their promise;
     * suspended fibers are resumed so they can observe `quit` and
     * unwind their stacks.
     */
    void drainQueue();

    using WorkItems = std::vector<std::pair<Executor::work_t, uint8_t>>;

    std::vector<std::future<void>> spawn(WorkItems && items);

    [[gnu::tls_model("initial-exec")]] static thread_local bool amWorkerThread;
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
