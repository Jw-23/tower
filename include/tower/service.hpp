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
    auto call(Args&&... args) &
        -> decltype(detail::call_result(std::declval<F&>(), std::forward<Args>(args)...)) {
        return detail::call_result(function_, std::forward<Args>(args)...);
    }

    template<class... Args>
    auto call(Args&&... args) const&
        -> decltype(detail::call_result(std::declval<const F&>(), std::forward<Args>(args)...)) {
        return detail::call_result(function_, std::forward<Args>(args)...);
    }

    template<class... Args>
    auto call(Args&&... args) &&
        -> decltype(detail::call_result(std::declval<F&&>(), std::forward<Args>(args)...)) {
        return detail::call_result(std::move(function_), std::forward<Args>(args)...);
    }

    template<class... Args>
    auto operator()(Args&&... args) & -> decltype(call(std::forward<Args>(args)...)) {
        return call(std::forward<Args>(args)...);
    }

    template<class... Args>
    auto operator()(Args&&... args) const& -> decltype(call(std::forward<Args>(args)...)) {
        return call(std::forward<Args>(args)...);
    }

    template<class... Args>
    auto operator()(Args&&... args) &&
        -> decltype(std::move(*this).call(std::forward<Args>(args)...)) {
        return std::move(*this).call(std::forward<Args>(args)...);
    }

private:
    F function_;
};

namespace detail {

template<class F>
struct AsService { using type = Service<F>; };

template<class F>
struct AsService<Service<F>> { using type = Service<F>; };

template<class F>
using as_service_t = typename AsService<std::decay_t<F>>::type;

template<class F>
using is_service_source = std::integral_constant<bool, std::is_class<std::decay_t<F>>::value ||
    std::is_union<std::decay_t<F>>::value || std::is_function<std::remove_pointer_t<std::decay_t<F>>>::value>;

template<class F>
using adapted_service_t = std::enable_if_t<all<is_service_source<F>,
    std::is_constructible<as_service_t<F>, F>>::value, as_service_t<F>>;

template<class F>
auto adapt_service(F&& handler) -> adapted_service_t<F&&> {
    return as_service_t<F>(std::forward<F>(handler));
}

}  // namespace detail

}  // namespace tower
