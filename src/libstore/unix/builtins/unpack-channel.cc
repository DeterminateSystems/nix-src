#include "builtins.hh"
#include "tarfile.hh"

namespace nix {

void builtinUnpackChannel(
    const BasicDerivation & drv,
    const std::map<std::string, Path> & outputs)
{
    auto getAttr = [&](const std::string & name) {
        auto i = drv.env.find(name);
        if (i == drv.env.end()) throw Error("attribute '%s' missing", name);
        return i->second;
    };

    auto out = outputs.at("out");
    auto channelName = getAttr("channelName");
    auto src = getAttr("src");

    createDirs(out);

    unpackTarfile(src, out);

    auto entries = std::filesystem::directory_iterator{out};
    auto file_count = std::distance(entries, std::filesystem::directory_iterator{});

    if (file_count != 1)
        throw Error("channel tarball '%s' contains more than one file", src);
    renameFile(entries->path().string(), (out + "/" + channelName));
}

}
