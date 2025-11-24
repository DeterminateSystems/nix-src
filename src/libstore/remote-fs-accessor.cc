#include <nlohmann/json.hpp>
#include "nix/store/remote-fs-accessor.hh"
#include "nix/store/nar-accessor.hh"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

namespace nix {

RemoteFSAccessor::RemoteFSAccessor(ref<Store> store, bool requireValidPath, std::filesystem::path cacheDir_)
    : store(store)
    , requireValidPath(requireValidPath)
    , cacheDir(std::move(cacheDir_))
{
    if (!cacheDir.empty())
        createDirs(cacheDir);
}

std::filesystem::path RemoteFSAccessor::makeCacheFile(const Hash & narHash, const std::string & ext)
{
    assert(!cacheDir.empty());
    return (cacheDir / narHash.to_string(HashFormat::Nix32, false)) + "." + ext;
}

ref<SourceAccessor> RemoteFSAccessor::addToCache(
    std::string_view hashPart,
    const std::filesystem::path & cacheFile,
    const std::filesystem::path & listingFile,
    std::string && nar)
{
    if (!cacheFile.empty()) {
        try {
            /* FIXME: do this asynchronously. */
            writeFile(cacheFile, nar);
        } catch (...) {
            ignoreExceptionExceptInterrupt();
        }
    }

    auto narAccessor = makeNarAccessor(std::move(nar));
    nars.emplace(hashPart, narAccessor);

    if (!listingFile.empty()) {
        try {
            nlohmann::json j = listNar(narAccessor, CanonPath::root, true);
            writeFile(listingFile, j.dump());
        } catch (...) {
            ignoreExceptionExceptInterrupt();
        }
    }

    return narAccessor;
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

    std::filesystem::path cacheFile, listingFile;

    if (!cacheDir.empty()) {
        auto info = store->queryPathInfo(storePath);

        cacheFile = makeCacheFile(info->narHash, "nar");
        listingFile = makeCacheFile(info->narHash, "ls");

        if (nix::pathExists(cacheFile)) {
            try {
                auto listing = nix::readFile(listingFile);
                auto narAccessor = makeLazyNarAccessor(listing, [cacheFile](uint64_t offset, uint64_t length) {
                    AutoCloseFD fd = toDescriptor(open(
                        cacheFile.c_str(),
                        O_RDONLY
#ifndef _WIN32
                            | O_CLOEXEC
#endif
                        ));
                    if (!fd)
                        throw SysError("opening NAR cache file '%s'", cacheFile);

                    if (lseek(fromDescriptorReadOnly(fd.get()), offset, SEEK_SET) != (off_t) offset)
                        throw SysError("seeking in '%s'", cacheFile);

                    std::string buf(length, 0);
                    readFull(fd.get(), buf.data(), length);

                    return buf;
                });

                nars.emplace(storePath.hashPart(), narAccessor);
                return narAccessor;

            } catch (SystemError &) {
            }

            try {
                auto narAccessor = makeNarAccessor(nix::readFile(cacheFile));
                nars.emplace(storePath.hashPart(), narAccessor);
                return narAccessor;
            } catch (SystemError &) {
            }
        }
    }

    StringSink sink;
    store->narFromPath(storePath, sink);
    return addToCache(storePath.hashPart(), cacheFile, listingFile, std::move(sink.s));
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
