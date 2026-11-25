#include <gtest/gtest.h>

#include "nix/fetchers/filtering-source-accessor.hh"
#include "nix/util/memory-source-accessor.hh"

namespace nix {

static ref<AllowListSourceAccessor> makeAllowList(
    ref<SourceAccessor> next,
    const std::set<CanonPath> & allowedPrefixes,
    const std::unordered_set<CanonPath> & allowedPaths = {})
{
    return AllowListSourceAccessor::create(next, allowedPrefixes, allowedPaths, [](const CanonPath & path) {
        return RestrictedPathError("access to '%s' is not allowed", path);
    });
}

TEST(AllowListSourceAccessor, isAllowed)
{
    auto accessor = makeAllowList(
        makeEmptySourceAccessor(),
        {
            CanonPath("foo/bar"),
            CanonPath("foo!"),
            CanonPath("xyzzy"),
            CanonPath("a/b/c"),
        });

    ASSERT_TRUE(accessor->isAllowed(CanonPath("foo/bar")));
    ASSERT_TRUE(accessor->isAllowed(CanonPath("foo/bar/bla")));
    ASSERT_TRUE(accessor->isAllowed(CanonPath("foo")));
    ASSERT_FALSE(accessor->isAllowed(CanonPath("foo/baz")));
    ASSERT_FALSE(accessor->isAllowed(CanonPath("foo/barx")));
    ASSERT_FALSE(accessor->isAllowed(CanonPath("bar")));
    ASSERT_FALSE(accessor->isAllowed(CanonPath("bar/a")));
    ASSERT_TRUE(accessor->isAllowed(CanonPath("a")));
    ASSERT_TRUE(accessor->isAllowed(CanonPath("a/b")));
    ASSERT_TRUE(accessor->isAllowed(CanonPath("a/b/c")));
    ASSERT_TRUE(accessor->isAllowed(CanonPath("a/b/c/d")));
    ASSERT_TRUE(accessor->isAllowed(CanonPath("a/b/c/d/e")));
    ASSERT_FALSE(accessor->isAllowed(CanonPath("a/b/a")));
    ASSERT_FALSE(accessor->isAllowed(CanonPath("a/b/d")));
    ASSERT_FALSE(accessor->isAllowed(CanonPath("aaa")));
    ASSERT_FALSE(accessor->isAllowed(CanonPath("zzz")));
    ASSERT_TRUE(accessor->isAllowed(CanonPath("xyzzy/a")));
    ASSERT_TRUE(accessor->isAllowed(CanonPath::root));
}

TEST(AllowListSourceAccessor, empty)
{
    auto accessor = makeAllowList(makeEmptySourceAccessor(), {});
    ASSERT_FALSE(accessor->isAllowed(CanonPath::root));
    ASSERT_FALSE(accessor->isAllowed(CanonPath("foo")));
}

TEST(AllowListSourceAccessor, allowRoot)
{
    auto accessor = makeAllowList(makeEmptySourceAccessor(), {CanonPath::root});
    ASSERT_TRUE(accessor->isAllowed(CanonPath::root));
    ASSERT_TRUE(accessor->isAllowed(CanonPath("foo/bar")));
}

TEST(AllowListSourceAccessor, allowedPaths)
{
    /* This is how the Git workdir accessor uses it: the root is
       always allowed, but its children only if they're listed. */
    auto accessor = makeAllowList(makeEmptySourceAccessor(), {CanonPath("dir/file")}, {CanonPath::root});
    ASSERT_TRUE(accessor->isAllowed(CanonPath::root));
    ASSERT_TRUE(accessor->isAllowed(CanonPath("dir")));
    ASSERT_TRUE(accessor->isAllowed(CanonPath("dir/file")));
    ASSERT_FALSE(accessor->isAllowed(CanonPath("dir/other")));
    ASSERT_FALSE(accessor->isAllowed(CanonPath("other")));

    auto accessor2 = makeAllowList(makeEmptySourceAccessor(), {}, {CanonPath::root});
    ASSERT_TRUE(accessor2->isAllowed(CanonPath::root));
    ASSERT_FALSE(accessor2->isAllowed(CanonPath("foo")));
}

TEST(AllowListSourceAccessor, allowPrefix)
{
    auto accessor = makeAllowList(makeEmptySourceAccessor(), {});
    ASSERT_FALSE(accessor->isAllowed(CanonPath("/nix/store/abc-foo/bar")));

    accessor->allowPrefix(CanonPath("/nix/store/abc-foo"));
    accessor->allowPrefix(CanonPath("/nix/store/def-bar"));
    accessor->allowPrefix(CanonPath("/nix/store/abc-foo"));

    ASSERT_TRUE(accessor->isAllowed(CanonPath::root));
    ASSERT_TRUE(accessor->isAllowed(CanonPath("/nix")));
    ASSERT_TRUE(accessor->isAllowed(CanonPath("/nix/store")));
    ASSERT_TRUE(accessor->isAllowed(CanonPath("/nix/store/abc-foo")));
    ASSERT_TRUE(accessor->isAllowed(CanonPath("/nix/store/abc-foo/bar")));
    ASSERT_TRUE(accessor->isAllowed(CanonPath("/nix/store/def-bar/x/y/z")));
    ASSERT_FALSE(accessor->isAllowed(CanonPath("/nix/store/ghi-baz")));
    ASSERT_FALSE(accessor->isAllowed(CanonPath("/nix/store/abc-foobar")));
    ASSERT_FALSE(accessor->isAllowed(CanonPath("/nix/var")));
    ASSERT_FALSE(accessor->isAllowed(CanonPath("/etc")));
}

TEST(AllowListSourceAccessor, readDirectory)
{
    auto fs = make_ref<MemorySourceAccessor>();
    fs->open(CanonPath::root, MemorySourceAccessor::File::Directory{});
    fs->open(CanonPath("/a"), MemorySourceAccessor::File::Directory{});
    fs->open(CanonPath("/b"), MemorySourceAccessor::File::Directory{});
    fs->addFile(CanonPath("/a/x"), "x");
    fs->addFile(CanonPath("/a/y"), "y");
    fs->addFile(CanonPath("/b/x"), "x");
    fs->addFile(CanonPath("/b/y"), "y");
    fs->addFile(CanonPath("/c"), "c");

    auto accessor = makeAllowList(fs, {CanonPath("/a"), CanonPath("/b/y")});

    /* The root is an ancestor of allowed prefixes, so it can be
       listed, but only the allowed entries are visible. */
    auto root = accessor->readDirectory(CanonPath::root);
    ASSERT_EQ(root.size(), 2);
    ASSERT_TRUE(root.contains("a"));
    ASSERT_TRUE(root.contains("b"));

    /* `/a` is an allowed prefix, so all its entries are visible. */
    ASSERT_EQ(accessor->readDirectory(CanonPath("/a")).size(), 2);

    /* `/b` is only an ancestor of `/b/y`. */
    auto b = accessor->readDirectory(CanonPath("/b"));
    ASSERT_EQ(b.size(), 1);
    ASSERT_TRUE(b.contains("y"));

    ASSERT_THROW(accessor->readDirectory(CanonPath("/c")), RestrictedPathError);
}

} // namespace nix
