#include "nix/expr/eval.hh"
#include "nix/expr/parallel-eval.hh"
#include "nix/store/globals.hh"
#include "nix/expr/primops.hh"

#include <boost/context/fiber.hpp>
#include <boost/context/protected_fixedsize_stack.hpp>

#include <unordered_map>

#if NIX_USE_BOEHMGC
#  include <gc.h>
#  include <gc/gc_mark.h>
#endif

namespace nix {

#if NIX_USE_BOEHMGC

/**
 * Registry of fiber stacks for the garbage collector. This is
 * append-only and grows in fixed-size blocks that never move, so the
 * GC callbacks (which run with the world stopped and therefore must
 * not take any locks that a frozen thread might hold) can safely
 * iterate it at any moment. Entries are keyed by stack, not by fiber:
 * thanks to the stack pool, only a modest number of stacks is ever
 * allocated, and pooled stacks keep their (inert) entry.
 */
struct FiberStackInfo
{
    /**
     * The hi end of the stack (i.e. `stack_context::sp`). Immutable
     * once published.
     */
    char * base = nullptr;

    /**
     * The size of the stack. Immutable once published.
     */
    size_t size = 0;

    /**
     * If non-null, the GC must scan `[scanSp, base)`, i.e. the used
     * portion of the stack of a suspended fiber. If null, the stack
     * must not be scanned at all: it's either free (in the stack
     * pool), unstarted (no GC roots yet), or running (in which case
     * it's covered by the thread stack scan via the sp corrector, see
     * `fixupBoehmStackPointer()`).
     */
    std::atomic<char *> scanSp{nullptr};
};

constexpr static size_t fiberStackBlockSize = 1024;
using FiberStackBlock = std::array<FiberStackInfo, fiberStackBlockSize>;
static std::array<std::atomic<FiberStackBlock *>, 1024> fiberStackBlocks;
static std::atomic<size_t> nrFiberStacks{0};
static std::mutex fiberStackRegistryMutex;

static FiberStackInfo & getFiberStackInfo(size_t i)
{
    return (*fiberStackBlocks[i / fiberStackBlockSize].load(std::memory_order_acquire))[i % fiberStackBlockSize];
}

static FiberStackInfo * registerFiberStack(char * base, size_t size)
{
    std::unique_lock lk(fiberStackRegistryMutex);
    auto i = nrFiberStacks.load(std::memory_order_relaxed);
    if (i / fiberStackBlockSize >= fiberStackBlocks.size())
        throw Error("too many fiber stacks");
    auto & blockPtr = fiberStackBlocks[i / fiberStackBlockSize];
    if (!blockPtr.load(std::memory_order_relaxed))
        blockPtr.store(new FiberStackBlock(), std::memory_order_release);
    auto & info = getFiberStackInfo(i);
    info.base = base;
    info.size = size;
    /* Publish the entry only after its fields are initialized. */
    nrFiberStacks.store(i + 1, std::memory_order_release);
    return &info;
}

void * fiberStackContaining(void * sp)
{
    auto n = nrFiberStacks.load(std::memory_order_acquire);
    for (size_t i = 0; i < n; ++i) {
        auto & info = getFiberStackInfo(i);
        if ((char *) sp < info.base && (char *) sp >= info.base - info.size)
            return info.base;
    }
    return nullptr;
}

void pushSuspendedFiberStacks()
{
    auto n = nrFiberStacks.load(std::memory_order_acquire);
    for (size_t i = 0; i < n; ++i) {
        auto & info = getFiberStackInfo(i);
        if (auto sp = info.scanSp.load(std::memory_order_acquire))
            GC_push_all(sp, info.base);
    }
}

/**
 * Registry of the worker threads' pthread ids, so that
 * `isEvalWorkerThread()` can be called from the GC's sp corrector
 * (with the world stopped, hence the lock-free fixed-size array).
 */
static std::array<std::atomic<std::uintptr_t>, 256> workerPthreads;

static void registerWorkerPthread(pthread_t p)
{
    for (auto & slot : workerPthreads) {
        std::uintptr_t expected = 0;
        if (slot.compare_exchange_strong(expected, (std::uintptr_t) p))
            return;
    }
    /* Table full; harmless (the thread will merely be scanned in full
       by the GC). */
}

static void unregisterWorkerPthread(pthread_t p)
{
    for (auto & slot : workerPthreads)
        if (slot.load(std::memory_order_relaxed) == (std::uintptr_t) p) {
            slot.store(0);
            return;
        }
}

bool isEvalWorkerThread(void * pthreadId)
{
    for (auto & slot : workerPthreads)
        if (slot.load(std::memory_order_relaxed) == (std::uintptr_t) (pthread_t) pthreadId)
            return true;
    return false;
}

#endif

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

struct Executor::Fiber
{
    Executor & executor;

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
     * `CallDepth`) on every switch-in/out, so that the depth
     * accounting survives the fiber being suspended mid-call-chain
     * and resumed on a different thread.
     */
    size_t callDepth = 0;

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
     * The GC registry entry for this fiber's stack.
     */
    FiberStackInfo * stackInfo = nullptr;
#endif

    Fiber(Executor & executor, Item && item)
        : executor(executor)
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
         * The GC registry entry for each stack ever allocated, keyed
         * by the stack's hi end (`stack_context::sp`).
         */
        std::unordered_map<void *, FiberStackInfo *> gcInfo;
#endif
    };

    Sync<State> state_;

    ~StackPool()
    {
        for (auto & sctx : state_.lock()->stacks)
            boost::context::protected_fixedsize_stack(evalStackSize).deallocate(sctx);
    }
};

#if NIX_USE_BOEHMGC
/**
 * Side channel from `PooledStackAllocator::allocate()` (called from
 * inside the `boost::context::fiber` constructor) to `makeFiber()`:
 * the GC registry entry of the most recently allocated stack.
 */
[[gnu::tls_model("initial-exec")]] static thread_local FiberStackInfo * lastFiberStackInfo{nullptr};
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
                lastFiberStackInfo = state->gcInfo.at(sctx.sp);
#endif
                return sctx;
            }
        }
        executor.nrFiberStacksAllocated++;
        auto sctx = boost::context::protected_fixedsize_stack(evalStackSize).allocate();
#if NIX_USE_BOEHMGC
        auto info = registerFiberStack((char *) sctx.sp, sctx.size);
        executor.stackPool->state_.lock()->gcInfo.emplace(sctx.sp, info);
        lastFiberStackInfo = info;
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
 * executor's ready queue, and wake up all non-fiber waiters. Called on
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

Executor::Executor(const EvalSettings & evalSettings)
    : stackPool(std::make_unique<StackPool>())
    , evalCores(getEvalCores(evalSettings))
    , enabled(evalCores > 1)
    , interruptCallback(createInterruptCallback([&]() {
        /* Wake up all waiting fibers and threads so they can observe
           the interrupt and unwind. Note: `_isInterrupted` has already
           been set at this point. */
        flushWaiters();
        wakeup.notify_all();
    }))
{
    debug("executor using %d threads", evalCores);
    auto state(state_.lock());
    // FIXME: create worker threads on demand?
    for (size_t n = 0; n < evalCores; ++n)
        try {
            createWorker(*state);
        } catch (std::system_error & e) {
            if (n == 0)
                throw Error("could not create any evaluator worker threads: %s", e.what());
            warn("could only create %d evaluator worker threads: %s", n, e.what());
            break;
        }
}

Executor::~Executor()
{
    std::vector<std::thread> threads;
    {
        auto state(state_.lock());
        quit = true;
        std::swap(threads, state->threads);
        debug("executor shutting down with %d items left", state->queue.size());
    }

    /* Wake up suspended fibers and idle workers so they can wind
       down. */
    flushWaiters();
    wakeup.notify_all();

    for (auto & thr : threads)
        thr.join();

    /* Handle any stragglers, e.g. fibers that were re-enqueued by
       `notifyWaiters()` on the main thread after the workers already
       exited. No fiber stack may outlive this destructor. */
    flushWaiters();
    drainQueue();
}

void Executor::createWorker(State & state)
{
    /* Note: worker threads can have a small (default-sized) stack,
       since they only run the scheduler loop; the actual evaluation
       happens on fibers, which have their own stacks. */
    state.threads.push_back(std::thread([&]() {
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
        /* Let the GC's sp corrector know that this thread's own stack
           holds no GC roots (see `fixupBoehmStackPointer()`). */
        registerWorkerPthread(pthread_self());
#endif
        worker();
#if NIX_USE_BOEHMGC
        unregisterWorkerPthread(pthread_self());
        GC_unregister_my_thread();
#endif
    }));
}

Executor::FiberPtr Executor::makeFiber(Item && item)
{
    auto fiber = std::make_unique<Fiber>(*this, std::move(item));
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
    fiber->stackInfo = lastFiberStackInfo;
    assert(fiber->stackInfo);
#endif
    nrFibersSpawned++;
    return fiber;
}

void Executor::runFiber(FiberPtr fiber)
{
    auto fib = fiber.get();
    assert(fib->ctx);
    assert(!currentFiber);

    auto savedThreadId = myEvalThreadId;
    auto savedCallDepth = CallDepth::callDepth;
    currentFiber = fib;
    myEvalThreadId = fib->evalThreadId;
    CallDepth::callDepth = fib->callDepth;

    fib->ctx = std::move(fib->ctx).resume();

    currentFiber = nullptr;
    myEvalThreadId = savedThreadId;
    fib->callDepth = CallDepth::callDepth;
    CallDepth::callDepth = savedCallDepth;

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
        /* `fib` may be resumed by another thread from this point on,
           so don't touch it anymore. */
    } else {
        /* The fiber has finished; its promise has been fulfilled
           inside the fiber. Destroying `fiber` frees the record (the
           stack was already freed on fiber termination). */
    }
}

void Executor::enqueueFiber(FiberPtr fiber)
{
    nrFiberWakeups++;
    currentSuspendedFibers--;
    {
        auto state(state_.lock());
        /* Note: key 0 means that resumed fibers run before any fresh
           work items, so existing work is drained first. */
        state->queue.emplace(0, std::move(fiber));
    }
    wakeup.notify_one();
}

void Executor::worker()
{
    ReceiveInterrupts receiveInterrupts;

    unix::interruptCheck = [&]() { return (bool) quit; };

    amWorkerThread = true;

    while (true) {
        QueueEntry entry;
        bool gotEntry = false;

        while (true) {
            auto state(state_.lock());
            if (quit)
                break;
            if (!state->queue.empty()) {
                entry = std::move(state->queue.begin()->second);
                state->queue.erase(state->queue.begin());
                gotEntry = true;
                break;
            }
            state.wait(wakeup);
        }

        if (!gotEntry) {
            drainQueue();
            return;
        }

        if (auto * item = std::get_if<Item>(&entry)) {
            if (auto fiber = makeFiber(std::move(*item)))
                runFiber(std::move(fiber));
        } else
            runFiber(std::move(std::get<FiberPtr>(entry)));
    }
}

void Executor::drainQueue()
{
    while (true) {
        /* Keep flushing the wait lists: a fiber resumed below can
           finish thunks, which normally re-enqueues their waiters, but
           late waiters may still be parked. */
        flushWaiters();

        QueueEntry entry;
        {
            auto state(state_.lock());
            if (state->queue.empty())
                return;
            entry = std::move(state->queue.begin()->second);
            state->queue.erase(state->queue.begin());
        }

        if (auto * item = std::get_if<Item>(&entry))
            // Set an `Interrupted` exception on work items that
            // haven't started, so we get a nicer error than
            // "std::future_error: Broken promise". Note: a fresh
            // exception per item, not a shared one.
            item->promise.set_exception(std::make_exception_ptr(Interrupted("interrupted by the user")));
        else
            /* Resume the fiber so it can observe `quit` (or the
               interrupt) and unwind; it cannot suspend again thanks to
               the guard in `waitOnThunk()`. Never destroy a suspended
               fiber. */
            runFiber(std::move(std::get<FiberPtr>(entry)));
    }
}

std::vector<std::future<void>> Executor::spawn(WorkItems && items)
{
    if (items.empty())
        return {};

    std::vector<std::future<void>> futures;

    {
        auto state(state_.lock());
        for (auto & item : items) {
            std::promise<void> promise;
            futures.push_back(promise.get_future());
            /* Note: this uses a cheap PRNG rather than std::random_device,
               since the latter costs hundreds of cycles per call (RDRAND or
               /dev/urandom), which adds up when spawning many work items. The
               key only needs to spread items of the same priority around the
               queue, not be cryptographically random. */
            [[gnu::tls_model("initial-exec")]] static thread_local std::mt19937_64 rng{std::random_device{}()};
            [[gnu::tls_model("initial-exec")]] static thread_local std::uniform_int_distribution<uint64_t> dist(
                0, 1ULL << 48);
            auto key = (uint64_t(item.second) << 48) | dist(rng);
            state->queue.emplace(key, Item{.promise = std::move(promise), .work = std::move(item.first)});
        }
    }

    if (items.size() == 1)
        wakeup.notify_one();
    else
        wakeup.notify_all();

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
 * return, the fiber has been resumed (possibly on a different thread)
 * and the lock has been released.
 *
 * Note: `noinline` so that the compiler doesn't move TLS accesses of
 * the caller across the context switch (the fiber may wake up on
 * another thread).
 */
[[gnu::noinline]] static void
suspendFiber(WaiterDomain & domain, std::unique_lock<std::mutex> & lk, detail::ValueBase & v)
{
    auto fib = currentFiber;
    assert(fib);
    /* A fiber must not suspend while an exception is being handled or
       unwound: the C++ exception state lives in thread-local storage,
       so it wouldn't survive being resumed on another thread. */
    assert(!std::current_exception() && !std::uncaught_exceptions());
    fib->waitingOn = &v;
    fib->suspendDomain = &domain;
    /* Hand ownership of the domain mutex over to the scheduler, which
       will unlock it (via `domain.mutex`) after registering us in the
       wait list. We must not let the scheduler unlock through `lk`:
       `unique_lock::unlock()` releases the mutex *before* clearing its
       owns-flag, and the moment the mutex is released, this fiber can
       be resumed on another thread, which would then read the flag on
       this stack concurrently with the scheduler's write. */
    lk.release();
#if NIX_USE_BOEHMGC
    /* Publish the used portion of our stack, so that the garbage
       collector will scan it while we're suspended. The slack below
       the current frame covers the register block that the context
       switch is about to push; those bytes are touched by the switch
       anyway, so this doesn't fault in any untouched stack pages.
       Note: from this point until the resumed fiber clears `scanSp`,
       the stack may be scanned both through this entry and through
       the thread stack scan (see `fixupBoehmStackPointer()`); the
       overlap is harmless. */
    fib->stackInfo->scanSp.store((char *) __builtin_frame_address(0) - 512, std::memory_order_release);
#endif
    /* Switch back to the scheduler (`Executor::runFiber()`), which
       will register us in the domain's wait list and then release the
       lock. We can't do that here: the fiber's continuation only
       materializes on the scheduler side, and publishing it before the
       switch-out completes would allow another thread to resume a
       half-suspended fiber. */
    fib->schedCtx = std::move(fib->schedCtx).resume();
#if NIX_USE_BOEHMGC
    /* We're running again (possibly on another thread), so our stack
       is covered by the thread stack scan from here on. */
    fib->stackInfo->scanSp.store(nullptr, std::memory_order_release);
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

        /* Note: use the `fib` captured before the suspension rather
           than TLS variables, since we may have been resumed on a
           different thread. */
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

} // namespace nix
