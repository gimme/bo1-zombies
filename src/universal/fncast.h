#pragma once

// BO1_FNCAST(Type, Func): Func used through a function pointer of a different Type.
//
// The decompiled code stores functions in tables and callbacks whose pointer type does not match the function:
// fewer or more parameters, int in place of float, void in place of a return value. x86 cdecl calls tolerate that
// (the caller pushes and pops the arguments; extra ones are ignored, missing ones are whatever is on the stack). A
// WebAssembly indirect call traps on any signature mismatch, so there BO1_FNCAST makes a thunk of exactly Type that
// calls Func the way cdecl would: the leading arguments Func takes, reinterpreted bit for bit where the types differ
// (an int slot read as a float), zero for parameters the caller does not pass, and a zero result where Func has none.
// With MSVC it is the plain cast it replaces.
#if defined(__EMSCRIPTEN__)

#include <cstdint>
#include <cstring>
#include <tuple>
#include <type_traits>
#include <utility>

namespace bo1_fncast
{
template <class To, class From> inline To arg(From v)
{
    using T = std::remove_cv_t<To>;
    using F = std::remove_cv_t<From>;
    if constexpr (std::is_same_v<T, F>)
        return v;
    else if constexpr (std::is_pointer_v<T> && std::is_pointer_v<F>)
        return (T)v;
    else if constexpr (std::is_pointer_v<T> && (std::is_integral_v<F> || std::is_enum_v<F>))
        return (T)(uintptr_t)v;
    else if constexpr ((std::is_integral_v<T> || std::is_enum_v<T>) && std::is_pointer_v<F>)
        return (T)(uintptr_t)v;
    else if constexpr ((std::is_integral_v<T> || std::is_enum_v<T>) && (std::is_integral_v<F> || std::is_enum_v<F>))
        return (T)v;
    else if constexpr (sizeof(T) == sizeof(F) && std::is_trivially_copyable_v<T> && std::is_trivially_copyable_v<F>)
    {
        // float <-> int, a pointer-sized union: the same bits in the same stack slot
        T t;
        memcpy(&t, &v, sizeof(T));
        return t;
    }
    else
    {
        static_assert(sizeof(T) == 0, "BO1_FNCAST: no cdecl-compatible conversion between these argument types");
    }
}

template <class Fn> struct traits;
template <class R, class... A> struct traits<R (*)(A...)>
{
    using ret = R;
    using params = std::tuple<A...>;
    static constexpr size_t count = sizeof...(A);
};

template <class R> inline R zero()
{
    if constexpr (std::is_void_v<R>)
        return;
    else
    {
        R r;
        memset((void *)&r, 0, sizeof(r));
        return r;
    }
}

template <auto F, class Target> struct thunk;
template <auto F, class R, class... A> struct thunk<F, R (*)(A...)>
{
    using FT = traits<decltype(F)>;
    template <size_t I, class Tuple> static auto pick(Tuple &t)
    {
        using P = std::tuple_element_t<I, typename FT::params>;
        if constexpr (I < sizeof...(A))
            return arg<P>(std::get<I>(t));
        else
            return zero<std::remove_cv_t<std::remove_reference_t<P>>>();
    }
    template <size_t... I> static auto invoke(std::tuple<A...> &t, std::index_sequence<I...>)
    {
        return F(pick<I>(t)...);
    }
    static R call(A... a)
    {
        std::tuple<A...> t(a...);
        if constexpr (std::is_void_v<typename FT::ret>)
        {
            invoke(t, std::make_index_sequence<FT::count>{});
            return zero<R>();
        }
        else if constexpr (std::is_void_v<R>)
            invoke(t, std::make_index_sequence<FT::count>{});
        else
            return arg<R>(invoke(t, std::make_index_sequence<FT::count>{}));
    }
};
} // namespace bo1_fncast

#define BO1_FNCAST(Type, Func) (&bo1_fncast::thunk<&Func, Type>::call)

#else
#define BO1_FNCAST(Type, Func) ((Type)(Func))
#endif
