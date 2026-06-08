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

    /**
     * Probe `path` against the cached Bloom filter for `uri`.
     *
     * Returns `std::nullopt` if there is no Bloom filter cached for this
     * cache, or the cached one is stale (older than the negative TTL) — the
     * caller should (re)fetch and try again. Otherwise returns whether the
     * filter says the path is *possibly present* (`true`) or *definitely not
     * present* (`false`).
     *
     * The filter parameters (`k`, `mBits`) and the bits are read from the
     * same stored blob in a single transaction, so they cannot drift.
     */
    virtual std::optional<bool> probeBloomFilter(const std::string & uri, const StorePath & path) = 0;

    /**
     * Store a freshly fetched Bloom filter blob (the full response body:
     * header + bit array).
     */
    virtual void
    upsertBloomFilter(const std::string & uri, const std::string & etag, std::span<const std::byte> blob) = 0;

    /**
     * Refresh the timestamp (and optionally the etag) of an existing Bloom filter
     * after a successful conditional GET returned 304 Not Modified.
     */
    virtual void touchBloomFilter(const std::string & uri, const std::string & etag) = 0;

    /**
     * Return the etag of the currently cached Bloom filter for `uri`
     * (regardless of its age), or nullopt if none is cached or it has no
     * etag. Used to send `If-None-Match` when refetching.
     */
    virtual std::optional<std::string> getBloomFilterETag(const std::string & uri) = 0;

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
