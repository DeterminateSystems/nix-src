#include "nix/util/file-descriptor.hh"

namespace nix {

struct ExternalDerivationBuilder : DerivationBuilderImpl
{
    Settings::ExternalBuilder externalBuilder;

    PathsInChroot pathsInChroot;

    /**
     * Whether full sandboxing is enabled. Note that macOS builds
     * always have *some* sandboxing (see sandbox-minimal.sb).
     */
    bool useSandbox;

    /**
     * Pipe for talking to the spawned builder.
     */
    Pipe toBuilder;

    ExternalDerivationBuilder(
        Store & store,
        std::unique_ptr<DerivationBuilderCallbacks> miscMethods,
        DerivationBuilderParams params,
        Settings::ExternalBuilder externalBuilder,
        bool useSandbox)
        : DerivationBuilderImpl(store, std::move(miscMethods), std::move(params))
        , externalBuilder(std::move(externalBuilder))
        , useSandbox(useSandbox)
    {
    }

    static std::unique_ptr<ExternalDerivationBuilder> newIfSupported(
        Store & store, std::unique_ptr<DerivationBuilderCallbacks> & miscMethods, DerivationBuilderParams & params)
    {
        for (auto & handler : settings.externalBuilders.get()) {
            for (auto & system : handler.systems)
                if (params.drv.platform == system)
                    return std::make_unique<ExternalDerivationBuilder>(
                        // FIXME: sandbox true breaks things -- maybe related to paths the builder uses or something not being in the sandbox
                        // FIXME: probably want to make it so that the external builder is probably what sets up the sandbox, but how do I do that
                        store, std::move(miscMethods), std::move(params), handler, false);
        }
        return {};
    }

    void prepareSandbox() override
    {
        // FIXME: doesn't work with the darwin sandboxing stuff cuz of "mismatched impure paths"
        // pathsInChroot = getPathsInSandbox();

        pathsInChroot[externalBuilder.program] = {externalBuilder.program, false};

        // FIXME: don't hardcode
        auto paths = {
            "/nix/store/5dy050xmn2645mxrvcjxfkbpdx0k7674-initrd/initrd",
            "/nix/store/jfi2qs9d52fazgjwzqvjz5cb5m20ivxf-linux-6.15.3/Image",
            "/etc/determinate/config.json",
        };

        for (auto & path : paths) {
            pathsInChroot[path] = {path, false};
        }
    }

    bool prepareBuild() override
    {
        // External builds don't use build users, so this always
        // succeeds.
        // TODO: maybe only do this if sandboxing is enabled?
        return DerivationBuilderImpl::prepareBuild();
    }

    Path tmpDirInSandbox() override
    {
        /* In a sandbox, for determinism, always use the same temporary
           directory. */
        return "/build";
    }

    void setBuildTmpDir() override
    {
        tmpDir = topTmpDir + "/build";
        createDir(tmpDir, 0700);
    }

    void prepareUser() override
    {
        // Nothing to do here since we don't have a build user.
    }

    void setUser() override
    {
        DerivationBuilderImpl::setUser();

#ifdef __APPLE__
        /* This has to appear before import statements. */
        std::string sandboxProfile = "(version 1)\n";

        if (useSandbox) {

            /* Lots and lots and lots of file functions freak out if they can't stat their full ancestry */
            PathSet ancestry;

            /* We build the ancestry before adding all inputPaths to the store because we know they'll
               all have the same parents (the store), and there might be lots of inputs. This isn't
               particularly efficient... I doubt it'll be a bottleneck in practice */
            for (auto & i : pathsInChroot) {
                Path cur = i.first;
                while (cur.compare("/") != 0) {
                    cur = dirOf(cur);
                    ancestry.insert(cur);
                }
            }

            /* And we want the store in there regardless of how empty pathsInChroot. We include the innermost
               path component this time, since it's typically /nix/store and we care about that. */
            Path cur = store.storeDir;
            while (cur.compare("/") != 0) {
                ancestry.insert(cur);
                cur = dirOf(cur);
            }

            /* Add all our input paths to the chroot */
            for (auto & i : inputPaths) {
                auto p = store.printStorePath(i);
                pathsInChroot.insert_or_assign(p, p);
            }

            /* Violations will go to the syslog if you set this. Unfortunately the destination does not appear to be
             * configurable */
            if (settings.darwinLogSandboxViolations) {
                sandboxProfile += "(deny default)\n";
            } else {
                sandboxProfile += "(deny default (with no-log))\n";
            }

            sandboxProfile +=
#  include "sandbox-defaults.sb"
                ;

            if (!derivationType.isSandboxed())
                sandboxProfile +=
#  include "sandbox-network.sb"
                    ;

            /* Add the output paths we'll use at build-time to the chroot */
            sandboxProfile += "(allow file-read* file-write* process-exec\n";
            for (auto & [_, path] : scratchOutputs)
                sandboxProfile += fmt("\t(subpath \"%s\")\n", store.printStorePath(path));

            sandboxProfile += ")\n";

            /* Our inputs (transitive dependencies and any impurities computed above)

               without file-write* allowed, access() incorrectly returns EPERM
             */
            sandboxProfile += "(allow file-read* file-write* process-exec\n";

            // We create multiple allow lists, to avoid exceeding a limit in the darwin sandbox interpreter.
            // See https://github.com/NixOS/nix/issues/4119
            // We split our allow groups approximately at half the actual limit, 1 << 16
            const size_t breakpoint = sandboxProfile.length() + (1 << 14);
            for (auto & i : pathsInChroot) {

                if (sandboxProfile.length() >= breakpoint) {
                    debug("Sandbox break: %d %d", sandboxProfile.length(), breakpoint);
                    sandboxProfile += ")\n(allow file-read* file-write* process-exec\n";
                }

                if (i.first != i.second.source)
                    throw Error(
                        "can't map '%1%' to '%2%': mismatched impure paths not supported on Darwin",
                        i.first,
                        i.second.source);

                std::string path = i.first;
                auto optSt = maybeLstat(path.c_str());
                if (!optSt) {
                    if (i.second.optional)
                        continue;
                    throw SysError("getting attributes of required path '%s", path);
                }
                if (S_ISDIR(optSt->st_mode))
                    sandboxProfile += fmt("\t(subpath \"%s\")\n", path);
                else
                    sandboxProfile += fmt("\t(literal \"%s\")\n", path);
            }
            sandboxProfile += ")\n";

            /* Allow file-read* on full directory hierarchy to self. Allows realpath() */
            sandboxProfile += "(allow file-read*\n";
            for (auto & i : ancestry) {
                sandboxProfile += fmt("\t(literal \"%s\")\n", i);
            }
            sandboxProfile += ")\n";

            sandboxProfile += drvOptions.additionalSandboxProfile;
        } else
            sandboxProfile +=
#  include "sandbox-minimal.sb"
                ;

        debug("Generated sandbox profile:");
        debug(sandboxProfile);


        /* The tmpDir in scope points at the temporary build directory for our derivation. Some packages try different
           mechanisms to find temporary directories, so we want to open up a broader place for them to put their files,
           if needed. */
        Path globalTmpDir = canonPath(defaultTempDir(), true);

        /* They don't like trailing slashes on subpath directives */
        while (!globalTmpDir.empty() && globalTmpDir.back() == '/')
            globalTmpDir.pop_back();

        if (getEnv("_NIX_TEST_NO_SANDBOX") != "1") {
            Strings sandboxArgs;
            sandboxArgs.push_back("_GLOBAL_TMP_DIR");
            sandboxArgs.push_back(globalTmpDir);
            if (drvOptions.allowLocalNetworking) {
                sandboxArgs.push_back("_ALLOW_LOCAL_NETWORKING");
                sandboxArgs.push_back("1");
            }
            char * sandbox_errbuf = nullptr;
            if (sandbox_init_with_parameters(
                    sandboxProfile.c_str(), 0, stringsToCharPtrs(sandboxArgs).data(), &sandbox_errbuf)) {
                writeFull(
                    STDERR_FILENO,
                    fmt("failed to configure sandbox: %s\n", sandbox_errbuf ? sandbox_errbuf : "(null)"));
                _exit(1);
            }
        }

#endif
    }

    void checkSystem() override
    {
        // FIXME: should check system features.
    }

    void startChild() override
    {
        if (drvOptions.getRequiredSystemFeatures(drv).count("recursive-nix"))
            throw Error("'recursive-nix' is not supported yet by external derivation builders");

        auto json = nlohmann::json::object();

        json.emplace("builder", drv.builder);
        {
            auto l = nlohmann::json::array();
            for (auto & i : drv.args)
                l.push_back(rewriteStrings(i, inputRewrites));
            json.emplace("args", std::move(l));
        }
        {
            auto j = nlohmann::json::object();
            for (auto & [name, value] : env)
                j.emplace(name, rewriteStrings(value, inputRewrites));
            json.emplace("env", std::move(j));
        }
        json.emplace("topTmpDir", topTmpDir);
        json.emplace("tmpDir", tmpDir);
        json.emplace("tmpDirInSandbox", tmpDirInSandbox());
        json.emplace("storeDir", store.storeDir);
        json.emplace("realStoreDir", getLocalStore(store).config->realStoreDir.get());
        json.emplace("system", drv.platform);

        toBuilder.create();
        // FIXME: this is probably bad...? but we need to make them accessible so we can do some writing stuff into them...
        chownToBuilder(tmpDir);
        chownToBuilder(topTmpDir);

        pid = startProcess([&]() {
            openSlave();
            try {
                commonChildInit();

                if (dup2(toBuilder.readSide.get(), STDIN_FILENO) == -1)
                    throw SysError("dupping to-builder read side to builder's stdin");

                Strings args = {externalBuilder.program};

                if (!externalBuilder.args.empty()) {
                    args.insert(args.end(), externalBuilder.args.begin(), externalBuilder.args.end());
                }

                setUser();

                if (chdir(tmpDir.c_str()) == -1)
                    throw SysError("changing into '%1%'", tmpDir);

                debug("executing external builder: %s", concatStringsSep(" ", args));
                execv(externalBuilder.program.c_str(), stringsToCharPtrs(args).data());

                throw SysError("executing '%s'", externalBuilder.program);
            } catch (...) {
                handleChildException(true);
                _exit(1);
            }
        });

        writeFull(toBuilder.writeSide.get(), json.dump());
        toBuilder.close();
    }
};

}
