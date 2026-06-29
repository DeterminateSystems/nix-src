#include "nix/cmd/command.hh"
#include "nix/main/shared.hh"
#include "nix/store/store-api.hh"
#include "nix/util/canon-path.hh"
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

        return -ENOENT;
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

        if (!path.isRoot())
            return -ENOENT;

        filler(buf, ".", nullptr, 0, (fuse_fill_dir_flags) 0);
        filler(buf, "..", nullptr, 0, (fuse_fill_dir_flags) 0);

        try {
            for (auto & storePath : store->queryAllValidPaths())
                filler(buf, std::string(storePath.to_string()).c_str(), nullptr, 0, (fuse_fill_dir_flags) 0);
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
        struct fuse_args args = FUSE_ARGS_INIT(0, nullptr);
        Finally freeArgs([&]() { fuse_opt_free_args(&args); });
        if (fuse_opt_add_arg(&args, "nix") != 0)
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
