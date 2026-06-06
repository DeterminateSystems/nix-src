#include "nix/store/nar-info-disk-cache.hh"
#include "nix/util/users.hh"
#include "nix/util/sync.hh"
#include "nix/util/finally.hh"
#include "nix/store/sqlite.hh"
#include "nix/store/globals.hh"
#include "nix/store/provenance.hh"

#include <sqlite3.h>
#include <nlohmann/json.hpp>

#include "nix/util/strings.hh"

namespace nix {

static const char * schema = R"sql(

create table if not exists BinaryCaches (
    id        integer primary key autoincrement not null,
    url       text unique not null,
    timestamp integer not null,
    storeDir  text not null,
    wantMassQuery integer not null,
    priority  integer not null,
    bloomFilterUrl text -- NULL if the cache doesn't advertise a bloom filter
);

create table if not exists BloomFilters (
    cache     integer primary key not null,
    timestamp integer not null,
    etag      text,
    k         integer not null,
    mBits     integer not null,
    bits      blob not null,
    foreign key (cache) references BinaryCaches(id) on delete cascade
);

create table if not exists NARs (
    cache            integer not null,
    hashPart         text not null,
    namePart         text,
    url              text,
    compression      text,
    fileHash         text,
    fileSize         integer,
    narHash          text,
    narSize          integer,
    refs             text,
    deriver          text,
    sigs             text,
    ca               text,
    provenance       text,
    timestamp        integer not null,
    present          integer not null,
    primary key (cache, hashPart),
    foreign key (cache) references BinaryCaches(id) on delete cascade
);

create table if not exists Realisations (
    cache integer not null,
    outputId text not null,
    content blob, -- Json serialisation of the realisation, or null if the realisation is absent
    timestamp        integer not null,
    primary key (cache, outputId),
    foreign key (cache) references BinaryCaches(id) on delete cascade
);

create table if not exists LastPurge (
    dummy            text primary key,
    value            integer
);

)sql";

struct NarInfoDiskCacheImpl : NarInfoDiskCache
{
    /* How often to purge expired entries from the cache. */
    const int purgeInterval = 24 * 3600;

    struct Cache
    {
        std::string storeDir;
        CacheInfo info;
    };

    struct State
    {
        SQLite db;
        SQLiteStmt insertCache, queryCache, insertNAR, insertMissingNAR, queryNAR, insertRealisation,
            insertMissingRealisation, queryRealisation, purgeCache, queryBloomFilter, insertBloomFilter,
            touchBloomFilter, queryBloomFilterRowId;
        std::map<std::string, Cache> caches;
    };

    Sync<State> _state;

    NarInfoDiskCacheImpl(
        const Settings & settings,
        SQLiteSettings sqliteSettings,
        std::filesystem::path dbPath = getCacheDir() / "binary-cache-detsys-v2.sqlite")
        : NarInfoDiskCache{settings}
    {
        auto state(_state.lock());

        createDirs(dbPath.parent_path());

        state->db = SQLite(dbPath, SQLite::Settings{sqliteSettings});

        state->db.isCache();

        state->db.exec(schema);

        state->insertCache.create(
            state->db,
            "insert into BinaryCaches(url, timestamp, storeDir, wantMassQuery, priority, bloomFilterUrl) values (?1, ?2, ?3, ?4, ?5, ?6) on conflict (url) do update set timestamp = ?2, storeDir = ?3, wantMassQuery = ?4, priority = ?5, bloomFilterUrl = ?6 returning id;");

        state->queryCache.create(
            state->db,
            "select id, storeDir, wantMassQuery, priority, bloomFilterUrl from BinaryCaches where url = ? and timestamp > ?");

        state->queryBloomFilter.create(state->db, "select timestamp, etag, k, mBits from BloomFilters where cache = ?");

        state->queryBloomFilterRowId.create(state->db, "select rowid from BloomFilters where cache = ?");

        state->insertBloomFilter.create(
            state->db,
            "insert or replace into BloomFilters(cache, timestamp, etag, k, mBits, bits) values (?, ?, ?, ?, ?, ?)");

        state->touchBloomFilter.create(state->db, "update BloomFilters set timestamp = ?, etag = ? where cache = ?");

        state->insertNAR.create(
            state->db,
            "insert or replace into NARs(cache, hashPart, namePart, url, compression, fileHash, fileSize, narHash, "
            "narSize, refs, deriver, sigs, ca, provenance, timestamp, present) values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 1)");

        state->insertMissingNAR.create(
            state->db, "insert or replace into NARs(cache, hashPart, timestamp, present) values (?, ?, ?, 0)");

        state->queryNAR.create(
            state->db,
            "select present, namePart, url, compression, fileHash, fileSize, narHash, narSize, refs, deriver, sigs, ca, provenance from NARs where cache = ? and hashPart = ? and ((present = 0 and timestamp > ?) or (present = 1 and timestamp > ?))");

        state->insertRealisation.create(
            state->db,
            R"(
                insert or replace into Realisations(cache, outputId, content, timestamp)
                    values (?, ?, ?, ?)
            )");

        state->insertMissingRealisation.create(
            state->db,
            R"(
                insert or replace into Realisations(cache, outputId, timestamp)
                    values (?, ?, ?)
            )");

        state->queryRealisation.create(
            state->db,
            R"(
                select content from Realisations
                    where cache = ? and outputId = ?  and
                        ((content is null and timestamp > ?) or
                         (content is not null and timestamp > ?))
            )");

        /* Periodically purge expired entries from the database. */
        retrySQLite<void>([&]() {
            auto now = time(nullptr);

            SQLiteStmt queryLastPurge(state->db, "select value from LastPurge");
            auto queryLastPurge_(queryLastPurge.use());

            if (!queryLastPurge_.next() || queryLastPurge_.getInt(0) < now - purgeInterval) {
                SQLiteStmt(
                    state->db,
                    "delete from NARs where ((present = 0 and timestamp < ?) or (present = 1 and timestamp < ?))")
                    .use()
                    // Use a minimum TTL to prevent --refresh from
                    // nuking the entire disk cache.
                    .apply(now - std::max(settings.ttlNegative.get(), 3600U))
                    .apply(now - std::max(settings.ttlPositive.get(), 30 * 24 * 3600U))
                    .exec();

                debug("deleted %d entries from the NAR info disk cache", sqlite3_changes(state->db));

                SQLiteStmt(state->db, "insert or replace into LastPurge(dummy, value) values ('', ?)")
                    .use()
                    .apply(now)
                    .exec();
            }
        });
    }

    Cache & getCache(State & state, const std::string & uri)
    {
        auto i = state.caches.find(uri);
        if (i == state.caches.end())
            unreachable();
        return i->second;
    }

private:

    std::optional<Cache> queryCacheRaw(State & state, const std::string & uri)
    {
        auto i = state.caches.find(uri);
        if (i == state.caches.end()) {
            /* Important: always use int64_t even on 32 bit systems. Otherwise
               the the subtraction would promote time_t to unsigned if time_t is
               32 bit. */
            auto timestamp = static_cast<int64_t>(time(nullptr)) - static_cast<int64_t>(settings.ttlMeta.get());
            auto queryCache(state.queryCache.use().apply(uri).apply(timestamp));
            if (!queryCache.next())
                return std::nullopt;
            auto cache = Cache{
                .storeDir = queryCache.getStr(1),
                .info = {
                    .id = (int) queryCache.getInt(0),
                    .wantMassQuery = queryCache.getInt(2) != 0,
                    .priority = (int) queryCache.getInt(3),
                    .bloomFilterUrl = queryCache.isNull(4) ? std::nullopt : std::optional(queryCache.getStr(4)),
                }};
            state.caches.emplace(uri, cache);
        }
        return getCache(state, uri);
    }

public:
    int createCache(const std::string & uri, const std::string & storeDir, const CacheInfo & info) override
    {
        return retrySQLite<int>([&]() {
            auto state(_state.lock());
            SQLiteTxn txn(state->db);

            // To avoid the race, we have to check if maybe someone hasn't yet created
            // the cache for this URI in the meantime.
            auto cache(queryCacheRaw(*state, uri));

            if (cache)
                return cache->info.id;

            Cache ret{.storeDir = storeDir, .info = info};

            {
                auto r(state->insertCache.use()
                           .apply(uri)
                           .apply(time(nullptr))
                           .apply(storeDir)
                           .apply(info.wantMassQuery)
                           .apply(info.priority)
                           .apply(info.bloomFilterUrl.value_or(""), info.bloomFilterUrl.has_value()));
                if (!r.next()) {
                    unreachable();
                }
                ret.info.id = (int) r.getInt(0);
            }

            state->caches[uri] = ret;

            txn.commit();
            return ret.info.id;
        });
    }

    std::optional<CacheInfo> upToDateCacheExists(const std::string & uri) override
    {
        return retrySQLite<std::optional<CacheInfo>>([&]() -> std::optional<CacheInfo> {
            auto state(_state.lock());
            auto cache(queryCacheRaw(*state, uri));
            if (!cache)
                return std::nullopt;
            return cache->info;
        });
    }

    std::pair<Outcome, std::shared_ptr<NarInfo>>
    lookupNarInfo(const std::string & uri, const std::string & hashPart) override
    {
        return retrySQLite<std::pair<Outcome, std::shared_ptr<NarInfo>>>(
            [&]() -> std::pair<Outcome, std::shared_ptr<NarInfo>> {
                auto state(_state.lock());

                auto & cache(getCache(*state, uri));

                auto now = time(nullptr);

                auto queryNAR(state->queryNAR.use()
                                  .apply(cache.info.id)
                                  .apply(hashPart)
                                  .apply(now - settings.ttlNegative)
                                  .apply(now - settings.ttlPositive));

                if (!queryNAR.next())
                    return {oUnknown, 0};

                if (!queryNAR.getInt(0))
                    return {oInvalid, 0};

                auto namePart = queryNAR.getStr(1);
                auto narInfo = make_ref<NarInfo>(
                    cache.storeDir, StorePath(hashPart + "-" + namePart), Hash::parseAnyPrefixed(queryNAR.getStr(6)));
                narInfo->url = queryNAR.getStr(2);
                narInfo->compression = queryNAR.getStr(3);
                if (!queryNAR.isNull(4))
                    narInfo->fileHash = Hash::parseAnyPrefixed(queryNAR.getStr(4));
                narInfo->fileSize = queryNAR.getInt(5);
                narInfo->narSize = queryNAR.getInt(7);
                for (auto & r : tokenizeString<Strings>(queryNAR.getStr(8), " "))
                    narInfo->references.insert(StorePath(r));
                if (!queryNAR.isNull(9))
                    narInfo->deriver = StorePath(queryNAR.getStr(9));
                for (auto & sig : tokenizeString<Strings>(queryNAR.getStr(10), " "))
                    narInfo->sigs.insert(Signature::parse(sig));
                narInfo->ca = ContentAddress::parseOpt(queryNAR.getStr(11));
                if (experimentalFeatureSettings.isEnabled(Xp::Provenance) && !queryNAR.isNull(12))
                    narInfo->provenance = Provenance::from_json_str_optional(queryNAR.getStr(12));

                return {oValid, narInfo};
            });
    }

    std::pair<Outcome, std::shared_ptr<Realisation>>
    lookupRealisation(const std::string & uri, const DrvOutput & id) override
    {
        return retrySQLite<std::pair<Outcome, std::shared_ptr<Realisation>>>(
            [&]() -> std::pair<Outcome, std::shared_ptr<Realisation>> {
                auto state(_state.lock());

                auto & cache(getCache(*state, uri));

                auto now = time(nullptr);

                auto queryRealisation(state->queryRealisation.use()
                                          .apply(cache.info.id)
                                          .apply(id.to_string())
                                          .apply(now - settings.ttlNegative)
                                          .apply(now - settings.ttlPositive));

                if (!queryRealisation.next())
                    return {oUnknown, 0};

                if (queryRealisation.isNull(0))
                    return {oInvalid, 0};

                try {
                    return {
                        oValid,
                        std::make_shared<Realisation>(nlohmann::json::parse(queryRealisation.getStr(0))),
                    };
                } catch (Error & e) {
                    e.addTrace({}, "while parsing the local disk cache");
                    throw;
                }
            });
    }

    void upsertNarInfo(
        const std::string & uri, const std::string & hashPart, std::shared_ptr<const ValidPathInfo> info) override
    {
        retrySQLite<void>([&]() {
            auto state(_state.lock());

            auto & cache(getCache(*state, uri));

            if (info) {

                auto narInfo = std::dynamic_pointer_cast<const NarInfo>(info);

                // assert(hashPart == storePathToHash(info->path));

                state->insertNAR.use()
                    .apply(cache.info.id)
                    .apply(hashPart)
                    .apply(std::string(info->path.name()))
                    .apply(narInfo ? narInfo->url : "", narInfo != 0)
                    .apply(narInfo ? narInfo->compression : "", narInfo != 0)
                    .apply(
                        narInfo && narInfo->fileHash ? narInfo->fileHash->to_string(HashFormat::Nix32, true) : "",
                        narInfo && narInfo->fileHash)
                    .apply(narInfo ? narInfo->fileSize : 0, narInfo != 0 && narInfo->fileSize)
                    .apply(info->narHash.to_string(HashFormat::Nix32, true))
                    .apply(info->narSize)
                    .apply(concatStringsSep(" ", info->shortRefs()))
                    .apply(info->deriver ? std::string(info->deriver->to_string()) : "", (bool) info->deriver)
                    .apply(concatStringsSep(" ", Signature::toStrings(info->sigs)))
                    .apply(renderContentAddress(info->ca))
                    .apply(
                        info->provenance ? info->provenance->to_json_str() : "",
                        experimentalFeatureSettings.isEnabled(Xp::Provenance) && info->provenance)
                    .apply(time(nullptr))
                    .exec();

            } else {
                state->insertMissingNAR.use().apply(cache.info.id).apply(hashPart).apply(time(nullptr)).exec();
            }
        });
    }

    void upsertRealisation(const std::string & uri, const Realisation & realisation) override
    {
        retrySQLite<void>([&]() {
            auto state(_state.lock());

            auto & cache(getCache(*state, uri));

            state->insertRealisation.use()
                .apply(cache.info.id)
                .apply(realisation.id.to_string())
                .apply(static_cast<nlohmann::json>(realisation).dump())
                .apply(time(nullptr))
                .exec();
        });
    }

    virtual void upsertAbsentRealisation(const std::string & uri, const DrvOutput & id) override
    {
        retrySQLite<void>([&]() {
            auto state(_state.lock());

            auto & cache(getCache(*state, uri));
            state->insertMissingRealisation.use()
                .apply(cache.info.id)
                .apply(id.to_string())
                .apply(time(nullptr))
                .exec();
        });
    }

    std::optional<BloomFilterMeta> lookupBloomFilter(const std::string & uri) override
    {
        return retrySQLite<std::optional<BloomFilterMeta>>([&]() -> std::optional<BloomFilterMeta> {
            auto state(_state.lock());
            auto & cache(getCache(*state, uri));
            auto q(state->queryBloomFilter.use().apply(cache.info.id));
            if (!q.next())
                return std::nullopt;
            return BloomFilterMeta{
                .k = (uint32_t) q.getInt(2),
                .mBits = (uint64_t) q.getInt(3),
                .etag = q.isNull(1) ? std::string{} : q.getStr(1),
                .timestamp = (time_t) q.getInt(0),
            };
        });
    }

    void upsertBloomFilter(
        const std::string & uri,
        const std::string & etag,
        uint32_t k,
        uint64_t mBits,
        std::span<const std::byte> bits) override
    {
        retrySQLite<void>([&]() {
            auto state(_state.lock());
            auto & cache(getCache(*state, uri));
            state->insertBloomFilter.use()
                .apply(cache.info.id)
                .apply(time(nullptr))
                .apply(etag, !etag.empty())
                .apply((uint64_t) k)
                .apply(mBits)
                .apply(reinterpret_cast<const unsigned char *>(bits.data()), bits.size())
                .exec();
        });
    }

    void touchBloomFilter(const std::string & uri, const std::string & etag) override
    {
        retrySQLite<void>([&]() {
            auto state(_state.lock());
            auto & cache(getCache(*state, uri));
            state->touchBloomFilter.use().apply(time(nullptr)).apply(etag, !etag.empty()).apply(cache.info.id).exec();
        });
    }

    bool probeBloomFilter(const std::string & uri, std::span<const uint64_t> bitPositions) override
    {
        return retrySQLite<bool>([&]() -> bool {
            auto state(_state.lock());
            auto & cache(getCache(*state, uri));

            int64_t rowid;
            {
                auto q(state->queryBloomFilterRowId.use().apply(cache.info.id));
                if (!q.next())
                    return false; // no cached filter
                rowid = q.getInt(0);
            }

            sqlite3_blob * blob = nullptr;
            if (sqlite3_blob_open(state->db, "main", "BloomFilters", "bits", rowid, /*write=*/0, &blob) != SQLITE_OK)
                SQLiteError::throw_(state->db, "opening bloom-filter blob");
            Finally _closeBlob([&] {
                if (blob)
                    sqlite3_blob_close(blob);
            });

            for (auto pos : bitPositions) {
                unsigned char byte = 0;
                int rc = sqlite3_blob_read(blob, &byte, 1, (int) (pos / 8));
                if (rc != SQLITE_OK)
                    SQLiteError::throw_(state->db, "reading bloom-filter blob");
                if (!((byte >> (pos % 8)) & 1))
                    return false;
            }
            return true;
        });
    }
};

ref<NarInfoDiskCache> NarInfoDiskCache::get(const Settings & settings, SQLiteSettings sqliteSettings)
{
    static ref<NarInfoDiskCache> cache = make_ref<NarInfoDiskCacheImpl>(settings, sqliteSettings);
    return cache;
}

ref<NarInfoDiskCache>
NarInfoDiskCache::getTest(const Settings & settings, SQLiteSettings sqliteSettings, std::filesystem::path dbPath)
{
    return make_ref<NarInfoDiskCacheImpl>(settings, sqliteSettings, dbPath);
}

} // namespace nix
