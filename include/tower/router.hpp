#pragma once

#include <tower/service.hpp>

namespace tower {

namespace detail {

template<int I>
struct Priority : Priority<I - 1> {};

template<>
struct Priority<0> {};

// Prefer the owned request form, but allow services that take mutable references.
template<class S, class Request>
auto route_result(Priority<2>) -> service_result_t<S&, Request&&>;

template<class S, class Request>
auto route_result(Priority<1>) -> service_result_t<S&, Request&>;

template<class S, class Request>
auto route_result(Priority<0>) -> service_result_t<S&, const Request&>;

template<class S, class Request>
using route_result_t = decltype(route_result<S, Request>(Priority<2>{}));

template<class Request, class... Routes>
struct FindRoute {};

template<class T>
struct Type { using type = T; };

template<class Request, class First, class... Rest>
struct FindRoute<Request, First, Rest...>
    : std::conditional_t<std::is_same<Request, typename First::request_type>::value,
                         Type<First>, FindRoute<Request, Rest...>> {};

template<class... Routes>
struct UniqueRequests : std::true_type {};

template<class First, class... Rest>
struct UniqueRequests<First, Rest...>
    : all<std::integral_constant<bool, !std::is_same<typename First::request_type,
                                                   typename Rest::request_type>::value>...,
          std::integral_constant<bool, UniqueRequests<Rest...>::value>> {};

template<class, class... Routes>
struct CompatibleRoutes : std::false_type {};

template<class First, class... Rest>
struct CompatibleRoutes<void_t<typename First::result_type, typename Rest::result_type...,
                               typename First::request_type, typename Rest::request_type...>,
                        First, Rest...>
    : std::integral_constant<bool, UniqueRequests<First, Rest...>::value &&
          all<std::is_same<typename First::result_type, typename Rest::result_type>...>::value> {};

}  // namespace detail

template<class Request, class S>
struct Route {
    using request_type = Request;
    using result_type = detail::route_result_t<S, Request>;

    S service;
};

template<class Request, class S, class Stored = std::decay_t<S>,
         class = detail::route_result_t<Stored, std::decay_t<Request>>,
         std::enable_if_t<std::is_constructible<Stored, S&&>::value, int> = 0>
auto route(S&& service) -> Route<std::decay_t<Request>, Stored> {
    return {std::forward<S>(service)};
}

template<class First, class... Rest>
class Router : private First, private Rest... {
    static_assert(detail::CompatibleRoutes<void, First, Rest...>::value,
                  "Router requires distinct request types and identical Result types");

    template<class Request>
    using Match = typename detail::FindRoute<std::decay_t<Request>, First, Rest...>::type;

public:
    using result_type = typename First::result_type;

    template<class F, class... Rs,
             std::enable_if_t<detail::all<std::is_constructible<First, F&&>,
                                           std::is_constructible<Rest, Rs&&>...>::value, int> = 0>
    explicit Router(F&& first, Rs&&... rest)
        : First(std::forward<F>(first)), Rest(std::forward<Rs>(rest))... {}

    template<class Request, class Selected = Match<Request>,
             class R = detail::service_result_t<decltype((std::declval<Selected&>().service)),
                                                Request&&>,
             std::enable_if_t<std::is_same<R, result_type>::value, int> = 0>
    result_type operator()(Request&& request) {
        return static_cast<Selected&>(*this).service(std::forward<Request>(request));
    }

    template<class Request, class Selected = Match<Request>,
             class R = detail::service_result_t<decltype((std::declval<const Selected&>().service)),
                                                Request&&>,
             std::enable_if_t<std::is_same<R, result_type>::value, int> = 0>
    result_type operator()(Request&& request) const {
        return static_cast<const Selected&>(*this).service(std::forward<Request>(request));
    }
};

template<class... Routes,
         std::enable_if_t<detail::CompatibleRoutes<void, std::decay_t<Routes>...>::value &&
                          detail::all<std::is_constructible<std::decay_t<Routes>, Routes&&>...>::value,
                          int> = 0>
auto make_router(Routes&&... routes) -> Router<std::decay_t<Routes>...> {
    return Router<std::decay_t<Routes>...>(std::forward<Routes>(routes)...);
}

}  // namespace tower
