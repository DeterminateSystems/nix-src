#pragma once
///@file

#include <rapidcheck/gen/Arbitrary.h>

#include "nix/expr/value/context.hh"

namespace rc {
using namespace nix;

template<>
struct Arbitrary<NixStringContextElem::Opaque>
{
    static Gen<NixStringContextElem::Opaque> arbitrary();
};

template<>
struct Arbitrary<NixStringContextElem::Built>
{
    static Gen<NixStringContextElem::Built> arbitrary();
};

template<>
struct Arbitrary<NixStringContextElem::DrvDeep>
{
    static Gen<NixStringContextElem::DrvDeep> arbitrary();
};

template<>
struct Arbitrary<NixStringContextElem::Path>
{
    static Gen<NixStringContextElem::Path> arbitrary();
};

template<>
struct Arbitrary<NixStringContextElem>
{
    static Gen<NixStringContextElem> arbitrary();
};

} // namespace rc
