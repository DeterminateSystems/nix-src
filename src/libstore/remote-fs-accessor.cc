#include <nlohmann/json.hpp>
#include "nix/store/remote-fs-accessor.hh"
#include "nix/store/nar-cache.hh"

namespace nix {

RemoteFSAccessor::RemoteFSAccessor(ref<Store> store, bool requireValidPath, std::shared_ptr<NarCache> narCache)
    : store(store)
    , requireValidPath(requireValidPath)
    , narCache(std::move(narCache))
{
}

std::pair<ref<SourceAccessor>, CanonPath> RemoteFSAccessor::fetch(const CanonPath & path)
{
    auto [storePath, restPath] = store->toStorePath(store->storeDir + path.abs());
    if (requireValidPath && !store->isValidPath(storePath))
        throw InvalidPath("path '%1%' is not a valid store path", store->printStorePath(storePath));
    return {ref{accessObject(storePath)}, CanonPath{restPath}};
}

std::shared_ptr<SourceAccessor> RemoteFSAccessor::accessObject(const StorePath & storePath)
{
    auto i = nars.find(std::string(storePath.hashPart()));
    if (i != nars.end())
        return i->second;

    Hash narHash{HashAlgorithm::SHA256};

    if (narCache) {
        auto info = store->queryPathInfo(storePath);
        narHash = info->narHash;

        if (auto listingData = narCache->getNarListing(narHash)) {
            auto listingJson = nlohmann::json::parse(*listingData);
            auto narAccessor = makeLazyNarAccessor(listingJson, narCache->getNarBytes(narHash));
            nars.emplace(storePath.hashPart(), narAccessor);
            return narAccessor;
        }

        if (auto nar = narCache->getNar(narHash)) {
            auto narAccessor = makeNarAccessor(std::move(*nar));
            nars.emplace(storePath.hashPart(), narAccessor);
            return narAccessor;
        }
    }

    StringSink sink;
    store->narFromPath(storePath, sink);

    if (narCache) {
        StringSource source{sink.s};
        narCache->upsertNar(narHash, source);
    }

    auto narAccessor = makeNarAccessor(std::move(sink.s));
    nars.emplace(storePath.hashPart(), narAccessor);

    if (narCache) {
        nlohmann::json j = listNarDeep(*narAccessor, CanonPath::root);
        narCache->upsertNarListing(narHash, j.dump());
    }

    return narAccessor;
}

std::optional<SourceAccessor::Stat> RemoteFSAccessor::maybeLstat(const CanonPath & path)
{
    auto res = fetch(path);
    return res.first->maybeLstat(res.second);
}

SourceAccessor::DirEntries RemoteFSAccessor::readDirectory(const CanonPath & path)
{
    auto res = fetch(path);
    return res.first->readDirectory(res.second);
}

std::string RemoteFSAccessor::readFile(const CanonPath & path)
{
    auto res = fetch(path);
    return res.first->readFile(res.second);
}

std::string RemoteFSAccessor::readLink(const CanonPath & path)
{
    auto res = fetch(path);
    return res.first->readLink(res.second);
}

} // namespace nix
