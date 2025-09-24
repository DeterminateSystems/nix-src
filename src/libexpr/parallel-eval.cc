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

thread_local bool Executor::amWorkerThread{false};

unsigned int Executor::getEvalCores(const EvalSettings & evalSettings)
{
    return evalSettings.evalCores == 0UL ? Settings::getDefaultCores() : evalSettings.evalCores;
}

Executor::Executor(const EvalSettings & evalSettings)
    : evalCores(getEvalCores(evalSettings))
    , enabled(evalCores > 1)
    , interruptCallback(createInterruptCallback([&]() { wakeAll(); }))
{
    debug("executor using %d threads", evalCores);
    auto state(state_.lock());
    for (size_t n = 0; n < evalCores; ++n)
        createWorker(*state);
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

    assert(state_.lock()->nrTotalThreads == 0);
    assert(state_.lock()->nrInactiveThreads == 0);
}

void Executor::wakeAll()
{
    for (auto & domain : waiterDomains)
        domain.lock()->cv.notify_all();
}

bool Executor::checkDeadlock(State & state, size_t extraActiveThreads)
{
    return nrUnblocked == 0
           && (state.queue.empty() ? state.nrInactiveThreads < state.nrTotalThreads + extraActiveThreads
                                   : state.nrInactiveThreads == 0);
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
    state.nrTotalThreads++;
    state.nrInactiveThreads++;
}

void Executor::worker()
{
    ReceiveInterrupts receiveInterrupts;

    unix::interruptCheck = [&]() { return (bool) quit; };

    amWorkerThread = true;
    bool markInactive = false;

    while (true) {
        Item item;

        while (true) {
            auto state(state_.lock());
            if (markInactive) {
                markInactive = false;
                state->nrInactiveThreads++;
            }
            if (checkDeadlock(*state, 0)) {
                deadlocked = true;
                wakeAll();
            }
            if (quit) {
                // Set an `Interrupted` exception on all promises so
                // we get a nicer error than "std::future_error:
                // Broken promise".
                auto ex = std::make_exception_ptr(Interrupted("interrupted by the user2"));
                for (auto & item : state->queue)
                    item.second.promise.set_exception(ex);
                state->queue.clear();
                state->nrTotalThreads--;
                state->nrInactiveThreads--;
                return;
            }
            if (!state->queue.empty()) {
                item = std::move(state->queue.begin()->second);
                state->queue.erase(state->queue.begin());
                state->nrInactiveThreads--;
                markInactive = true;
                nrUnblocked++;
                break;
            }
            state.wait(wakeup);
        }

        Finally f = [&]() { nrUnblocked--; };

        try {
            item.work();
            item.promise.set_value();
        } catch (const Interrupted &) {
            quit = true;
            item.promise.set_exception(std::current_exception());
        } catch (...) {
            item.promise.set_exception(std::current_exception());
        }
    }
}

std::vector<std::future<void>> Executor::spawn(std::vector<std::pair<work_t, uint8_t>> && items)
{
    if (items.empty())
        return {};

    std::vector<std::future<void>> futures;

    {
        auto state(state_.lock());
        for (auto & item : items) {
            std::promise<void> promise;
            futures.push_back(promise.get_future());
            thread_local std::random_device rd;
            thread_local std::uniform_int_distribution<uint64_t> dist(0, 1ULL << 48);
            auto key = (uint64_t(item.second) << 48) | dist(rd);
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

void FutureVector::spawn(std::vector<std::pair<Executor::work_t, uint8_t>> && work)
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
                        ignoreExceptionExceptInterrupt();
                } else
                    ex = std::current_exception();
            }
    }
    if (ex)
        std::rethrow_exception(ex);
}

static Sync<WaiterDomain> & getWaiterDomain(detail::ValueBase & v)
{
    auto domain = (((size_t) &v) >> 5) % waiterDomains.size();
    return waiterDomains[domain];
}

static void throwInfiniteRecursion(EvalState & state, Value & v)
{
    state.error<InfiniteRecursionError>("infinite recursion encountered").atPos(v.determinePos(noPos)).debugThrow();
}

template<>
ValueStorage<sizeof(void *)>::PackedPointer ValueStorage<sizeof(void *)>::waitOnThunk(EvalState & state, bool awaited)
{
    state.nrThunksAwaited++;

    auto & _domain = getWaiterDomain(*this);
    auto domain = _domain.lock();

    if (awaited) {
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
        PackedPointer p0_ = pdPending;
        if (!p0.compare_exchange_strong(p0_, pdAwaited, std::memory_order_acquire, std::memory_order_acquire)) {
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
    if (state.executor->evalCores <= 1)
        throwInfiniteRecursion(state, (Value &) *this);

    state.nrThunksAwaitedSlow++;
    state.currentlyWaiting++;
    state.maxWaiting = std::max<uint64_t>(state.maxWaiting, state.currentlyWaiting);

    /* Deadlock detection. */
    Finally incrUnblocked = [&]() {
        if (Executor::amWorkerThread)
            state.executor->nrUnblocked++;
    };
    /* Handle the case where we're evaluating on a non-worker thread
       (typically the main thread). Ideally those threads would
       increment `nrUnblocked`... */
    auto active = Executor::amWorkerThread ? --state.executor->nrUnblocked : 0;
    assert(active >= 0);
    if (active == 0) {
        if (state.executor->deadlocked
            || state.executor->checkDeadlock(*state.executor->state_.lock(), Executor::amWorkerThread ? 0 : 1)) {
            state.executor->deadlocked = true;
            domain->cv.notify_all();
            for (auto & d : waiterDomains)
                if (&d != &_domain)
                    d.lock()->cv.notify_all();
            throwInfiniteRecursion(state, (Value &) *this);
        }
    }

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
        if (state.executor->deadlocked)
            throwInfiniteRecursion(state, (Value &) *this);
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

    if (state.executor->evalCores > 1) {
        std::vector<std::pair<Executor::work_t, uint8_t>> work;
        for (auto v : args[0]->listView())
            if (!v->isFinished())
                work.emplace_back([v, &state, pos]() { state.forceValue(*v, pos); }, 0);
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
    .fun = prim_parallel,
    .experimentalFeature = Xp::ParallelEval,
});

} // namespace nix
