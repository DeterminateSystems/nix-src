#include "nix/expr/eval.hh"
#include "nix/expr/parallel-eval.hh"
#include "nix/store/globals.hh"
#include "nix/expr/primops.hh"

#include <boost/context/fiber.hpp>
#include <boost/context/protected_fixedsize_stack.hpp>

#include <optional>
#include <unordered_map>

#if NIX_USE_BOEHMGC
#  include <gc.h>
#endif

#include <boost/unordered/concurrent_flat_set.hpp>

namespace nix {

struct WaiterDomain;

[[gnu::tls_model("initial-exec")]] thread_local bool Executor::amWorkerThread{false};

static std::atomic<uint32_t> nextEvalThreadId{1};
[[gnu::tls_model("initial-exec")]] thread_local uint32_t myEvalThreadId(nextEvalThreadId++);

/**
 * The fiber currently executing on this thread, or null if we're not
 * inside a fiber (e.g. on the main thread or in a worker's scheduler
 * loop).
 */
[[gnu::tls_model("initial-exec")]] static thread_local Executor::Fiber * currentFiber{nullptr};

/**
 * The worker whose scheduler loop is running on this thread, or null
 * on non-worker threads. Only used to assert that fibers are resumed
 * by their owner.
 */
[[gnu::tls_model("initial-exec")]] static thread_local Executor::Worker * currentWorker{nullptr};

struct Executor::Fiber
{
    Executor & executor;

    /**
     * The worker that created this fiber, which is the only thread
     * that ever runs it (see `Executor::Worker`).
     */
    Worker & owner;

    /**
     * The value of `myEvalThreadId` while this fiber is running. Each
     * fiber needs its own id (rather than a per-thread id) since the
     * self-wait check in `waitOnThunk()` would otherwise produce false
     * "infinite recursion" errors when two fibers running on the same
     * thread touch the same thunk.
     */
    const uint32_t evalThreadId;

    /**
     * This fiber's Nix call stack depth while it's not running.
     * `runFiber()` swaps it with the thread-local counter (see
     * `CallDepth`) on every switch-in/out, since several fibers, each
     * with their own call chain, are interleaved on the same thread.
     */
    size_t callDepth = 0;

    /**
     * This fiber's evaluation context while it's not running.
     * `runFiber()` swaps it with the thread-local
     * `EvalState::evalContext` on every switch-in/out (a mere pointer
     * exchange), while this slot holds the thread's own context in the
     * meantime.
     */
    EvalState::EvalContext evalContext;

    std::promise<void> promise;

    work_t work;

    /**
     * The fiber's continuation. Valid while the fiber is queued or
     * suspended; invalid while it's running or after it has finished.
     */
    boost::context::fiber ctx;

    /**
     * The scheduler's continuation. Valid while the fiber is running.
     */
    boost::context::fiber schedCtx;

    /**
     * Side channel for the suspension handshake, filled in by
     * `suspendFiber()` immediately before switching back to the
     * scheduler, and consumed by `runFiber()`.
     */
    detail::ValueBase * waitingOn = nullptr;
    WaiterDomain * suspendDomain = nullptr;

#if NIX_USE_BOEHMGC
    /**
     * The registered GC descriptor of this fiber's stack.
     */
    struct GC_stack * gcStack = nullptr;
#endif

    Fiber(Executor & executor, Worker & owner, Item && item)
        : executor(executor)
        , owner(owner)
        , evalThreadId(nextEvalThreadId++)
        , promise(std::move(item.promise))
        , work(std::move(item.work))
    {
    }

    ~Fiber()
    {
        /* A fiber must never be destroyed while suspended, since that
           would forcibly unwind its stack. Suspended fibers are always
           resumed (with `quit` or an interrupt flag set) so that they
           exit normally. */
        assert(!ctx);
    }
};

struct Executor::StackPool
{
    struct State
    {
        std::vector<boost::context::stack_context> stacks;
#if NIX_USE_BOEHMGC
        /**
         * The registered GC descriptor of each stack ever allocated,
         * keyed by the stack's hi end (`stack_context::sp`). Pooled
         * stacks keep their registration; their descriptors have a
         * null `saved_sp`, so the GC never scans them.
         */
        std::unordered_map<void *, struct GC_stack *> gcInfo;
#endif
    };

    Sync<State> state_;

    ~StackPool()
    {
        auto state(state_.lock());
#if NIX_USE_BOEHMGC
        for (auto & [_, gs] : state->gcInfo) {
            GC_unregister_stack(gs);
            delete gs;
        }
#endif
        for (auto & sctx : state->stacks)
            boost::context::protected_fixedsize_stack(evalStackSize).deallocate(sctx);
    }
};

#if NIX_USE_BOEHMGC
/**
 * Side channel from `PooledStackAllocator::allocate()` (called from
 * inside the `boost::context::fiber` constructor) to `makeFiber()`:
 * the GC descriptor of the most recently allocated stack.
 */
[[gnu::tls_model("initial-exec")]] static thread_local struct GC_stack * lastFiberGCStack{nullptr};
#endif

/**
 * A Boost.Context stack allocator that reuses stacks from the
 * executor's pool. Reused stacks also come with their previously
 * faulted-in pages, avoiding both the mmap/munmap system calls and
 * the page faults of a fresh stack for every work item.
 */
struct PooledStackAllocator
{
    Executor & executor;

    boost::context::stack_context allocate()
    {
        {
            auto state(executor.stackPool->state_.lock());
            if (!state->stacks.empty()) {
                auto sctx = state->stacks.back();
                state->stacks.pop_back();
#if NIX_USE_BOEHMGC
                lastFiberGCStack = state->gcInfo.at(sctx.sp);
#endif
                return sctx;
            }
        }
        executor.nrFiberStacksAllocated++;
        auto sctx = boost::context::protected_fixedsize_stack(evalStackSize).allocate();
#if NIX_USE_BOEHMGC
        auto gs = new GC_stack{};
        gs->base = sctx.sp;
        gs->limit = (char *) sctx.sp - sctx.size;
        GC_register_stack(gs);
        executor.stackPool->state_.lock()->gcInfo.emplace(sctx.sp, gs);
        lastFiberGCStack = gs;
#endif
        return sctx;
    }

    void deallocate(boost::context::stack_context & sctx)
    {
        executor.stackPool->state_.lock()->stacks.push_back(sctx);
    }
};

// cache line alignment to prevent false sharing
struct alignas(64) WaiterDomain
{
    /* Note: not using `Sync` because the suspension handshake requires
       locking the mutex on the fiber's stack and unlocking it from the
       scheduler (see `Executor::runFiber()`), which `Sync::Lock` can't
       express. */
    std::mutex mutex;

    /**
     * Wakes up non-fiber threads (e.g. the main thread) blocked on a
     * thunk in this domain.
     */
    std::condition_variable cv;

    /**
     * Fibers suspended waiting for a specific value to be finished.
     * Unlike the condition variable, this is keyed on the exact value,
     * so fiber wakeups are never spurious.
     */
    std::unordered_map<detail::ValueBase *, std::vector<Executor::FiberPtr>> waiters;
};

static std::array<WaiterDomain, 128> waiterDomains;

/**
 * Move all suspended fibers out of the wait lists and back onto their
 * owner's ready queue, and wake up all non-fiber waiters. Called on
 * interrupt and shutdown so that waiting fibers/threads can observe
 * the interrupt/`quit` flag and unwind.
 */
static void flushWaiters()
{
    std::vector<Executor::FiberPtr> woken;
    for (auto & domain : waiterDomains) {
        std::unique_lock lk(domain.mutex);
        for (auto & [_, fibers] : domain.waiters)
            for (auto & fiber : fibers)
                woken.push_back(std::move(fiber));
        domain.waiters.clear();
        domain.cv.notify_all();
    }
    for (auto & fiber : woken) {
        auto & executor = fiber->executor;
        executor.enqueueFiber(std::move(fiber));
    }
}

unsigned int Executor::getEvalCores(const EvalSettings & evalSettings)
{
    /* Note: the default number of cores is currently limited to 32
       due to scalability bottlenecks. */
    return evalSettings.evalProfilerMode != EvalProfilerMode::disabled ? 1
           : evalSettings.evalCores == 0UL                             ? std::min(32U, Settings::getDefaultCores())
                                                                       : evalSettings.evalCores;
}

unsigned int Executor::getMaxFibersPerWorker(const EvalSettings & evalSettings, unsigned int evalCores)
{
    /* By default, allow three suspended fibers per thread on top of
       the running one. This bounds the number of stacks (and thus
       memory and page faults) without leaving threads idle in typical
       workloads: e.g. `nix search nixpkgs` on 24 threads peaks at
       120-220 live fibers when unlimited, and is not measurably slower
       with 96 (4 per thread), while a limit of 48 costs about 5%
       elapsed time. */
    return evalSettings.evalMaxFibers == 0U ? 4 : std::max(1U, evalSettings.evalMaxFibers / evalCores);
}

Executor::Executor(const EvalSettings & evalSettings)
    : stackPool(std::make_unique<StackPool>())
    , evalCores(getEvalCores(evalSettings))
    , maxFibersPerWorker(getMaxFibersPerWorker(evalSettings, evalCores))
    , enabled(evalCores > 1)
    , interruptCallback(createInterruptCallback([&]() {
        /* Wake up all waiting fibers and threads so they can observe
           the interrupt and unwind. Note: `_isInterrupted` has already
           been set at this point. */
        flushWaiters();
        wakeupAllWorkers();
    }))
{
    debug("executor using %d threads and at most %d fibers per thread", evalCores, maxFibersPerWorker);
    // FIXME: create worker threads on demand?
    for (size_t n = 0; n < evalCores; ++n)
        try {
            createWorker();
        } catch (std::system_error & e) {
            if (n == 0)
                throw Error("could not create any evaluator worker threads: %s", e.what());
            warn("could only create %d evaluator worker threads: %s", n, e.what());
            break;
        }
}

Executor::~Executor()
{
    {
        auto state(state_.lock());
        quit = true;
        debug("executor shutting down with %d items left", state->queue.size());
    }

    /* Hand suspended fibers back to their owners and wake up idle
       workers so they can wind down. Note: the shutdown guard in
       `waitOnThunk()` ensures that no fiber registers in a wait list
       after this flush. */
    flushWaiters();
    wakeupAllWorkers();

    for (auto & worker : workers)
        worker->thread.join();

    /* A worker doesn't exit while it owns fibers, so no fiber stack
       can outlive this destructor. */
    for (auto & worker : workers) {
        assert(worker->readyFibers.empty());
        assert(worker->nrLiveFibers == 0);
    }

    /* Fail any work items that were queued after the workers exited. */
    failQueuedItems();
}

/**
 * Wake up `worker` if it's asleep. Must be called with the state lock
 * held. Clears `sleeping` right away, so that producers running before
 * the worker has reacquired the lock don't notify it again (and, in
 * `spawn()`, don't spend their wake budget on a worker that is already
 * being woken). The worker re-checks all its conditions after waking
 * up, so one pending notification is enough.
 */
static bool wakeWorker(Executor::Worker & worker)
{
    if (!worker.sleeping)
        return false;
    worker.sleeping = false;
    worker.wakeup.notify_one();
    return true;
}

void Executor::wakeupAllWorkers()
{
    auto state(state_.lock());
    for (auto & worker : workers)
        wakeWorker(*worker);
}

void Executor::createWorker()
{
    auto worker = std::make_unique<Worker>();
    auto w = worker.get();
    /* Note: worker threads can have a small (default-sized) stack,
       since they only run the scheduler loop; the actual evaluation
       happens on fibers, which have their own stacks. */
    worker->thread = std::thread([this, w]() {
#if NIX_USE_BOEHMGC
        /* Register the worker thread with the garbage collector. This
           is not for the sake of the worker stack (which holds no GC
           roots), but because fibers running on this thread allocate
           from it: without registration, Boehm has no thread-local
           allocation freelists for this thread and every allocation
           takes the global allocation lock, which is several times
           slower. */
        GC_stack_base sb;
        GC_get_stack_base(&sb);
        GC_register_my_thread(&sb);
#endif
        this->worker(*w);
#if NIX_USE_BOEHMGC
        GC_unregister_my_thread();
#endif
    });
    /* Only register the worker once its thread exists, so that a
       failure to create the thread doesn't leave a thread-less worker
       behind. Note: `workers` is read under the state lock by
       `spawn()` and `wakeupAllWorkers()`, which the interrupt
       callback can invoke concurrently. */
    auto state(state_.lock());
    workers.push_back(std::move(worker));
}

Executor::FiberPtr Executor::makeFiber(Worker & owner, Item && item)
{
    auto fiber = std::make_unique<Fiber>(*this, owner, std::move(item));
    auto fib = fiber.get();
    try {
        fiber->ctx = boost::context::fiber(
            std::allocator_arg,
            PooledStackAllocator{*this},
            [fib](boost::context::fiber && sched) -> boost::context::fiber {
                fib->schedCtx = std::move(sched);
                try {
                    fib->work();
                    fib->promise.set_value();
                } catch (const Interrupted &) {
                    fib->executor.quit = true;
                    fib->promise.set_exception(std::current_exception());
                } catch (...) {
                    fib->promise.set_exception(std::current_exception());
                }
                // Terminate the fiber, freeing its stack, and return
                // to the scheduler.
                return std::move(fib->schedCtx);
            });
    } catch (...) {
        // Stack allocation failure. Report it to whoever is waiting
        // on this work item.
        fiber->promise.set_exception(std::current_exception());
        return nullptr;
    }
#if NIX_USE_BOEHMGC
    fiber->gcStack = lastFiberGCStack;
    assert(fiber->gcStack);
#endif
    nrFibersSpawned++;
    return fiber;
}

bool Executor::runFiber(FiberPtr fiber)
{
    auto fib = fiber.get();
    assert(fib->ctx);
    assert(!currentFiber);
    /* Fibers must only ever run on their owner thread, see
       `Executor::Worker`. */
    assert(currentWorker == &fib->owner);

    auto savedThreadId = myEvalThreadId;
    auto savedCallDepth = CallDepth::callDepth;
    currentFiber = fib;
    myEvalThreadId = fib->evalThreadId;
    CallDepth::callDepth = fib->callDepth;
    std::swap(EvalState::evalContext, fib->evalContext);

#if NIX_USE_BOEHMGC
    /* Make this thread's stack scannable by the GC while the fiber
       runs, and make the fiber's stack the current one. The fiber
       clears its own `saved_sp` after it has been resumed (see
       `suspendFiber()`). */
    auto prevStack = GC_current_stack;
    if (prevStack)
        prevStack->saved_sp = (char *) GC_get_approx_sp() - gcStackSwitchSlack;
    GC_current_stack = fib->gcStack;
#endif

    fib->ctx = std::move(fib->ctx).resume();

#if NIX_USE_BOEHMGC
    GC_current_stack = prevStack;
    if (prevStack)
        prevStack->saved_sp = nullptr;
#endif

    currentFiber = nullptr;
    myEvalThreadId = savedThreadId;
    fib->callDepth = CallDepth::callDepth;
    CallDepth::callDepth = savedCallDepth;
    std::swap(EvalState::evalContext, fib->evalContext);

    if (fib->ctx) {
        /* The fiber suspended itself in `waitOnThunk()`. We are still
           holding the waiter domain mutex, which the fiber locked on
           this thread before switching back to us (so `std::mutex`
           thread ownership is respected). Register the fiber in the
           wait list, then release the mutex. Only after the unlock can
           `notifyWaiters()` extract and resume the fiber — at which
           point `fib->ctx` is fully formed. */
        auto domain = fib->suspendDomain;
        assert(domain && fib->waitingOn);
        auto n = ++currentSuspendedFibers;
        if (n > maxSuspendedFibers)
            maxSuspendedFibers = n;
        domain->waiters[fib->waitingOn].push_back(std::move(fiber));
        domain->mutex.unlock();
        /* Ownership of `fib` has passed to the wait list; it may be
           moved back onto our ready queue by another thread from this
           point on, so don't touch it anymore. */
        return false;
    } else {
        /* The fiber has finished; its promise has been fulfilled
           inside the fiber. Destroying `fiber` frees the record (the
           stack was already freed on fiber termination). */
        return true;
    }
}

void Executor::enqueueFiber(FiberPtr fiber)
{
    nrFiberWakeups++;
    currentSuspendedFibers--;
    auto & owner = fiber->owner;
    auto state(state_.lock());
    owner.readyFibers.push_back(std::move(fiber));
    /* Note: `sleeping` is maintained under the state lock, so the
       owner either sees our push in its queue check, or is already
       blocked on `wakeup` and gets notified — no lost wakeups. */
    wakeWorker(owner);
}

/**
 * Fail work items that haven't started with an `Interrupted`
 * exception, so we get a nicer error than "std::future_error: Broken
 * promise". Note: a fresh exception per item, not a shared one.
 */
static void failItems(std::multimap<uint64_t, Executor::Item> && items)
{
    for (auto & [_, item] : items)
        item.promise.set_exception(std::make_exception_ptr(Interrupted("interrupted by the user")));
}

void Executor::failQueuedItems()
{
    failItems(std::exchange(state_.lock()->queue, {}));
}

void Executor::worker(Worker & self)
{
    ReceiveInterrupts receiveInterrupts;

    unix::interruptCheck = [&]() { return (bool) quit; };

    amWorkerThread = true;
    currentWorker = &self;

    /* Whether the fiber we ran in the previous iteration finished (as
       opposed to suspending itself), if any. The fiber accounting for
       it is done under the state lock at the start of the next
       iteration. */
    std::optional<bool> finished;

    while (true) {
        FiberPtr fiber;
        std::optional<Item> item;

        while (true) {
            auto state(state_.lock());

            if (finished) {
                if (*finished) {
                    self.nrLiveFibers--;
                    state->nrLiveFibers--;
                }
                finished.reset();
            }

            /* Resume ready fibers before anything else, so that
               existing work is drained first. This includes shutdown:
               a suspended fiber can only be resumed by its owner
               (i.e. us), and it has to be resumed so that it can
               observe `quit` and unwind its stack. */
            if (!self.readyFibers.empty()) {
                fiber = std::move(self.readyFibers.front());
                self.readyFibers.pop_front();
                break;
            }

            if (quit) {
                /* Fail queued work items that haven't started, so that
                   threads blocked on their futures are unblocked
                   promptly. */
                failItems(std::exchange(state->queue, {}));
                /* Don't exit while we still own fibers: they're
                   suspended in a wait list and will be handed back to
                   us by `flushWaiters()`. */
                if (self.nrLiveFibers == 0)
                    return;
            } else if (!state->queue.empty() && canStartFiber(self)) {
                item = std::move(state->queue.begin()->second);
                state->queue.erase(state->queue.begin());
                self.nrLiveFibers++;
                state->nrLiveFibers++;
                if (state->nrLiveFibers > maxLiveFibers)
                    maxLiveFibers = state->nrLiveFibers;
                break;
            }

            /* Nothing to do (or our fiber limit has been reached);
               sleep until a producer wakes us up. Note: `sleeping` is
               maintained under the state lock, so a producer either
               sees it (and wakes us), or its insertion happened before
               our checks above — no lost wakeups. */
            self.sleeping = true;
            state.wait(self.wakeup);
            self.sleeping = false;
        }

        if (item) {
            fiber = makeFiber(self, std::move(*item));
            if (!fiber) {
                /* Stack allocation failure; the item's promise has
                   received the exception. Release the fiber slot. */
                finished = true;
                continue;
            }
        }

        finished = runFiber(std::move(fiber));
    }
}

bool Executor::hasBacklog()
{
    auto state(state_.lock());
    auto n = state->queue.size();
    for (auto & worker : workers)
        n += worker->readyFibers.size();
    return n >= evalCores;
}

std::vector<std::future<void>> Executor::spawn(WorkItems && items)
{
    if (items.empty())
        return {};

    std::vector<std::future<void>> futures;

    auto state(state_.lock());

    if (quit) {
        /* The workers may have exited already, so nobody would ever
           pick up these items. */
        for (auto & _ : items) {
            std::promise<void> promise;
            futures.push_back(promise.get_future());
            promise.set_exception(std::make_exception_ptr(Interrupted("interrupted by the user")));
        }
        return futures;
    }

    for (auto & item : items) {
        std::promise<void> promise;
        futures.push_back(promise.get_future());
        static thread_local uint32_t local = 0;
        auto key = (uint64_t(item.second) << 48) | local++;
        state->queue.emplace(key, Item{.promise = std::move(promise), .work = std::move(item.first)});
    }

    /* Wake up one worker per item, but only workers that are asleep
       and can actually start a fresh item (a worker at its fiber
       limit would just go back to sleep). In the steady state (all
       workers busy), this does no futex calls at all. */
    auto toWake = items.size();
    for (auto & worker : workers) {
        if (toWake == 0)
            break;
        if (canStartFiber(*worker) && wakeWorker(*worker))
            toWake--;
    }

    return futures;
}

FutureVector::~FutureVector()
{
    try {
        finishAll();
    } catch (...) {
        ignoreExceptionInDestructor();
    }
}

void FutureVector::spawn(Executor::WorkItems && work)
{
    auto futures = executor.spawn(std::move(work));
    auto state(state_.lock());
    for (auto & future : futures)
        state->futures.push_back(std::move(future));
}

void FutureVector::finishAll()
{
    std::exception_ptr ex;
    while (true) {
        std::vector<std::future<void>> futures;
        {
            auto state(state_.lock());
            std::swap(futures, state->futures);
        }
        debug("got %d futures", futures.size());
        if (futures.empty())
            break;
        for (auto & future : futures)
            try {
                future.get();
            } catch (...) {
                if (ex) {
                    if (!getInterrupted())
                        logExceptionExceptInterrupt();
                } else
                    ex = std::current_exception();
            }
    }
    if (ex)
        std::rethrow_exception(ex);
}

static WaiterDomain & getWaiterDomain(detail::ValueBase & v)
{
    auto domain = (((size_t) &v) >> 5) % waiterDomains.size();
    return waiterDomains[domain];
}

/**
 * Suspend the current fiber until `v` is finished. Must be called with
 * `lk` holding the domain's mutex and `v` in the "awaited" state. On
 * return, the fiber has been resumed (by its owner thread, after other
 * fibers may have run on it) and the lock has been released.
 */
[[gnu::noinline]] static void
suspendFiber(WaiterDomain & domain, std::unique_lock<std::mutex> & lk, detail::ValueBase & v)
{
    auto fib = currentFiber;
    assert(fib);
    /* A fiber must not suspend while an exception is being handled or
       unwound: the C++ exception state lives in thread-local storage,
       so it would get mixed up with that of the other fibers that run
       on this thread in the meantime. */
    assert(!std::current_exception() && !std::uncaught_exceptions());
    fib->waitingOn = &v;
    fib->suspendDomain = &domain;
    /* Hand ownership of the domain mutex over to the scheduler, which
       will unlock it (via `domain.mutex`) after registering us in the
       wait list. `lk` lives on this stack, so it must not be
       considered owning anymore once we've switched away. */
    lk.release();
#if NIX_USE_BOEHMGC
    /* Publish the used portion of our stack, so that the garbage
       collector will scan it while we're suspended (see
       `gcStackSwitchSlack` for why the approximation is lowered).
       Note: from this point until we clear `saved_sp` after being
       resumed, the GC may see the stack both as some thread's active
       stack and as a suspended one; it scans it only once (from the
       lower stack pointer). */
    fib->gcStack->saved_sp = (char *) GC_get_approx_sp() - gcStackSwitchSlack;
#endif
    /* Switch back to the scheduler (`Executor::runFiber()`), which
       will register us in the domain's wait list and then release the
       lock. We can't do that here: the fiber's continuation only
       materializes on the scheduler side, and publishing it before the
       switch-out completes would allow another thread to resume a
       half-suspended fiber. */
    fib->schedCtx = std::move(fib->schedCtx).resume();
#if NIX_USE_BOEHMGC
    /* We're running again, so our stack is scanned as the thread's
       active stack from here on. */
    fib->gcStack->saved_sp = nullptr;
#endif
    /* We've been resumed because the value was finished (or because
       we're shutting down); the scheduler released the lock long
       ago. */
    assert(!lk.owns_lock());
}

template<>
ValueStorage<sizeof(void *)>::PackedPointer
ValueStorage<sizeof(void *)>::waitOnThunk(EvalState & state, PackedPointer expectedP0)
{
    state.nrThunksAwaited++;

    auto & domain = getWaiterDomain(*this);
    std::unique_lock lk(domain.mutex);

    auto threadId = expectedP0 >> discriminatorBits;

    if (static_cast<PrimaryDiscriminator>(expectedP0 & discriminatorMask) == pdAwaited) {
        /* Make sure that the value is still awaited, now that we're
           holding the domain lock. */
        auto p0_ = p0.load(std::memory_order_acquire);
        auto pd = static_cast<PrimaryDiscriminator>(p0_ & discriminatorMask);

        /* If the value has been finalized in the meantime (i.e. is no
           longer pending), we're done. */
        if (pd != pdAwaited) {
            assert(pd != pdThunk && pd != pdPending);
            return p0_;
        }
    } else {
        /* Mark this value as being waited on. */
        PackedPointer p0_ = expectedP0;
        if (!p0.compare_exchange_strong(
                p0_,
                pdAwaited | (threadId << discriminatorBits),
                std::memory_order_acquire,
                std::memory_order_acquire)) {
            /* If the value has been finalized in the meantime (i.e. is
               no longer pending), we're done. */
            auto pd = static_cast<PrimaryDiscriminator>(p0_ & discriminatorMask);
            if (pd != pdAwaited) {
                assert(pd != pdThunk && pd != pdPending);
                return p0_;
            }
            /* The value was already in the "waited on" state, so we're
               not the only thread waiting on it. */
        }
    }

    /* Wait for another thread to finish this value. */
    if (threadId == myEvalThreadId)
        state.error<InfiniteRecursionError>("infinite recursion encountered")
            .atPos(((Value &) *this).determinePos(noPos))
            .debugThrow();

    state.nrThunksAwaitedSlow++;
    state.currentlyWaiting++;
    state.maxWaiting = std::max<uint64_t>(state.maxWaiting, state.currentlyWaiting);

    auto now1 = std::chrono::steady_clock::now();

    if (auto fib = currentFiber) {
        /* Shutdown guard: `quit`/`_isInterrupted` are always set
           *before* the wait lists are flushed, and flushing takes the
           domain lock that we're currently holding. So either we see
           the flag here and throw, or our registration completes
           before the flush and we get woken by it — we can't be
           stranded in the wait list. */
        if (fib->executor.quit)
            throw Interrupted("interrupted by the user");
        checkInterrupt();

        /* We're running on a fiber, so suspend it and let this thread
           run other work. `notifyWaiters()` will re-enqueue the fiber
           when the value is finished. */
        suspendFiber(domain, lk, *this);

        /* Note: `currentFiber` has been restored to `fib` by
           `runFiber()`, but the captured `fib` is cheaper. */
        if (fib->executor.quit)
            throw Interrupted("interrupted by the user");
        checkInterrupt();

        auto p0_ = p0.load(std::memory_order_acquire);
        auto pd = static_cast<PrimaryDiscriminator>(p0_ & discriminatorMask);
        /* Unlike the condition variable path below, fiber wakeups
           cannot be spurious: the wait list is keyed on the exact
           value and we're only woken after it has been finished (or on
           shutdown/interrupt, handled above). */
        assert(pd != pdThunk && pd != pdPending && pd != pdAwaited);
        auto now2 = std::chrono::steady_clock::now();
        state.microsecondsWaiting += std::chrono::duration_cast<std::chrono::microseconds>(now2 - now1).count();
        state.currentlyWaiting--;
        return p0_;
    }

    while (true) {
        domain.cv.wait(lk);
        auto p0_ = p0.load(std::memory_order_acquire);
        auto pd = static_cast<PrimaryDiscriminator>(p0_ & discriminatorMask);
        if (pd != pdAwaited) {
            assert(pd != pdThunk && pd != pdPending);
            auto now2 = std::chrono::steady_clock::now();
            state.microsecondsWaiting += std::chrono::duration_cast<std::chrono::microseconds>(now2 - now1).count();
            state.currentlyWaiting--;
            return p0_;
        }
        state.nrSpuriousWakeups++;
        checkInterrupt();
    }
}

template<>
void ValueStorage<sizeof(void *)>::notifyWaiters()
{
    auto & domain = getWaiterDomain(*this);

    /* Extract the fibers waiting on this value, then re-enqueue them
       after releasing the domain lock (to keep a trivial lock order
       between domain mutexes and the executor's state lock). */
    std::vector<Executor::FiberPtr> woken;
    {
        std::unique_lock lk(domain.mutex);
        if (auto nh = domain.waiters.extract(this))
            woken = std::move(nh.mapped());
        /* Wake up any non-fiber waiters (e.g. the main thread). */
        domain.cv.notify_all();
    }

    for (auto & fiber : woken) {
        auto & executor = fiber->executor;
        executor.enqueueFiber(std::move(fiber));
    }
}

static void prim_parallel(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceList(*args[0], pos, "while evaluating the first argument passed to builtins.parallel");

    if (state.executor->enabled) {
        Executor::WorkItems work;
        for (auto value : args[0]->listView())
            if (!value->isFinished())
                state.addWork(work, 0, [value(RootValue(value)), &state, pos]() { state.forceValue(**value, pos); });
        state.executor->spawn(std::move(work));
    }

    state.forceValue(*args[1], pos);
    v = *args[1];
}

// FIXME: gate this behind an experimental feature.
static RegisterPrimOp r_parallel({
    .name = "__parallel",
    .args = {"xs", "x"},
    .arity = 2,
    .doc = R"(
      Start evaluation of the values `xs` in the background and return `x`.
    )",
    .impl = prim_parallel,
    .experimentalFeature = Xp::ParallelEval,
});

#pragma GCC diagnostic ignored "-Wswitch-enum"

void EvalState::forceValueDeepParallel(Value & vRoot, PosIdx pos, bool spawnThunks)
{
    if (!executor->enabled)
        return;

    // FIXME: the pointers in this set can refer to values that been GCed and then reallocated. That's not a problem for
    // correctness, since at worst it prevents background evaluation of some values. But we should probably register a
    // GC hook to clear this set at GC time.
    static boost::concurrent_flat_set<Value *> seen;

    Executor::WorkItems work;

    /* Bound the recursion depth of the walk: deeply nested values
       (e.g. a 100000-element linked list of attrsets) would otherwise
       overflow the C++ stack, and the walk is best-effort anyway. The
       demand path will report a proper `max-call-depth` error for
       such values. */
    constexpr size_t maxDepth = 1024;

    auto recurse =
        [&](this const auto & recurse, EvalState & state, Value & v, PosIdx pos, bool isRoot, size_t depth) -> void {
        if (depth > maxDepth)
            return;

        auto type = v.type();
        if (type == nString || type == nPath || type == nNull || type == nInt || type == nFloat || type == nBool
            || type == nFailed || type == nExternal)
            return;

        /* The root of this walk was typically already added to `seen`
           by the walk that spawned us (when it was still a thunk), so
           it must be visited regardless. But that exemption must not
           apply when the root is reached again *inside* its own
           subtree (e.g. a nixpkgs package set `pkgs` has an attribute
           `pkgs` that is the same value), since the walk would then
           never terminate. */
        if (!seen.insert(&v) && !isRoot)
            return;

        if (type == nThunk) {
            if (spawnThunks) {
                state.addWork(work, 0, [v(RootValue(&v)), pos, &state]() { state.forceValueDeepParallel(**v, pos); });
                return;
            }
            /* Force the thunk right here. Most thunks are cheap (e.g.
               the attributes of a derivation), so a work item per
               thunk would cost far more in scheduling than it gains
               in parallelism. Errors are left to whoever needs this
               value (e.g. `derivationStrict`), which will report them
               with the proper context. */
            try {
                state.forceValue(v, pos);
            } catch (Interrupted &) {
                throw;
            } catch (Error &) {
                return;
            }
            type = v.type();
            if (type == nString || type == nPath || type == nNull || type == nInt || type == nFloat || type == nBool
                || type == nFailed || type == nExternal)
                return;
        }

        switch (v.type()) {

        case nAttrs: {

            NixStringContext context;
            if (state.tryAttrsToString(pos, v, context, false, false))
                return;

            if (auto aOutPath = v.attrs()->get(s.outPath)) {
                /* This attrset is coercible to a string via its
                   `outPath` (e.g. a derivation or a flake input source
                   tree), which is all that string coercion and JSON
                   serialisation ever look at. So only force `outPath`
                   in the background; for a derivation, that
                   instantiates it (calling `derivationStrict`, which
                   in turn recursively spawns *its* inputs). We must
                   not walk any other attribute: they are user-visible
                   and may hold arbitrary expressions (e.g. nixpkgs's
                   `nodejs` wraps its `drvAttrs` in `lib.warn`, and a
                   flake input's `inputs` leads to entire package
                   sets). */
                if (!aOutPath->value->isFinished())
                    state.addWork(work, 0, [v(RootValue(aOutPath->value)), pos(aOutPath->pos), &state]() {
                        state.forceValue(**v, pos);
                    });

            } else {
                for (auto & a : *v.attrs())
                    recurse(state, *a.value, a.pos, false, depth + 1);
            }

            break;
        }

        case nList: {
            for (const auto & elem : v.listView())
                recurse(state, *elem, pos, false, depth + 1);
            break;
        }

        default:
            break;
        }
    };

    forceValue(vRoot, pos);

    recurse(*this, vRoot, pos, true, 0);

    if (work.size() == 1)
        // Only one work item, so we may as well do it on the current thread right away.
        work[0].first();
    else
        executor->spawn(std::move(work));
}

} // namespace nix
