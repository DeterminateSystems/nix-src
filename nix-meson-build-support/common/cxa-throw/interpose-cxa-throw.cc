#include <cstdlib>
#include <dlfcn.h>
#include <typeinfo>

#include "is-logic-error.hh"

typedef void (*cxa_throw_type)(void *, std::type_info *, void (*)(void *));

extern "C" void __cxa_throw(void * exc, std::type_info * tinfo, void (*dest)(void *))
{
    if (is_logic_error(tinfo))
        abort_on_exception(exc, tinfo);

    static auto * orig = (cxa_throw_type) dlsym(RTLD_NEXT, "__cxa_throw");
    if (!orig)
        abort();

    orig(exc, tinfo, dest);

    __builtin_unreachable();
}
