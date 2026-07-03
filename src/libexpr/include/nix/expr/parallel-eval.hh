#pragma once

#include <functional>
#include <queue>
#include <future>
#include <random>

#include <boost/thread/thread.hpp>

#include "nix/util/sync.hh"
#include "nix/util/logging.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/util.hh"
#include "nix/util/signals.hh"

#if NIX_USE_BOEHMGC
#  include <gc.h>
#endif

namespace nix {

struct Executor
{
    using work_t = std::function<void()>;

    /**
     * Completion state shared between a set of work items and a
     * `FutureVector` waiting for them. This is used instead of a
     * `std::promise`/`std::future` per work item, since fulfilling a
     * promise does an unconditional futex wake syscall (and its shared
     * state is a heap allocation), which is wasteful when spawning
     * hundreds of thousands of work items whose completion is only
     * ever awaited in aggregate.
     */
    struct Completion
    {
        std::atomic<uint64_t> pending{0};

        struct State
        {
            /**
             * Exceptions thrown by work items.
             */
            std::vector<std::exception_ptr> exceptions;
        };

        Sync<State> state_;

        std::condition_variable cv;

        /**
         * Record the completion of one work item, waking up any
         * thread waiting for the pending count to drop to zero.
         */
        void finish(std::exception_ptr ex)
        {
            if (ex)
                state_.lock()->exceptions.push_back(std::move(ex));
            if (pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                /* Take the lock to make sure that a waiter that has
                   seen a non-zero pending count has started waiting on
                   the condition variable before we notify it. */
                auto state(state_.lock());
                cv.notify_all();
            }
        }
    };

    struct Item
    {
        work_t work;
        std::shared_ptr<Completion> completion;
    };

    struct State
    {
        std::multimap<uint64_t, Item> queue;
        std::vector<boost::thread> threads;
    };

    std::atomic_bool quit{false};

    const unsigned int evalCores;

    const bool enabled;

    const std::unique_ptr<InterruptCallback> interruptCallback;

    Sync<State> state_;

    std::condition_variable wakeup;

    static unsigned int getEvalCores(const EvalSettings & evalSettings);

    Executor(const EvalSettings & evalSettings);

    ~Executor();

    void createWorker(State & state);

    void worker();

    using WorkItems = std::vector<std::pair<Executor::work_t, uint8_t>>;

    /**
     * Queue the given work items for execution. If `completion` is
     * non-null, it is notified as the items finish; otherwise the
     * items are fire-and-forget.
     */
    void spawn(WorkItems && items, std::shared_ptr<Completion> completion = {});

    [[gnu::tls_model("initial-exec")]] static thread_local bool amWorkerThread;
};

struct FutureVector
{
    Executor & executor;

    std::shared_ptr<Executor::Completion> completion{std::make_shared<Executor::Completion>()};

    ~FutureVector();

    void spawn(Executor::WorkItems && work);

    void spawn(uint8_t prioPrefix, Executor::work_t && work)
    {
        spawn({{std::move(work), prioPrefix}});
    }

    /**
     * Wait for all spawned work items (including ones spawned by
     * other work items after this call) to finish, then rethrow the
     * first recorded exception, if any.
     */
    void finishAll();
};

} // namespace nix
