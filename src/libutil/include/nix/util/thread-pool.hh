#pragma once
///@file

#include "nix/util/error.hh"
#include "nix/util/fun.hh"
#include "nix/util/sync.hh"

#include <queue>
#include <functional>
#include <thread>
#include <map>
#include <atomic>
#include <algorithm>
#include <optional>

namespace nix {

MakeError(ThreadPoolShutDown, Error);

/**
 * A simple thread pool that executes a queue of work items
 * (lambdas).
 */
class ThreadPool
{
public:

    ThreadPool(size_t maxThreads = 0);

    ~ThreadPool();

    /**
     * An individual work item.
     *
     * \todo use std::packaged_task?
     */
    typedef fun<void()> work_t;

    /**
     * Enqueue a function to be executed by the thread pool.
     */
    void enqueue(work_t t);

    /**
     * Execute work items until the queue is empty.
     *
     * \note Note that work items are allowed to add new items to the
     * queue; this is handled correctly.
     *
     * Queue processing stops prematurely if any work item throws an
     * exception. This exception is propagated to the calling thread. If
     * multiple work items throw an exception concurrently, only one
     * item is propagated; the others are printed on stderr and
     * otherwise ignored.
     */
    void process();

    /**
     * Shut down all worker threads and wait until they've exited.
     * Active work items are finished, but any pending work items are discarded.
     */
    void shutdown();

private:

    size_t maxThreads;

    struct State
    {
        std::queue<work_t> pending;
        size_t active = 0;
        std::exception_ptr exception;
        std::vector<std::thread> workers;
        bool draining = false;
    };

    std::atomic_bool quit{false};

    Sync<State> state_;

    std::condition_variable work;

    void doWork(bool mainThread);
};

/**
 * Process in parallel a set of items of type T that have a partial
 * ordering between them. Thus, any item is only processed after all
 * its dependencies have been processed.
 */
template<typename T>
void processGraph(
    const std::set<T> & nodes,
    fun<std::set<T>(const T &)> getEdges,
    fun<void(const T &)> processNode,
    bool discoverNodes = false,
    size_t maxThreads = 0)
{
    struct Graph
    {
        std::set<T> known;
        std::set<T> left;
        std::map<T, std::set<T>> refs, rrefs;
    };

    Sync<Graph> graph_(Graph{nodes, nodes, {}, {}});

    std::function<void(const T &)> worker;

    /* Create pool last to ensure threads are stopped before other
       destructors run. */
    ThreadPool pool(maxThreads);

    worker = [&](const T & node) {
        {
            auto graph(graph_.lock());
            auto i = graph->refs.find(node);
            if (i == graph->refs.end())
                goto getRefs;
            goto doWork;
        }

    getRefs: {
        auto refs = getEdges(node);
        refs.erase(node);

        {
            auto graph(graph_.lock());
            for (auto & ref : refs) {
                if (discoverNodes) {
                    auto [i, inserted] = graph->known.insert(ref);
                    if (inserted) {
                        pool.enqueue(std::bind(worker, std::ref(*i)));
                        graph->left.insert(ref);
                    }
                }
                if (graph->left.count(ref)) {
                    graph->refs[node].insert(ref);
                    graph->rrefs[ref].insert(node);
                }
            }
            if (graph->refs[node].empty())
                goto doWork;
        }
    }

        return;

    doWork:
        processNode(node);

        /* Enqueue work for all nodes that were waiting on this one
           and have no unprocessed dependencies. */
        {
            auto graph(graph_.lock());
            for (auto & rref : graph->rrefs[node]) {
                auto & refs(graph->refs[rref]);
                auto i = refs.find(node);
                assert(i != refs.end());
                refs.erase(i);
                if (refs.empty())
                    pool.enqueue(std::bind(worker, rref));
            }
            graph->left.erase(node);
            graph->refs.erase(node);
            graph->rrefs.erase(node);
        }
    };

    for (auto & node : nodes) {
        try {
            pool.enqueue(std::bind(worker, std::ref(node)));
        } catch (ThreadPoolShutDown &) {
            /* Stop if the thread pool is shutting down. It means a
               previous work item threw an exception, so process()
               below will rethrow it. */
            break;
        }
    }

    pool.process();

    if (!graph_.lock()->left.empty())
        throw Error("graph processing incomplete (cyclic reference?)");
}

/**
 * Compute `produce(i)` for each `i` in `[0, n)` in parallel, and call
 * `consume(i, result)` for each result strictly in order of `i`, one
 * at a time (though not necessarily on the same thread).
 *
 * Results that have been produced but not yet consumed are buffered.
 * To bound the amount of buffered data, no new items are started
 * while the total `getSize()` of the buffered results exceeds
 * `maxBuffered`. (The limit can be exceeded by the results of the
 * items that are already being produced.) Thus, if results are small,
 * a slow item doesn't prevent the other threads from making progress
 * on subsequent items.
 */
template<typename T>
void processOrdered(
    size_t n,
    fun<T(size_t)> produce,
    fun<void(size_t, T &&)> consume,
    fun<size_t(const T &)> getSize,
    size_t maxBuffered,
    size_t maxThreads = 0)
{
    if (!maxThreads)
        maxThreads = std::max(1U, std::thread::hardware_concurrency());

    struct Result
    {
        T value;
        size_t size;
    };

    struct State
    {
        std::map<size_t, Result> ready;
        size_t nextToConsume = 0;
        size_t nextToEnqueue = 0;
        /* Number of items enqueued but not yet produced. */
        size_t active = 0;
        /* Total size of the results in `ready`. */
        size_t buffered = 0;
        bool consuming = false;
    };

    Sync<State> state_;

    std::function<void(size_t)> worker;

    /* Create pool last to ensure threads are stopped before other
       destructors run. */
    ThreadPool pool(maxThreads);

    /* Start new items, as long as the buffer limit hasn't been
       reached. We keep up to twice as many items active as there are
       threads, since `ThreadPool` only starts a new thread if there
       are more pending items than threads. Note that this always
       makes progress: if nothing is active, the next item to be
       consumed is either in `ready` or hasn't been enqueued yet, in
       which case `ready` is empty and so `buffered` is 0. */
    auto enqueueMore = [&](State & state) {
        while (state.nextToEnqueue < n && state.active < 2 * maxThreads && state.buffered <= maxBuffered) {
            pool.enqueue(std::bind(worker, state.nextToEnqueue++));
            state.active++;
        }
    };

    worker = [&](size_t i) {
        auto value = produce(i);
        auto size = getSize(value);

        {
            auto state(state_.lock());
            state->ready.emplace(i, Result{std::move(value), size});
            state->buffered += size;
            state->active--;
            enqueueMore(*state);

            /* If another thread is already consuming results, it will
               pick up this one. */
            if (state->consuming)
                return;
            state->consuming = true;
        }

        while (true) {
            size_t j;
            std::optional<Result> result;

            {
                auto state(state_.lock());
                auto k = state->ready.find(state->nextToConsume);
                if (k == state->ready.end()) {
                    state->consuming = false;
                    return;
                }
                j = state->nextToConsume;
                result.emplace(std::move(k->second));
                state->ready.erase(k);
            }

            consume(j, std::move(result->value));

            {
                auto state(state_.lock());
                state->nextToConsume++;
                state->buffered -= result->size;
                enqueueMore(*state);
            }
        }
    };

    enqueueMore(*state_.lock());

    pool.process();
}

} // namespace nix
