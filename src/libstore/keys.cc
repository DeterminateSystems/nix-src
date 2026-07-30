#include "nix/util/file-system.hh"
#include "nix/store/globals.hh"
#include "nix/util/signature/local-keys.hh"
#include "nix/store/keys.hh"

namespace nix {

PublicKeys getDefaultPublicKeys()
{
    PublicKeys publicKeys;

    // FIXME: filter duplicates

    for (const auto & s : settings.trustedPublicKeys.get()) {
        auto key = PublicKey::parse(s);
        auto name = key->name;
        publicKeys.emplace(name, std::move(key));
    }

    // FIXME: keep secret keys in memory (see Store::signRealisation()).
    auto keystoreEnabled = experimentalFeatureSettings.isEnabled(Xp::Keystore);
    for (const auto & secretKeyFile : settings.secretKeyFiles.get()) {
        try {
            auto isUri = keystoreEnabled && !std::get<0>(splitColon(secretKeyFile)).empty();
            auto secretKey = SecretKey::parse(isUri ? secretKeyFile : readFile(secretKeyFile), isUri);
            publicKeys.emplace(secretKey->name, secretKey->toPublicKey());
        } catch (SystemError & e) {
            /* Ignore unreadable key files. That's normal in a
               multi-user installation. */
        }
    }

    return publicKeys;
}

} // namespace nix
