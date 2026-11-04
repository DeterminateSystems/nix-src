#include "builder-child.hh"

#include "nix/store/build/child.hh"
#include "nix/store/builtins.hh"
#include "nix/store/globals.hh"
#include "nix/util/config-global.hh"
#include "nix/util/logging.hh"
#include "nix/util/terminal.hh"
#include "nix/util/wire-serialise.hh"

#include <fcntl.h>
#include <grp.h>
#include <termios.h>
#include <unistd.h>
#include <sys/resource.h>

namespace nix {

void runBuilderChild(const BuilderChildSpec & spec, const BuilderChildHooks & hooks)
{
    /* Warning: in the child we should absolutely not make any SQLite
       calls! */

    bool sendException = true;

    try { /* child */

        commonChildInit();

        BasicDerivation builtinDrv;

        if (spec.builtin) {
            /* Apply the parent's settings first, since parsing the
               derivation depends on them (e.g. on experimental
               features). */
            StringSource settingsSource(spec.builtin->settings);
            applyGlobalSettings(settingsSource);

            StringSource drvSource(spec.builtin->drv);
            readDerivation(drvSource, StoreDirConfig{spec.storeDir}, builtinDrv, spec.builtin->drvName);

            /* Builtin builders use libstore (e.g. builtin:fetchurl
               uses curl). Note that this must be done before entering
               the sandbox, since it preloads NSS modules. */
            initLibStore(false);
        }

        hooks.enterSandbox();

        if (chdir(spec.tmpDirInSandbox.c_str()) == -1)
            throw SysError("changing into %1%", PathFmt(spec.tmpDirInSandbox));

        /* Close all other file descriptors. */
        unix::closeExtraFDs();

        /* Disable core dumps by default. */
        struct rlimit limit = {0, RLIM_INFINITY};
        setrlimit(RLIMIT_CORE, &limit);

        /* Make sure the builder inherits a predictable umask. It must not be group-writable, since registerOutputs
         * rejects those as defense-in-depth. */
        umask(0022);

        // FIXME: set other limits to deterministic values?

        hooks.setUser();

        /* Indicate that we managed to set up the build environment. */
        writeFull(STDERR_FILENO, std::string("\2\n"));

        sendException = false;

        /* If this is a builtin builder, call it now. This should not return. */
        if (spec.builtin) {
            try {
                logger = makeJSONLogger(getStandardError()).release();

                BuiltinBuilderContext ctx{
                    .drv = builtinDrv,
                    .outputs = spec.builtin->outputs,
                    .netrcData = spec.builtin->netrcData,
                    .caFileData = spec.builtin->caFileData,
                    .hashedMirrors = spec.builtin->hashedMirrors,
                    .tmpDirInSandbox = spec.tmpDirInSandbox,
#if NIX_WITH_AWS_AUTH
                    .awsCredentials = spec.builtin->awsCredentials,
#endif
                };

                std::string builtinName = ctx.drv.builder.substr(8);
                assert(RegisterBuiltinBuilder::builtinBuilders);
                if (auto builtin = get(RegisterBuiltinBuilder::builtinBuilders(), builtinName))
                    (*builtin)(ctx);
                else
                    throw Error("unsupported builtin builder '%1%'", builtinName);
                _exit(0);
            } catch (std::exception & e) {
                writeFull(STDERR_FILENO, e.what() + std::string("\n"));
                _exit(1);
            }
        }

        /* It's not a builtin builder, so execute the program. */
        hooks.execBuilder(spec.args, spec.envStrs);

        throw SysError("executing '%1%'", PathFmt(spec.builder));

    } catch (...) {
        handleChildException(sendException);
        _exit(1);
    }
}

AutoCloseFD openPtySlave(const std::string & slaveName)
{
    AutoCloseFD builderOut = open(slaveName.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (!builderOut)
        throw SysError("opening pseudoterminal slave");

    // Put the pt into raw mode to prevent \n -> \r\n translation.
    struct termios term;
    if (tcgetattr(builderOut.get(), &term))
        throw SysError("getting pseudoterminal attributes");

    cfmakeraw(&term);

    if (tcsetattr(builderOut.get(), TCSANOW, &term))
        throw SysError("putting pseudoterminal into raw mode");

    return builderOut;
}

void setCredentials(const BuilderChildSpec::Credentials & credentials)
{
    preserveDeathSignal([&]() {
        if (credentials.supplementaryGids
            && setgroups(credentials.supplementaryGids->size(), credentials.supplementaryGids->data()) == -1)
            throw SysError("cannot set supplementary groups of build user");

        if (setgid(credentials.gid) == -1 || getgid() != credentials.gid || getegid() != credentials.gid)
            throw SysError("setgid failed");

        if (setuid(credentials.uid) == -1 || getuid() != credentials.uid || geteuid() != credentials.uid)
            throw SysError("setuid failed");
    });
}

static RegisterSpawnOp buildOp("build", [](Source & source, std::vector<AutoCloseFD> fds) {
    size_t nextFd = 0;

    /* Set up stderr first, so that errors from here on are reported
       to the parent. */
    {
        auto ptySlaveName = readWire<std::optional<std::string>>(source);
        auto slave = ptySlaveName ? openPtySlave(*ptySlaveName) : std::move(fds.at(nextFd++));
        if (dup2(slave.get(), STDERR_FILENO) == -1)
            throw SysError("cannot pipe standard error into log file");
    }

    auto spec = [&]() {
        try {
            return readWire<BuilderChildSpec>(source);
        } catch (...) {
            handleChildException(true);
            _exit(1);
        }
    }();

#ifdef __linux__
    AutoCloseFD userNamespaceSync;
    if (spec.linuxSandbox)
        userNamespaceSync = std::move(fds.at(nextFd++));
#endif

    runBuilderChild(
        spec,
        {
            .enterSandbox =
                [&]() {
#ifdef __linux__
                    if (spec.linuxSandbox)
                        enterLinuxSandbox(spec.storeDir, *spec.linuxSandbox, userNamespaceSync.get());
                    userNamespaceSync.close();
                    if (spec.linuxSecurity)
                        applyLinuxSecurity(*spec.linuxSecurity);
#endif
                },
            .setUser =
                [&]() {
                    if (spec.credentials)
                        setCredentials(*spec.credentials);
#ifdef __APPLE__
                    if (spec.darwinSandbox)
                        enterDarwinSandbox(spec.storeDir, *spec.darwinSandbox);
#endif
                },
            .execBuilder =
                [&](const Strings & args, const Strings & envStrs) {
#ifdef __APPLE__
                    if (spec.darwinSandbox) {
                        execDarwinBuilder(spec.darwinSandbox->platform, spec.builder, args, envStrs);
                        return;
                    }
#endif
                    execve(spec.builder.c_str(), stringsToCharPtrs(args).data(), stringsToCharPtrs(envStrs).data());
                },
        });
});

Pid spawnBuilderChild(
    const BuilderChildSpec & spec,
    const std::optional<std::string> & ptySlaveName,
    std::span<const Descriptor> fds,
    const SpawnOptions & options)
{
    return buildOp.spawn(
        [&](Sink & sink) {
            writeWire(sink, ptySlaveName);
            writeWire(sink, spec);
        },
        fds,
        options);
}

} // namespace nix
