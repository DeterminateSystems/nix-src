#include <cstdlib>
#include <typeinfo>

#include "is-logic-error.hh"

extern "C" void __real___cxa_throw(void *, std::type_info *, void (*)(void *));

extern "C" void __wrap___cxa_throw(void * exc, std::type_info * tinfo, void (*dest)(void *))
{
    if (is_logic_error(tinfo))
        abort_on_exception(exc, tinfo);

    __real___cxa_throw(exc, tinfo, dest);

    __builtin_unreachable();
}
