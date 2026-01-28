#include "nix/fetchers/provenance.hh"
#include "nix/fetchers/attrs.hh"

#include <nlohmann/json.hpp>

namespace nix {

TreeProvenance::TreeProvenance(const fetchers::Input & input)
    : attrs(make_ref<nlohmann::json>([&]() {
        // Remove the narHash attribute from the provenance info, as it's redundant (it's already recorded in the store
        // path info).
        auto attrs2 = input.attrs;
        attrs2.erase("narHash");
        return fetchers::attrsToJSON(attrs2);
    }()))
{
}

nlohmann::json TreeProvenance::to_json() const
{
    return nlohmann::json{
        {"type", "tree"},
        {"attrs", *attrs},
    };
}

} // namespace nix
