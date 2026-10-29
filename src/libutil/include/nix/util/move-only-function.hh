#pragma once
///@file

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace nix {

#ifdef __cpp_lib_move_only_function

template<typename Sig>
using MoveOnlyFunction = std::move_only_function<Sig>;

#else

/**
 * Fallback implementation of `std::move_only_function` for standard
 * libraries that don't provide it yet (e.g. libc++ as of version 21):
 * a type-erased callable wrapper that, unlike `std::function`, only
 * requires the callable to be move-constructible, so it can hold
 * lambdas that capture move-only types.
 */
template<typename Sig>
class MoveOnlyFunction;

template<typename Ret, typename... Args>
class MoveOnlyFunction<Ret(Args...)>
{
    struct Base
    {
        virtual Ret call(Args &&... args) = 0;
        virtual ~Base() = default;
    };

    template<typename F>
    struct Impl final : Base
    {
        F f;

        template<typename G>
            requires std::is_same_v<std::decay_t<G>, F>
        Impl(G && g)
            : f(std::forward<G>(g))
        {
        }

        Ret call(Args &&... args) override
        {
            return f(std::forward<Args>(args)...);
        }
    };

    std::unique_ptr<Base> impl;

public:
    MoveOnlyFunction() = default;

    template<typename F>
        requires(!std::is_same_v<std::decay_t<F>, MoveOnlyFunction> && std::is_invocable_r_v<Ret, F &, Args...>)
    MoveOnlyFunction(F && f)
        : impl(std::make_unique<Impl<std::decay_t<F>>>(std::forward<F>(f)))
    {
    }

    MoveOnlyFunction(MoveOnlyFunction &&) noexcept = default;
    MoveOnlyFunction & operator=(MoveOnlyFunction &&) noexcept = default;

    Ret operator()(Args... args)
    {
        return impl->call(std::forward<Args>(args)...);
    }

    explicit operator bool() const
    {
        return (bool) impl;
    }
};

#endif

} // namespace nix
