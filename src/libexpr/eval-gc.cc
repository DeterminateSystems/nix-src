#include "nix/util/environment-variables.hh"
#include "nix/util/processes.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/util/config-global.hh"
#include "nix/expr/eval-gc.hh"
#include "nix/expr/value.hh"

#include "expr-config-private.hh"

#if NIX_USE_BOEHMGC

#  include <gc/gc_allocator.h>
#  include <gc/gc_tiny_fl.h> // For GC_GRANULE_BYTES

#  include "nix/util/coroutine-gc.hh"

#endif

namespace nix {

#if NIX_USE_BOEHMGC

/*
 * Ensure that Boehm satisfies our alignment requirements. This is the default configuration [^]
 * and this assertion should never break for any platform. Let's assert it just in case.
 *
 * This alignment is particularly useful to be able to use aligned
 * load/store instructions for loading/writing Values.
 *
 * [^]: https://github.com/bdwgc/bdwgc/blob/54ac18ccbc5a833dd7edaff94a10ab9b65044d61/include/gc/gc_tiny_fl.h#L31-L33
 */
static_assert(sizeof(void *) * 2 == GC_GRANULE_BYTES, "Boehm GC must use GC_GRANULE_WORDS = 2");

/* Called when the Boehm GC runs out of memory. */
static void * oomHandler(size_t requested)
{
    outOfMemory();
}

static size_t getFreeMem()
{
    /* On Linux, use the `MemAvailable` or `MemFree` fields from
       /proc/cpuinfo. */
#  ifdef __linux__
    {
        std::unordered_map<std::string, std::string> fields;
        for (auto & line :
             tokenizeString<std::vector<std::string>>(readFile(std::filesystem::path("/proc/meminfo")), "\n")) {
            auto colon = line.find(':');
            if (colon == line.npos)
                continue;
            fields.emplace(line.substr(0, colon), trim(line.substr(colon + 1)));
        }

        auto i = fields.find("MemAvailable");
        if (i == fields.end())
            i = fields.find("MemFree");
        if (i != fields.end()) {
            auto kb = tokenizeString<std::vector<std::string>>(i->second, " ");
            if (kb.size() == 2 && kb[1] == "kB")
                return string2Int<size_t>(kb[0]).value_or(0) * 1024;
        }
    }
#  endif

    /* On non-Linux systems, conservatively assume that 25% of memory is free. */
    long pageSize = sysconf(_SC_PAGESIZE);
    long pages = sysconf(_SC_PHYS_PAGES);
    if (pageSize > 0 && pages > 0)
        return (static_cast<size_t>(pageSize) * static_cast<size_t>(pages)) / 4;
    return 0;
}

/**
 * Implementations of the libutil coroutine GC hooks (see
 * `coroutine-gc.hh`) in terms of bdwgc's registered stacks. Together
 * with the fiber stack registration in `parallel-eval.cc`, this makes
 * every stack that can hold GC roots — thread stacks (registered
 * automatically), fiber stacks and coroutine stacks — scannable by
 * the collector, including the frames of a fiber that has switched
 * onto a coroutine stack.
 *
 * The invariant maintained here is that `GC_current_stack` is the
 * registered stack the thread is executing on, and that every other
 * stack holding live frames has a `saved_sp` that lies *within* that
 * stack. So the stack pointer is always recorded into
 * `GC_current_stack` (the stack actually being left), never into a
 * stack named by the caller: as explained in `coroutine-gc.hh`, a
 * coroutine may yield from another coroutine's stack, and recording
 * such a yield into the coroutine's own stack would make the
 * collector scan from one stack up to the base of another, running
 * into guard pages and unrelated mappings along the way.
 */

void gcSaveStackPointer(struct GC_stack * stk)
{
    auto sp = (char *) GC_get_approx_sp();
    /* Check the stack pointer before lowering it by the slack, so that
       a stack that is nearly exhausted (but whose guard page is not
       included in `limit`, as for thread stacks) doesn't trip the
       check. */
    if (!(sp < (char *) stk->base && (!stk->limit || sp >= (char *) stk->limit)))
        panic(
            fmt("stack pointer %p is not within the stack [%p, %p) that is being switched away from",
                (void *) sp,
                stk->limit,
                stk->base));
    stk->saved_sp = sp - gcStackSwitchSlack;
}

static void * coroStackRegisterImpl(void * base, size_t size)
{
    auto stk = new GC_stack{};
    stk->base = base;
    stk->limit = (char *) base - size;
    GC_register_stack(stk);
    return stk;
}

static void coroStackUnregisterImpl(void * cookie)
{
    auto stk = (struct GC_stack *) cookie;
    GC_unregister_stack(stk);
    delete stk;
}

static void * coroSwitchToImpl(void * cookie)
{
    auto prev = GC_current_stack;
    /* `prev` is null on threads not registered with the GC; such
       threads hold no GC roots and need no scanning. */
    if (prev)
        gcSaveStackPointer(prev);
    /* Provisional: the resumed coroutine may actually continue on
       another stack, in which case `coroResumeImpl()` corrects this
       right after the switch. The body start (`coroEnterImpl()`)
       corrects the null cookie of a coroutine that is being started. */
    GC_current_stack = (struct GC_stack *) cookie;
    return prev;
}

static void coroSwitchBackImpl(void * prevHandle)
{
    auto prev = (struct GC_stack *) prevHandle;
    GC_current_stack = prev;
    if (prev)
        prev->saved_sp = nullptr;
}

static void coroEnterImpl(void * cookie)
{
    GC_current_stack = (struct GC_stack *) cookie;
}

static void * coroYieldImpl()
{
    auto cur = GC_current_stack;
    if (cur)
        gcSaveStackPointer(cur);
    return cur;
}

static void coroResumeImpl(void * handle)
{
    auto cur = (struct GC_stack *) handle;
    GC_current_stack = cur;
    if (cur)
        cur->saved_sp = nullptr;
}

/**
 * A forked child process (e.g. a builtin builder, or an in-process
 * build sandbox running on a `clone()` stack) inherits the parent
 * thread's `GC_current_stack` and registered stacks, but it is not
 * executing on the stack that `GC_current_stack` refers to (if it
 * inherited the main thread's, it may be running on an entirely
 * different one) and it never runs the collector, so the stack
 * bookkeeping must not be done there: at best it is useless, at worst
 * it trips the consistency check in `gcSaveStackPointer()` or takes
 * the collector's lock, which may have been held by another thread of
 * the parent at the time of the fork. So disable the hooks in the
 * child.
 */
static RegisterForkCallback disableCoroutineGCHooks([]() {
    coroStackRegister = nullptr;
    coroStackUnregister = nullptr;
    coroSwitchTo = nullptr;
    coroSwitchBack = nullptr;
    coroEnter = nullptr;
    coroYield = nullptr;
    coroResume = nullptr;
    GC_current_stack = nullptr;
});

static inline void initGCReal()
{
    /* Initialise the Boehm garbage collector. */

    /* Don't look for interior pointers. This reduces the odds of
       misdetection a bit. */
    GC_set_all_interior_pointers(0);

    /* We don't have any roots in data segments, so don't scan from
       there. */
    GC_set_no_dls(1);

    /* Enable perf measurements. This is just a setting; not much of a
       start of something. */
    GC_start_performance_measurement();

    GC_INIT();

    /* Enable parallel marking. */
    GC_allow_register_threads();

    /* Register valid displacements in case we are using alignment niches
       for storing the type information. This way tagged pointers are considered
       to be valid, even when they are not aligned. */
    if constexpr (detail::useBitPackedValueStorage<sizeof(void *)>)
        for (std::size_t i = 1; i < sizeof(std::uintptr_t); ++i)
            GC_register_displacement(i);

    GC_set_oom_fn(oomHandler);

    /* Make the coroutine stacks of libutil scannable by the GC (fiber
       stacks are registered in `parallel-eval.cc`; thread stacks are
       registered automatically by bdwgc). */
    coroStackRegister = coroStackRegisterImpl;
    coroStackUnregister = coroStackUnregisterImpl;
    coroSwitchTo = coroSwitchToImpl;
    coroSwitchBack = coroSwitchBackImpl;
    coroEnter = coroEnterImpl;
    coroYield = coroYieldImpl;
    coroResume = coroResumeImpl;

    /* Funnel boehm warnings into debug logs. */
    GC_set_warn_proc([](const char * msg, GC_word word) noexcept {
        std::array<char, 4096> buffer{};
        auto res = snprintf(buffer.data(), buffer.size(), msg, word);
        /* Ignore garbage. */
        if (res < 0)
            return;

        try {
            debug("%s", chomp(std::string_view(buffer.data(), std::min<size_t>(res, buffer.size() - 1))));
        } catch (...) {
            /* Swallow all errors. */
        }
    });

    /* Set the initial heap size to something fairly big (80% of
       free RAM, up to a maximum of 4 GiB) so that in most cases
       we don't need to garbage collect at all.  (Collection has a
       fairly significant overhead.)  The heap size can be overridden
       through libgc's GC_INITIAL_HEAP_SIZE environment variable.  We
       should probably also provide a nix.conf setting for this.  Note
       that GC_expand_hp() causes a lot of virtual, but not physical
       (resident) memory to be allocated.  This might be a problem on
       systems that don't overcommit. */
    if (!getEnv("GC_INITIAL_HEAP_SIZE")) {
        size_t size = 32 * 1024 * 1024;
#  if HAVE_SYSCONF && defined(_SC_PAGESIZE) && defined(_SC_PHYS_PAGES)
        size_t maxSize = 4ULL * 1024 * 1024 * 1024;
        auto free = getFreeMem();
        size = std::max(size, std::min((size_t) (free * 0.5), maxSize));
#  endif
        GC_expand_hp(size);
    }
}

static size_t gcCyclesAfterInit = 0;

size_t getGCCycles()
{
    assertGCInitialized();
    return static_cast<size_t>(GC_get_gc_no()) - gcCyclesAfterInit;
}

#endif

static bool gcInitialised = false;

void initGC()
{
    if (gcInitialised)
        return;

#if NIX_USE_BOEHMGC
    initGCReal();

    gcCyclesAfterInit = GC_get_gc_no();
#endif

    // NIX_PATH must override the regular setting
    // See the comment in applyConfig
    if (auto nixPathEnv = getEnv("NIX_PATH")) {
        globalConfig.set("nix-path", concatStringsSep(" ", EvalSettings::parseNixPath(nixPathEnv.value())));
    }

    gcInitialised = true;
}

void assertGCInitialized()
{
    assert(gcInitialised);
}

} // namespace nix
