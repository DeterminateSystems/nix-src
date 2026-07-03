#include "nix/expr/eval.hh"
#include "nix/expr/parallel-eval.hh"
#include "nix/store/globals.hh"
#include "nix/expr/primops.hh"

namespace nix {

// cache line alignment to prevent false sharing
struct alignas(64) WaiterDomain
{
    std::condition_variable cv;
};

static std::array<Sync<WaiterDomain>, 128> waiterDomains;

[[gnu::tls_model("initial-exec")]] thread_local bool Executor::amWorkerThread{false};

unsigned int Executor::getEvalCores(const EvalSettings & evalSettings)
{
    /* Note: the default number of cores is currently limited to 32
       due to scalability bottlenecks. */
    return evalSettings.evalProfilerMode != EvalProfilerMode::disabled ? 1
           : evalSettings.evalCores == 0UL                             ? std::min(32U, Settings::getDefaultCores())
                                                                       : evalSettings.evalCores;
}

Executor::Executor(const EvalSettings & evalSettings)
    : evalCores(getEvalCores(evalSettings))
    , enabled(evalCores > 1)
    , interruptCallback(createInterruptCallback([&]() {
        for (auto & domain : waiterDomains)
            domain.lock()->cv.notify_all();
    }))
{
    debug("executor using %d threads", evalCores);
    auto state(state_.lock());
    // FIXME: create worker threads on demand?
    for (size_t n = 0; n < evalCores; ++n)
        try {
            createWorker(*state);
        } catch (boost::thread_resource_error & e) {
            if (n == 0)
                throw Error("could not create any evaluator worker threads: %s", e.what());
            warn("could only create %d evaluator worker threads: %s", n, e.what());
            break;
        }
}

Executor::~Executor()
{
    std::vector<boost::thread> threads;
    {
        auto state(state_.lock());
        quit = true;
        std::swap(threads, state->threads);
        debug("executor shutting down with %d items left", state->queue.size());
    }

    wakeup.notify_all();

    for (auto & thr : threads)
        thr.join();
}

void Executor::createWorker(State & state)
{
    boost::thread::attributes attrs;
    attrs.set_stack_size(evalStackSize);
    state.threads.push_back(boost::thread(attrs, [&]() {
#if NIX_USE_BOEHMGC
        GC_stack_base sb;
        GC_get_stack_base(&sb);
        GC_register_my_thread(&sb);
#endif
        worker();
#if NIX_USE_BOEHMGC
        GC_unregister_my_thread();
#endif
    }));
}

void Executor::worker()
{
    ReceiveInterrupts receiveInterrupts;

    unix::interruptCheck = [&]() { return (bool) quit; };

    amWorkerThread = true;

    while (true) {
        Item item;

        while (true) {
            auto state(state_.lock());
            if (quit) {
                // Record an `Interrupted` exception for all queued
                // items so that `FutureVector::finishAll()` wakes up
                // and reports a nice error.
                auto ex = std::make_exception_ptr(Interrupted("interrupted by the user"));
                for (auto & item : state->queue)
                    if (item.second.completion)
                        item.second.completion->finish(ex);
                state->queue.clear();
                return;
            }
            if (!state->queue.empty()) {
                item = std::move(state->queue.begin()->second);
                state->queue.erase(state->queue.begin());
                break;
            }
            state.wait(wakeup);
        }

        std::exception_ptr ex;
        try {
            item.work();
        } catch (const Interrupted &) {
            quit = true;
            ex = std::current_exception();
        } catch (...) {
            ex = std::current_exception();
        }
        if (item.completion)
            item.completion->finish(std::move(ex));
    }
}

void Executor::spawn(WorkItems && items, std::shared_ptr<Completion> completion)
{
    if (items.empty())
        return;

    {
        auto state(state_.lock());
        for (auto & item : items) {
            /* Note: this uses a cheap PRNG rather than std::random_device,
               since the latter costs hundreds of cycles per call (RDRAND or
               /dev/urandom), which adds up when spawning many work items. The
               key only needs to spread items of the same priority around the
               queue, not be cryptographically random. */
            [[gnu::tls_model("initial-exec")]] static thread_local std::mt19937_64 rng{std::random_device{}()};
            [[gnu::tls_model("initial-exec")]] static thread_local std::uniform_int_distribution<uint64_t> dist(
                0, 1ULL << 48);
            auto key = (uint64_t(item.second) << 48) | dist(rng);
            state->queue.emplace(key, Item{.work = std::move(item.first), .completion = completion});
        }
    }

    if (items.size() == 1)
        wakeup.notify_one();
    else
        wakeup.notify_all();
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
    /* Increment the pending count before queueing the items, since
       they may finish immediately. */
    completion->pending.fetch_add(work.size(), std::memory_order_relaxed);
    executor.spawn(std::move(work), completion);
}

void FutureVector::finishAll()
{
    std::vector<std::exception_ptr> exceptions;

    {
        auto state(completion->state_.lock());
        while (completion->pending.load(std::memory_order_acquire) != 0)
            state.wait(completion->cv);
        std::swap(exceptions, state->exceptions);
    }

    std::exception_ptr ex;
    for (auto & ex2 : exceptions) {
        if (ex) {
            if (!getInterrupted())
                try {
                    std::rethrow_exception(ex2);
                } catch (...) {
                    logExceptionExceptInterrupt();
                }
        } else
            ex = ex2;
    }
    if (ex)
        std::rethrow_exception(ex);
}

static Sync<WaiterDomain> & getWaiterDomain(detail::ValueBase & v)
{
    auto domain = (((size_t) &v) >> 5) % waiterDomains.size();
    return waiterDomains[domain];
}

static std::atomic<uint32_t> nextEvalThreadId{1};
[[gnu::tls_model("initial-exec")]] thread_local uint32_t myEvalThreadId(nextEvalThreadId++);

template<>
ValueStorage<sizeof(void *)>::PackedPointer
ValueStorage<sizeof(void *)>::waitOnThunk(EvalState & state, PackedPointer expectedP0)
{
    state.nrThunksAwaited++;

    auto domain = getWaiterDomain(*this).lock();

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

    while (true) {
        domain.wait(domain->cv);
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
    auto domain = getWaiterDomain(*this).lock();

    domain->cv.notify_all();
}

static void prim_parallel(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceList(*args[0], pos, "while evaluating the first argument passed to builtins.parallel");

    if (state.executor->enabled) {
        Executor::WorkItems work;
        for (auto value : args[0]->listView())
            if (!value->isFinished())
                state.addWork(
                    work, 0, [value(allocRootValue(value)), &state, pos]() { state.forceValue(**value, pos); });
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
