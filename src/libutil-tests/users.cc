#include <gtest/gtest.h>

#include "nix/util/environment-variables.hh"
#include "nix/util/users.hh"

namespace nix {

/**
 * Set (or unset, if `value` is nullopt) an environment variable for the
 * lifetime of this object, restoring the previous value afterwards.
 */
struct ScopedEnv
{
    std::string name;
    std::optional<std::string> oldValue;

    ScopedEnv(std::string name, std::optional<std::string> value)
        : name(std::move(name))
        , oldValue(getEnv(this->name))
    {
        set(value);
    }

    ~ScopedEnv()
    {
        set(oldValue);
    }

    void set(const std::optional<std::string> & value)
    {
        if (value)
            setEnv(name.c_str(), value->c_str());
        else
            unsetenv(name.c_str());
    }
};

TEST(getEnvOsAbsPath, unset)
{
    ScopedEnv env("NIX_TEST_ABS_PATH", std::nullopt);
    ASSERT_EQ(getEnvOsAbsPath(OS_STR("NIX_TEST_ABS_PATH")), std::nullopt);
}

TEST(getEnvOsAbsPath, empty)
{
    ScopedEnv env("NIX_TEST_ABS_PATH", "");
    ASSERT_EQ(getEnvOsAbsPath(OS_STR("NIX_TEST_ABS_PATH")), std::nullopt);
}

TEST(getEnvOsAbsPath, relative)
{
    ScopedEnv env("NIX_TEST_ABS_PATH", "foo/bar");
    ASSERT_EQ(getEnvOsAbsPath(OS_STR("NIX_TEST_ABS_PATH")), std::nullopt);
}

#ifndef _WIN32

TEST(getEnvOsAbsPath, absolute)
{
    ScopedEnv env("NIX_TEST_ABS_PATH", "/foo/bar");
    ASSERT_EQ(getEnvOsAbsPath(OS_STR("NIX_TEST_ABS_PATH")), std::filesystem::path("/foo/bar"));
}

TEST(getCacheDir, nixCacheHome)
{
    ScopedEnv env1("NIX_CACHE_HOME", "/nix-cache");
    ScopedEnv env2("XDG_CACHE_HOME", "/xdg-cache");
    ASSERT_EQ(getCacheDir(), std::filesystem::path("/nix-cache"));
}

TEST(getCacheDir, ignoresEmptyOrRelativeNixCacheHome)
{
    ScopedEnv env2("XDG_CACHE_HOME", "/xdg-cache");
    for (auto value : {"", "relative"}) {
        ScopedEnv env1("NIX_CACHE_HOME", value);
        ASSERT_EQ(getCacheDir(), std::filesystem::path("/xdg-cache/nix"));
    }
}

/**
 * An empty or relative `$XDG_CACHE_HOME` must not cause a cache
 * directory relative to the current directory
 * (https://github.com/DeterminateSystems/nix-src/issues/626).
 */
TEST(getCacheDir, ignoresEmptyOrRelativeXdgCacheHome)
{
    ScopedEnv env1("NIX_CACHE_HOME", std::nullopt);
    for (auto value : {"", "relative"}) {
        ScopedEnv env2("XDG_CACHE_HOME", value);
        auto dir = getCacheDir();
        ASSERT_TRUE(dir.is_absolute()) << dir;
        ASSERT_EQ(dir, getHome() / ".cache" / "nix");
    }
}

TEST(getConfigDirs, ignoresRelativeXdgConfigDirs)
{
    ScopedEnv env1("NIX_CONFIG_HOME", "/nix-config");
    ScopedEnv env2("XDG_CONFIG_DIRS", "relative::/etc/foo");
    ASSERT_EQ(getConfigDirs(), (std::vector<std::filesystem::path>{"/nix-config", "/etc/foo/nix"}));
}

#endif

} // namespace nix
