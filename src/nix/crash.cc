#include "nix/cmd/command.hh"

#include <bitset>
#include <limits>

using namespace nix;

static void doThrow()
{
    throw Error("exception in destructor");
}

/**
 * Recurse until the stack overflows. Every frame is big and is first
 * accessed at its lowest address, so the stack pointer is outside of
 * the stack mapping when the overflow happens, as is often the case
 * for real stack overflows. The check on `depth` is never true, but
 * it keeps compilers from flagging the infinite recursion.
 */
[[gnu::noinline]] static size_t overflowStack(size_t depth)
{
    volatile char buf[64 * 1024];
    buf[0] = depth;
    if (depth == std::numeric_limits<size_t>::max())
        return 0;
    return overflowStack(depth + 1) + buf[0];
}

struct CmdCrash : Command
{
    std::string type;

    CmdCrash()
    {
        expectArg("type", &type);
    }

    Category category() override
    {
        return catUndocumented;
    }

    std::string description() override
    {
        return "crash the program to test crash reporting";
    }

    void run() override
    {
        if (type == "segfault") {
            printError("Triggering a segfault...");
            volatile int * p = nullptr;
            *p = 123;
        }

        else if (type == "assert") {
            printError("Triggering an assertion failure...");
            assert(false && "This is an assertion failure");
        }

        else if (type == "logic-error") {
            printError("Triggering a C++ logic error...");
            std::bitset<4>{"012"};
        }

        else if (type == "panic") {
            printError("Triggering a panic...");
            panic("test panic");
        }

        else if (type == "stack-overflow") {
            printError("Triggering a stack overflow...");
            overflowStack(0);
        }

        else if (type == "abort") {
            printError("Triggering an abort...");
            std::abort();
        }

        else if (type == "terminate") {
            printError("Triggering an std::terminate...");

            struct Foo
            {
                ~Foo()
                {
                    doThrow();
                }
            };

            Foo foo;
        }

        else {
            throw Error("unknown crash type '%s'", type);
        }
    }
};

static auto rCrash = registerCommand<CmdCrash>("__crash");
