#include "nix/expr/value.hh"

namespace nix {

Value Value::vEmptyList{List{.size = 0, .elems = nullptr}};

Value Value::vNull{Null{}};

Value Value::vTrue{true};

Value Value::vFalse{false};

} // namespace nix
