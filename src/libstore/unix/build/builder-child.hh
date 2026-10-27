#pragma once
///@file

#include "nix/store/aws-creds.hh"
#include "nix/store/build/derivation-builder.hh"
#include "nix/store/derivations.hh"
#include "nix/util/processes.hh"
#include "nix/util/spawner.hh"

#include "store-config-private.hh"

namespace nix {

/**
 * Everything the child process of a build needs to know to set up
 * the build environment and start the builder. It is computed in the
 * parent and passed to the child, which may be created by the spawner
 * (see spawner.hh) and therefore doesn't have access to the parent's
 * state.
 */
struct BuilderChildSpec
{
    std::string storeDir;

    /**
     * The builder program, its arguments (including `argv[0]`) and
     * its environment, with input rewrites already applied.
     */
    std::filesystem::path builder;
    Strings args;
    Strings envStrs;

    std::filesystem::path tmpDirInSandbox;

    /**
     * If set, the child opens the pseudoterminal slave by name rather
     * than receiving it as a file descriptor. This is needed on macOS
     * (see c536e00c9deeac58bc4b3299dbc702604c32adbe).
     */
    std::optional<std::string> ptySlaveName;

    struct Builtin
    {
        BasicDerivation drv;
        std::map<std::string, std::string> outputs;
        std::string netrcData;
        std::string caFileData;
        Strings hashedMirrors;
#if NIX_WITH_AWS_AUTH
        std::optional<AwsCredentials> awsCredentials;
#endif
        /**
         * The parent's settings, as written by
         * `writeGlobalSettings()`. Builtin builders like
         * `builtin:fetchurl` depend on global settings.
         */
        std::string settings;
    };

    /**
     * Set if this is a builtin builder.
     */
    std::optional<Builtin> builtin;

    struct Credentials
    {
        uid_t uid;
        gid_t gid;
        /**
         * If set, `setgroups()` is called with these groups.
         */
        std::optional<std::vector<gid_t>> supplementaryGids;
    };

    /**
     * The credentials to switch to before starting the builder.
     */
    std::optional<Credentials> credentials;

#ifdef __linux__
    struct LinuxSecurity
    {
        bool allowNewPrivileges;
        bool filterSyscalls;
        bool impersonateLinux26;
        std::string platform;
    };

    /**
     * Restrictions (seccomp, landlock, personality) applied to the
     * builder.
     */
    std::optional<LinuxSecurity> linuxSecurity;

    struct LinuxSandbox
    {
        std::filesystem::path chrootRootDir;

        /**
         * The paths to bind-mount into the chroot. This is the
         * complete list, i.e. it includes the paths in `/dev` and
         * `/etc` added by the parent.
         */
        PathsInChroot pathsInChroot;

        /**
         * Whether the build has a private network namespace. If not,
         * it gets a `/etc/nsswitch.conf`.
         */
        bool privateNetwork;

        /**
         * Whether to create a minimal `/dev` (i.e. the host `/dev`
         * isn't bind-mounted).
         */
        bool minimalDev;

        bool mountSys;

        bool makeEtcReadOnly;

        bool unshareCgroup;

        std::string shmSize;
    };

    /**
     * Set for sandboxed Linux builds. The child receives the read
     * side of the user namespace synchronisation pipe.
     */
    std::optional<LinuxSandbox> linuxSandbox;
#endif

#ifdef __APPLE__
    struct DarwinSandbox
    {
        bool useSandbox;
        PathsInChroot pathsInChroot;
        bool logViolations;
        bool privateNetwork;
        Strings scratchOutputPaths;
        std::string additionalSandboxProfile;
        std::filesystem::path tmpDir;
        std::string globalTmpDir;
        bool allowLocalNetworking;
        bool disableSandbox;
        std::string platform;
    };

    std::optional<DarwinSandbox> darwinSandbox;
#endif

    void write(Sink & sink) const;

    static BuilderChildSpec read(Source & source);
};

/**
 * Functions called by `runBuilderChild()` to set up the build
 * environment. These are virtual methods for builders that don't use
 * the spawner yet.
 */
struct BuilderChildHooks
{
    /**
     * Enter the sandbox, if any.
     */
    fun<void()> enterSandbox;

    /**
     * Switch to the build user, if any.
     */
    fun<void()> setUser;

    /**
     * Execute the builder. Should not return unless there is an error.
     */
    fun<void(const Strings & args, const Strings & envStrs)> execBuilder;
};

/**
 * Run the child side of a build: set up the build environment and
 * execute the builder (or run the builtin builder). stderr must be the
 * pseudoterminal slave.
 */
[[noreturn]] void runBuilderChild(const BuilderChildSpec & spec, const BuilderChildHooks & hooks);

/**
 * Start the child side of a build via the spawner. `fds` are the
 * pseudoterminal slave (unless `spec.ptySlaveName` is set) and, for
 * sandboxed Linux builds, the read side of the user namespace
 * synchronisation pipe.
 */
Pid spawnBuilderChild(const BuilderChildSpec & spec, std::span<const Descriptor> fds, const SpawnOptions & options);

/**
 * Open the pseudoterminal slave with the specified name, and put it in
 * raw mode.
 */
AutoCloseFD openPtySlave(const std::string & slaveName);

/**
 * Switch to the specified credentials, preserving the parent death
 * signal.
 */
void setCredentials(const BuilderChildSpec::Credentials & credentials);

#ifdef __linux__
/**
 * Set up the chroot, mounts etc. of a sandboxed build. Implemented in
 * linux-derivation-builder.cc.
 */
void enterLinuxSandbox(
    const std::string & storeDir, const BuilderChildSpec::LinuxSandbox & sandbox, Descriptor userNamespaceSync);

/**
 * Apply seccomp, landlock etc. Implemented in linux-derivation-builder.cc.
 */
void applyLinuxSecurity(const BuilderChildSpec::LinuxSecurity & security);
#endif

#ifdef __APPLE__
/**
 * Apply the macOS sandbox profile. Must be called after switching to
 * the build user. Implemented in darwin-derivation-builder.cc.
 */
void enterDarwinSandbox(const std::string & storeDir, const BuilderChildSpec::DarwinSandbox & sandbox);

/**
 * Execute the builder on macOS. Implemented in darwin-derivation-builder.cc.
 */
void execDarwinBuilder(
    const std::string & platform, const std::filesystem::path & builder, const Strings & args, const Strings & envStrs);
#endif

} // namespace nix
