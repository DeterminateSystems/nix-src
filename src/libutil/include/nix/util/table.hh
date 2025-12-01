#pragma once

#include "nix/util/types.hh"

#include <list>

namespace nix {

typedef std::vector<std::vector<std::string>> Table;

void printTable(Table & table);

} // namespace nix
