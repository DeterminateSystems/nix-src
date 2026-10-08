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
 * at a time (though not necessarily on the same thread). At most
 * `window` items are being produced or waiting to be consumed at any
 * time. If `window` is 0, it defaults to twice the number of threads,
 * so that a slow item doesn't leave the other threads idle.
 */
template<typename T>
void processOrdered(
    size_t n, fun<T(size_t)> produce, fun<void(size_t, T &&)> consume, size_t window = 0, size_t maxThreads = 0)
{
    if (!maxThreads)
        maxThreads = std::max(1U, std::thread::hardware_concurrency());

    if (!window)
        window = 2 * maxThreads;

    struct State
    {
        std::map<size_t, T> ready;
        size_t nextToConsume = 0;
        size_t nextToEnqueue = 0;
        bool consuming = false;
    };

    Sync<State> state_;

    std::function<void(size_t)> worker;

    /* Create pool last to ensure threads are stopped before other
       destructors run. */
    ThreadPool pool(maxThreads);

    worker = [&](size_t i) {
        auto res = produce(i);

        {
            auto state(state_.lock());
            state->ready.emplace(i, std::move(res));

            /* If another thread is already consuming results, it will
               pick up this one. */
            if (state->consuming)
                return;
            state->consuming = true;
        }

        while (true) {
            size_t j;
            std::optional<T> value;

            {
                auto state(state_.lock());
                auto k = state->ready.find(state->nextToConsume);
                if (k == state->ready.end()) {
                    state->consuming = false;
                    return;
                }
                j = state->nextToConsume;
                value.emplace(std::move(k->second));
                state->ready.erase(k);
            }

            consume(j, std::move(*value));

            {
                auto state(state_.lock());
                state->nextToConsume++;
                if (state->nextToEnqueue < n)
                    pool.enqueue(std::bind(worker, state->nextToEnqueue++));
            }
        }
    };

    {
        auto state(state_.lock());
        while (state->nextToEnqueue < std::min(n, window))
            pool.enqueue(std::bind(worker, state->nextToEnqueue++));
    }

    pool.process();
}

} // namespace nix
