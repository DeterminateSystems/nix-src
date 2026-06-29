#include "nix/cmd/command.hh"
#include "nix/main/shared.hh"
#include "nix/store/store-api.hh"
#include "nix/util/canon-path.hh"
#include "nix/util/lru-cache.hh"
#include "nix/util/serialise.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/sync.hh"
#include "nix/util/finally.hh"
#include "nix/util/signals.hh"

#define FUSE_USE_VERSION 31
#include <fuse.h>

#include <cerrno>
#include <cstring>

using namespace nix;

namespace {

/* State shared with the FUSE operation callbacks, passed via
   `fuse_new`'s `private_data` and retrieved with
   `fuse_get_context()->private_data`. */
struct NixFs
{
    ref<Store> store;

    using AccessorCache = LRUCache<StorePath, std::shared_ptr<SourceAccessor>>;

    /* A bounded cache of FS accessors keyed by store path. Constructing
       an accessor re-validates the store path, so caching them avoids
       that work on every operation. Wrapped in `Sync` because the FUSE
       callbacks run on multiple threads and `LRUCache` is not
       thread-safe. */
    Sync<AccessorCache> accessors{AccessorCache{1024}};

    /* Return the FS accessor for `storePath`, constructing and caching
       it on a miss. Returns null if `storePath` is not a valid store
       path; such negative results are not cached, since a path that is
       invalid now may become valid later. */
    std::shared_ptr<SourceAccessor> getAccessor(const StorePath & storePath)
    {
        if (auto accessor = accessors.lock()->get(storePath))
            return *accessor;

        auto accessor = store->getFSAccessor(storePath);
        if (!accessor)
            return nullptr;

        accessors.lock()->upsert(storePath, accessor);

        return accessor;
    }

    struct Resolved
    {
        ref<SourceAccessor> accessor;
        CanonPath subPath;
    };

    /* Resolve a mount-relative path (`/<hash>-<name>/<sub>`) to the FS
       accessor for its store path and the subpath within it. Returns
       nullopt if the store path is not valid. Throws `BadStorePath` if
       the path does not name a store path. */
    std::optional<Resolved> resolve(const CanonPath & path)
    {
        if (path.isRoot())
            return std::nullopt;

        auto accessor = getAccessor(StorePath(*path.begin()));
        if (!accessor)
            return std::nullopt;

        /* The subpath within the store path is everything after the
           first component (the store path name). */
        return Resolved{ref(accessor), path.dropPrefix()};
    }

    /* The Nix store is immutable: once a store path exists, its
       contents and metadata never change. So we let the kernel cache
       name lookups, file attributes and file data indefinitely,
       avoiding round-trips into this filesystem for data we've already
       served.

       Negative lookups are deliberately *not* cached: a store path that
       is absent now may appear later (e.g. after a build or an
       on-demand substitution), so caching its absence would hide it. */
    void * init(struct fuse_conn_info *, struct fuse_config * cfg)
    {
        /* ~100 years; effectively forever. */
        constexpr double forever = 100.0 * 365 * 24 * 60 * 60;
        cfg->entry_timeout = forever;
        cfg->attr_timeout = forever;
        cfg->negative_timeout = 0;

        /* Keep file data in the kernel page cache across opens instead
           of dropping it each time a file is (re)opened. */
        cfg->kernel_cache = 1;

        /* Preserve the `private_data` we passed to `fuse_new`. */
        return fuse_get_context()->private_data;
    }

    int getattr(const CanonPath & path, struct stat * st, struct fuse_file_info *)
    {
        debug("getattr: %s", path);

        memset(st, 0, sizeof(*st));

        if (path.isRoot()) {
            st->st_mode = S_IFDIR | 0555;
            st->st_nlink = 2;
            return 0;
        }

        try {
            /* Map the mount-relative path (`/<hash>-<name>/<sub>`) to a
               real store path and a subpath within it. */
            auto resolved = resolve(path);
            if (!resolved)
                return -ENOENT;
            auto & [accessor, subPath] = *resolved;

            auto stat = accessor->maybeLstat(subPath);
            if (!stat)
                return -ENOENT;

            switch (stat->type) {
            case SourceAccessor::tRegular:
                st->st_mode = S_IFREG | (stat->isExecutable ? 0555 : 0444);
                st->st_nlink = 1;
                if (stat->fileSize)
                    st->st_size = *stat->fileSize;
                break;
            case SourceAccessor::tDirectory:
                st->st_mode = S_IFDIR | 0555;
                st->st_nlink = 2;
                break;
            case SourceAccessor::tSymlink:
                st->st_mode = S_IFLNK | 0777;
                st->st_nlink = 1;
                /* For a symlink, `st_size` is the length of the target. */
                st->st_size = accessor->readLink(subPath).size();
                break;
            case SourceAccessor::tChar:
            case SourceAccessor::tBlock:
            case SourceAccessor::tSocket:
            case SourceAccessor::tFifo:
            case SourceAccessor::tUnknown:
                /* These cannot occur in a NAR-serialised store object. */
                return -EIO;
            }
        } catch (BadStorePath &) {
            /* The path doesn't name a valid store path. */
            return -ENOENT;
        } catch (...) {
            return -EIO;
        }

        return 0;
    }

    int readlink(const CanonPath & path, char * buf, size_t size)
    {
        debug("readlink: %s", path);

        if (path.isRoot())
            return -EINVAL;

        try {
            auto resolved = resolve(path);
            if (!resolved)
                return -ENOENT;
            auto & [accessor, subPath] = *resolved;

            auto stat = accessor->maybeLstat(subPath);
            if (!stat)
                return -ENOENT;
            if (stat->type != SourceAccessor::tSymlink)
                return -EINVAL;

            auto target = accessor->readLink(subPath);

            /* Copy the target into the buffer, truncating if it doesn't
               fit. `size` includes space for the terminating null. */
            if (size == 0)
                return 0;
            auto n = std::min(target.size(), size - 1);
            memcpy(buf, target.data(), n);
            buf[n] = '\0';
        } catch (BadStorePath &) {
            return -ENOENT;
        } catch (...) {
            return -EIO;
        }

        return 0;
    }

    /* State for an open file: the resolved accessor and subpath, stashed
       in `fuse_file_info::fh` by `open()` and freed by `release()`. Each
       `read()` fetches only the requested byte range from the accessor,
       so we never buffer the whole file. */
    struct FileHandle
    {
        Resolved resolved;
    };

    int open(const CanonPath & path, struct fuse_file_info * fi)
    {
        debug("open: %s", path);

        try {
            auto resolved = resolve(path);
            if (!resolved)
                return -ENOENT;

            auto stat = resolved->accessor->maybeLstat(resolved->subPath);
            if (!stat)
                return -ENOENT;
            if (stat->type == SourceAccessor::tDirectory)
                return -EISDIR;
            if (stat->type != SourceAccessor::tRegular)
                return -EINVAL;

            auto fh = std::make_unique<FileHandle>(std::move(*resolved));
            fi->fh = reinterpret_cast<uint64_t>(fh.release());
        } catch (BadStorePath &) {
            return -ENOENT;
        } catch (...) {
            return -EIO;
        }

        return 0;
    }

    int read(const CanonPath & path, char * buf, size_t size, off_t offset, struct fuse_file_info * fi)
    {
        debug("read: %s (size=%d, offset=%d)", path, size, offset);

        auto * fh = reinterpret_cast<FileHandle *>(fi->fh);
        if (!fh)
            return -EIO;

        if (offset < 0)
            return -EINVAL;

        try {
            /* Fetch only the requested range, writing each chunk
               directly into `buf`. */
            size_t n = 0;
            LambdaSink sink([&](std::string_view data) {
                assert(n + data.size() <= size);
                memcpy(buf + n, data.data(), data.size());
                n += data.size();
            });
            fh->resolved.accessor->readFile(
                fh->resolved.subPath, sink, [](uint64_t) {}, (uint64_t) offset, (uint64_t) size);

            return n;
        } catch (...) {
            return -EIO;
        }
    }

    int release(const CanonPath & path, struct fuse_file_info * fi)
    {
        debug("release: %s", path);

        delete reinterpret_cast<FileHandle *>(fi->fh);
        fi->fh = 0;
        return 0;
    }

    int readdir(
        const CanonPath & path,
        void * buf,
        fuse_fill_dir_t filler,
        off_t off,
        struct fuse_file_info * info,
        enum fuse_readdir_flags flags)
    {
        debug("readdir: %s", path);

        filler(buf, ".", nullptr, 0, (fuse_fill_dir_flags) 0);
        filler(buf, "..", nullptr, 0, (fuse_fill_dir_flags) 0);

        try {
            if (path.isRoot()) {
                /* The root lists every valid store path. */
                for (auto & storePath : store->queryAllValidPaths())
                    filler(buf, std::string(storePath.to_string()).c_str(), nullptr, 0, (fuse_fill_dir_flags) 0);
            } else {
                /* A subdirectory within (or the top level of) a store
                   path. */
                auto resolved = resolve(path);
                if (!resolved)
                    return -ENOENT;
                auto & [accessor, subPath] = *resolved;

                for (auto & [name, type] : accessor->readDirectory(subPath))
                    filler(buf, name.c_str(), nullptr, 0, (fuse_fill_dir_flags) 0);
            }
        } catch (BadStorePath &) {
            return -ENOENT;
        } catch (...) {
            return -EIO;
        }

        return 0;
    }
};

NixFs & getNixFs()
{
    return *static_cast<NixFs *>(fuse_get_context()->private_data);
}

const fuse_operations nixfsOps = {
    .getattr = [](const char * path, struct stat * st, struct fuse_file_info * info) -> int {
        return getNixFs().getattr(CanonPath(path), st, info);
    },
    .readlink = [](const char * path, char * buf, size_t size) -> int {
        return getNixFs().readlink(CanonPath(path), buf, size);
    },
    .open = [](const char * path, struct fuse_file_info * fi) -> int { return getNixFs().open(CanonPath(path), fi); },
    .read = [](const char * path, char * buf, size_t size, off_t offset, struct fuse_file_info * fi) -> int {
        return getNixFs().read(CanonPath(path), buf, size, offset, fi);
    },
    .release = [](const char * path, struct fuse_file_info * fi) -> int {
        return getNixFs().release(CanonPath(path), fi);
    },
    .readdir = [](const char * path,
                  void * buf,
                  fuse_fill_dir_t filler,
                  off_t off,
                  struct fuse_file_info * info,
                  enum fuse_readdir_flags flags) -> int {
        return getNixFs().readdir(CanonPath(path), buf, filler, off, info, flags);
    },
    .init = [](struct fuse_conn_info * conn, struct fuse_config * cfg) -> void * {
        return getNixFs().init(conn, cfg);
    }};
} // namespace

struct CmdFuse : StoreCommand
{
    std::filesystem::path mountPoint;

    CmdFuse()
    {
        expectArg("mountpoint", &mountPoint);
    }

    std::string description() override
    {
        return "serve the Nix store via a FUSE filesystem";
    }

    Category category() override
    {
        return catUtility;
    }

    void run(ref<Store> store) override
    {
        logger->stop();

        struct fuse_args args = FUSE_ARGS_INIT(0, nullptr);
        Finally freeArgs([&]() { fuse_opt_free_args(&args); });
        /* Mount read-only: the store is immutable, so the kernel rejects
           any modifying operation with EROFS at the VFS layer. */
        for (auto arg : {"nix", "-o", "ro"})
            if (fuse_opt_add_arg(&args, arg) != 0)
                throw Error("could not set up FUSE arguments");

        NixFs nixfs{.store = store};

        auto * fuse = fuse_new(&args, &nixfsOps, sizeof(nixfsOps), &nixfs);
        if (!fuse)
            throw Error("could not create FUSE filesystem");
        Finally destroyFuse([&]() { fuse_destroy(fuse); });

        if (fuse_mount(fuse, mountPoint.c_str()) != 0)
            throw SysError("could not mount FUSE filesystem at '%s'", mountPoint.string());
        Finally unmount([&]() { fuse_unmount(fuse); });

        notice("Serving the Nix store at '%s'.", mountPoint.string());

        /* Process requests on multiple threads. The operation
           callbacks must be thread-safe. `clone_fd` gives each worker
           thread its own `/dev/fuse` file descriptor to reduce kernel
           contention. */
        if (fuse_loop_mt(fuse, /*clone_fd=*/1) != 0)
            throw Error("FUSE event loop failed");

        // FIXME: not reached on Ctrl-C.
        notice("Exiting.");
    }
};

static auto rCmdFuse = registerCommand<CmdFuse>("fuse");
