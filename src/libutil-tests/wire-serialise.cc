#include "nix/util/wire-serialise.hh"

#include <gtest/gtest.h>

namespace nix {

namespace {

enum class Colour : int8_t { Red = -1, Green = 1 };

struct Inner
{
    std::filesystem::path path;
    bool flag = false;

    bool operator==(const Inner &) const = default;
};

struct Outer
{
    std::string name;
    int signedNum;
    uint64_t bigNum;
    Colour colour;
    std::optional<Inner> inner;
    std::optional<std::string> missing;
    std::vector<int> numbers;
    Strings strings;
    std::set<std::string> set;
    std::map<std::filesystem::path, Inner> map;

    bool operator==(const Outer &) const = default;
};

template<typename T>
T roundTrip(const T & x)
{
    StringSink sink;
    writeWire(sink, x);
    StringSource source(sink.s);
    auto y = readWire<T>(source);
    EXPECT_EQ(source.drain(), "");
    return y;
}

} // namespace

TEST(WireSerialise, integers)
{
    EXPECT_EQ(roundTrip(true), true);
    EXPECT_EQ(roundTrip(-1), -1);
    EXPECT_EQ(roundTrip(std::numeric_limits<int64_t>::min()), std::numeric_limits<int64_t>::min());
    EXPECT_EQ(roundTrip(std::numeric_limits<uint64_t>::max()), std::numeric_limits<uint64_t>::max());
    EXPECT_EQ(roundTrip(Colour::Red), Colour::Red);
}

TEST(WireSerialise, aggregate)
{
    Outer x{
        .name = "foo",
        .signedNum = -42,
        .bigNum = 1ULL << 63,
        .colour = Colour::Green,
        .inner = Inner{.path = "/foo/bar", .flag = true},
        .numbers = {1, -2, 3},
        .strings = {"a", "b"},
        .set = {"x", "y"},
        .map = {{"/a", Inner{.path = "/b"}}, {"/c", Inner{.path = "/d", .flag = true}}},
    };

    EXPECT_EQ(roundTrip(x), x);
    EXPECT_EQ(roundTrip(Outer{}), Outer{});
}

TEST(WireSerialise, readTooLarge)
{
    StringSink sink;
    writeWire(sink, (uint64_t) 256);
    StringSource source(sink.s);
    EXPECT_THROW(readWire<uint8_t>(source), SerialisationError);
}

} // namespace nix
