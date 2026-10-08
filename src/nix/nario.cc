#include "nix/cmd/command.hh"
#include "nix/main/shared.hh"
#include "nix/store/store-api.hh"
#include "nix/store/export-import.hh"
#include "nix/util/callback.hh"
#include "nix/util/fs-sink.hh"
#include "nix/util/archive.hh"

#include "ls.hh"

#include <nlohmann/json.hpp>

using namespace nix;

struct CmdNario : NixMultiCommand
{
    CmdNario()
        : NixMultiCommand("nario", RegisterCommand::getCommandsFor({"nario"}))
    {
    }

    std::string description() override
    {
        return "operations for manipulating nario files";
    }

    Category category() override
    {
        return catUtility;
    }
};

static auto rCmdNario = registerCommand<CmdNario>("nario");

struct CmdNarioExport : StorePathsCommand
{
    unsigned int version = 0;
    std::vector<std::string> baseArgs;
    BaseSelectionMethod baseSelectionMethod = BaseSelectionMethod::byName;
    CompressionAlgo compression = CompressionAlgo::none;
    std::optional<int> compressionLevel;

    CmdNarioExport()
    {
        addFlag({
            .longName = "format",
            .description = "Version of the nario format to use. Must be `1` or `2`.",
            .labels = {"nario-format"},
            .handler = {&version},
            .required = true,
        });

        addFlag({
            .longName = "base",
            .description =
                "Assume that the closure of *installable* is already present at the destination. Paths in this closure are not exported, and other paths are exported as binary diffs against paths in this closure where possible. Can be specified multiple times.",
            .labels = {"installable"},
            .handler = {[this](std::string s) { baseArgs.push_back(s); }},
            .completer = getCompleteInstallable(),
        });

        addFlag({
            .longName = "base-selection-method",
            .description =
                "How to select the path in the base closure against which to diff a path. Currently the only method is `by-name` (select a path with the same name, ignoring the version), which is the default.",
            .labels = {"method"},
            .handler = {[this](std::string s) { baseSelectionMethod = parseBaseSelectionMethod(s); }},
            .completer = {[](AddCompletions & completions, size_t, std::string_view prefix) {
                for (auto method : {BaseSelectionMethod::byName}) {
                    auto s = showBaseSelectionMethod(method);
                    if (s.starts_with(prefix))
                        completions.add(s);
                }
            }},
        });

        addFlag({
            .longName = "compression",
            .description =
                "Compression method (e.g. `zstd`) to use for NARs that are not exported as binary diffs. The default is `none`. Requires nario format 2.",
            .labels = {"method"},
            .handler = {[this](std::string s) { compression = parseCompressionAlgo(s, true); }},
        });

        addFlag({
            .longName = "compression-level",
            .description =
                "Compression level to use for the method specified by `--compression`. The default is 9 for `zstd`, and the compression method's own default for other methods.",
            .labels = {"level"},
            .handler = {&compressionLevel},
        });
    }

    std::string description() override
    {
        return "serialize store paths to standard output in nario format";
    }

    std::string doc() override
    {
        return
#include "nario-export.md"
            ;
    }

    void run(ref<Store> store, StorePaths && storePaths) override
    {
        auto fd = getStandardOutput();
        if (isatty(fd))
            throw UsageError("refusing to write nario to a terminal");
        FdSink sink(std::move(fd));

        NarioExportOptions options{
            .version = version,
            .baseSelectionMethod = baseSelectionMethod,
            .compression = compression,
            .compressionLevel = compressionLevel,
        };

        if (!baseArgs.empty())
            options.basePaths = Installable::toStorePathSet(
                getEvalStore(), store, Realise::Outputs, operateOn, parseInstallables(store, baseArgs));

        exportPaths(*store, StorePathSet(storePaths.begin(), storePaths.end()), sink, options);
    }
};

static auto rCmdNarioExport = registerCommand2<CmdNarioExport>({"nario", "export"});

static FdSource getNarioSource()
{
    auto fd = getStandardInput();
    if (isatty(fd))
        throw UsageError("refusing to read nario from a terminal");
    return FdSource(std::move(fd));
}

struct CmdNarioImport : StoreCommand, MixNoCheckSigs
{
    std::string description() override
    {
        return "import store paths from a nario file on standard input";
    }

    std::string doc() override
    {
        return
#include "nario-import.md"
            ;
    }

    void run(ref<Store> store) override
    {
        auto source{getNarioSource()};
        importPaths(*store, source, checkSigs);
    }
};

static auto rCmdNarioImport = registerCommand2<CmdNarioImport>({"nario", "import"});

nlohmann::json listNar(Source & source)
{
    struct : FileSystemObjectSink
    {
        nlohmann::json root = nlohmann::json::object();

        nlohmann::json & makeObject(const CanonPath & path, std::string_view type)
        {
            auto * cur = &root;
            for (auto & c : path) {
                assert((*cur)["type"] == "directory");
                auto i = (*cur)["entries"].emplace(c, nlohmann::json::object()).first;
                cur = &i.value();
            }
            auto inserted = cur->emplace("type", type).second;
            assert(inserted);
            return *cur;
        }

        void createDirectory(const CanonPath & path) override
        {
            auto & j = makeObject(path, "directory");
            j["entries"] = nlohmann::json::object();
        }

        void createRegularFile(const CanonPath & path, fun<void(CreateRegularFileSink &)> func) override
        {
            struct : CreateRegularFileSink
            {
                bool executable = false;
                std::optional<uint64_t> size;

                void operator()(std::string_view data) override {}

                void preallocateContents(uint64_t s) override
                {
                    size = s;
                }

                void isExecutable() override
                {
                    executable = true;
                }
            } crf;

            crf.skipContents = true;

            func(crf);

            auto & j = makeObject(path, "regular");
            j.emplace("size", crf.size.value());
            if (crf.executable)
                j.emplace("executable", true);
        }

        void createSymlink(const CanonPath & path, const std::string & target) override
        {
            auto & j = makeObject(path, "symlink");
            j.emplace("target", target);
        }

    } parseSink;

    parseDump(parseSink, source);

    return parseSink.root;
}

void renderNarListing(const CanonPath & prefix, const nlohmann::json & root, bool longListing)
{
    std::function<void(const nlohmann::json & json, const CanonPath & path)> recurse;
    recurse = [&](const nlohmann::json & json, const CanonPath & path) {
        auto type = json["type"];

        if (longListing) {
            auto tp = type == "regular"   ? (json.find("executable") != json.end() ? "-r-xr-xr-x" : "-r--r--r--")
                      : type == "symlink" ? "lrwxrwxrwx"
                                          : "dr-xr-xr-x";
            auto line = fmt("%s %9d %s", tp, type == "regular" ? (uint64_t) json["size"] : 0, prefix / path);
            if (type == "symlink")
                line += " -> " + (std::string) json["target"];
            logger->cout(line);
        } else
            logger->cout(fmt("%s", prefix / path));

        if (type == "directory") {
            for (auto & entry : json["entries"].items()) {
                recurse(entry.value(), path / entry.key());
            }
        }
    };

    recurse(root, CanonPath::root);
}

struct CmdNarioList : Command, MixJSON, MixLongListing
{
    bool listContents = false;

    CmdNarioList()
    {
        addFlag({
            .longName = "recursive",
            .shortName = 'R',
            .description = "List the contents of NARs inside the nario.",
            .handler = {&listContents, true},
        });
    }

    std::string description() override
    {
        return "list the contents of a nario file";
    }

    std::string doc() override
    {
        return
#include "nario-list.md"
            ;
    }

    void run() override
    {
        struct Config : StoreConfig
        {
            Config(const Params & params)
                : StoreConfig(params, FilePathType::Unix)
            {
            }

            ref<Store> openStore() const override
            {
                abort();
            }

            void anchor() override {}
        };

        struct ListingStore : Store
        {
            ListingStore(ref<const Config> config)
                : Store{*config}
            {
            }

            void queryPathInfoUncached(
                const StorePath & path, Callback<std::shared_ptr<const ValidPathInfo>> callback) noexcept override
            {
                callback(nullptr);
            }

            std::optional<TrustedFlag> isTrustedClient() override
            {
                return Trusted;
            }

            std::optional<StorePath> queryPathFromHashPart(const std::string & hashPart) override
            {
                return std::nullopt;
            }

            void
            addToStore(const ValidPathInfo & info, Source & source, RepairFlag repair, CheckSigsFlag checkSigs) override
            {
                unsupported("addToStore");
            }

            StorePath addToStoreFromDump(
                Source & dump,
                std::string_view name,
                FileSerialisationMethod dumpMethod,
                ContentAddressMethod hashMethod,
                HashAlgorithm hashAlgo,
                const StorePathSet & references,
                RepairFlag repair,
                std::shared_ptr<const Provenance> provenance) override
            {
                unsupported("addToStoreFromDump");
            }

            void narFromPath(const StorePath & path, Sink & sink) override
            {
                unsupported("narFromPath");
            }

            void queryRealisationUncached(
                const DrvOutput &, Callback<std::shared_ptr<const UnkeyedRealisation>> callback) noexcept override
            {
                callback(nullptr);
            }

            ref<SourceAccessor> getFSAccessor(bool requireValidPath) override
            {
                return makeEmptySourceAccessor();
            }

            std::shared_ptr<SourceAccessor> getFSAccessor(const StorePath & path, bool requireValidPath) override
            {
                unsupported("getFSAccessor");
            }

            void registerDrvOutput(const Realisation & output) override
            {
                unsupported("registerDrvOutput");
            }

            void anchor() override {}
        };

        struct Lister : NarioVisitor
        {
            Store & store;
            CmdNarioList & cmd;
            std::optional<nlohmann::json> json;

            Lister(Store & store, CmdNarioList & cmd)
                : store(store)
                , cmd(cmd)
            {
            }

            void
            add(const ValidPathInfo & info,
                std::optional<nlohmann::json> contents,
                std::function<void(nlohmann::json &)> extendJSON = {})
            {
                if (!json)
                    return;
                // FIXME: make the JSON format configurable.
                auto obj = info.toJSON(&store, true, PathInfoJsonFormat::V1);
                if (contents)
                    obj.emplace("contents", std::move(*contents));
                if (extendJSON)
                    extendJSON(obj);
                json->emplace(store.printStorePath(info.path), std::move(obj));
            }

            void
            fullPath(const ValidPathInfo & info, Source & nar, std::optional<NarioCompression> compression) override
            {
                std::optional<nlohmann::json> contents;
                if (cmd.listContents)
                    contents = listNar(nar);
                else
                    nar.skip(info.narSize);

                if (!json) {
                    if (contents)
                        renderNarListing(CanonPath(store.printStorePath(info.path)), *contents, cmd.longListing);
                    else if (compression)
                        logger->cout(
                            fmt("%s: %d bytes, %s-compressed (%d bytes)",
                                store.printStorePath(info.path),
                                info.narSize,
                                showCompressionAlgo(compression->algo),
                                compression->size));
                    else
                        logger->cout(fmt("%s: %d bytes", store.printStorePath(info.path), info.narSize));
                }

                add(info, std::move(contents), [&](nlohmann::json & obj) {
                    if (compression)
                        obj.emplace(
                            "compression",
                            nlohmann::json{
                                {"method", showCompressionAlgo(compression->algo)},
                                {"size", compression->size},
                            });
                });
            }

            void diffPath(
                const ValidPathInfo & info,
                NarioDiffAlgo algo,
                const StorePath & basePath,
                const Hash & baseNarHash,
                std::string_view patch) override
            {
                if (!json)
                    logger->cout(
                        fmt("%s: %d bytes, %s diff against %s (%d bytes)",
                            store.printStorePath(info.path),
                            info.narSize,
                            showNarioDiffAlgo(algo),
                            store.printStorePath(basePath),
                            patch.size()));

                add(info, std::nullopt, [&](nlohmann::json & obj) {
                    obj.emplace(
                        "diff",
                        nlohmann::json{
                            {"algorithm", showNarioDiffAlgo(algo)},
                            {"base", store.printStorePath(basePath)},
                            {"baseNarHash", baseNarHash.to_string(HashFormat::SRI, true)},
                            {"size", patch.size()},
                        });
                });
            }

            void presentPath(const ValidPathInfo & info) override
            {
                if (!json)
                    logger->cout(fmt("%s: expected to be present", store.printStorePath(info.path)));

                add(info, std::nullopt, [](nlohmann::json & obj) { obj.emplace("present", true); });
            }
        };

        auto source{getNarioSource()};
        auto config = make_ref<Config>(StoreConfig::Params());
        ListingStore store(config);
        Lister lister(store, *this);
        if (json)
            lister.json = nlohmann::json::object();
        parseNario(store, source, lister);
        if (json) {
            auto j = nlohmann::json::object();
            j["version"] = 1;
            j["paths"] = std::move(*lister.json);
            printJSON(j);
        }
    }
};

static auto rCmdNarioList = registerCommand2<CmdNarioList>({"nario", "list"});
