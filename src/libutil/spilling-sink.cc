#include "nix/util/spilling-sink.hh"
#include "nix/util/file-system.hh"

#include <cassert>

namespace nix {

void SpillingStringSink::operator()(std::string_view data)
{
    assert(!finished);

    written += data.size();

    if (!fileSink && memory.s.size() + data.size() <= maxMemory) {
        memory(data);
        return;
    }

    if (!fileSink) {
        fd = createAnonymousTempFile();
        fileSink.emplace(fd.get());
        (*fileSink)(memory.s);
        memory.s = {};
    }

    (*fileSink)(data);
}

std::unique_ptr<Source> SpillingStringSink::getSource()
{
    assert(!finished);
    finished = true;

    if (fileSink) {
        fileSink->flush();
        auto source = std::make_unique<FdSource>(fd.get());
        source->restart();
        return source;
    }

    return std::make_unique<StringSource>(std::move(memory.s));
}

} // namespace nix
