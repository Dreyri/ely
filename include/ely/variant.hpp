#pragma once

#include <initializer_list>
#include <variant>

namespace ely
{
namespace variant
{
constexpr bool check_any_false(std::initializer_list<bool> blist) noexcept
{
    for (bool b : blist)
    {
        if (!b)
        {
            return false;
        }
    }

    return true;
}

template<std::size_t I, typename... Ts>
using nth_element_t =
    void; /* decltype(variant::get_base_class<I>(
static_cast<
variant::VariantImpl<variant::index_seq<sizeof...(Ts)>, Ts...>*>(0))); */
} // namespace variant

template<typename... Ts>
using Variant = std::variant<Ts...>;

template<typename F, typename... Ts>
constexpr auto visit(Variant<Ts...>& var, F&& fn) -> decltype(auto)
{
    return std::visit(static_cast<F&&>(fn), var);
}

template<typename F, typename... Ts>
constexpr auto visit(const Variant<Ts...>& var, F&& fn) -> decltype(auto)
{
    return std::visit(static_cast<F&&>(fn), var);
}

template<typename F, typename... Ts>
constexpr auto visit(Variant<Ts...>&& var, F&& fn) -> decltype(auto)
{
    return std::visit(static_cast<F&&>(fn), std::move(var));
}

template<typename F, typename... Ts>
constexpr auto visit(const Variant<Ts...>&& var, F&& fn) -> decltype(auto)
{
    return std::visit(static_cast<F&&>(fn), std::move(var));
}
} // namespace ely