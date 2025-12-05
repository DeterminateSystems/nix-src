#include "nix/fetchers/fetch-settings.hh"
#include "nix/fetchers/registry.hh"
#include "nix/fetchers/tarball.hh"
#include "nix/util/users.hh"
#include "nix/store/globals.hh"
#include "nix/store/store-api.hh"
#include "nix/store/local-fs-store.hh"

#include <nlohmann/json.hpp>

namespace nix::fetchers {

std::shared_ptr<Registry> Registry::read(const Settings & settings, const Path & path, RegistryType type)
{
    debug("reading registry '%s'", path);

    if (!pathExists(path))
        return std::make_shared<Registry>(type);

    try {
        return read(settings, path, readFile(path), type);
    } catch (Error & e) {
        warn("cannot read flake registry '%s': %s", path, e.what());
        return std::make_shared<Registry>(type);
    }
}

std::shared_ptr<Registry>
Registry::read(const Settings & settings, std::string_view whence, std::string_view jsonStr, RegistryType type)
{
    auto registry = std::make_shared<Registry>(type);

    try {

        auto json = nlohmann::json::parse(jsonStr);

        auto version = json.value("version", 0);

        if (version == 2) {
            for (auto & i : json["flakes"]) {
                auto toAttrs = jsonToAttrs(i["to"]);
                Attrs extraAttrs;
                auto j = toAttrs.find("dir");
                if (j != toAttrs.end()) {
                    extraAttrs.insert(*j);
                    toAttrs.erase(j);
                }
                auto exact = i.find("exact");
                registry->entries.push_back(
                    Entry{
                        .from = Input::fromAttrs(settings, jsonToAttrs(i["from"])),
                        .to = Input::fromAttrs(settings, std::move(toAttrs)),
                        .extraAttrs = extraAttrs,
                        .exact = exact != i.end() && exact.value()});
            }
        }

        else
            warn("flake registry '%s' has unsupported version %d", whence, version);

    } catch (nlohmann::json::exception & e) {
        warn("cannot parse flake registry '%s': %s", whence, e.what());
    }

    return registry;
}

void Registry::write(const Path & path)
{
    nlohmann::json arr;
    for (auto & entry : entries) {
        nlohmann::json obj;
        obj["from"] = attrsToJSON(entry.from.toAttrs());
        obj["to"] = attrsToJSON(entry.to.toAttrs());
        if (!entry.extraAttrs.empty())
            obj["to"].update(attrsToJSON(entry.extraAttrs));
        if (entry.exact)
            obj["exact"] = true;
        arr.emplace_back(std::move(obj));
    }

    nlohmann::json json;
    json["version"] = 2;
    json["flakes"] = std::move(arr);

    createDirs(dirOf(path));
    writeFile(path, json.dump(2));
}

void Registry::add(const Input & from, const Input & to, const Attrs & extraAttrs)
{
    entries.emplace_back(Entry{.from = from, .to = to, .extraAttrs = extraAttrs});
}

void Registry::remove(const Input & input)
{
    entries.erase(
        std::remove_if(entries.begin(), entries.end(), [&](const Entry & entry) { return entry.from == input; }),
        entries.end());
}

static Path getSystemRegistryPath()
{
    return settings.nixConfDir + "/registry.json";
}

static std::shared_ptr<Registry> getSystemRegistry(const Settings & settings)
{
    static auto systemRegistry = Registry::read(settings, getSystemRegistryPath(), Registry::System);
    return systemRegistry;
}

Path getUserRegistryPath()
{
    return getConfigDir() + "/registry.json";
}

std::shared_ptr<Registry> getUserRegistry(const Settings & settings)
{
    static auto userRegistry = Registry::read(settings, getUserRegistryPath(), Registry::User);
    return userRegistry;
}

std::shared_ptr<Registry> getCustomRegistry(const Settings & settings, const Path & p)
{
    static auto customRegistry = Registry::read(settings, p, Registry::Custom);
    return customRegistry;
}

std::shared_ptr<Registry> getFlagRegistry()
{
    static auto flagRegistry = std::make_shared<Registry>(Registry::Flag);
    return flagRegistry;
}

void overrideRegistry(const Input & from, const Input & to, const Attrs & extraAttrs)
{
    getFlagRegistry()->add(from, to, extraAttrs);
}

static std::shared_ptr<Registry> getGlobalRegistry(const Settings & settings, ref<Store> store)
{
    static auto reg = [&]() {
        try {
            auto path = settings.flakeRegistry.get();
            if (path == "") {
                return std::make_shared<Registry>(Registry::Global); // empty registry
            }

            if (!isAbsolute(path)) {
                auto storePath = downloadFile(store, settings, path, "flake-registry.json").storePath;
                if (auto store2 = store.dynamic_pointer_cast<LocalFSStore>())
                    store2->addPermRoot(storePath, getCacheDir() + "/flake-registry.json");
                path = store->toRealPath(storePath);
            }

            return Registry::read(settings, path, Registry::Global);
        } catch (Error & e) {
            warn(
                "cannot fetch global flake registry '%s', will use builtin fallback registry: %s",
                settings.flakeRegistry.get(),
                e.info().msg);
            // Use builtin registry as fallback
            return Registry::read(
                settings,
                "builtin flake registry",
#include "builtin-flake-registry.json.gen.hh"
                ,
                Registry::Global);
        }
    }();

    return reg;
}

Registries getRegistries(const Settings & settings, ref<Store> store)
{
    Registries registries;
    registries.push_back(getFlagRegistry());
    registries.push_back(getUserRegistry(settings));
    registries.push_back(getSystemRegistry(settings));
    registries.push_back(getGlobalRegistry(settings, store));
    return registries;
}

std::pair<Input, Attrs>
lookupInRegistries(const Settings & settings, ref<Store> store, const Input & _input, UseRegistries useRegistries)
{
    Attrs extraAttrs;
    int n = 0;
    Input input(_input);

    if (useRegistries == UseRegistries::No)
        return {input, extraAttrs};

restart:

    n++;
    if (n > 100)
        throw Error("cycle detected in flake registry for '%s'", input.to_string());

    for (auto & registry : getRegistries(settings, store)) {
        if (useRegistries == UseRegistries::Limited
            && !(registry->type == fetchers::Registry::Flag || registry->type == fetchers::Registry::Global))
            continue;
        // FIXME: O(n)
        for (auto & entry : registry->entries) {
            if (entry.exact) {
                if (entry.from == input) {
                    debug("resolved flakeref '%s' against registry %d exactly", input.to_string(), registry->type);
                    input = entry.to;
                    extraAttrs = entry.extraAttrs;
                    goto restart;
                }
            } else {
                if (entry.from.contains(input)) {
                    debug("resolved flakeref '%s' against registry %d", input.to_string(), registry->type);
                    input = entry.to.applyOverrides(
                        !entry.from.getRef() && input.getRef() ? input.getRef() : std::optional<std::string>(),
                        !entry.from.getRev() && input.getRev() ? input.getRev() : std::optional<Hash>());
                    extraAttrs = entry.extraAttrs;
                    goto restart;
                }
            }
        }
    }

    if (!input.isDirect())
        throw Error("cannot find flake '%s' in the flake registries", input.to_string());

    debug("looked up '%s' -> '%s'", _input.to_string(), input.to_string());

    return {input, extraAttrs};
}

} // namespace nix::fetchers
