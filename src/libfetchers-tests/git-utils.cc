#include "nix/fetchers/git-utils.hh"
#include "nix/util/file-system.hh"
#include "nix/util/fs-sink.hh"
#include "nix/util/tests/gmock-matchers.hh"
#include "nix/util/serialise.hh"
#include "nix/util/git.hh"

#include <span>
#include <tuple>
#include <array>

#include <git2/global.h>
#include <git2/repository.h>
#include <git2/signature.h>
#include <git2/types.h>
#include <git2/object.h>
#include <git2/tag.h>
#include <git2/blob.h>
#include <git2/tree.h>
#include <git2/version.h>

#include <zlib.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace nix::fetchers {

class GitUtilsTest : public ::testing::Test
{
    // We use a single repository for all tests.
    std::unique_ptr<AutoDelete> delTmpDir;

protected:
    std::filesystem::path tmpDir;

public:
    void SetUp() override
    {
        auto parentDir = createTempDir();
        delTmpDir = std::make_unique<AutoDelete>(parentDir, true);
        tmpDir = parentDir / "test-git-repo";

        // Create the repo with libgit2
        git_libgit2_init();
        git_repository * repo = nullptr;
        auto r = git_repository_init(&repo, tmpDir.string().c_str(), 0);
        ASSERT_EQ(r, 0);
        git_repository_free(repo);
    }

    void TearDown() override
    {
        // Destroy the AutoDelete, triggering removal
        // not AutoDelete::reset(), which would cancel the deletion.
        delTmpDir.reset();
    }

    ref<GitRepo> openRepo()
    {
        return GitRepo::openRepo(tmpDir, {.create = true});
    }

    std::string getRepoName() const
    {
        return tmpDir.filename().string();
    }
};

void writeString(CreateRegularFileSink & fileSink, std::string contents, bool executable)
{
    if (executable)
        fileSink.isExecutable();
    fileSink.preallocateContents(contents.size());
    fileSink(contents);
}

TEST_F(GitUtilsTest, sink_basic)
{
    auto repo = openRepo();
    auto sink = repo->getFileSystemObjectSink();

    // TODO/Question: It seems a little odd that we use the tarball-like convention of requiring a top-level directory
    // here
    //                The sync method does not document this behavior, should probably renamed because it's not very
    //                general, and I can't imagine that "non-conventional" archives or any other source to be handled by
    //                this sink.

    sink->createDirectory(CanonPath("foo-1.1"));

    sink->createRegularFile(CanonPath("foo-1.1/hello"), [](CreateRegularFileSink & fileSink) {
        writeString(fileSink, "hello world", false);
    });
    sink->createRegularFile(CanonPath("foo-1.1/bye"), [](CreateRegularFileSink & fileSink) {
        writeString(fileSink, "thanks for all the fish", false);
    });
    sink->createSymlink(CanonPath("foo-1.1/bye-link"), "bye");
    sink->createDirectory(CanonPath("foo-1.1/empty"));
    sink->createDirectory(CanonPath("foo-1.1/links"));
    sink->createHardlink(CanonPath("foo-1.1/links/foo"), CanonPath("foo-1.1/hello"));

    // sink->createHardlink("foo-1.1/links/foo-2", CanonPath("foo-1.1/hello"));

    auto result = repo->dereferenceSingletonDirectory(sink->flush());
    auto accessor = repo->getAccessor(result, {}, getRepoName());
    auto entries = accessor->readDirectory(CanonPath::root);
    ASSERT_EQ(entries.size(), 5u);
    ASSERT_EQ(accessor->readFile(CanonPath("hello")), "hello world");
    ASSERT_EQ(accessor->readFile(CanonPath("bye")), "thanks for all the fish");
    ASSERT_EQ(accessor->readLink(CanonPath("bye-link")), "bye");
    ASSERT_EQ(accessor->readDirectory(CanonPath("empty")).size(), 0u);
    ASSERT_EQ(accessor->readFile(CanonPath("links/foo")), "hello world");
};

TEST_F(GitUtilsTest, sink_hardlink)
{
    auto repo = openRepo();
    auto sink = repo->getFileSystemObjectSink();

    sink->createDirectory(CanonPath("foo-1.1"));

    sink->createRegularFile(CanonPath("foo-1.1/hello"), [](CreateRegularFileSink & fileSink) {
        writeString(fileSink, "hello world", false);
    });

    try {
        sink->createHardlink(CanonPath("foo-1.1/link"), CanonPath("hello"));
        sink->flush();
        FAIL() << "Expected an exception";
    } catch (const nix::Error & e) {
        ASSERT_THAT(e.msg(), ::testing::HasSubstr("does not exist"));
        ASSERT_THAT(e.msg(), ::testing::HasSubstr("/hello"));
        ASSERT_THAT(e.msg(), ::testing::HasSubstr("foo-1.1/link"));
    }
};

TEST_F(GitUtilsTest, peel_reference)
{
    // Create a commit in the repo
    git_repository * rawRepo = nullptr;
    ASSERT_EQ(git_repository_open(&rawRepo, tmpDir.string().c_str()), 0);

    // Create a blob
    git_oid blob_oid;
    const char * blob_content = "hello world";
    ASSERT_EQ(git_blob_create_from_buffer(&blob_oid, rawRepo, blob_content, strlen(blob_content)), 0);

    // Create a tree with that blob
    git_treebuilder * builder = nullptr;
    ASSERT_EQ(git_treebuilder_new(&builder, rawRepo, nullptr), 0);
    ASSERT_EQ(git_treebuilder_insert(nullptr, builder, "file.txt", &blob_oid, GIT_FILEMODE_BLOB), 0);

    git_oid tree_oid;
    ASSERT_EQ(git_treebuilder_write(&tree_oid, builder), 0);
    git_treebuilder_free(builder);

    git_tree * tree = nullptr;
    ASSERT_EQ(git_tree_lookup(&tree, rawRepo, &tree_oid), 0);

    // Create a commit
    git_signature * sig = nullptr;
    ASSERT_EQ(git_signature_now(&sig, "nix", "nix@example.com"), 0);

    git_oid commit_oid;
    ASSERT_EQ(git_commit_create_v(&commit_oid, rawRepo, "HEAD", sig, sig, nullptr, "initial commit", tree, 0), 0);

    // Lookup our commit
    git_object * commit_object = nullptr;
    ASSERT_EQ(git_object_lookup(&commit_object, rawRepo, &commit_oid, GIT_OBJECT_COMMIT), 0);

    // Create annotated tag
    git_oid tag_oid;
    ASSERT_EQ(git_tag_create(&tag_oid, rawRepo, "v1", commit_object, sig, "annotated tag", 0), 0);

    auto repo = openRepo();

    // Use resolveRef to get peeled object
    auto resolved = repo->resolveRef("refs/tags/v1");

    // Now assert that we have unpeeled it!
    ASSERT_STREQ(resolved.gitRev().c_str(), git_oid_tostr_s(&commit_oid));

    git_signature_free(sig);
    git_repository_free(rawRepo);
}

TEST_F(GitUtilsTest, duplicateTreeEntries)
{
    StringSink packSink;

    auto serialiseBigEndian = [](auto x, Sink & sink) {
        std::array<unsigned char, sizeof(x)> bytes;
        writeBigEndian(bytes.data(), x);
        sink(std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()));
    };

    auto writeObjectPackHeader = [&packSink](int type, std::size_t size) {
        std::vector<char> bytes;
        unsigned char byte = type << 4 | (size & 0x0f);
        size >>= 4;

        while (size) {
            byte |= 0x80;
            bytes.push_back(byte);
            byte = size & 0x7f;
            size >>= 7;
        }

        bytes.push_back(byte);
        packSink(std::string_view(bytes.data(), bytes.size()));
    };

    /* We don't have a deflate compression sink and seems excessive to write it just for tests.
       Maybe it the future we might want to write pack files manually to work in constant memory? */
    auto compressObject = [](std::string_view data) -> std::string {
        z_stream stream = {};
        if (deflateInit(&stream, Z_DEFAULT_COMPRESSION) != Z_OK)
            unreachable();
        Finally cleanup([&stream] { ::deflateEnd(&stream); });
        stream.next_in = reinterpret_cast<unsigned char *>(const_cast<char *>(data.data()));
        stream.avail_in = data.size();
        auto maxOutputSize = ::deflateBound(&stream, data.size());
        std::vector<unsigned char> result(maxOutputSize, '\0');
        stream.next_out = result.data();
        stream.avail_out = result.size();
        if (::deflate(&stream, Z_FINISH) != Z_STREAM_END)
            unreachable();
        result.resize(result.size() - stream.avail_out);
        return std::string(result.begin(), result.end());
    };

    /* https://git-scm.com/docs/pack-format */
    packSink("PACK");
    serialiseBigEndian(/*version=*/2u, packSink);
    serialiseBigEndian(/*numObjects=*/4u, packSink);

    auto writeBlobToPackfile = [&](std::string_view data) -> Hash {
        HashSink hashSink(HashAlgorithm::SHA1);
        git::dumpBlobPrefix(data.size(), hashSink);
        hashSink(data);
        auto hash = hashSink.finish();
        writeObjectPackHeader(/*OBJ_BLOB=*/3, data.size());
        packSink(compressObject(data));
        return hash.hash;
    };

    auto writeTreeToPackfile = [&](std::span<const std::tuple<std::string, git::Mode, Hash>> entries) -> Hash {
        using namespace std::string_literals;
        std::string tree;

        for (const auto & [name, mode, hash] : entries) {
            tree += fmt("%o %s\0"s, std::to_underlying(mode), name);
            tree += std::string_view(reinterpret_cast<const char *>(hash.hash), hash.hashSize);
        }

        writeObjectPackHeader(/*OBJ_TREE=*/2, tree.size());
        packSink(compressObject(tree));
        HashSink treeHashSink(HashAlgorithm::SHA1);
        treeHashSink(fmt("tree %d\0"s, tree.size()));
        treeHashSink(tree);
        return treeHashSink.finish().hash;
    };

    auto blob1 = writeBlobToPackfile("hi");
    ASSERT_EQ(blob1.gitRev(), "32f95c0d1244a78b2be1bab8de17906fabb2c4a8");
    auto blob2 = writeBlobToPackfile("there");
    ASSERT_EQ(blob2.gitRev(), "c78ee1a5bdf46d22da300b68d50bc45c587c3293");
    auto tree1 = writeTreeToPackfile(
        std::to_array<std::tuple<std::string, git::Mode, Hash>>({
            {"a.txt", git::Mode::Regular, blob1},
            {"a.txt", git::Mode::Executable, blob2},
        }));
    ASSERT_EQ(tree1.gitRev(), "164e45a3d3f73578e4d4f83651b5d0edc4564ba0");
    auto tree2 = writeTreeToPackfile(
        std::to_array<std::tuple<std::string, git::Mode, Hash>>({
            /* Fake blob that actually points to a tree. */
            {"a.txt", git::Mode::Regular, tree1},
        }));

    auto packHash = hashString(HashAlgorithm::SHA1, packSink.s);
    packSink(std::string_view(reinterpret_cast<const char *>(packHash.hash), packHash.hashSize));

    /* Now that the pack file is built, index it.
       TODO: If libgit2 ever hardens itself against such pack files we might need to
       rework the test and write the pack file manually. */
    ::git_indexer * indexer = nullptr;
#if LIBGIT2_VERSION_CHECK(2, 0, 0)
    ::git_indexer_options indexerOpts = GIT_INDEXER_OPTIONS_INIT;
    indexerOpts.oid_type = GIT_OID_SHA1;
#endif

    Finally freeIndexer{[&indexer] { ::git_indexer_free(indexer); }};
    ASSERT_EQ(
        ::git_indexer_new(
            &indexer,
            (tmpDir / ".git" / "objects" / "pack").string().c_str(),
#if LIBGIT2_VERSION_CHECK(2, 0, 0)
            &indexerOpts
#else
            0,
            nullptr,
            nullptr
#endif
            ),
        0);

    ::git_indexer_progress progress = {};
    ASSERT_EQ(::git_indexer_append(indexer, packSink.s.data(), packSink.s.size(), &progress), 0);
    ASSERT_EQ(::git_indexer_commit(indexer, &progress), 0);

    auto repo = openRepo();
    auto accessor1 = repo->getAccessor(tree1, {}, getRepoName());
    ASSERT_THAT(
        [&] { accessor1->readDirectory(CanonPath::root); },
        ::testing::ThrowsMessage<Error>(
            testing::HasSubstrIgnoreANSIMatcher("duplicate tree entry 'test-git-repo/a.txt'")));
    ASSERT_THAT(
        [&] { accessor1->readFile(CanonPath("a.txt")); },
        ::testing::ThrowsMessage<Error>(
            testing::HasSubstrIgnoreANSIMatcher("duplicate tree entry 'test-git-repo/a.txt'")));

    auto accessor2 = repo->getAccessor(tree2, {}, getRepoName());
    ASSERT_THAT(
        [&] { accessor2->readFile(CanonPath("a.txt")); },
        ::testing::ThrowsMessage<Error>(
            testing::HasSubstrIgnoreANSIMatcher("'test-git-repo/a.txt' is not a regular file")));
}

TEST(GitUtils, isLegalRefName)
{
    ASSERT_TRUE(isLegalRefName("A/b"));
    ASSERT_TRUE(isLegalRefName("AaA/b"));
    ASSERT_TRUE(isLegalRefName("FOO/BAR/BAZ"));
    ASSERT_TRUE(isLegalRefName("HEAD"));
    ASSERT_TRUE(isLegalRefName("refs/tags/1.2.3"));
    ASSERT_TRUE(isLegalRefName("refs/heads/master"));
    ASSERT_TRUE(isLegalRefName("foox"));
    ASSERT_TRUE(isLegalRefName("1337"));
    ASSERT_TRUE(isLegalRefName("foo.baz"));
    ASSERT_TRUE(isLegalRefName("foo/bar/baz"));
    ASSERT_TRUE(isLegalRefName("foo./bar"));
    ASSERT_TRUE(isLegalRefName("heads/foo@bar"));
    ASSERT_TRUE(isLegalRefName("heads/fu\303\237"));
    ASSERT_TRUE(isLegalRefName("foo-bar-baz"));
    ASSERT_TRUE(isLegalRefName("branch#"));
    ASSERT_TRUE(isLegalRefName("$1"));
    ASSERT_TRUE(isLegalRefName("foo.locke"));

    ASSERT_FALSE(isLegalRefName("refs///heads/foo"));
    ASSERT_FALSE(isLegalRefName("heads/foo/"));
    ASSERT_FALSE(isLegalRefName("///heads/foo"));
    ASSERT_FALSE(isLegalRefName(".foo"));
    ASSERT_FALSE(isLegalRefName("./foo"));
    ASSERT_FALSE(isLegalRefName("./foo/bar"));
    ASSERT_FALSE(isLegalRefName("foo/./bar"));
    ASSERT_FALSE(isLegalRefName("foo/bar/."));
    ASSERT_FALSE(isLegalRefName("foo bar"));
    ASSERT_FALSE(isLegalRefName("foo?bar"));
    ASSERT_FALSE(isLegalRefName("foo^bar"));
    ASSERT_FALSE(isLegalRefName("foo~bar"));
    ASSERT_FALSE(isLegalRefName("foo:bar"));
    ASSERT_FALSE(isLegalRefName("foo[bar"));
    ASSERT_FALSE(isLegalRefName("foo/bar/."));
    ASSERT_FALSE(isLegalRefName(".refs/foo"));
    ASSERT_FALSE(isLegalRefName("refs/heads/foo."));
    ASSERT_FALSE(isLegalRefName("heads/foo..bar"));
    ASSERT_FALSE(isLegalRefName("heads/foo?bar"));
    ASSERT_FALSE(isLegalRefName("heads/foo.lock"));
    ASSERT_FALSE(isLegalRefName("heads///foo.lock"));
    ASSERT_FALSE(isLegalRefName("foo.lock/bar"));
    ASSERT_FALSE(isLegalRefName("foo.lock///bar"));
    ASSERT_FALSE(isLegalRefName("heads/v@{ation"));
    ASSERT_FALSE(isLegalRefName("heads/foo\bar"));

    ASSERT_FALSE(isLegalRefName("@"));
    ASSERT_FALSE(isLegalRefName("\37"));
    ASSERT_FALSE(isLegalRefName("\177"));

    ASSERT_FALSE(isLegalRefName("foo/*"));
    ASSERT_FALSE(isLegalRefName("*/foo"));
    ASSERT_FALSE(isLegalRefName("foo/*/bar"));
    ASSERT_FALSE(isLegalRefName("*"));
    ASSERT_FALSE(isLegalRefName("foo/*/*"));
    ASSERT_FALSE(isLegalRefName("*/foo/*"));
    ASSERT_FALSE(isLegalRefName("/foo"));
    ASSERT_FALSE(isLegalRefName(""));
}

} // namespace nix::fetchers
