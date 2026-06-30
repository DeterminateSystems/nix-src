#pragma once

#include <unistd.h>
#include <cxxabi.h>
#include <stdexcept>
#include <typeinfo>
#include <cstring>

#ifndef CXA_THROW_ON_LOGIC_ERROR
#  define CXA_THROW_ON_LOGIC_ERROR() abort()
#endif

static bool is_logic_error(const std::type_info * tinfo)
{
    if (*tinfo == typeid(std::logic_error))
        return true;

    auto * si = dynamic_cast<const __cxxabiv1::__si_class_type_info *>(tinfo);
    if (si)
        return is_logic_error(si->__base_type);

    return false;
}

static void abort_on_exception(void * exc, const std::type_info * tinfo)
{
    if (!is_logic_error(tinfo))
        return;

    char buf[512];
    snprintf(
        buf,
        sizeof(buf),
        "Aborting on unexpected exception of type '%s', error: %s\n",
        tinfo->name(),
        ((std::exception *) exc)->what());
    [[maybe_unused]] auto r = write(STDERR_FILENO, buf, strlen(buf));

    CXA_THROW_ON_LOGIC_ERROR();
}
