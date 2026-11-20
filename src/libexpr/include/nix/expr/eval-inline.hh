#pragma once
///@file

#include "nix/expr/print.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-error.hh"
#include "nix/expr/eval-settings.hh"
#include <exception>

namespace nix {

/**
 * Note: Various places expect the allocated memory to be zeroed.
 */
[[gnu::always_inline]]
inline void * EvalMemory::allocBytes(size_t n)
{
    void * p;
#if NIX_USE_BOEHMGC
    p = GC_MALLOC(n);
#else
    p = calloc(n, 1);
#endif
    if (!p)
        outOfMemory();
    return p;
}

namespace detail {

/**
 * @return count * sizeof(T) + N
 * @throws bad_alloc if the computation would overflow.
 */
template<typename T, std::size_t N>
[[gnu::always_inline]]
inline size_t checkedAllocSize(size_t count)
{
    size_t res = 0;

    if (__builtin_mul_overflow(count, sizeof(T), &res)) [[unlikely]]
        outOfMemory();

    if (__builtin_add_overflow(res, N, &res)) [[unlikely]]
        outOfMemory();

    return res;
}

} // namespace detail

template<typename T, std::size_t N>
[[gnu::always_inline]]
inline void * EvalMemory::allocBytes(size_t count)
{
    return allocBytes(detail::checkedAllocSize<T, N>(count));
}

[[gnu::always_inline]]
inline void * EvalMemory::allocBytesAtomic(size_t n)
{
    void * p = GC_MALLOC_ATOMIC(n);
    if (!p)
        throw std::bad_alloc();
    return p;
}

template<typename T, std::size_t N>
[[gnu::always_inline]]
inline void * EvalMemory::allocBytesAtomic(size_t count)
{
    return allocBytesAtomic(detail::checkedAllocSize<T, N>(count));
}

[[gnu::always_inline]]
Value * EvalMemory::allocValue()
{
#if NIX_USE_BOEHMGC
    /* Allocation cache for GC'd Value objects. Boehm GC is already a global resource, so thread_local is
       a natural solution. Multiple EvalState instances on the same thread will reuse the same cache. */
    [[gnu::tls_model("initial-exec")]] static thread_local std::shared_ptr<void *> valueAllocCache{
        std::allocate_shared<void *>(traceable_allocator<void *>(), nullptr)};

    /* We use the boehm batch allocator to speed up allocations of Values (of which there are many).
       GC_malloc_many returns a linked list of objects of the given size, where the first word
       of each object is also the pointer to the next object in the list. This also means that we
       have to explicitly clear the first word of every object we take. */
    if (!*valueAllocCache) {
        *valueAllocCache = GC_malloc_many(sizeof(Value));
        if (!*valueAllocCache)
            outOfMemory();
    }

    /* GC_NEXT is a convenience macro for accessing the first word of an object.
       Take the first list item, advance the list to the next item, and clear the next pointer. */
    void * p = *valueAllocCache;
    *valueAllocCache = GC_NEXT(p);
    GC_NEXT(p) = nullptr;
#else
    void * p = allocBytes(sizeof(Value));
#endif

    stats.nrValues++;
    return (Value *) p;
}

[[gnu::always_inline]]
Env & EvalMemory::allocEnv(size_t size)
{
    stats.nrEnvs++;
    stats.nrValuesInEnvs += size;

    Env * env;

#if NIX_USE_BOEHMGC
    if (size == 1) {
        /* Allocation cache for size-1 Env objects. Boehm GC is already a global resource, so thread_local is
           a natural solution. Multiple EvalState instances on the same thread will reuse the same cache. */
        [[gnu::tls_model("initial-exec")]] static thread_local std::shared_ptr<void *> env1AllocCache{
            std::allocate_shared<void *>(traceable_allocator<void *>(), nullptr)};
        /* see allocValue for explanations. */
        if (!*env1AllocCache) {
            *env1AllocCache = GC_malloc_many(sizeof(Env) + sizeof(Value *));
            if (!*env1AllocCache)
                outOfMemory();
        }

        void * p = *env1AllocCache;
        *env1AllocCache = GC_NEXT(p);
        GC_NEXT(p) = nullptr;
        env = (Env *) p;
    } else
#endif
        env = (Env *) allocBytes<Value *, sizeof(Env)>(size);

    /* We assume that env->values has been cleared by the allocator; maybeThunk() and lookupVar fromWith expect this. */

    return *env;
}

/**
 * An identifier of the current thread for deadlock detection, stored
 * in p0 of pending/awaited thunks. We're not using std::thread::id
 * because it's not guaranteed to fit.
 */
[[gnu::tls_model("initial-exec")]] extern thread_local uint32_t myEvalThreadId;

template<std::size_t ptrSize>
void ValueStorage<ptrSize, std::enable_if_t<detail::useBitPackedValueStorage<ptrSize>>>::force(
    EvalState & state, PosIdx pos)
{
    auto p0_ = p0.load(std::memory_order_acquire);

    auto pd = static_cast<PrimaryDiscriminator>(p0_ & discriminatorMask);

    if (pd == pdThunk) {
        try {
            // The value we get here is only valid if we can set the
            // thunk to pending.
            auto p1_ = p1;

            // Atomically set the thunk to "pending".
            if (!p0.compare_exchange_strong(
                    p0_,
                    pdPending | (myEvalThreadId << discriminatorBits),
                    std::memory_order_acquire,
                    std::memory_order_acquire)) {
                pd = static_cast<PrimaryDiscriminator>(p0_ & discriminatorMask);
                if (pd == pdPending || pd == pdAwaited) {
                    // The thunk is already "pending" or "awaited", so
                    // we need to wait for it.
                    p0_ = waitOnThunk(state, p0_);
                    goto done;
                }
                assert(pd != pdThunk);
                // Another thread finished this thunk, no need to wait.
                goto done;
            }

            bool isApp = p1_ & discriminatorMask;
            if (isApp) {
                auto left = untagPointer<Value *>(p0_);
                auto right = untagPointer<Value *>(p1_);
                state.callFunction(*left, *right, (Value &) *this, pos);
            } else {
                auto env = untagPointer<Env *>(p0_);
                auto expr = untagPointer<Expr *>(p1_);
                expr->eval(state, *env, (Value &) *this);
            }
        } catch (InfiniteRecursionError & e) {
            /* Note: this is a separate handler (rather than a check
               in the `catch (...)` below) because figuring out the
               type of the current exception there would require
               rethrowing it, which is expensive to do for every
               thunk that an error propagates through. */
            state.fixupBlackHolePos(e, (Value &) *this, pos);
            setStorage(new Value::Failed{.ex = std::current_exception()});
            throw;
        } catch (...) {
            setStorage(new Value::Failed{.ex = std::current_exception()});
            throw;
        }
    }

    else if (pd == pdPending || pd == pdAwaited)
        p0_ = waitOnThunk(state, p0_);

done:
    if (InternalType(p0_ & 0xff) == tFailed)
        std::rethrow_exception((std::bit_cast<Failed *>(p1))->ex);
}

[[gnu::always_inline]]
inline void EvalState::forceAttrs(Value & v, const PosIdx pos, std::string_view errorCtx)
{
    forceAttrs(v, [&]() { return pos; }, errorCtx);
}

template<typename Callable>
[[gnu::always_inline]]
inline void EvalState::forceAttrs(Value & v, Callable getPos, std::string_view errorCtx)
{
    PosIdx pos = getPos();
    forceValue(v, pos);
    if (v.type() != nAttrs) {
        error<TypeError>("expected a set but found %1%: %2%", showType(v), ValuePrinter(*this, v, errorPrintOptions))
            .withTrace(pos, errorCtx)
            .debugThrow();
    }
}

[[gnu::always_inline]]
inline void EvalState::forceList(Value & v, const PosIdx pos, std::string_view errorCtx)
{
    forceValue(v, pos);
    if (!v.isList()) {
        error<TypeError>("expected a list but found %1%: %2%", showType(v), ValuePrinter(*this, v, errorPrintOptions))
            .withTrace(pos, errorCtx)
            .debugThrow();
    }
}

[[gnu::always_inline]]
inline CallDepth EvalState::addCallDepth(const PosIdx pos)
{
    if (CallDepth::callDepth > settings.maxCallDepth)
        error<StackOverflowError>().atPos(pos).debugThrow();

    return CallDepth();
};

template<typename Cb>
std::invoke_result_t<Cb, Value *, bool>
EvalState::peelToStringOutPath(const PosIdx pos, Value & v, bool checkToStringReturn, Cb && cb)
{
    using R = std::invoke_result_t<Cb, Value *, bool>;
    bool cameThroughToString = false;

    auto peel = [&, &state = *this](this const auto & peel, const PosIdx pos, Value & v) -> R {
        state.forceValue(v, pos);
        auto vType = v.type();
        if (vType != nAttrs) {
            /* Trivially string-coercible types (i.e. coerceMore = false compatible)
               get to return happily.
               String and path terminate happily. External needs to be handled
               by caller. E.g. `printValueAsJSON` routes to `ExternalValueBase::printValueAsJSON`,
               `coerceToString` to `ExternalValueBase::coerceToString`).
               Everything else violates the "`__toString` returns a
               string" contract.
               */
            if (checkToStringReturn && cameThroughToString && vType != nString && vType != nPath && vType != nExternal)
                state
                    .error<TypeError>(
                        "`__toString` should return a string, but got %1%: %2%",
                        showType(v),
                        ValuePrinter(state, v, errorPrintOptions))
                    .atPos(pos)
                    .debugThrow();
            return std::forward<Cb>(cb)(&v, cameThroughToString);
        }
        if (auto i = v.attrs()->get(state.s.toString)) {
            Value & v1 = *state.allocValue();
            try {
                state.callFunction(*i->value, v, v1, i->pos);
            } catch (Error & e) {
                e.addTrace(state.positions[i->pos], "while calling the `__toString` attribute");
                throw;
            }
            cameThroughToString = true;
            try {
                auto _level = state.addCallDepth(pos);
                return peel(i->pos, v1);
            } catch (Error & e) {
                e.addTrace(
                    state.positions[i->pos], "while %s the result of the `__toString` attribute", WhileTryingToUse{v1});
                throw;
            }
        }
        if (auto i = v.attrs()->get(state.s.outPath)) {
            try {
                state.forceValue(*i->value, i->pos);
                auto _level = state.addCallDepth(pos);
                return peel(i->pos, *i->value);
            } catch (Error & e) {
                e.addTrace(state.positions[i->pos], "while %s the `outPath` attribute", WhileTryingToUse{*i->value});
                throw;
            }
        }
        /* Can't peel more. (no `__toString`, no `outPath`).
           If we came through a `__toString` call at any depth, this violates
           the string-return contract. (usually not enforced) */
        if (checkToStringReturn && cameThroughToString)
            state
                .error<TypeError>(
                    "`__toString` must return a string, but got %1%: %2%",
                    showType(v),
                    ValuePrinter(state, v, errorPrintOptions))
                .atPos(pos)
                .debugThrow();
        return std::forward<Cb>(cb)(&v, cameThroughToString);
    };
    return peel(pos, v);
}

} // namespace nix
