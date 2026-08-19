#pragma once

#include <functional>
#include <memory>
#include <queue>
#include <future>
#include <random>
#include <thread>
#include <variant>

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

    /**
     * Queue entries are either fresh work items (which get a new fiber
     * when they're picked up) or suspended fibers to be resumed.
     */
    using QueueEntry = std::variant<Item, FiberPtr>;

    struct State
    {
        std::multimap<uint64_t, QueueEntry> queue;
        std::vector<std::thread> threads;
    };

    std::atomic_bool quit{false};

    const unsigned int evalCores;

    const bool enabled;

    const std::unique_ptr<InterruptCallback> interruptCallback;

    Sync<State> state_;

    std::condition_variable wakeup;

    std::atomic<uint64_t> nrFibersSpawned{0};
    std::atomic<uint64_t> nrFiberWakeups{0};
    std::atomic<uint64_t> currentSuspendedFibers{0};
    std::atomic<uint64_t> maxSuspendedFibers{0};
    std::atomic<uint64_t> nrFiberStacksAllocated{0};

    static unsigned int getEvalCores(const EvalSettings & evalSettings);

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
     * domain).
     */
    void runFiber(FiberPtr fiber);

    /**
     * Put a previously suspended fiber back onto the ready queue, at
     * the highest priority. Called when the thunk it was waiting on
     * has been finished.
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
