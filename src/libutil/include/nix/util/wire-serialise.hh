#pragma once
///@file

#include "nix/util/serialise.hh"

#include <boost/pfr.hpp>

#include <filesystem>
#include <list>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace nix {

/**
 * Generic serialisation of values in the Nix wire format (see
 * `serialise.hh`), using `writeWire()` and `readWire()`.
 *
 * Aggregates (i.e. structs without constructors, base classes or
 * private members) are serialised field by field, in declaration
 * order. The format therefore changes whenever such a struct changes,
 * so this is only suitable for communication between processes running
 * the same executable (such as the spawner, see `spawner.hh`), not for
 * protocols that need to be stable across Nix versions.
 *
 * Other types can be supported by specialising `WireSerialise`.
 */
template<typename T>
struct WireSerialise;

template<typename T>
void writeWire(Sink & sink, const T & x)
{
    WireSerialise<T>::write(sink, x);
}

template<typename T>
T readWire(Source & source)
{
    return WireSerialise<T>::read(source);
}

template<typename T>
    requires std::is_integral_v<T> && std::is_unsigned_v<T>
struct WireSerialise<T>
{
    static void write(Sink & sink, T n)
    {
        sink << (uint64_t) n;
    }

    static T read(Source & source)
    {
        return readNum<T>(source);
    }
};

/**
 * Signed integers are serialised as their two's complement
 * representation, so that negative values round-trip.
 */
template<typename T>
    requires std::is_integral_v<T> && std::is_signed_v<T>
struct WireSerialise<T>
{
    static void write(Sink & sink, T n)
    {
        sink << (uint64_t) (std::make_unsigned_t<T>) n;
    }

    static T read(Source & source)
    {
        return (T) readNum<std::make_unsigned_t<T>>(source);
    }
};

template<typename T>
    requires std::is_enum_v<T>
struct WireSerialise<T>
{
    static void write(Sink & sink, T x)
    {
        writeWire(sink, (std::underlying_type_t<T>) x);
    }

    static T read(Source & source)
    {
        return (T) readWire<std::underlying_type_t<T>>(source);
    }
};

template<>
struct WireSerialise<std::string>
{
    static void write(Sink & sink, const std::string & s)
    {
        sink << s;
    }

    static std::string read(Source & source)
    {
        return readString(source);
    }
};

template<>
struct WireSerialise<std::filesystem::path>
{
    static void write(Sink & sink, const std::filesystem::path & path)
    {
        sink << path.string();
    }

    static std::filesystem::path read(Source & source)
    {
        return readString(source);
    }
};

template<typename T>
struct WireSerialise<std::optional<T>>
{
    static void write(Sink & sink, const std::optional<T> & x)
    {
        sink << (uint64_t) x.has_value();
        if (x)
            writeWire(sink, *x);
    }

    static std::optional<T> read(Source & source)
    {
        if (!readNum<uint64_t>(source))
            return std::nullopt;
        return readWire<T>(source);
    }
};

template<typename A, typename B>
struct WireSerialise<std::pair<A, B>>
{
    static void write(Sink & sink, const std::pair<A, B> & x)
    {
        writeWire(sink, x.first);
        writeWire(sink, x.second);
    }

    static std::pair<A, B> read(Source & source)
    {
        auto first = readWire<A>(source);
        return {std::move(first), readWire<B>(source)};
    }
};

/**
 * Helper for containers: the number of elements followed by the
 * elements.
 */
template<typename C, typename Elem = typename C::value_type>
struct WireSerialiseContainer
{
    static void write(Sink & sink, const C & c)
    {
        sink << (uint64_t) c.size();
        for (auto & x : c)
            writeWire<Elem>(sink, x);
    }

    static C read(Source & source)
    {
        C c;
        auto size = readNum<size_t>(source);
        for (size_t i = 0; i < size; ++i)
            c.insert(c.end(), readWire<Elem>(source));
        return c;
    }
};

template<typename T>
struct WireSerialise<std::vector<T>> : WireSerialiseContainer<std::vector<T>>
{};

template<typename T>
struct WireSerialise<std::list<T>> : WireSerialiseContainer<std::list<T>>
{};

template<typename T, typename Compare>
struct WireSerialise<std::set<T, Compare>> : WireSerialiseContainer<std::set<T, Compare>>
{};

template<typename K, typename V, typename Compare>
struct WireSerialise<std::map<K, V, Compare>> : WireSerialiseContainer<std::map<K, V, Compare>, std::pair<K, V>>
{};

template<typename T>
    requires std::is_aggregate_v<T> && (!std::is_array_v<T>)
struct WireSerialise<T>
{
    static void write(Sink & sink, const T & x)
    {
        boost::pfr::for_each_field(x, [&](const auto & field) { writeWire(sink, field); });
    }

    static T read(Source & source)
    {
        T x{};
        boost::pfr::for_each_field(
            x, [&](auto & field) { field = readWire<std::remove_cvref_t<decltype(field)>>(source); });
        return x;
    }
};

} // namespace nix
