#include <cstdlib>
#include <dlfcn.h>
#include <typeinfo>

#include "is-logic-error.hh"

#ifndef CXA_THROW_ON_LOGIC_ERROR
#  define CXA_THROW_ON_LOGIC_ERROR() abort()
#endif

typedef void (*cxa_throw_type)(void *, std::type_info *, void (*)(void *));

extern "C" void __cxa_throw(void * exc, std::type_info * tinfo, void (*dest)(void *))
{
    if (is_logic_error(tinfo))
        CXA_THROW_ON_LOGIC_ERROR();

    static auto * orig = (cxa_throw_type) dlsym(RTLD_NEXT, "__cxa_throw");
    if (!orig)
        abort();

    orig(exc, tinfo, dest);

    __builtin_unreachable();
}
