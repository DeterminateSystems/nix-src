#include "nix/util/compression.hh"
#include "nix/util/current-process.hh"
#include "nix/util/error.hh"
#include <gtest/gtest.h>
#include <thread>
#include <zstd.h>

namespace nix {

/* ----------------------------------------------------------------------------
 * compress / decompress
 * --------------------------------------------------------------------------*/

TEST(compress, noneMethodDoesNothingToTheInput)
{
    auto o = compress(CompressionAlgo::none, "this-is-a-test");

    ASSERT_EQ(o, "this-is-a-test");
}

struct CompressionDecompressionTest : ::testing::WithParamInterface<CompressionAlgo>, public ::testing::Test
{
    static constexpr std::string_view dummyInput = "slfja;sljfklsa;jfklsjfkl;sdjfkl;sadjfkl;sdjf;lsdfjsadlf";
};

TEST_P(CompressionDecompressionTest, roundtrip)
{
    auto o = decompress(GetParam(), compress(GetParam(), dummyInput));
    ASSERT_EQ(o, dummyInput);
}

TEST_P(CompressionDecompressionTest, empty)
{
    auto compressed = compress(GetParam(), "");
    if (GetParam() != CompressionAlgo::none) {
        ASSERT_FALSE(compressed.empty());
    }
    auto o = decompress(GetParam(), compressed);
    ASSERT_EQ(o, "");
}

TEST_P(CompressionDecompressionTest, invalidDecompression)
{
    if (GetParam() == CompressionAlgo::none)
        GTEST_SKIP();

    ASSERT_THROW(
        decompress(GetParam(), "this is a string that does not qualify as valid compressed data"), CompressionError);
}

TEST_P(CompressionDecompressionTest, roundtripsWithSourceAndSink)
{
    StringSink strSink;
    auto decompressionSink = makeDecompressionSink(CompressionAlgo::bzip2, strSink);
    auto sink = makeCompressionSink(CompressionAlgo::bzip2, *decompressionSink);

    (*sink)(dummyInput);
    sink->finish();
    decompressionSink->finish();

    ASSERT_EQ(strSink.s.c_str(), dummyInput);
}

TEST_P(CompressionDecompressionTest, failingSink)
{
    /* Like a full disk: every write fails, including the ones made while the compression sink is being destroyed. */
    struct FailingSink : Sink
    {
        void operator()(std::string_view data) override
        {
            throw SysError(ENOSPC, "write of %d bytes", data.size());
        }
    };

    /* Make the input incompressible enough to produce output before finish(). */
    std::string input(1024 * 1024, 0);
    uint32_t x = 1;
    for (auto & c : input) {
        x = x * 1664525 + 1013904223;
        c = x >> 24;
    }

    FailingSink failingSink;
    ASSERT_THROW(
        {
            auto sink = makeCompressionSink(GetParam(), failingSink);
            (*sink)(input);
            sink->finish();
        },
        SysError);
}

INSTANTIATE_TEST_SUITE_P(
    CompressionDecompression,
    CompressionDecompressionTest,
    ::testing::Values(
        CompressionAlgo::none,
        CompressionAlgo::xz,
        CompressionAlgo::bzip2,
        CompressionAlgo::brotli,
        CompressionAlgo::zstd),
    [](const ::testing::TestParamInfo<CompressionAlgo> & info) { return showCompressionAlgo(info.param); });

TEST(decompress, decompressNoneCompressed)
{
    auto str = "slfja;sljfklsa;jfklsjfkl;sdjfkl;sadjfkl;sdjf;lsdfjsadlf";
    auto o = decompress(CompressionAlgo::none, str);

    ASSERT_EQ(o, str);
}

TEST(decompress, decompressZstdCompressedParallel)
{
    auto str = "slfja;sljfklsa;jfklsjfkl;sdjfkl;sadjfkl;sdjf;lsdfjsadlf";
    auto o = decompress(CompressionAlgo::zstd, compress(CompressionAlgo::zstd, str, true));

    ASSERT_EQ(o, str);
}

TEST(decompress, decompressZstdMultiFrameLargeInput)
{
    // Create input larger than the 16 MiB frame boundary to exercise
    // multi-frame emission.
    std::string str(20 * 1024 * 1024, 'x');
    for (size_t i = 0; i < str.size(); i += 997)
        str[i] = 'y'; // add some variation
    auto compressed = compress(CompressionAlgo::zstd, str);
    auto o = decompress(CompressionAlgo::zstd, compressed);

    ASSERT_EQ(o, str);
}

TEST(compress, zstdExactFrameBoundary)
{
    // Input exactly equal to the 16 MiB frame boundary should not
    // emit a trailing zero-content frame.
    std::string str(16 * 1024 * 1024, 'z');
    auto compressed = compress(CompressionAlgo::zstd, str);
    auto o = decompress(CompressionAlgo::zstd, compressed);
    ASSERT_EQ(o, str);

    // Verify there is exactly one frame by checking that
    // the first frame's compressed size equals the total size.
    size_t frameSize = ZSTD_findFrameCompressedSize(compressed.data(), compressed.size());
    ASSERT_FALSE(ZSTD_isError(frameSize));
    ASSERT_EQ(frameSize, compressed.size());
}

/* ----------------------------------------------------------------------------
 * compression sinks
 * --------------------------------------------------------------------------*/

TEST(makeCompressionSink, noneSinkDoesNothingToInput)
{
    StringSink strSink;
    auto inputString = "slfja;sljfklsa;jfklsjfkl;sdjfkl;sadjfkl;sdjf;lsdfjsadlf";
    auto sink = makeCompressionSink(CompressionAlgo::none, strSink);
    (*sink)(inputString);
    sink->finish();

    ASSERT_STREQ(strSink.s.c_str(), inputString);
}

/* ----------------------------------------------------------------------------
 * parallel zstd
 * --------------------------------------------------------------------------*/

struct ZstdParallelTest : ::testing::TestWithParam<int>
{
    static constexpr size_t bytesPerFrame = 16 * 1024 * 1024;

    /* Compressible input spanning a full frame and a partial one:
       random-length pieces of a 64 KiB block of random numbers. */
    static std::string makeInput()
    {
        constexpr size_t size = bytesPerFrame + 1024 * 1024;
        uint32_t x = 1;
        auto next = [&]() {
            x = x * 1664525 + 1013904223;
            return x >> 8;
        };
        std::string block;
        while (block.size() < 64 * 1024)
            block += std::to_string(next()) + ' ';
        std::string s;
        s.reserve(size + block.size());
        while (s.size() < size) {
            auto start = next() % block.size();
            s.append(block, start, 64 + next() % 4096);
        }
        s.resize(size);
        return s;
    }

    /* Compress `in` like ZstdMultiFrameCompressionSink does, using
       libzstd directly with the given multi-threading parameters. */
    static std::string compressFrames(std::string_view in, int level, int nbWorkers, int jobSize, int overlapLog)
    {
        std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> cctx{ZSTD_createCCtx(), ZSTD_freeCCtx};
        auto check = [](size_t ret) {
            if (ZSTD_isError(ret))
                throw Error("zstd error: %s", ZSTD_getErrorName(ret));
        };
        if (level != -1)
            check(ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_compressionLevel, level));
        check(ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_nbWorkers, nbWorkers));
        check(ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_jobSize, jobSize));
        check(ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_overlapLog, overlapLog));
        std::string out(ZSTD_compressBound(in.size()) + 1024, '\0');
        ZSTD_outBuffer outBuf{out.data(), out.size(), 0};
        while (!in.empty()) {
            auto frame = in.substr(0, bytesPerFrame);
            in.remove_prefix(frame.size());
            check(ZSTD_CCtx_reset(cctx.get(), ZSTD_reset_session_only));
            check(ZSTD_CCtx_setPledgedSrcSize(cctx.get(), frame.size()));
            ZSTD_inBuffer inBuf{frame.data(), frame.size(), 0};
            size_t remaining;
            do {
                remaining = ZSTD_compressStream2(cctx.get(), &outBuf, &inBuf, ZSTD_e_end);
                check(remaining);
            } while (remaining != 0);
        }
        out.resize(outBuf.pos);
        return out;
    }
};

TEST_P(ZstdParallelTest, roundtripsAcrossFrames)
{
    auto input = makeInput();
    auto compressed = compress(CompressionAlgo::zstd, input, true, GetParam());

    size_t frames = 0;
    for (std::string_view rest = compressed; !rest.empty(); ++frames) {
        auto frameSize = ZSTD_findFrameCompressedSize(rest.data(), rest.size());
        ASSERT_FALSE(ZSTD_isError(frameSize));
        rest.remove_prefix(frameSize);
    }
    ASSERT_EQ(frames, 2u);

    ASSERT_TRUE(decompress(CompressionAlgo::zstd, compressed) == input);
}

/* With 2 or more CPUs, the sink compresses each 16 MiB frame as 4 MiB
   jobs with a full-window overlap.  zstd's output depends on the job
   size but not on the number of workers, so the result must be
   byte-identical to libzstd's with those parameters and any number of
   workers: the compressed NAR, and thus its file hash, doesn't depend
   on the CPU count of the machine that compressed it. */
TEST_P(ZstdParallelTest, sameOutputForAnyWorkerCount)
{
    auto ncpu = getMaxCPU();
    if (ncpu == 0)
        ncpu = std::thread::hardware_concurrency();
    if (ncpu < 2)
        GTEST_SKIP() << "the zstd sink only uses worker threads with 2 or more CPUs";
    {
        std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> cctx{ZSTD_createCCtx(), ZSTD_freeCCtx};
        if (ZSTD_isError(ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_nbWorkers, 2)))
            GTEST_SKIP() << "libzstd was built without multi-threading support";
    }

    auto level = GetParam();
    auto input = makeInput();
    auto compressed = compress(CompressionAlgo::zstd, input, true, level);

    for (int nbWorkers = 1; nbWorkers <= 4; ++nbWorkers) {
        auto expected = compressFrames(input, level, nbWorkers, 4 * 1024 * 1024, 9);
        EXPECT_EQ(expected.size(), compressed.size()) << "nbWorkers = " << nbWorkers;
        EXPECT_TRUE(expected == compressed) << "nbWorkers = " << nbWorkers;
    }

    /* zstd's default job size gives a different output, so the above
       does check the parameters the sink uses. */
    EXPECT_FALSE(compressFrames(input, level, 2, 0, 0) == compressed);
}

INSTANTIATE_TEST_SUITE_P(
    makeCompressionSink, ZstdParallelTest, ::testing::Values(-1, 1, 9), [](const ::testing::TestParamInfo<int> & info) {
        return info.param == -1 ? std::string("defaultLevel") : "level" + std::to_string(info.param);
    });

} // namespace nix
