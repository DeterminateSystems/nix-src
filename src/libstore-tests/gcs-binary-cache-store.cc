#include "nix/store/gcs-binary-cache-store.hh"
#include "nix/store/store-open.hh"

#include <gtest/gtest.h>

namespace nix {

TEST(GCSBinaryCacheStore, resolveSchemesAndParameters)
{
    for (auto scheme : {"gs", "gcs"}) {
        auto config =
            resolveStoreConfig(StoreReference::parse(std::string(scheme) + "://my-bucket/cache/?priority=10"));
        EXPECT_EQ(config->getReference().render(false), "gs://my-bucket/cache");
        EXPECT_EQ(config->priority.get(), 10);
        EXPECT_EQ(config->getReference().params.at("priority"), "10");
    }
}

TEST(GCSBinaryCacheStore, bucketRoot)
{
    for (auto uri : {"gs://my-bucket", "gs://my-bucket/"}) {
        GCSBinaryCacheStoreConfig config{parseURL(uri), {}};
        EXPECT_EQ(config.objectName("nix-cache-info"), "nix-cache-info");
        EXPECT_EQ(config.objectName("nar/hash.nar.zst"), "nar/hash.nar.zst");
    }
}

TEST(GCSBinaryCacheStore, prefixAndEscaping)
{
    GCSBinaryCacheStoreConfig config{parseURL("gcs://my-bucket/team%20cache/nested/"), {}};
    EXPECT_EQ(config.objectName("nar/hash.nar.zst"), "team cache/nested/nar/hash.nar.zst");
    EXPECT_EQ(config.objectName("log/a%2Bb%20c"), "team cache/nested/log/a+b c");
    EXPECT_EQ(config.getReference().render(false), "gs://my-bucket/team%20cache/nested");
}

TEST(GCSBinaryCacheStore, rejectInvalidStoreURLs)
{
    for (auto uri :
         {"gs:///prefix",
          "gs://user@bucket",
          "gs://bucket:443",
          "gs://bucket/cache#fragment",
          "https://bucket",
          "gs://bucket/../cache"})
        EXPECT_THROW(GCSBinaryCacheStoreConfig(parseURL(uri), {}), UsageError) << uri;
}

TEST(GCSBinaryCacheStore, rejectObjectURLsOutsideCache)
{
    GCSBinaryCacheStoreConfig config{parseURL("gs://my-bucket/cache"), {}};
    for (auto path :
         {"",
          "../outside",
          "/outside",
          "//other-bucket/cache/nar",
          "gs://other-bucket/cache/nar",
          "https://example.com/nar",
          "nar?query=1",
          "nar#fragment"})
        EXPECT_THROW(config.objectName(path), Error) << path;
}

} // namespace nix
