#include <iomanip>
#include <ctime>
#include <map>
#include <string>
#include <variant>

#include "nix/flake/flake.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/flake/flakeref.hh"
#include "nix/util/ansicolor.hh"
#include "nix/util/fmt.hh"

namespace nix::flake {

static std::string describe(const FlakeRef & flakeRef)
{
    auto s = fmt("'%s'", flakeRef.to_string(true));

    if (auto lastModified = flakeRef.input.getLastModified())
        s += fmt(" (%s)", std::put_time(std::gmtime(&*lastModified), "%Y-%m-%d"));

    return s;
}

static std::string describe(const LockedFlake::LockEntry & entry)
{
    if (auto lockedRef = std::get_if<FlakeRef>(&entry))
        return describe(*lockedRef);
    else
        return fmt("follows '%s'", printInputAttrPath(std::get<InputAttrPath>(entry)));
}

std::string
diffLockedFlakes(const LockedFlake & oldLockedFlake, const LockedFlake & newLockedFlake, bool fetchTransitive)
{
    std::string res;

    if (oldLockedFlake.version() != newLockedFlake.version())
        res +=
            fmt("• " ANSI_BOLD "Updated lock file version from %d to %d" ANSI_NORMAL "\n",
                oldLockedFlake.version(),
                newLockedFlake.version());

    auto oldFlat = oldLockedFlake.getAllLockEntries(fetchTransitive);
    auto newFlat = newLockedFlake.getAllLockEntries(fetchTransitive);

    auto i = oldFlat.begin();
    auto j = newFlat.begin();

    while (i != oldFlat.end() || j != newFlat.end()) {
        if (j != newFlat.end() && (i == oldFlat.end() || i->first > j->first)) {
            res +=
                fmt("• " ANSI_GREEN "Added input '%s':" ANSI_NORMAL "\n    %s\n",
                    printInputAttrPath(j->first),
                    describe(j->second));
            ++j;
        } else if (i != oldFlat.end() && (j == newFlat.end() || i->first < j->first)) {
            res += fmt("• " ANSI_RED "Removed input '%s'" ANSI_NORMAL "\n", printInputAttrPath(i->first));
            ++i;
        } else {
            if (i->second != j->second) {
                res +=
                    fmt("• " ANSI_BOLD "Updated input '%s':" ANSI_NORMAL "\n    %s\n  → %s\n",
                        printInputAttrPath(i->first),
                        describe(i->second),
                        describe(j->second));
            }
            ++i;
            ++j;
        }
    }

    return res;
}

} // namespace nix::flake
