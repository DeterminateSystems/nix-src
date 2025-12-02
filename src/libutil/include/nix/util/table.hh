#pragma once

#include "nix/util/types.hh"

#include <list>
#include <limits>

namespace nix {

typedef std::vector<std::vector<std::string>> Table;

void printTable(std::ostream & out, Table & table, unsigned int width = std::numeric_limits<unsigned int>::max());

} // namespace nix
