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
        return "build a bloom filter from the store's valid paths";
    }

    Category category() override
    {
        return catUndocumented;
    }

    void run(ref<Store> store) override
    {
        auto fd = getStandardOutput();
        if (isatty(fd))
            throw UsageError("refusing to write bloom filter to a terminal");

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
            "Wrote bloom filter (%d bytes) for %d store paths (%f false positive rate).",
            blob.size(),
            paths.size(),
            falsePositiveRate);

#if 0
        /* Self-check the empirical false-positive rate by probing the
           just-built filter with 10 000 random store paths. */
        auto readU64 = [&](size_t off) {
            uint64_t v = 0;
            for (int i = 0; i < 8; ++i)
                v |= uint64_t((unsigned char) blob[off + i]) << (8 * i);
            return v;
        };
        uint32_t k = uint32_t(readU64(16));
        uint64_t mBits = readU64(24);
        const char * bits = blob.data() + 32;

        constexpr size_t numSamples = 1000000;
        size_t falsePositives = 0;
        for (size_t i = 0; i < numSamples; ++i) {
            auto p = StorePath::random("nix-bloom-fpr-probe");
            bool allSet = true;
            forEachBloomBitPosition(p, k, mBits, [&](uint64_t pos) {
                if (!((uint8_t(bits[pos / 8]) >> (pos % 8)) & 1))
                    allSet = false;
            });
            if (allSet)
                ++falsePositives;
        }
        notice(
            "Empirical false-positive rate over %d random probes: %d (%f, target %f).",
            numSamples,
            falsePositives,
            double(falsePositives) / double(numSamples),
            falsePositiveRate);
#endif
    }
};

static auto rCmdGenerateBloomFilter = registerCommand2<CmdGenerateBloomFilter>({"store", "generate-bloom-filter"});
