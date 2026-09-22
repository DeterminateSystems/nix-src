#include "nix/store/async-path-writer.hh"
#include "nix/store/globals.hh"
#include "nix/util/archive.hh"
#include "nix/util/provenance.hh"

#include <thread>
#include <future>

namespace nix {

struct AsyncPathWriterImpl : AsyncPathWriter
{
    ref<Store> store;

    struct Item
    {
        StorePath storePath;
        std::string contents;
        std::string name;
        Hash hash;
        StorePathSet references;
        RepairFlag repair;
        std::shared_ptr<const Provenance> provenance;
        std::promise<void> promise;
    };

    struct State
    {
        std::vector<Item> items;
        std::unordered_map<StorePath, std::shared_future<void>> futures;
        bool quit = false;
    };

    Sync<State> state_;

    std::thread workerThread;

    std::condition_variable wakeupCV;

    AsyncPathWriterImpl(ref<Store> store)
        : store(store)
    {
        workerThread = std::thread([&]() {
            while (true) {
                std::vector<Item> items;

                {
                    auto state(state_.lock());
                    while (!state->quit && state->items.empty())
                        state.wait(wakeupCV);
                    if (state->items.empty() && state->quit)
                        return;
                    std::swap(items, state->items);
                }

                try {
                    writePaths(items);
                    for (auto & item : items)
                        item.promise.set_value();
                } catch (...) {
                    for (auto & item : items)
                        item.promise.set_exception(std::current_exception());
                }
            }
        });
    }

    virtual ~AsyncPathWriterImpl()
    {
        state_.lock()->quit = true;
        wakeupCV.notify_all();
        workerThread.join();
    }

    StorePath addPath(
        std::string contents,
        std::string name,
        StorePathSet references,
        RepairFlag repair,
        std::shared_ptr<const Provenance> provenance) override
    {
        auto hash = hashString(HashAlgorithm::SHA256, contents);

        auto storePath = store->makeFixedOutputPathFromCA(
            name,
            TextInfo{
                .hash = hash,
                .references = references,
            });

        /* In read-only mode, only compute the store path; don't
           write anything. */
        if (settings.readOnlyMode)
            return storePath;

        auto state(state_.lock());

        /* If we've already written or queued this path, there is
           nothing to do. This also applies when `repair` is set,
           since the first request already repaired it. */
        if (state->futures.contains(storePath))
            return storePath;

        std::promise<void> promise;
        state->futures.emplace(storePath, promise.get_future());
        state->items.push_back(
            Item{
                .storePath = storePath,
                .contents = std::move(contents),
                .name = std::move(name),
                .hash = hash,
                .references = std::move(references),
                .repair = repair,
                .provenance = provenance,
                .promise = std::move(promise),
            });
        wakeupCV.notify_all();

        return storePath;
    }

    void waitForPath(const StorePath & path) override
    {
        auto future = ({
            auto state = state_.lock();
            auto i = state->futures.find(path);
            if (i == state->futures.end())
                return;
            i->second;
        });
        future.get();
    }

    void waitForAllPaths() override
    {
        /* Copy rather than move the futures, since `futures` also
           serves as the record of paths already written (see
           `addPath()`). */
        auto futures = ({
            auto state(state_.lock());
            state->futures;
        });
        for (auto & future : futures)
            future.second.get();
    }

    void writePaths(const std::vector<Item> & items)
    {
// FIXME: addMultipeToStore() shouldn't require a NAR hash.
#if 0
        Store::PathsSource sources;
        RepairFlag repair = NoRepair;

        for (auto & item : items) {
            ValidPathInfo info{item.storePath, Hash(HashAlgorithm::SHA256)};
            info.references = item.references;
            info.ca = ContentAddress {
                .method = ContentAddressMethod::Raw::Text,
                .hash = item.hash,
            };
            if (item.repair) repair = item.repair;
            auto source = sinkToSource([&](Sink & sink)
            {
                dumpString(item.contents, sink);
            });
            sources.push_back({std::move(info), std::move(source)});
        }

        Activity act(*logger, lvlDebug, actUnknown, fmt("adding %d paths to the store", items.size()));

        store->addMultipleToStore(std::move(sources), act, repair);
#endif

        for (auto & item : items) {
            StringSource source(item.contents);
            store->addTempRoot(item.storePath);
            auto storePath = store->addToStoreFromDump(
                source,
                item.storePath.name(),
                FileSerialisationMethod::Flat,
                ContentAddressMethod::Raw::Text,
                HashAlgorithm::SHA256,
                item.references,
                item.repair,
                item.provenance);
            assert(storePath == item.storePath);
        }
    }

    void anchor() override;
};

void AsyncPathWriter::anchor() {}

void AsyncPathWriterImpl::anchor() {}

ref<AsyncPathWriter> AsyncPathWriter::make(ref<Store> store)
{
    return make_ref<AsyncPathWriterImpl>(store);
}

} // namespace nix
