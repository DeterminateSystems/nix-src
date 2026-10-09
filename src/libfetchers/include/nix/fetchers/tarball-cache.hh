#pragma once
///@file

#include "nix/fetchers/git-utils.hh"
#include "nix/util/hash.hh"
#include "nix/util/ref.hh"
#include "nix/util/source-accessor.hh"

#include <filesystem>

namespace nix::fetchers {

struct Settings;

/**
 * A content-addressed cache of unpacked tarballs, stored in a SQLite
 * database. Files and directories are identified by their Git (SHA-1)
 * blob and tree hashes.
 */
struct TarballCache
{
    virtual ~TarballCache() = default;

    /**
     * Open (and create if necessary) the tarball cache stored in the
     * SQLite database `dbPath`.
     */
    static ref<TarballCache> open(const std::filesystem::path & dbPath);

    /**
     * Whether the cache contains the tree `treeHash`. If so, then
     * everything reachable from that tree is present as well.
     */
    virtual bool hasTree(const Hash & treeHash) = 0;

    virtual ref<SourceAccessor> getAccessor(const Hash & treeHash, std::string displayPrefix) = 0;

    /**
     * Return a sink that imports a file system tree into the cache.
     * Calling `flush()` on the sink returns the hash of the root
     * tree.
     */
    virtual ref<GitFileSystemObjectSink> getFileSystemObjectSink() = 0;

    /**
     * Given a Git tree hash, compute the hash of its NAR
     * serialisation. This is memoised on-disk.
     */
    virtual Hash treeHashToNarHash(const Settings & settings, const Hash & treeHash) = 0;

    /**
     * If the specified tree is a directory with a single entry that
     * is a directory, return the hash of that entry. Otherwise return
     * the passed hash unchanged.
     */
    virtual Hash dereferenceSingletonDirectory(const Hash & treeHash) = 0;
};

} // namespace nix::fetchers
