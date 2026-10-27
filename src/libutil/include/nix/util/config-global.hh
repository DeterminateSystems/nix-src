#pragma once
///@file

#include "nix/util/configuration.hh"

namespace nix {

class GlobalConfig : public AbstractConfig
{
    void anchor() override;

public:
    typedef std::vector<Config *> ConfigRegistrations;

    static ConfigRegistrations & configRegistrations();

    bool set(const std::string & name, const std::string & value) override;

    void getSettings(std::map<std::string, SettingInfo> & res, bool overriddenOnly = false) const override;

    void resetOverridden() override;

    nlohmann::json toJSON() override;

    std::string toKeyValue() override;

    void convertToArgs(Args & args, const std::string & category) override;

    struct Register
    {
        Register(Config * config);
    };
};

extern GlobalConfig globalConfig;

struct Sink;
struct Source;

/**
 * Serialise the settings of all registered `Config`s, e.g. to pass
 * them to a child process.
 */
void writeGlobalSettings(Sink & sink);

/**
 * Read settings written by `writeGlobalSettings()` and apply them.
 * Only those that differ from ours are applied: the rest are
 * presumably defaults (or set by the same configuration files), and
 * setting a deprecated setting warns even if it's to its default.
 * (Note that the sender can't just send its overridden settings:
 * things like `--store` set the setting without marking it as
 * overridden.)
 */
void applyGlobalSettings(Source & source);

} // namespace nix
