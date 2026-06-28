#include "nix/cmd/command.hh"
#include "nix/main/shared.hh"
#include "nix/store/store-api.hh"
#include "nix/util/finally.hh"
#include "nix/util/signals.hh"

#define FUSE_USE_VERSION 31
#include <fuse.h>

#include <cerrno>
#include <cstring>

using namespace nix;

namespace {

int nixfsGetattr(const char * path, struct stat * st, struct fuse_file_info *)
{
    memset(st, 0, sizeof(*st));

    if (strcmp(path, "/") == 0) {
        st->st_mode = S_IFDIR | 0555;
        st->st_nlink = 2;
        return 0;
    }

    return -ENOENT;
}

int nixfsReaddir(
    const char * path, void * buf, fuse_fill_dir_t filler, off_t, struct fuse_file_info *, enum fuse_readdir_flags)
{
    if (strcmp(path, "/") != 0)
        return -ENOENT;

    filler(buf, ".", nullptr, 0, (fuse_fill_dir_flags) 0);
    filler(buf, "..", nullptr, 0, (fuse_fill_dir_flags) 0);

    return 0;
}

const fuse_operations nixfsOps = {
    .getattr = nixfsGetattr,
    .readdir = nixfsReaddir,
};

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

        auto * fuse = fuse_new(&args, &nixfsOps, sizeof(nixfsOps), nullptr);
        if (!fuse)
            throw Error("could not create FUSE filesystem");
        Finally destroyFuse([&]() { fuse_destroy(fuse); });

        if (fuse_mount(fuse, mountPoint.c_str()) != 0)
            throw SysError("could not mount FUSE filesystem at '%s'", mountPoint.string());
        Finally unmount([&]() { fuse_unmount(fuse); });

        notice("Serving the Nix store at '%s'.", mountPoint.string());

        if (fuse_loop(fuse) != 0)
            throw Error("FUSE event loop failed");

        // FIXME: not reached on Ctrl-C.
        notice("Exiting.");
    }
};

static auto rCmdFuse = registerCommand<CmdFuse>("fuse");
