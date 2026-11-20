#include <regex>

#include <nlohmann/json.hpp>
#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

#include "nix/store/path-regex.hh"
#include "nix/store/store-api.hh"

#include "nix/util/tests/json-characterization.hh"
#include "nix/store/tests/libstore.hh"
#include "nix/store/tests/path.hh"

namespace nix {

#define STORE_DIR "/nix/store/"
#define HASH_PART "g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q"

class StorePathTest : public virtual CharacterizationTest, public LibStoreTest
{
    std::filesystem::path unitTestData = getUnitTestData() / "store-path";

public:

    std::filesystem::path goldenMaster(std::string_view testStem) const override
    {
        return unitTestData / testStem;
    }
};

static std::regex nameRegex{std::string{nameRegexStr}};

#define TEST_DONT_PARSE(NAME, STR)                                           \
    TEST_F(StorePathTest, bad_##NAME)                                        \
    {                                                                        \
        std::string_view str = STORE_DIR HASH_PART "-" STR;                  \
        /* ASSERT_THROW generates a duplicate goto label */                  \
        /* A lambda isolates those labels. */                                \
        [&]() { ASSERT_THROW(store->parseStorePath(str), BadStorePath); }(); \
        EXPECT_FALSE(store->maybeParseStorePath(str));                       \
        EXPECT_FALSE(StorePath::parse(HASH_PART "-" STR));                   \
        std::string name{STR};                                               \
        [&]() { ASSERT_THROW(nix::checkName(name), BadStorePathName); }();   \
        EXPECT_FALSE(nix::validateStorePathName(name));                      \
        EXPECT_FALSE(std::regex_match(name, nameRegex));                     \
    }

TEST_DONT_PARSE(empty, "")
TEST_DONT_PARSE(garbage, "&*()")
TEST_DONT_PARSE(double_star, "**")
TEST_DONT_PARSE(star_first, "*,foo")
TEST_DONT_PARSE(star_second, "foo,*")
TEST_DONT_PARSE(bang, "foo!o")
TEST_DONT_PARSE(dot, ".")
TEST_DONT_PARSE(dot_dot, "..")
TEST_DONT_PARSE(dot_dot_dash, "..-1")
TEST_DONT_PARSE(dot_dash, ".-1")
TEST_DONT_PARSE(dot_dot_dash_a, "..-a")
TEST_DONT_PARSE(dot_dash_a, ".-a")
TEST_DONT_PARSE(slash, "a/b")

#undef TEST_DONT_PARSE

#define TEST_DO_PARSE(NAME, STR)                               \
    TEST_F(StorePathTest, good_##NAME)                         \
    {                                                          \
        std::string_view str = STORE_DIR HASH_PART "-" STR;    \
        auto p = store->parseStorePath(str);                   \
        std::string name{p.name()};                            \
        EXPECT_EQ(p.name(), STR);                              \
        EXPECT_TRUE(store->maybeParseStorePath(str) == p);     \
        EXPECT_TRUE(StorePath::parse(HASH_PART "-" STR) == p); \
        EXPECT_TRUE(nix::validateStorePathName(name));         \
        EXPECT_TRUE(std::regex_match(name, nameRegex));        \
    }

// 0-9 a-z A-Z + - . _ ? =

TEST_DO_PARSE(numbers, "02345")
TEST_DO_PARSE(lower_case, "foo")
TEST_DO_PARSE(upper_case, "FOO")
TEST_DO_PARSE(plus, "foo+bar")
TEST_DO_PARSE(dash, "foo-dev")
TEST_DO_PARSE(underscore, "foo_bar")
TEST_DO_PARSE(period, "foo.txt")
TEST_DO_PARSE(question_mark, "foo?why")
TEST_DO_PARSE(equals_sign, "foo=foo")
TEST_DO_PARSE(dotfile, ".gitignore")
TEST_DO_PARSE(triple_dot_a, "...a")
TEST_DO_PARSE(triple_dot_1, "...1")
TEST_DO_PARSE(triple_dot_dash, "...-")
TEST_DO_PARSE(triple_dot, "...")

#undef TEST_DO_PARSE

RC_GTEST_FIXTURE_PROP(StorePathTest, prop_regex_accept, (const StorePath & p))
{
    RC_ASSERT(std::regex_match(std::string{p.name()}, nameRegex));
}

RC_GTEST_FIXTURE_PROP(StorePathTest, prop_round_rip, (const StorePath & p))
{
    RC_ASSERT(p == store->parseStorePath(store->printStorePath(p)));
}

RC_GTEST_FIXTURE_PROP(StorePathTest, prop_check_regex_eq_parse, ())
{
    static auto nameFuzzer = rc::gen::container<std::string>(rc::gen::oneOf(
        // alphanum, repeated to weigh heavier
        rc::gen::oneOf(rc::gen::inRange('0', '9'), rc::gen::inRange('a', 'z'), rc::gen::inRange('A', 'Z')),
        // valid symbols
        rc::gen::oneOf(
            rc::gen::just('+'),
            rc::gen::just('-'),
            rc::gen::just('.'),
            rc::gen::just('_'),
            rc::gen::just('?'),
            rc::gen::just('=')),
        // symbols for scary .- and ..- cases, repeated for weight
        rc::gen::just('.'),
        rc::gen::just('.'),
        rc::gen::just('.'),
        rc::gen::just('.'),
        rc::gen::just('-'),
        rc::gen::just('-'),
        // ascii symbol ranges, excluding '/' (see below)
        rc::gen::oneOf(
            rc::gen::inRange(' ', '.'),
            rc::gen::inRange(':', '@'),
            rc::gen::inRange('[', '`'),
            rc::gen::inRange('{', '~')),
        // typical whitespace
        rc::gen::oneOf(rc::gen::just(' '), rc::gen::just('\t'), rc::gen::just('\n'), rc::gen::just('\r')),
        // some chance of control codes, non-ascii or other garbage we missed, excluding '/' (see below)
        rc::gen::suchThat(rc::gen::inRange('\0', '\xff'), [](char c) { return c != '/'; })));

    /* Note: names containing '/' are not generated, since `parseStorePath()` canonicalises the path, so such a
       name may be parsed as a different name (e.g. a trailing slash is removed, and `/.` and `/..` components
       are resolved) and can't be compared against the regex. `bad_slash` above covers them. */
    auto name = *nameFuzzer;

    std::string path = store->storeDir + "/575s52sh487i0ylmbs9pvi606ljdszr0-" + name;
    bool parsed = false;
    try {
        store->parseStorePath(path);
        parsed = true;
    } catch (const BadStorePath &) {
    }
    RC_ASSERT(parsed == std::regex_match(std::string{name}, nameRegex));
}

TEST_F(StorePathTest, mustBeDashAfterHashPart)
{
    std::string hashPart = "575s52sh487i0ylmbs9pvi606ljdszr0";
    for (char c : {'_', '\0'}) {
        EXPECT_THROW(StorePath(hashPart + c + "name"), BadStorePath);
    }
    EXPECT_NO_THROW(StorePath(hashPart + "-" + "name"));
}

#ifndef _WIN32
// On Windows, `parseStorePath()` doesn't canonicalise.
TEST_F(StorePathTest, parseNonCanonical)
{
    auto expected = store->parseStorePath(STORE_DIR HASH_PART "-foo");
    for (auto s : {
             STORE_DIR HASH_PART "-foo/",
             STORE_DIR "/" HASH_PART "-foo",
             STORE_DIR "./" HASH_PART "-foo",
             STORE_DIR HASH_PART "-foo/bar/..",
             STORE_DIR HASH_PART "-foo/.",
             "/nix/../nix/store/" HASH_PART "-foo",
         })
        EXPECT_EQ(store->parseStorePath(s), expected) << s;
}
#endif

TEST_F(StorePathTest, notAStorePath)
{
    for (auto s : {
             "",
             "/",
             "/nix",
             "/nix/store",
             "/nix/store/",
             STORE_DIR HASH_PART "-foo/bar",
             STORE_DIR HASH_PART "-foo/bar/",
             STORE_DIR HASH_PART "-foo/..",
             "/nix/storefoo/" HASH_PART "-foo",
             "/nix/" HASH_PART "-foo",
             "/foo/store/" HASH_PART "-foo",
         }) {
        EXPECT_THROW(store->parseStorePath(s), BadStorePath) << s;
        EXPECT_FALSE(store->maybeParseStorePath(s)) << s;
        EXPECT_FALSE(store->isStorePath(s)) << s;
    }
}

TEST_F(StorePathTest, badStorePathReasons)
{
    using enum BadStorePathReason;
    std::vector<std::pair<std::string, BadStorePathReason>> cases{
        {"", EmptyPath},
        {"foo", NotInStore},
        {HASH_PART "-foo", NotInStore},
        {STORE_DIR HASH_PART "-foo/bar", NotInStore},
        {"/nix/storefoo/" HASH_PART "-foo", NotInStore},
        {STORE_DIR "abc", TooShort},
        {STORE_DIR HASH_PART "_foo", NoDashAfterHash},
        {STORE_DIR "e1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-foo", IllegalHashCharacter},
        {STORE_DIR HASH_PART "-", NameEmpty},
        {STORE_DIR HASH_PART "-" + std::string(StorePath::MaxPathLen + 1, 'a'), NameTooLong},
        {STORE_DIR HASH_PART "-..", NameReserved},
        {STORE_DIR HASH_PART "-.-a", NameReservedComponent},
        {STORE_DIR HASH_PART "-foo!", NameIllegalCharacter},
    };
    for (auto & [s, reason] : cases) {
        auto res = store->maybeParseStorePath(s);
        ASSERT_FALSE(res) << s;
        EXPECT_EQ(res.error(), reason) << s;
        EXPECT_THROW(store->parseStorePath(s), BadStorePath) << s;
    }
}

TEST_F(StorePathTest, maybeToStorePath)
{
    auto expected = store->parseStorePath(STORE_DIR HASH_PART "-foo");

    auto res = store->maybeToStorePath(CanonPath(STORE_DIR HASH_PART "-foo"));
    ASSERT_TRUE(res);
    EXPECT_EQ(res->first, expected);
    EXPECT_EQ(res->second, CanonPath::root);

    res = store->maybeToStorePath(CanonPath(STORE_DIR HASH_PART "-foo/bar/xyzzy"));
    ASSERT_TRUE(res);
    EXPECT_EQ(res->first, expected);
    EXPECT_EQ(res->second, CanonPath("/bar/xyzzy"));

    using enum BadStorePathReason;
    for (auto & [s, reason] : std::vector<std::pair<std::string, BadStorePathReason>>{
             {"/", NotInStore},
             {"/nix", NotInStore},
             {"/nix/store", NotInStore},
             {"/nix/storefoo/" HASH_PART "-foo", NotInStore},
             {"/foo/store/" HASH_PART "-foo/bar", NotInStore},
             {STORE_DIR "abc/bar", TooShort},
             {STORE_DIR HASH_PART "-foo!/bar", NameIllegalCharacter},
         }) {
        auto res = store->maybeToStorePath(CanonPath(s));
        ASSERT_FALSE(res) << s;
        EXPECT_EQ(res.error(), reason) << s;
    }
}

TEST_F(StorePathTest, errorMessages)
{
    auto parseError = [&](std::string_view s) -> std::string {
        try {
            store->parseStorePath(s);
        } catch (BadStorePath & e) {
            return e.what();
        }
        return "";
    };
    auto expectError = [&](std::string_view s, std::string_view expected) {
        EXPECT_NE(parseError(s).find(expected), std::string::npos) << s << ": " << parseError(s);
    };
    expectError("", "empty path is not a valid store path");
    expectError("/foo/bar", "is not in the Nix store");
    expectError(STORE_DIR "abc", "is too short to be a valid store path");
    expectError(STORE_DIR HASH_PART "_foo", "hash part is not followed by a '-'");
    expectError(STORE_DIR "e1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-foo", "contains illegal base-32 character");
    expectError(STORE_DIR HASH_PART "-..", "is not a valid store path: name");
    expectError(STORE_DIR HASH_PART "-..-a", "first dash-separated component must not be");
    expectError(STORE_DIR HASH_PART "-foo!", "contains illegal character");

    try {
        checkName("foo!");
        FAIL();
    } catch (BadStorePathName & e) {
        EXPECT_NE(std::string(e.what()).find("contains illegal character"), std::string::npos);
    }
}

/* ----------------------------------------------------------------------------
 * JSON
 * --------------------------------------------------------------------------*/

using nlohmann::json;

struct StorePathJsonTest : StorePathTest,
                           JsonCharacterizationTest<StorePath>,
                           ::testing::WithParamInterface<std::pair<std::string_view, StorePath>>
{};

TEST_P(StorePathJsonTest, from_json)
{
    auto & [name, expected] = GetParam();
    readJsonTest(name, expected);
}

TEST_P(StorePathJsonTest, to_json)
{
    auto & [name, value] = GetParam();
    writeJsonTest(name, value);
}

INSTANTIATE_TEST_SUITE_P(
    StorePathJSON,
    StorePathJsonTest,
    ::testing::Values(
        std::pair{
            "simple",
            StorePath{"g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-foo.drv"},
        }));

} // namespace nix
