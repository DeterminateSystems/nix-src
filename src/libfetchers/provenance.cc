#include "nix/fetchers/provenance.hh"
#include "nix/fetchers/attrs.hh"
#include "nix/util/json-utils.hh"

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

Provenance::Register registerTreeProvenance("tree", [](nlohmann::json json) {
    auto & obj = getObject(json);
    auto & attrsJson = valueAt(obj, "attrs");
    return make_ref<TreeProvenance>(make_ref<nlohmann::json>(attrsJson));
});

FetchurlProvenance::FetchurlProvenance(std::string _url, bool sanitize)
    : url(std::move(_url))
{
    if (sanitize) {
        try {
            url = parseURL(url, true).renderSanitized();
        } catch (BadURL &) {
        }
    }
}

nlohmann::json FetchurlProvenance::to_json() const
{
    return nlohmann::json{
        {"type", "fetchurl"},
        {"url", url},
    };
}

Provenance::Register registerFetchurlProvenance("fetchurl", [](nlohmann::json json) {
    auto & obj = getObject(json);
    return make_ref<FetchurlProvenance>(getString(valueAt(obj, "url")), false);
});

} // namespace nix
