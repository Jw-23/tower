#pragma once

#include <tower/result.hpp>

namespace tower {

namespace detail {

template<class S, class... Args>
using service_result_t = std::enable_if_t<is_result<raw_result_t<S, Args...>>::value,
                                          raw_result_t<S, Args...>>;

template<class, class S, class... Args>
struct is_service_for : std::false_type {};

template<class S, class... Args>
struct is_service_for<void_t<service_result_t<S, Args...>>, S, Args...> : std::true_type {};

}  // namespace detail

template<class S, class... Args>
using is_service_for = detail::is_service_for<void, S, Args...>;

template<class F>
class Service {
public:
    template<class Fn, std::enable_if_t<std::is_constructible<F, Fn&&>::value, int> = 0>
    explicit Service(Fn&& function) : function_(std::forward<Fn>(function)) {}

    template<class... Args>
    auto operator()(Args&&... args) &
        -> decltype(detail::call_result(std::declval<F&>(), std::forward<Args>(args)...)) {
        return detail::call_result(function_, std::forward<Args>(args)...);
    }

    template<class... Args>
    auto operator()(Args&&... args) const&
        -> decltype(detail::call_result(std::declval<const F&>(), std::forward<Args>(args)...)) {
        return detail::call_result(function_, std::forward<Args>(args)...);
    }

    template<class... Args>
    auto operator()(Args&&... args) &&
        -> decltype(detail::call_result(std::declval<F&&>(), std::forward<Args>(args)...)) {
        return detail::call_result(std::move(function_), std::forward<Args>(args)...);
    }

private:
    F function_;
};

template<class F, std::enable_if_t<std::is_constructible<std::decay_t<F>, F&&>::value, int> = 0>
auto make_service(F&& function) -> Service<std::decay_t<F>> {
    return Service<std::decay_t<F>>(std::forward<F>(function));
}

}  // namespace tower
