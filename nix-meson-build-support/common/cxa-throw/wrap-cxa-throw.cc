#include <cstdlib>
#include <typeinfo>

#include "is-logic-error.hh"

#ifndef CXA_THROW_ON_LOGIC_ERROR
#  define CXA_THROW_ON_LOGIC_ERROR() abort()
#endif

extern "C" void __real___cxa_throw(void *, std::type_info *, void (*)(void *));

extern "C" void __wrap___cxa_throw(void * exc, std::type_info * tinfo, void (*dest)(void *))
{
    if (is_logic_error(tinfo))
        CXA_THROW_ON_LOGIC_ERROR();

    __real___cxa_throw(exc, tinfo, dest);

    __builtin_unreachable();
}
