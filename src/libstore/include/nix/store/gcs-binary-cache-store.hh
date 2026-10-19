#pragma once
///@file

#include "nix/store/binary-cache-store.hh"
#include "nix/util/url.hh"

namespace nix {

struct GCSBinaryCacheStoreConfig : std::enable_shared_from_this<GCSBinaryCacheStoreConfig>, BinaryCacheStoreConfig
{
private:
    void anchor() override;

public:
    GCSBinaryCacheStoreConfig(const Params & params)
        : StoreConfig(params, FilePathType::Unix)
        , BinaryCacheStoreConfig(params)
    {
    }

    GCSBinaryCacheStoreConfig(ParsedURL cacheUri, const Params & params);

    ParsedURL cacheUri;

    /** Resolve a cache-relative URL to an object in this bucket and prefix. */
    std::string objectName(std::string_view path) const;

    static const std::string name()
    {
        return "GCS Binary Cache Store";
    }

    static StringSet uriSchemes();
    static std::string doc();
    StoreReference getReference() const override;
    ref<Store> openStore() const override;
};

} // namespace nix
