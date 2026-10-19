#include "nix/store/gcs-binary-cache-store.hh"
#include "nix/store/config.hh"
#include "nix/store/globals.hh"
#include "nix/store/nar-info-disk-cache.hh"
#include "nix/store/store-registration.hh"
#include "nix/util/signals.hh"

#if NIX_WITH_GCS
#  include <google/cloud/credentials.h>
#  include <google/cloud/common_options.h>
#  include <google/cloud/storage/client.h>
#endif

#include <array>

namespace nix {

void GCSBinaryCacheStoreConfig::anchor() {}

GCSBinaryCacheStoreConfig::GCSBinaryCacheStoreConfig(ParsedURL uri, const Params & params)
    : StoreConfig(params, FilePathType::Unix)
    , BinaryCacheStoreConfig(params)
    , cacheUri(std::move(uri))
{
    if (!uriSchemes().contains(cacheUri.scheme) || !cacheUri.authority || cacheUri.authority->host.empty()
        || cacheUri.authority->user || cacheUri.authority->password || cacheUri.authority->port
        || cacheUri.authority->hostType != ParsedURL::Authority::HostType::Name || !cacheUri.fragment.empty())
        throw UsageError("GCS binary cache requires a URL of the form 'gs://bucket[/prefix]'");

    // Both spellings identify the same cache, including in the narinfo cache.
    cacheUri.scheme = "gs";
    cacheUri.query.clear();
    while (!cacheUri.path.empty() && cacheUri.path.back().empty())
        cacheUri.path.pop_back();
    for (auto & segment : cacheUri.path)
        if (segment == "." || segment == "..")
            throw UsageError("GCS binary cache prefix must not contain '.' or '..' segments");
}

StringSet GCSBinaryCacheStoreConfig::uriSchemes()
{
    return {"gs", "gcs"};
}

StoreReference GCSBinaryCacheStoreConfig::getReference() const
{
    return {
        .variant =
            StoreReference::Specified{
                .scheme = cacheUri.scheme,
                .authority = cacheUri.renderAuthorityAndPath(),
            },
        .params = getQueryParams(),
    };
}

std::string GCSBinaryCacheStoreConfig::objectName(std::string_view path) const
{
    auto base = cacheUri;
    if (base.path.empty())
        base.path.push_back("");
    base.path.push_back("");
    auto resolved = parseURLRelative(path, base);
    auto prefix = base.renderPath();
    auto name = resolved.renderPath();
    if (path.empty() || resolved.scheme != base.scheme || resolved.authority != base.authority
        || !resolved.query.empty() || !resolved.fragment.empty() || !name.starts_with(prefix)
        || name.size() == prefix.size())
        throw Error("invalid object URL '%s' in GCS binary cache '%s'", path, cacheUri);
    // The initial slash separates the bucket from the object name.
    return name.substr(1);
}

std::string GCSBinaryCacheStoreConfig::doc()
{
    return
#include "gcs-binary-cache-store.md"
        ;
}

#if NIX_WITH_GCS

namespace gcs = google::cloud::storage;

class GCSRetryPolicy : public gcs::LimitedErrorCountRetryPolicy
{
public:
    GCSRetryPolicy()
        : LimitedErrorCountRetryPolicy(3)
    {
    }

    std::unique_ptr<gcs::RetryPolicy> clone() const override;

    bool OnFailure(const google::cloud::Status & status) override
    {
        return !isInterrupted() && LimitedErrorCountRetryPolicy::OnFailure(status);
    }

    bool IsExhausted() const override
    {
        return isInterrupted() || LimitedErrorCountRetryPolicy::IsExhausted();
    }
};

std::unique_ptr<gcs::RetryPolicy> GCSRetryPolicy::clone() const
{
    return std::make_unique<GCSRetryPolicy>();
}

struct GCSBinaryCacheStore : virtual BinaryCacheStore
{
private:
    void anchor() override;

    ref<GCSBinaryCacheStoreConfig> config;
    // A long-lived client shares and refreshes ADC credentials across requests.
    gcs::Client client{
        google::cloud::Options{}
            .set<google::cloud::UnifiedCredentialsOption>(google::cloud::MakeGoogleDefaultCredentials())
            .set<gcs::RetryPolicyOption>(std::make_shared<GCSRetryPolicy>())
            // Keep retry sleeps short so cancellation is noticed promptly.
            .set<gcs::BackoffPolicyOption>(
                gcs::ExponentialBackoffPolicy(std::chrono::milliseconds(100), std::chrono::seconds(1), 2.0).clone())};

public:
    explicit GCSBinaryCacheStore(ref<GCSBinaryCacheStoreConfig> config)
        : Store{*config}
        , BinaryCacheStore{*config}
        , config{config}
    {
        diskCache = NarInfoDiskCache::get(settings.getNarInfoDiskCacheSettings(), {.useWAL = settings.useSQLiteWAL});
    }

    void init() override
    {
        auto cacheKey = config->getReference().render(false);
        if (auto info = diskCache->upToDateCacheExists(cacheKey))
            applyCacheInfoFields(info->fields);
        else {
            auto fields = parseNixCacheInfo();
            applyCacheInfoFields(fields);
            diskCache->createCache(cacheKey, config->storeDir, {.fields = std::move(fields)});
        }
    }

    bool fileExists(const std::string & path) override
    {
        checkInterrupt();
        auto result = client.GetObjectMetadata(config->cacheUri.authority->host, config->objectName(path));
        checkInterrupt();
        if (result)
            return true;
        if (result.status().code() == google::cloud::StatusCode::kNotFound)
            return false;
        throw Error("checking '%s' in GCS binary cache '%s': %s", path, config->cacheUri, result.status());
    }

    void getFile(const std::string & path, Sink & sink) override
    {
        checkInterrupt();
        auto stream = client.ReadObject(config->cacheUri.authority->host, config->objectName(path));
        checkInterrupt();
        if (stream.status().code() == google::cloud::StatusCode::kNotFound)
            throw NoSuchBinaryCacheFile("file '%s' does not exist in GCS binary cache '%s'", path, config->cacheUri);

        std::array<char, 64 * 1024> buffer;
        while (stream) {
            checkInterrupt();
            stream.read(buffer.data(), buffer.size());
            checkInterrupt();
            if (stream.gcount())
                sink({buffer.data(), static_cast<size_t>(stream.gcount())});
        }
        if (!stream.status().ok())
            throw Error("reading '%s' from GCS binary cache '%s': %s", path, config->cacheUri, stream.status());
        if (stream.bad())
            throw Error("reading '%s' from GCS binary cache '%s' failed", path, config->cacheUri);
    }

    void upsertFile(
        const std::string & path, RestartableSource & source, const std::string & mimeType, uint64_t sizeHint) override
    {
        checkInterrupt();
        // WriteObject uses a resumable upload and buffers only a chunk at a time.
        auto stream =
            client.WriteObject(config->cacheUri.authority->host, config->objectName(path), gcs::ContentType(mimeType));
        try {
            checkInterrupt();
            uint64_t size = 0;
            LambdaSink sink{[&](std::string_view data) {
                checkInterrupt();
                stream.write(data.data(), data.size());
                checkInterrupt();
                if (!stream)
                    throw Error(
                        "writing '%s' to GCS binary cache '%s': %s", path, config->cacheUri, stream.last_status());
                size += data.size();
            }};
            source.drainInto(sink);
            if (size != sizeHint)
                throw Error("unexpected size for GCS upload '%s': expected %d, got %d", path, sizeHint, size);
            stream.Close();
            checkInterrupt();
            if (!stream.metadata())
                throw Error(
                    "writing '%s' to GCS binary cache '%s': %s", path, config->cacheUri, stream.metadata().status());
            // The SDK can return metadata successfully but set badbit when its checksum disagrees.
            if (!stream)
                throw Error("checksum mismatch uploading '%s' to GCS binary cache '%s'", path, config->cacheUri);
        } catch (...) {
            std::move(stream).Suspend();
            throw;
        }
    }

    std::optional<std::string> getBuildLogExact(const StorePath & path) override
    {
        return BinaryCacheStore::getFile("log/" + percentEncode(path.to_string()));
    }

    void addBuildLog(const StorePath & drvPath, std::string_view log) override
    {
        assert(drvPath.isDerivation());
        BinaryCacheStore::upsertFile(
            "log/" + percentEncode(drvPath.to_string()), std::string(log), "text/plain; charset=utf-8");
    }

    std::optional<TrustedFlag> isTrustedClient() override
    {
        return std::nullopt;
    }
};

void GCSBinaryCacheStore::anchor() {}

#endif

ref<Store> GCSBinaryCacheStoreConfig::openStore() const
{
#if NIX_WITH_GCS
    return make_ref<GCSBinaryCacheStore>(ref{std::const_pointer_cast<GCSBinaryCacheStoreConfig>(shared_from_this())});
#else
    throw Error("Nix was built without GCS support");
#endif
}

static RegisterStoreImplementation<GCSBinaryCacheStoreConfig> regGCSBinaryCacheStore;

} // namespace nix
