#include "nix/fetchers/tarball-cache.hh"
#include "nix/fetchers/cache.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/store/globals.hh"
#include "nix/store/sqlite.hh"
#include "nix/util/file-system.hh"
#include "nix/util/finally.hh"
#include "nix/util/git.hh"
#include "nix/util/pool.hh"
#include "nix/util/signals.hh"
#include "nix/util/sync.hh"
#include "nix/util/thread-pool.hh"
#include "nix/util/users.hh"

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

#include <zstd.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>

namespace nix::fetchers {

namespace {

const char * schema = R"sql(

create table if not exists Blobs (
    oid         blob primary key not null,
    size        integer not null,
    compression integer not null,
    data        blob not null
);

create table if not exists Trees (
    oid blob primary key not null
) without rowid;

create table if not exists TreeEntries (
    tree  blob not null,
    name  text not null,
    mode  integer not null,
    child blob not null,
    primary key (tree, name)
) without rowid;

)sql";

/**
 * Values of the `Blobs.compression` column.
 */
enum struct Compression : int64_t {
    None = 0,
    Zstd = 1,
};

Hash toOid(std::string_view s)
{
    Hash oid(HashAlgorithm::SHA1);
    if (s.size() != oid.hashSize)
        throw Error("tarball cache contains an object ID of %d bytes", s.size());
    memcpy(oid.hash, s.data(), oid.hashSize);
    return oid;
}

Hash hashBlob(std::string_view contents)
{
    HashSink sink(HashAlgorithm::SHA1);
    git::dumpBlobPrefix(contents.size(), sink);
    sink(contents);
    return sink.finish().hash;
}

/* Note: we call libzstd directly rather than using `compress()` / `makeDecompressionSink()` from
   `nix/util/compression.hh`. The latter set up a new zstd context (or for decompression, a libarchive reader) for every
   call, which is expensive when (de)compressing a large number of small blobs: it made importing and reading Nixpkgs
   about 60% slower. Here we reuse one zstd context per thread instead.

   The zstd contexts are per-thread, but they're only used for the duration of a single (de)compression call, so
   they're never shared between fibers running on the same thread. */

/**
 * Compress `contents` using zstd. Return `std::nullopt` if that doesn't make it smaller.
 */
std::optional<std::string> compress(std::string_view contents)
{
    if (contents.empty())
        return std::nullopt;

    thread_local std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> cctx{ZSTD_createCCtx(), ZSTD_freeCCtx};
    if (!cctx)
        throw Error("unable to create a zstd compression context");

    std::string res;
    res.resize(ZSTD_compressBound(contents.size()));
    auto n =
        ZSTD_compressCCtx(cctx.get(), res.data(), res.size(), contents.data(), contents.size(), ZSTD_CLEVEL_DEFAULT);
    if (ZSTD_isError(n))
        throw Error("zstd compression failed: %s", ZSTD_getErrorName(n));
    if (n >= contents.size())
        return std::nullopt;
    res.resize(n);
    return res;
}

std::string decompress(std::string_view data, uint64_t size)
{
    thread_local std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> dctx{ZSTD_createDCtx(), ZSTD_freeDCtx};
    if (!dctx)
        throw Error("unable to create a zstd decompression context");

    std::string res;
    res.resize(size);
    auto n = ZSTD_decompressDCtx(dctx.get(), res.data(), res.size(), data.data(), data.size());
    if (ZSTD_isError(n))
        throw Error("zstd decompression failed: %s", ZSTD_getErrorName(n));
    if (n != size)
        throw Error("blob in tarball cache has size %d, expected %d", n, size);
    return res;
}

struct Entry
{
    git::Mode mode;
    Hash oid;
};

using Dir = std::map<std::string, Entry>;

struct Connection
{
    SQLite db;
    SQLiteStmt hasTree, queryEntries, hasBlob, queryBlob, insertBlob, insertTree, insertEntry;

    Connection(const std::filesystem::path & dbPath)
    {
        db = SQLite(dbPath, {.useWAL = settings.useSQLiteWAL});
        db.exec("pragma synchronous = normal");

        hasTree.create(db, "select 1 from Trees where oid = ?");
        queryEntries.create(db, "select name, mode, child from TreeEntries where tree = ?");
        hasBlob.create(db, "select 1 from Blobs where oid = ?");
        queryBlob.create(db, "select size, compression, data from Blobs where oid = ?");
        insertBlob.create(db, "insert or ignore into Blobs(oid, size, compression, data) values (?, ?, ?, ?)");
        insertTree.create(db, "insert or ignore into Trees(oid) values (?)");
        insertEntry.create(db, "insert or ignore into TreeEntries(tree, name, mode, child) values (?, ?, ?, ?)");
    }
};

/**
 * A blob that is ready to be written to the database.
 */
struct PendingBlob
{
    Hash oid;
    uint64_t size;
    Compression compression;
    std::string data;
};

struct TarballCacheImpl : TarballCache, std::enable_shared_from_this<TarballCacheImpl>
{
    std::filesystem::path dbPath;

    /**
     * SQLite connections. Every thread that accesses the database
     * takes its own connection from this pool.
     */
    Pool<Connection> pool;

    /**
     * Mutex to serialize write transactions within this process, since SQLite only allows one writer at a time anyway.
     * (Writers in other processes are handled by SQLite's busy handler.)
     */
    std::mutex writeMutex;

    TarballCacheImpl(std::filesystem::path _dbPath)
        : dbPath(std::move(_dbPath))
        , pool(std::numeric_limits<size_t>::max(), [this]() { return make_ref<Connection>(dbPath); })
    {
        createDirs(dbPath.parent_path());

        SQLite db(dbPath, {.useWAL = settings.useSQLiteWAL});
        if (settings.useSQLiteWAL)
            db.exec("pragma main.journal_mode = wal");
        db.exec(schema);
    }

    bool hasTree(const Hash & treeHash) override
    {
        auto conn(pool.get());
        return conn->hasTree.use().apply(treeHash.hash, treeHash.hashSize).next();
    }

    ref<const Dir> readTree(const Hash & treeHash)
    {
        auto dir = make_ref<Dir>();
        auto conn(pool.get());
        auto stmt(conn->queryEntries.use().apply(treeHash.hash, treeHash.hashSize));
        while (stmt.next()) {
            auto rawMode = (git::RawMode) stmt.getInt(1);
            auto mode = git::decodeMode(rawMode);
            if (!mode)
                throw Error("tarball cache contains an entry with unknown mode %o", rawMode);
            dir->emplace(stmt.getStr(0), Entry{.mode = *mode, .oid = toOid(stmt.getBlob(2))});
        }
        return dir;
    }

    void readBlob(const Hash & oid, Sink & sink, fun<void(uint64_t)> sizeCallback)
    {
        auto conn(pool.get());
        auto stmt(conn->queryBlob.use().apply(oid.hash, oid.hashSize));
        if (!stmt.next())
            throw Error("blob '%s' is missing from the tarball cache", oid.gitRev());

        uint64_t size = stmt.getInt(0);
        auto compression = (Compression) stmt.getInt(1);
        auto data = stmt.getBlob(2);

        switch (compression) {
        case Compression::None:
            sizeCallback(size);
            sink(data);
            break;
        case Compression::Zstd: {
            auto contents = decompress(data, size);
            sizeCallback(size);
            sink(contents);
            break;
        }
        default:
            throw Error(
                "blob '%s' in the tarball cache has unknown compression type %d", oid.gitRev(), (int64_t) compression);
        }
    }

    void writeBlobs(const std::vector<PendingBlob> & blobs)
    {
        if (blobs.empty())
            return;
        std::lock_guard lock(writeMutex);
        auto conn(pool.get());
        retrySQLite<void>([&]() {
            SQLiteTxn txn(conn->db);
            for (auto & blob : blobs)
                conn->insertBlob.use()
                    .apply(blob.oid.hash, blob.oid.hashSize)
                    .apply((int64_t) blob.size)
                    .apply((int64_t) blob.compression)
                    .apply((const unsigned char *) blob.data.data(), blob.data.size())
                    .exec();
            txn.commit();
        });
    }

    ref<SourceAccessor> getAccessor(const Hash & treeHash, std::string displayPrefix) override;

    ref<GitFileSystemObjectSink> getFileSystemObjectSink() override;

    Hash treeHashToNarHash(const Settings & settings, const Hash & treeHash) override
    {
        auto accessor = getAccessor(treeHash, "");

        Cache::Key cacheKey{"treeHashToNarHash", {{"treeHash", treeHash.gitRev()}}};

        if (auto res = settings.getCache()->lookup(cacheKey))
            return Hash::parseAny(getStrAttr(*res, "narHash"), HashAlgorithm::SHA256);

        auto narHash = accessor->hashPath(CanonPath::root);

        settings.getCache()->upsert(cacheKey, Attrs({{"narHash", narHash.to_string(HashFormat::SRI, true)}}));

        return narHash;
    }

    Hash dereferenceSingletonDirectory(const Hash & treeHash) override
    {
        auto dir = readTree(treeHash);
        if (dir->size() == 1 && dir->begin()->second.mode == git::Mode::Directory)
            return dir->begin()->second.oid;
        return treeHash;
    }
};

struct TarballCacheAccessor : SourceAccessor
{
    ref<TarballCacheImpl> cache;

    Hash root;

    /**
     * Cache of directory listings. A null value denotes a path that is not a directory.
     */
    SharedSync<boost::unordered_flat_map<CanonPath, std::shared_ptr<const Dir>>> dirCache;

    TarballCacheAccessor(ref<TarballCacheImpl> cache, const Hash & root)
        : cache(cache)
        , root(root)
    {
        if (!cache->hasTree(root))
            throw Error("tree '%s' is missing from the tarball cache", root.gitRev());
        fingerprint = GitAccessorOptions{}.makeFingerprint(root);
    }

    void anchor() override {}

    /**
     * Return the contents of the directory `path`, or null if `path` doesn't exist or is not a directory.
     */
    std::shared_ptr<const Dir> getDir(const CanonPath & path)
    {
        {
            auto dirCache_(dirCache.readLock());
            if (auto i = dirCache_->find(path); i != dirCache_->end())
                return i->second;
        }

        std::shared_ptr<const Dir> dir;
        if (auto entry = lookup(path); entry && entry->mode == git::Mode::Directory)
            dir = cache->readTree(entry->oid).get_ptr();

        dirCache.lock()->emplace(path, dir);

        return dir;
    }

    std::optional<Entry> lookup(const CanonPath & path)
    {
        if (path.isRoot())
            return Entry{.mode = git::Mode::Directory, .oid = root};

        auto dir = getDir(*path.parent());
        if (!dir)
            return std::nullopt;

        auto i = dir->find(std::string(*path.baseName()));
        if (i == dir->end())
            return std::nullopt;

        return i->second;
    }

    Entry need(const CanonPath & path)
    {
        auto entry = lookup(path);
        if (!entry)
            throw FileNotFound("path '%s' does not exist", showPath(path));
        return *entry;
    }

    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
    {
        auto entry = need(path);
        if (entry.mode != git::Mode::Regular && entry.mode != git::Mode::Executable)
            throw Error("'%s' is not a regular file", showPath(path));
        cache->readBlob(entry.oid, sink, sizeCallback);
    }

    bool pathExists(const CanonPath & path) override
    {
        return (bool) lookup(path);
    }

    std::optional<Stat> maybeLstat(const CanonPath & path) override
    {
        auto entry = lookup(path);
        if (!entry)
            return std::nullopt;

        switch (entry->mode) {
        case git::Mode::Directory:
            return Stat{.type = tDirectory};
        case git::Mode::Regular:
            return Stat{.type = tRegular};
        case git::Mode::Executable:
            return Stat{.type = tRegular, .isExecutable = true};
        case git::Mode::Symlink:
            return Stat{.type = tSymlink};
        }
        unreachable();
    }

    DirEntries readDirectory(const CanonPath & path) override
    {
        auto entry = need(path);
        if (entry.mode != git::Mode::Directory)
            throw Error("'%s' is not a directory", showPath(path));

        auto dir = getDir(path);
        assert(dir);

        DirEntries res;
        for (auto & [name, entry] : *dir)
            res.emplace(
                name,
                entry.mode == git::Mode::Directory ? tDirectory
                : entry.mode == git::Mode::Symlink ? tSymlink
                                                   : tRegular);
        return res;
    }

    std::string readLink(const CanonPath & path) override
    {
        auto entry = need(path);
        if (entry.mode != git::Mode::Symlink)
            throw Error("'%s' is not a symlink", showPath(path));
        StringSink sink;
        cache->readBlob(entry.oid, sink, [&](uint64_t size) { sink.s.reserve(size); });
        return std::move(sink.s);
    }
};

ref<SourceAccessor> TarballCacheImpl::getAccessor(const Hash & treeHash, std::string displayPrefix)
{
    auto accessor = make_ref<TarballCacheAccessor>(ref<TarballCacheImpl>(shared_from_this()), treeHash);
    accessor->setPathDisplay(std::move(displayPrefix));
    return accessor;
}

struct TarballCacheSink : GitFileSystemObjectSink
{
    ref<TarballCacheImpl> cache;

    unsigned int concurrency = std::min(std::thread::hardware_concurrency(), 10U);

    ThreadPool workers{concurrency};

    /** Total file contents in flight. */
    std::atomic<size_t> totalBufSize{0};

    /** If the file contents in flight exceed this threshold, files are processed synchronously. */
    static constexpr size_t maxBufSize = 64 * 1024 * 1024;

    /** The amount of (compressed) blob data to write to the database in a single transaction. */
    static constexpr size_t maxBatchSize = 16 * 1024 * 1024;

    struct Child;

    /// A directory to be written as a tree.
    struct Directory
    {
        std::map<std::string, Child> children;
        std::optional<Hash> oid;

        Child & lookup(const CanonPath & path)
        {
            assert(!path.isRoot());
            auto parent = path.parent();
            auto cur = this;
            for (auto & name : *parent) {
                auto i = cur->children.find(std::string(name));
                if (i == cur->children.end())
                    throw Error("path '%s' does not exist", path);
                auto dir = std::get_if<Directory>(&i->second.file);
                if (!dir)
                    throw Error("path '%s' has a non-directory parent", path);
                cur = dir;
            }

            auto i = cur->children.find(std::string(*path.baseName()));
            if (i == cur->children.end())
                throw Error("path '%s' does not exist", path);
            return i->second;
        }
    };

    size_t nextId = 0; // for Child.id

    struct Child
    {
        git::Mode mode;
        std::variant<Directory, Hash> file;

        /// Sequential numbering of the file in the tarball. This is
        /// used to make sure we only import the latest version of a
        /// path.
        size_t id{0};
    };

    struct State
    {
        Directory root;
    };

    Sync<State> _state;

    struct Pending
    {
        std::vector<PendingBlob> blobs;
        size_t size = 0;
    };

    /** Blobs waiting to be written to the database. */
    Sync<Pending> _pending;

    /** The blobs that have already been seen during this import. */
    Sync<boost::unordered_flat_set<Hash>> _seen;

    TarballCacheSink(ref<TarballCacheImpl> cache)
        : cache(cache)
    {
    }

    ~TarballCacheSink()
    {
        // Make sure the worker threads are destroyed before any state
        // they're referring to.
        workers.shutdown();
    }

    void addNode(State & state, const CanonPath & path, Child && child)
    {
        assert(!path.isRoot());
        auto parent = path.parent();

        Directory * cur = &state.root;

        for (auto & i : *parent) {
            auto child = std::get_if<Directory>(
                &cur->children.emplace(std::string(i), Child{git::Mode::Directory, {Directory()}}).first->second.file);
            if (!child)
                throw Error("path '%s' has a non-directory parent", path);
            cur = child;
        }

        std::string name(*path.baseName());

        if (auto prev = cur->children.find(name); prev == cur->children.end() || prev->second.id < child.id)
            cur->children.insert_or_assign(name, std::move(child));
    }

    /**
     * Add a blob to the cache, unless it's already there. Returns its Git hash.
     */
    Hash addBlob(std::string_view contents)
    {
        auto oid = hashBlob(contents);

        if (!_seen.lock()->insert(oid).second)
            return oid;

        {
            auto conn(cache->pool.get());
            if (conn->hasBlob.use().apply(oid.hash, oid.hashSize).next())
                return oid;
        }

        PendingBlob blob{.oid = oid, .size = contents.size()};
        if (auto compressed = compress(contents)) {
            blob.compression = Compression::Zstd;
            blob.data = std::move(*compressed);
        } else {
            blob.compression = Compression::None;
            blob.data = contents;
        }

        std::vector<PendingBlob> batch;

        {
            auto pending(_pending.lock());
            pending->size += blob.data.size();
            pending->blobs.push_back(std::move(blob));
            if (pending->size < maxBatchSize)
                return oid;
            batch = std::move(pending->blobs);
            pending->blobs.clear();
            pending->size = 0;
        }

        cache->writeBlobs(batch);

        return oid;
    }

    /**
     * Asynchronously add a blob to the cache and register it as `path` in the tree.
     */
    void addBlobNode(const CanonPath & path, git::Mode mode, std::string contents_, size_t id)
    {
        auto contents = std::make_shared<std::string>(std::move(contents_));

        totalBufSize += contents->size();

        auto work = [this, path, mode, contents, id]() {
            Finally releaseBuf([&]() { totalBufSize -= contents->size(); });
            auto oid = addBlob(*contents);
            addNode(*_state.lock(), path, Child{mode, oid, id});
        };

        /* To avoid unbounded memory usage, process the file synchronously if there is too much data in flight. */
        if (totalBufSize > maxBufSize)
            work();
        else
            workers.enqueue(std::move(work));
    }

    void createRegularFile(const CanonPath & path, fun<void(CreateRegularFileSink &)> func) override
    {
        checkInterrupt();

        struct CRF : CreateRegularFileSink
        {
            std::string contents;
            bool executable = false;

            void operator()(std::string_view data) override
            {
                contents.append(data);
            }

            void isExecutable() override
            {
                executable = true;
            }

            void preallocateContents(uint64_t size) override
            {
                contents.reserve(size);
            }
        };

        CRF crf;

        func(crf);

        addBlobNode(
            path, crf.executable ? git::Mode::Executable : git::Mode::Regular, std::move(crf.contents), nextId++);
    }

    void createDirectory(const CanonPath & path) override
    {
        if (path.isRoot())
            return;
        auto state(_state.lock());
        addNode(*state, path, {git::Mode::Directory, Directory()});
    }

    void createSymlink(const CanonPath & path, const std::string & target) override
    {
        addBlobNode(path, git::Mode::Symlink, target, 0);
    }

    std::map<CanonPath, CanonPath> hardLinks;

    void createHardlink(const CanonPath & path, const CanonPath & target) override
    {
        hardLinks.insert_or_assign(path, target);
    }

    /**
     * Compute the Git tree hashes of `dir` and its subdirectories.
     */
    static Hash hashTree(Directory & dir)
    {
        git::Tree tree;

        for (auto & [name, child] : dir.children) {
            if (auto subdir = std::get_if<Directory>(&child.file))
                // `git::Tree` expects directory names to have a trailing slash.
                tree.emplace(name + "/", git::TreeEntry{.mode = git::Mode::Directory, .hash = hashTree(*subdir)});
            else
                tree.emplace(name, git::TreeEntry{.mode = child.mode, .hash = std::get<Hash>(child.file)});
        }

        HashSink sink(HashAlgorithm::SHA1);
        git::dumpTree(tree, sink);
        dir.oid = sink.finish().hash;
        return *dir.oid;
    }

    Hash flush() override
    {
        workers.process();

        // Write the remaining blobs.
        {
            auto pending(_pending.lock());
            cache->writeBlobs(pending->blobs);
            pending->blobs.clear();
            pending->size = 0;
        }

        auto state(_state.lock());

        /* Create hard links. */
        for (auto & [path, target] : hardLinks) {
            if (target.isRoot())
                continue;
            try {
                auto child = state->root.lookup(target);
                auto oid = std::get_if<Hash>(&child.file);
                if (!oid)
                    throw Error("cannot create a hard link to a directory");
                addNode(*state, path, {child.mode, *oid});
            } catch (Error & e) {
                e.addTrace(nullptr, "while creating a hard link from '%s' to '%s'", path, target);
                throw;
            }
        }

        auto rootOid = hashTree(state->root);

        /* Figure out which trees are not in the database yet. Note that if a tree is already in the database, then so
           are all its children. */
        std::vector<const Directory *> missing;
        {
            auto conn(cache->pool.get());
            boost::unordered_flat_set<Hash> visited;

            [&](this const auto & visit, const Directory & dir) -> void {
                checkInterrupt();

                auto & oid = dir.oid.value();
                if (!visited.insert(oid).second)
                    return;
                if (conn->hasTree.use().apply(oid.hash, oid.hashSize).next())
                    return;

                for (auto & child : dir.children)
                    if (auto subdir = std::get_if<Directory>(&child.second.file))
                        visit(*subdir);

                missing.push_back(&dir);
            }(state->root);
        }

        /* Write the missing trees. This must be done after writing the blobs, since the existence of a tree implies
           that all its children exist. */
        if (!missing.empty()) {
            std::lock_guard lock(cache->writeMutex);
            auto conn(cache->pool.get());
            retrySQLite<void>([&]() {
                SQLiteTxn txn(conn->db);
                for (auto dir : missing) {
                    auto & oid = dir->oid.value();
                    for (auto & [name, child] : dir->children) {
                        auto subdir = std::get_if<Directory>(&child.file);
                        auto & childOid = subdir ? subdir->oid.value() : std::get<Hash>(child.file);
                        conn->insertEntry.use()
                            .apply(oid.hash, oid.hashSize)
                            .apply(name)
                            .apply((int64_t) child.mode)
                            .apply(childOid.hash, childOid.hashSize)
                            .exec();
                    }
                    conn->insertTree.use().apply(oid.hash, oid.hashSize).exec();
                }
                txn.commit();
            });
        }

        return rootOid;
    }
};

ref<GitFileSystemObjectSink> TarballCacheImpl::getFileSystemObjectSink()
{
    return make_ref<TarballCacheSink>(ref<TarballCacheImpl>(shared_from_this()));
}

} // namespace

ref<TarballCache> TarballCache::open(const std::filesystem::path & dbPath)
{
    return make_ref<TarballCacheImpl>(dbPath);
}

ref<TarballCache> Settings::getTarballCache() const
{
    auto tarballCache(_tarballCache.lock());
    if (!*tarballCache)
        *tarballCache = TarballCache::open(getCacheDir() / "tarball-cache-v3.sqlite").get_ptr();
    return ref<TarballCache>(*tarballCache);
}

} // namespace nix::fetchers
