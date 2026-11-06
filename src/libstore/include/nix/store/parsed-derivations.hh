#pragma once
///@file

#include <nlohmann/json.hpp>

#include "nix/util/types.hh"
#include "nix/store/path.hh"

namespace nix {

class Store;
template<typename Input>
struct DerivationOptions;
struct DerivationOutput;

typedef std::map<std::string, DerivationOutput> DerivationOutputs;

struct StructuredAttrs
{
    static constexpr std::string_view envVarName{"__json"};

    /**
     * The structured attributes. This is always a JSON object; use
     * `object()` to access it as a map. It's stored as a
     * `nlohmann::json` rather than an `object_t` so that it can be
     * serialised without copying.
     */
    nlohmann::json structuredAttrs = nlohmann::json::object();

    bool operator==(const StructuredAttrs &) const = default;

    const nlohmann::json::object_t & object() const
    {
        return structuredAttrs.get_ref<const nlohmann::json::object_t &>();
    }

    nlohmann::json::object_t & object()
    {
        return structuredAttrs.get_ref<nlohmann::json::object_t &>();
    }

    /**
     * Construct from a JSON value, which must be an object.
     *
     * @throws Error if `json` is not an object.
     */
    static StructuredAttrs fromJSON(nlohmann::json json);

    /**
     * Unconditionally parse from a JSON string. Used by `tryExtract`.
     */
    static StructuredAttrs parse(std::string_view encoded);

    /**
     * Like `tryParse`, but removes the env var which encoded the structured
     * attrs from the map if one is found.
     */
    static std::optional<StructuredAttrs> tryExtract(StringPairs & env);

    /**
     * Opposite of `tryParse`, at least if one makes a map from this
     * single key-value PR.
     */
    std::pair<std::string_view, std::string> unparse() const;

    /**
     * Ensures that the structured attrs "env var" is not in used, so we
     * are free to use it instead.
     */
    static void checkKeyNotInUse(const StringPairs & env);

    nlohmann::json::object_t prepareStructuredAttrs(
        Store & store,
        const DerivationOptions<StorePath> & drvOptions,
        const StorePathSet & inputPaths,
        const DerivationOutputs & outputs) const;

    /**
     * As a convenience to bash scripts, write a shell file that
     * maps all attributes that are representable in bash -
     * namely, strings, integers, nulls, Booleans, and arrays and
     * objects consisting entirely of those values. (So nested
     * arrays or objects are not supported.)
     *
     * @param prepared This should be the result of
     * `prepareStructuredAttrs`, *not* the original `structuredAttrs`
     * field.
     */
    static std::string writeShell(const nlohmann::json::object_t & prepared);
};

} // namespace nix
