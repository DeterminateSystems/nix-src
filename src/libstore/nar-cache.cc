#include "nix/store/nar-cache.hh"
#include "nix/util/file-system.hh"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

namespace nix {

NarCache::NarCache(std::filesystem::path cacheDir_)
    : cacheDir(std::move(cacheDir_))
{
    assert(!cacheDir.empty());
    createDirs(cacheDir);
}

std::filesystem::path NarCache::makeCacheFile(const Hash & narHash, const std::string & ext)
{
    return (cacheDir / narHash.to_string(HashFormat::Nix32, false)) + "." + ext;
}

void NarCache::upsertNar(const Hash & narHash, Source & source)
{
    try {
        /* FIXME: do this asynchronously. */
        writeFile(makeCacheFile(narHash, "nar"), source);
    } catch (SystemError &) {
        ignoreExceptionExceptInterrupt();
    }
}

void NarCache::upsertNarListing(const Hash & narHash, std::string_view narListingData)
{
    try {
        writeFile(makeCacheFile(narHash, "ls"), narListingData);
    } catch (SystemError &) {
        ignoreExceptionExceptInterrupt();
    }
}

std::optional<std::string> NarCache::getNar(const Hash & narHash)
{
    try {
        return nix::readFile(makeCacheFile(narHash, "nar"));
    } catch (SystemError &) {
        return std::nullopt;
    }
}

std::string NarCache::getNarBytes(const Hash & narHash, uint64_t offset, uint64_t length)
{
    auto cacheFile = makeCacheFile(narHash, "nar");

    AutoCloseFD fd = toDescriptor(open(
        cacheFile.c_str(),
        O_RDONLY
#ifndef _WIN32
            | O_CLOEXEC
#endif
        ));
    if (!fd)
        throw SysError("opening NAR cache file %s", cacheFile);

    if (lseek(fromDescriptorReadOnly(fd.get()), offset, SEEK_SET) != (off_t) offset)
        throw SysError("seeking in %s", cacheFile);

    std::string buf(length, 0);
    readFull(fd.get(), buf.data(), length);
    return buf;
}

std::optional<std::string> NarCache::getNarListing(const Hash & narHash)
{
    try {
        return nix::readFile(makeCacheFile(narHash, "ls"));
    } catch (SystemError &) {
        return std::nullopt;
    }
}

} // namespace nix
