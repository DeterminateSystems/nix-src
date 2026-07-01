#include "nix/store/nar-info-disk-cache.hh"
#include "nix/store/bloom-filter.hh"
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
    fields    text not null
);

create table if not exists BloomFilters (
    cache     integer primary key not null,
    timestamp integer not null,
    etag      text,
    blob      blob not null, -- full filter body (header + bit array)
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
            insertMissingRealisation, queryRealisation, purgeCache, queryBloomFilterETag, insertBloomFilter,
            touchBloomFilter, queryFreshBloomFilter;
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
            "insert into BinaryCaches(url, timestamp, storeDir, fields) values (?1, ?2, ?3, ?4) on conflict (url) do update set timestamp = ?2, storeDir = ?3, fields = ?4 returning id;");

        state->queryCache.create(
            state->db, "select id, storeDir, fields from BinaryCaches where url = ? and timestamp > ?");

        state->queryBloomFilterETag.create(state->db, "select etag from BloomFilters where cache = ?");

        /* `>=` (not `>`) so a filter (re)fetched and stamped at the probe's
           reference time still counts as fresh; see `probeBloomFilter`. */
        state->queryFreshBloomFilter.create(
            state->db, "select rowid from BloomFilters where cache = ? and timestamp >= ?");

        state->insertBloomFilter.create(
            state->db, "insert or replace into BloomFilters(cache, timestamp, etag, blob) values (?, ?, ?, ?)");

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
                    .fields = nlohmann::json::parse(queryCache.getStr(2)).get<std::map<std::string, std::string>>(),
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
                           .apply(nlohmann::json(info.fields).dump()));
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

    std::optional<bool> probeBloomFilter(const std::string & uri, const StorePath & path) override
    {
        return retrySQLite<std::optional<bool>>([&]() -> std::optional<bool> {
            auto state(_state.lock());
            auto & cache(getCache(*state, uri));

            /* Use a fixed reference time (captured at the first probe in
               this process) rather than the moving wall clock. Otherwise a
               filter we (re)fetched and stamped a moment ago could already
               read as "stale" — especially under `--refresh`, which sets
               `ttlNegative` to 0 — and we'd re-fetch the shared filter on
               every query. With a fixed `startTime`, a filter stamped at or
               after `startTime` stays fresh for the rest of the process. */
            static auto startTime = time(nullptr);

            int64_t rowid;
            {
                auto q(state->queryFreshBloomFilter.use().apply(cache.info.id).apply(startTime - settings.ttlNegative));
                if (!q.next())
                    return std::nullopt; // no filter cached, or stale
                rowid = q.getInt(0);
            }

            sqlite3_blob * blob = nullptr;
            if (sqlite3_blob_open(state->db, "main", "BloomFilters", "blob", rowid, /*write=*/0, &blob) != SQLITE_OK)
                SQLiteError::throw_(state->db, "opening bloom-filter blob");
            Finally _closeBlob([&] {
                if (blob)
                    sqlite3_blob_close(blob);
            });

            /* Read and parse the header to get the filter parameters. */
            char header[bloomFilterHeaderLen];
            if (sqlite3_blob_bytes(blob) < (int) bloomFilterHeaderLen
                || sqlite3_blob_read(blob, header, bloomFilterHeaderLen, 0) != SQLITE_OK)
                return std::nullopt; // corrupt; treat as absent so we refetch
            auto params = parseBloomFilterHeader({header, bloomFilterHeaderLen});
            if (!params)
                return std::nullopt;

            bool allSet = true;
            forEachBloomBitPosition(path, params->k, params->mBits, [&](uint64_t pos) {
                if (!allSet)
                    return;
                unsigned char byte = 0;
                if (sqlite3_blob_read(blob, &byte, 1, (int) (bloomFilterHeaderLen + pos / 8)) != SQLITE_OK)
                    SQLiteError::throw_(state->db, "reading bloom-filter blob");
                if (!((byte >> (pos % 8)) & 1))
                    allSet = false;
            });
            return allSet;
        });
    }

    void upsertBloomFilter(const std::string & uri, const std::string & etag, std::span<const std::byte> blob) override
    {
        retrySQLite<void>([&]() {
            auto state(_state.lock());
            auto & cache(getCache(*state, uri));
            state->insertBloomFilter.use()
                .apply(cache.info.id)
                .apply(time(nullptr))
                .apply(etag, !etag.empty())
                .apply(reinterpret_cast<const unsigned char *>(blob.data()), blob.size())
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

    std::optional<std::string> getBloomFilterETag(const std::string & uri) override
    {
        return retrySQLite<std::optional<std::string>>([&]() -> std::optional<std::string> {
            auto state(_state.lock());
            auto & cache(getCache(*state, uri));
            auto q(state->queryBloomFilterETag.use().apply(cache.info.id));
            if (!q.next() || q.isNull(0))
                return std::nullopt;
            return q.getStr(0);
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
