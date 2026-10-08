#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

TEST(CxaThrow, catchesLogicErrorFromStdlib)
{
    const char * volatile p = nullptr;
    ASSERT_DEATH({ std::string s(p); }, "");
}

TEST(CxaThrow, catchesLogicError)
{
    ASSERT_DEATH({ throw std::logic_error("test"); }, "");
}

TEST(CxaThrow, catchesOutOfRange)
{
    ASSERT_DEATH({ throw std::out_of_range("test"); }, "");
}

TEST(CxaThrow, catchesInvalidArgument)
{
    ASSERT_DEATH({ throw std::invalid_argument("test"); }, "");
}

TEST(CxaThrow, catchesDomainError)
{
    ASSERT_DEATH({ throw std::domain_error("test"); }, "");
}

TEST(CxaThrow, catchesLengthError)
{
    ASSERT_DEATH({ throw std::length_error("test"); }, "");
}
