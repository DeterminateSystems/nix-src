#include <gtest/gtest.h>

#include "nix/store/gc-store.hh"
#include "nix/store/local-store.hh"
#include "nix/store/store-open.hh"
#include "nix/util/file-system.hh"
#include "nix/util/serialise.hh"

// Needed for template specialisations. This is not good! When we
// overhaul how store configs work, this should be fixed.
#include "nix/util/args.hh"
#include "nix/util/config-impl.hh"
#include "nix/util/abstract-setting-to-json.hh"

namespace nix {

TEST(LocalStore, storeDir_absolutePath)
{
    std::filesystem::path storeDir =
#ifdef _WIN32
        "C:\\";
#else
        "/";
#endif
    storeDir /= "nix";
    storeDir /= "store";
    LocalStoreConfig config{"", {{"store", storeDir.string()}}};
    EXPECT_EQ(config.storeDir, storeDir.string());
}

TEST(LocalStore, storeDir_relativePath_rejected)
{
    EXPECT_THROW(LocalStoreConfig("", {{"store", (std::filesystem::path{"nix"} / "store").string()}}), UsageError);
}

TEST(LocalStore, storeDir_empty_rejected)
{
    EXPECT_THROW(LocalStoreConfig("", {{"store", ""}}), UsageError);
}

TEST(LocalStore, constructConfig_rootQueryParam)
{
#ifdef _WIN32
    constexpr std::string_view root = "C:\\foo\\bar";
#else
    constexpr std::string_view root = "/foo/bar";
#endif
    LocalStoreConfig config{
        "",
        {
            {
                "root",
                std::string{root},
            },
        },
    };

    EXPECT_EQ(config.rootDir.get(), std::optional<AbsolutePath>{std::string{root}});
}

TEST(LocalStore, constructConfig_rootPath)
{
#ifdef _WIN32
    constexpr std::string_view root = "C:\\foo\\bar";
#else
    constexpr std::string_view root = "/foo/bar";
#endif
    LocalStoreConfig config{std::string{root}, {}};

    EXPECT_EQ(config.rootDir.get(), std::optional<AbsolutePath>{std::string{root}});
}

TEST(LocalStore, constructConfig_to_string)
{
    LocalStoreConfig config{"", {}};
    EXPECT_EQ(config.getReference().to_string(), "local");
}

#ifndef _WIN32

/**
 * The garbage collector must not trust a path info cache entry that
 * says that a path is invalid, since another process may have
 * registered the path (and a root for it) in the meantime.
 */
TEST(LocalStore, collectGarbage_ignoresStalePathInfoCache)
{
    auto tmpRoot = createTempDir();
    AutoDelete delTmpRoot(tmpRoot, true);

    auto uri = fmt("local?root=%s", tmpRoot.string());

    std::string contents = "hello";
    std::string name = "stale-path-info-cache";

    /* The store that will do the garbage collection. */
    auto gcStore = openStore(uri).dynamic_pointer_cast<LocalStore>();
    ASSERT_TRUE(gcStore);

    auto path = gcStore->makeFixedOutputPath(
        name,
        FixedOutputInfo{
            .method = FileIngestionMethod::Flat,
            .hash = hashString(HashAlgorithm::SHA256, contents),
            .references = {},
        });

    /* Make `gcStore` cache the fact that `path` is invalid. */
    EXPECT_THROW(gcStore->queryPathInfo(path), InvalidPath);

    /* Add `path` and a root for it using another store instance,
       which has its own path info cache, like another process
       would. Close it afterwards so that `path` is not a temporary
       root. */
    {
        auto otherStore = openStore(uri).dynamic_pointer_cast<LocalStore>();
        ASSERT_TRUE(otherStore);
        StringSource source{contents};
        auto added = static_cast<Store &>(*otherStore)
                         .addToStoreFromDump(
                             source,
                             name,
                             FileSerialisationMethod::Flat,
                             ContentAddressMethod::Raw::Flat,
                             HashAlgorithm::SHA256);
        ASSERT_EQ(added, path);
        otherStore->addPermRoot(path, tmpRoot / "root");
    }

    /* Without this, the test wouldn't test anything. */
    ASSERT_FALSE(gcStore->isValidPath(path));

    GCOptions options;
    GCResults results;
    gcStore->collectGarbage(options, results);

    EXPECT_FALSE(results.paths.contains(gcStore->printStorePath(path)));
    EXPECT_TRUE(pathExists(gcStore->toRealPath(path)));
    EXPECT_TRUE(openStore(uri)->isValidPath(path));
}

#endif

} // namespace nix
