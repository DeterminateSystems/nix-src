#include "builder-child.hh"

#include "nix/store/build/child.hh"
#include "nix/store/builtins.hh"
#include "nix/store/globals.hh"
#include "nix/util/config-global.hh"
#include "nix/util/logging.hh"
#include "nix/util/terminal.hh"

#include <fcntl.h>
#include <grp.h>
#include <termios.h>
#include <unistd.h>
#include <sys/resource.h>

namespace nix {

static void writePathsInChroot(Sink & sink, const PathsInChroot & paths)
{
    sink << paths.size();
    for (auto & [target, path] : paths)
        sink << target.string() << path.source.string() << (uint64_t) path.optional;
}

static PathsInChroot readPathsInChroot(Source & source)
{
    PathsInChroot paths;
    auto count = readNum<size_t>(source);
    for (size_t i = 0; i < count; ++i) {
        std::filesystem::path target = readString(source);
        ChrootPath path{.source = readString(source)};
        path.optional = readNum<uint64_t>(source);
        paths.insert_or_assign(std::move(target), std::move(path));
    }
    return paths;
}

static void writeOptionalString(Sink & sink, const std::optional<std::string> & s)
{
    sink << (uint64_t) s.has_value();
    if (s)
        sink << *s;
}

static std::optional<std::string> readOptionalString(Source & source)
{
    if (!readNum<uint64_t>(source))
        return std::nullopt;
    return readString(source);
}

void BuilderChildSpec::write(Sink & sink) const
{
    sink << storeDir << builder.string() << args << envStrs << tmpDirInSandbox.string();

    sink << (uint64_t) builtin.has_value();
    if (builtin) {
        sink << builtin->settings << builtin->drv.name;
        writeDerivation(sink, StoreDirConfig{storeDir}, builtin->drv);
        sink << builtin->outputs.size();
        for (auto & [name, path] : builtin->outputs)
            sink << name << path;
        sink << builtin->netrcData << builtin->caFileData << builtin->hashedMirrors;
#if NIX_WITH_AWS_AUTH
        sink << (uint64_t) builtin->awsCredentials.has_value();
        if (builtin->awsCredentials) {
            sink << builtin->awsCredentials->accessKeyId << builtin->awsCredentials->secretAccessKey;
            writeOptionalString(sink, builtin->awsCredentials->sessionToken);
        }
#endif
    }

    sink << (uint64_t) credentials.has_value();
    if (credentials) {
        sink << credentials->uid << credentials->gid << (uint64_t) credentials->supplementaryGids.has_value();
        if (credentials->supplementaryGids) {
            sink << credentials->supplementaryGids->size();
            for (auto gid : *credentials->supplementaryGids)
                sink << gid;
        }
    }

#ifdef __linux__
    sink << (uint64_t) linuxSecurity.has_value();
    if (linuxSecurity)
        sink << (uint64_t) linuxSecurity->allowNewPrivileges << (uint64_t) linuxSecurity->filterSyscalls
             << (uint64_t) linuxSecurity->impersonateLinux26 << linuxSecurity->platform;

    sink << (uint64_t) linuxSandbox.has_value();
    if (linuxSandbox) {
        sink << linuxSandbox->chrootRootDir.string();
        writePathsInChroot(sink, linuxSandbox->pathsInChroot);
        sink << (uint64_t) linuxSandbox->privateNetwork << (uint64_t) linuxSandbox->minimalDev
             << (uint64_t) linuxSandbox->mountSys << (uint64_t) linuxSandbox->makeEtcReadOnly
             << (uint64_t) linuxSandbox->unshareCgroup << linuxSandbox->shmSize;
    }
#endif

#ifdef __APPLE__
    sink << (uint64_t) darwinSandbox.has_value();
    if (darwinSandbox) {
        sink << (uint64_t) darwinSandbox->useSandbox;
        writePathsInChroot(sink, darwinSandbox->pathsInChroot);
        sink << (uint64_t) darwinSandbox->logViolations << (uint64_t) darwinSandbox->privateNetwork
             << darwinSandbox->scratchOutputPaths << darwinSandbox->additionalSandboxProfile
             << darwinSandbox->tmpDir.string() << darwinSandbox->globalTmpDir
             << (uint64_t) darwinSandbox->allowLocalNetworking << (uint64_t) darwinSandbox->disableSandbox
             << darwinSandbox->platform;
    }
#endif
}

BuilderChildSpec BuilderChildSpec::read(Source & source)
{
    BuilderChildSpec spec;

    spec.storeDir = readString(source);
    spec.builder = readString(source);
    spec.args = readStrings<Strings>(source);
    spec.envStrs = readStrings<Strings>(source);
    spec.tmpDirInSandbox = readString(source);

    if (readNum<uint64_t>(source)) {
        /* Apply the parent's settings first, since parsing the
           derivation depends on them (e.g. on experimental
           features). */
        auto settings = readString(source);
        StringSource settingsSource(settings);
        applyGlobalSettings(settingsSource);

        auto name = readString(source);
        BasicDerivation drv;
        readDerivation(source, StoreDirConfig{spec.storeDir}, drv, name);
        Builtin builtin{
            .drv = std::move(drv),
            .settings = std::move(settings),
        };
        auto nrOutputs = readNum<size_t>(source);
        for (size_t i = 0; i < nrOutputs; ++i) {
            auto outputName = readString(source);
            builtin.outputs.insert_or_assign(outputName, readString(source));
        }
        builtin.netrcData = readString(source);
        builtin.caFileData = readString(source);
        builtin.hashedMirrors = readStrings<Strings>(source);
#if NIX_WITH_AWS_AUTH
        if (readNum<uint64_t>(source)) {
            auto accessKeyId = readString(source);
            auto secretAccessKey = readString(source);
            builtin.awsCredentials.emplace(accessKeyId, secretAccessKey, readOptionalString(source));
        }
#endif
        spec.builtin = std::move(builtin);
    }

    if (readNum<uint64_t>(source)) {
        Credentials credentials{
            .uid = readNum<uid_t>(source),
            .gid = readNum<gid_t>(source),
        };
        if (readNum<uint64_t>(source)) {
            std::vector<gid_t> gids;
            auto count = readNum<size_t>(source);
            for (size_t i = 0; i < count; ++i)
                gids.push_back(readNum<gid_t>(source));
            credentials.supplementaryGids = std::move(gids);
        }
        spec.credentials = std::move(credentials);
    }

#ifdef __linux__
    if (readNum<uint64_t>(source)) {
        LinuxSecurity security;
        security.allowNewPrivileges = readNum<uint64_t>(source);
        security.filterSyscalls = readNum<uint64_t>(source);
        security.impersonateLinux26 = readNum<uint64_t>(source);
        security.platform = readString(source);
        spec.linuxSecurity = std::move(security);
    }

    if (readNum<uint64_t>(source)) {
        LinuxSandbox sandbox;
        sandbox.chrootRootDir = readString(source);
        sandbox.pathsInChroot = readPathsInChroot(source);
        sandbox.privateNetwork = readNum<uint64_t>(source);
        sandbox.minimalDev = readNum<uint64_t>(source);
        sandbox.mountSys = readNum<uint64_t>(source);
        sandbox.makeEtcReadOnly = readNum<uint64_t>(source);
        sandbox.unshareCgroup = readNum<uint64_t>(source);
        sandbox.shmSize = readString(source);
        spec.linuxSandbox = std::move(sandbox);
    }
#endif

#ifdef __APPLE__
    if (readNum<uint64_t>(source)) {
        DarwinSandbox sandbox;
        sandbox.useSandbox = readNum<uint64_t>(source);
        sandbox.pathsInChroot = readPathsInChroot(source);
        sandbox.logViolations = readNum<uint64_t>(source);
        sandbox.privateNetwork = readNum<uint64_t>(source);
        sandbox.scratchOutputPaths = readStrings<Strings>(source);
        sandbox.additionalSandboxProfile = readString(source);
        sandbox.tmpDir = readString(source);
        sandbox.globalTmpDir = readString(source);
        sandbox.allowLocalNetworking = readNum<uint64_t>(source);
        sandbox.disableSandbox = readNum<uint64_t>(source);
        sandbox.platform = readString(source);
        spec.darwinSandbox = std::move(sandbox);
    }
#endif

    return spec;
}

void runBuilderChild(const BuilderChildSpec & spec, const BuilderChildHooks & hooks)
{
    /* Warning: in the child we should absolutely not make any SQLite
       calls! */

    bool sendException = true;

    try { /* child */

        commonChildInit();

        /* Builtin builders use libstore (e.g. builtin:fetchurl uses
           curl). Note that this must be done before entering the
           sandbox, since it preloads NSS modules. */
        if (spec.builtin)
            initLibStore(false);

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
                    .drv = spec.builtin->drv,
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
        auto ptySlaveName = readOptionalString(source);
        auto slave = ptySlaveName ? openPtySlave(*ptySlaveName) : std::move(fds.at(nextFd++));
        if (dup2(slave.get(), STDERR_FILENO) == -1)
            throw SysError("cannot pipe standard error into log file");
    }

    auto spec = [&]() {
        try {
            return BuilderChildSpec::read(source);
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

Pid spawnBuilderChild(const BuilderChildSpec & spec, std::span<const Descriptor> fds, const SpawnOptions & options)
{
    return buildOp.spawn(
        [&](Sink & sink) {
            writeOptionalString(sink, spec.ptySlaveName);
            spec.write(sink);
        },
        fds,
        options);
}

} // namespace nix
