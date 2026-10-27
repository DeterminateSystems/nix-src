#include <gtest/gtest.h>

#ifndef _WIN32
#  include "nix/util/spawner.hh"
#endif

int main(int argc, char ** argv)
{
#ifndef _WIN32
    nix::runSpawnerIfRequested();
#endif

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
