#include "nix/fetchers/fetchers.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/store/store-api.hh"
#include "nix/util/logging.hh"
#include "nix/util/processes.hh"
#include "nix/util/strings.hh"

#include <nlohmann/json.hpp>

namespace nix::fetchers {

/* A fetcher that resolves a FlakeHub output reference like
   `DeterminateSystems/nix-wasm-rust/^0#packages.x86_64-linux.default`
   to a prebuilt store path, using the `fh` CLI. The store path is
   substituted rather than fetched as a source tree, so this provides
   a convenient way for flakes to depend on prebuilt binary artifacts
   (such as WASM plugins). */
struct FhResolveInputScheme : InputScheme
{
    std::string_view schemeName() const override
    {
        return "fh-resolve";
    }

    std::string schemeDescription() const override
    {
        return "Resolves a FlakeHub output reference (`«org»/«project»/«version»#«output»`) to a prebuilt store path using `fh resolve`.";
    }

    const std::map<std::string, AttributeInfo> & allowedAttrs() const override
    {
        static const std::map<std::string, AttributeInfo> attrs = {
            {
                "org",
                {.doc = "The FlakeHub organization (e.g. `DeterminateSystems`)."},
            },
            {
                "project",
                {.doc = "The name of the flake on FlakeHub (e.g. `nix-wasm-rust`)."},
            },
            {
                "version",
                {.doc = "The semantic version requirement of the flake (e.g. `^0` or `=0.1.88`)."},
            },
            {
                "output",
                {.doc = "The flake output attribute path to resolve (e.g. `packages.x86_64-linux.default`)."},
            },
            {
                "storePath",
                {.required = false, .doc = "The store path that the reference resolved to."},
            },
            {
                "narHash",
                {.required = false, .doc = "The NAR hash of the resolved store path."},
            },
        };
        return attrs;
    }

    std::optional<Input> inputFromAttrs(const Settings & settings, const Attrs & attrs) const override
    {
        getStrAttr(attrs, "org");
        getStrAttr(attrs, "project");
        getStrAttr(attrs, "version");
        getStrAttr(attrs, "output");

        Input input{};
        input.attrs = attrs;
        return input;
    }

    std::optional<Input> inputFromURL(const Settings & settings, const ParsedURL & url, bool requireTree) const override
    {
        if (url.scheme != schemeName())
            return {};

        auto path = url.pathSegments(/*skipEmpty=*/true) | std::ranges::to<std::vector<std::string>>();
        if (path.size() != 3)
            throw BadURL("URL '%s' should have the form 'fh-resolve:«org»/«project»/«version»#«output»'", url);

        if (url.fragment.empty())
            throw BadURL("URL '%s' lacks an output attribute path (e.g. '#packages.x86_64-linux.default')", url);

        Attrs attrs;
        attrs.insert_or_assign("type", std::string{schemeName()});
        attrs.insert_or_assign("org", path[0]);
        attrs.insert_or_assign("project", path[1]);
        attrs.insert_or_assign("version", path[2]);
        attrs.insert_or_assign("output", url.fragment);

        for (auto & [name, value] : url.query)
            if (name == "narHash" || name == "storePath")
                attrs.insert_or_assign(name, value);
            else
                throw BadURL("URL '%s' has unsupported parameter '%s'", url, name);

        return inputFromAttrs(settings, attrs);
    }

    ParsedURL toURL(const Input & input, bool abbreviate) const override
    {
        auto url = ParsedURL{
            .scheme = std::string{schemeName()},
            .path =
                {getStrAttr(input.attrs, "org"),
                 getStrAttr(input.attrs, "project"),
                 getStrAttr(input.attrs, "version")},
            .fragment = getStrAttr(input.attrs, "output"),
        };
        if (!abbreviate) {
            if (auto storePath = maybeGetStrAttr(input.attrs, "storePath"))
                url.query.insert_or_assign("storePath", *storePath);
            if (auto narHash = input.getNarHash())
                url.query.insert_or_assign("narHash", narHash->to_string(HashFormat::SRI, true));
        }
        return url;
    }

    bool isLocked(const Settings & settings, const Input & input) const override
    {
        return maybeGetStrAttr(input.attrs, "storePath").has_value() && input.getNarHash().has_value();
    }

    std::optional<std::string> getFingerprint(Store & store, const Input & input) const override
    {
        if (auto narHash = input.getNarHash())
            return "fh-resolve:" + narHash->to_string(HashFormat::SRI, true);
        return std::nullopt;
    }

    std::optional<std::pair<ref<SourceAccessor>, Input>>
    getAccessor(const Settings & settings, Store & store, const Input & _input, bool fastOnly) const override
    {
        Input input(_input);

        std::optional<StorePath> storePath;

        if (auto storePathS = maybeGetStrAttr(input.attrs, "storePath")) {
            /* Use the previously resolved store path, substituting it
               if it's not already valid. Note: for final locked inputs,
               `Input::getAccessorUnchecked()` will usually have done
               this already via `computeStorePath()`. */
            storePath = store.parseStorePath(*storePathS);
            store.addTempRoot(*storePath);
            if (!store.isValidPath(*storePath)) {
                if (fastOnly)
                    return std::nullopt;
                store.ensurePath(*storePath);
            }
        } else {
            if (fastOnly)
                return std::nullopt;

            auto fhRef =
                fmt("%s/%s/%s#%s",
                    getStrAttr(input.attrs, "org"),
                    getStrAttr(input.attrs, "project"),
                    getStrAttr(input.attrs, "version"),
                    getStrAttr(input.attrs, "output"));

            Activity act(*logger, lvlTalkative, actUnknown, fmt("resolving FlakeHub reference '%s'", fhRef));

            auto json = nlohmann::json::parse(runProgram("fh", true, {"resolve", "--json", fhRef}));

            storePath = store.parseStorePath(json.at("store_path").get<std::string>());
            store.addTempRoot(*storePath);
            if (!store.isValidPath(*storePath))
                store.ensurePath(*storePath);

            input.attrs.insert_or_assign("storePath", store.printStorePath(*storePath));
        }

        auto info = store.queryPathInfo(*storePath);

        input.checkNarHash(info->narHash, store.printStorePath(*storePath));

        if (!info->references.empty())
            throw Error(
                "store path '%s' of input '%s' has references (%s), which is not supported by 'fh-resolve' inputs",
                store.printStorePath(*storePath),
                input.to_string(),
                concatStringsSep(", ", info->references | std::views::transform([&](const StorePath & p) {
                                           return store.printStorePath(p);
                                       }) | std::ranges::to<std::vector<std::string>>()));

        input.attrs.insert_or_assign("narHash", info->narHash.to_string(HashFormat::SRI, true));

        return {{store.requireStoreObjectAccessor(*storePath), std::move(input)}};
    }
};

static auto rFhResolveInputScheme = OnStartup([] { registerInputScheme(std::make_unique<FhResolveInputScheme>()); });

} // namespace nix::fetchers
