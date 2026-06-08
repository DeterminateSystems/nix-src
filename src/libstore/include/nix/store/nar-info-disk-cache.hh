#pragma once
///@file

#include "nix/util/ref.hh"
#include "nix/store/nar-info.hh"
#include "nix/store/realisation.hh"

#include <span>

namespace nix {

struct SQLiteSettings;
struct NarInfoDiskCacheSettings;

struct NarInfoDiskCache
{
    using Settings = NarInfoDiskCacheSettings;

    const Settings & settings;

    NarInfoDiskCache(const Settings & settings)
        : settings(settings)
    {
    }

    typedef enum { oValid, oInvalid, oUnknown } Outcome;

    virtual ~NarInfoDiskCache() {}

    struct CacheInfo
    {
        int id = 0;
        bool wantMassQuery = false;
        int priority = 0;
        std::optional<std::string> bloomFilterUrl;
    };

    /**
     * Create or update the cached nix-cache-info for the binary cache at `uri`.
     * Note that `info.id` is ignored. This function returns the id of the cache entry.
     */
    virtual int createCache(const std::string & uri, const std::string & storeDir, const CacheInfo & info) = 0;

    virtual std::optional<CacheInfo> upToDateCacheExists(const std::string & uri) = 0;

    virtual std::pair<Outcome, std::shared_ptr<NarInfo>>
    lookupNarInfo(const std::string & uri, const std::string & hashPart) = 0;

    virtual void
    upsertNarInfo(const std::string & uri, const std::string & hashPart, std::shared_ptr<const ValidPathInfo> info) = 0;

    virtual void upsertRealisation(const std::string & uri, const Realisation & realisation) = 0;
    virtual void upsertAbsentRealisation(const std::string & uri, const DrvOutput & id) = 0;
    virtual std::pair<Outcome, std::shared_ptr<Realisation>>
    lookupRealisation(const std::string & uri, const DrvOutput & id) = 0;

    struct BloomFilterMeta
    {
        uint32_t k;
        uint64_t mBits;
        std::string etag;
        time_t timestamp;
    };

    /**
     * Return the metadata for a cached Bloom filter, or nullopt if none is cached.
     * Does not check the TTL; the caller decides whether to refresh.
     */
    virtual std::optional<BloomFilterMeta> lookupBloomFilter(const std::string & uri) = 0;

    /**
     * Store a freshly fetched Bloom filter blob (just the bit array, no header).
     */
    virtual void upsertBloomFilter(
        const std::string & uri,
        const std::string & etag,
        uint32_t k,
        uint64_t mBits,
        std::span<const std::byte> bits) = 0;

    /**
     * Refresh the timestamp (and optionally the etag) of an existing Bloom filter
     * after a successful conditional GET returned 304 Not Modified.
     */
    virtual void touchBloomFilter(const std::string & uri, const std::string & etag) = 0;

    /**
     * Probe `bitPositions` against the cached Bloom filter via random-access
     * blob reads. Returns true if every position has its bit set (i.e. the
     * Bloom filter says "possibly present"), false otherwise (definitely
     * not present, OR no filter is cached).
     */
    virtual bool probeBloomFilter(const std::string & uri, std::span<const uint64_t> bitPositions) = 0;

    /**
     * Return a singleton cache object that can be used concurrently by
     * multiple threads.
     *
     * @note the parameters are only used to initialize this the first time this is called.
     * In subsequent calls, these arguments are ignored.
     *
     * @todo Probably should instead create a memo table so multiple settings -> multiple instances,
     * but this is not yet a problem in practice.
     */
    static ref<NarInfoDiskCache> get(const Settings & settings, SQLiteSettings);

    static ref<NarInfoDiskCache> getTest(const Settings & settings, SQLiteSettings, std::filesystem::path dbPath);
};

} // namespace nix
