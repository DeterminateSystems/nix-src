#include "nix/cmd/command.hh"
#include "nix/store/bloom-filter.hh"
#include "nix/store/store-api.hh"
#include "nix/util/file-system.hh"
#include "nix/util/serialise.hh"
#include "nix/util/strings.hh"

#include <unistd.h>

using namespace nix;

struct CmdGenerateBloomFilter : StoreCommand
{
    std::optional<std::filesystem::path> fromFile;
    double falsePositiveRate = 0.01;

    CmdGenerateBloomFilter()
    {
        addFlag({
            .longName = "from-file",
            .description = "Read newline-separated store paths from *file* instead of "
                           "enumerating every valid path in the store.",
            .labels = {"file"},
            .handler = {[this](std::string s) { fromFile = s; }},
        });
        addFlag({
            .longName = "false-positive-rate",
            .description = "Target false-positive rate (default: 0.01).",
            .labels = {"rate"},
            .handler = {[this](std::string s) { falsePositiveRate = std::stod(s); }},
        });
    }

    std::string description() override
    {
        return "build a Bloom filter from the store's valid paths";
    }

    Category category() override
    {
        return catUndocumented;
    }

    void run(ref<Store> store) override
    {
        auto fd = getStandardOutput();
        if (isatty(fd))
            throw UsageError("refusing to write Bloom filter to a terminal");

        StorePathSet paths;
        if (fromFile) {
            for (auto & line : tokenizeString<Strings>(readFile(*fromFile), "\n")) {
                auto trimmed = trim(line);
                if (trimmed.empty())
                    continue;
                paths.insert(store->parseStorePath(trimmed));
            }
        } else {
            paths = store->queryAllValidPaths();
        }

        auto blob = buildBloomFilter(paths, falsePositiveRate);

        FdSink sink(std::move(fd));
        sink(blob);
        sink.flush();

        notice(
            "Wrote Bloom filter (%d bytes) for %d store paths (%f false positive rate).",
            blob.size(),
            paths.size(),
            falsePositiveRate);
    }
};

static auto rCmdGenerateBloomFilter = registerCommand2<CmdGenerateBloomFilter>({"store", "generate-bloom-filter"});
