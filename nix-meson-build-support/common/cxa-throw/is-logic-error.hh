#pragma once

#include <cxxabi.h>
#include <stdexcept>
#include <typeinfo>

static bool is_logic_error(const std::type_info * tinfo)
{
    if (*tinfo == typeid(std::logic_error))
        return true;

    auto * si = dynamic_cast<const __cxxabiv1::__si_class_type_info *>(tinfo);
    if (si)
        return is_logic_error(si->__base_type);

    return false;
}
