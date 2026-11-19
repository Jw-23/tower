#pragma once

#include <tower/service.hpp>

namespace tower {

namespace detail {

template<int I>
struct Priority : Priority<I - 1> {};

template<>
struct Priority<0> {};

// Prefer the owned request form, with reference forms for borrowed requests.
template<class S, class Request>
auto route_result(Priority<2>) -> service_result_t<S&, Request&&>;

template<class S, class Request>
auto route_result(Priority<1>) -> service_result_t<S&, Request&>;

template<class S, class Request>
auto route_result(Priority<0>) -> service_result_t<S&, const Request&>;

template<class S, class Request>
using route_result_t = decltype(route_result<S, Request>(Priority<2>{}));

template<class Request, class S, class Response>
struct Route {
    using request_type = Request;
    using result_type = Response;
    using service_type = S;

    S service;
};

template<class Request, class... Routes>
struct FindRoute {};

template<class T>
struct Type { using type = T; };

template<class Request, class First, class... Rest>
struct FindRoute<Request, First, Rest...>
    : std::conditional_t<std::is_same<Request, typename First::request_type>::value,
                         Type<First>, FindRoute<Request, Rest...>> {};

template<class... Routes>
struct RouteResponse { using type = void; };

template<class First, class... Rest>
struct RouteResponse<First, Rest...> { using type = typename First::result_type; };

template<class Request, class F, class... Routes>
using appended_route_t = std::enable_if_t<all<
    std::integral_constant<bool, !std::is_same<Request, typename Routes::request_type>::value>...,
    std::is_same<route_result_t<adapted_service_t<F>, Request>, typename Routes::result_type>...>::value,
    Route<Request, adapted_service_t<F>, route_result_t<adapted_service_t<F>, Request>>>;

}  // namespace detail

template<class... Routes>
class Router : private Routes... {
    template<class...>
    friend class Router;

    template<class... Rs, std::enable_if_t<detail::all<std::is_constructible<Routes, Rs&&>...>::value,
                                          int> = 0>
    explicit Router(Rs&&... routes) : Routes(std::forward<Rs>(routes))... {}

    template<class Request>
    using Match = typename detail::FindRoute<std::decay_t<Request>, Routes...>::type;

public:
    Router() = default;

    using result_type = typename detail::RouteResponse<Routes...>::type;

    template<class Request, class F,
             class Added = detail::appended_route_t<std::decay_t<Request>, F&&, Routes...>,
             std::enable_if_t<detail::all<std::is_copy_constructible<Routes>...,
                                         std::is_move_constructible<Added>>::value, int> = 0>
    auto route(F&& handler) const& -> Router<Routes..., Added> {
        return Router<Routes..., Added>(static_cast<const Routes&>(*this)...,
                                       Added{detail::adapt_service(std::forward<F>(handler))});
    }

    template<class Request, class F,
             class Added = detail::appended_route_t<std::decay_t<Request>, F&&, Routes...>>
    auto route(F&& handler) && -> Router<Routes..., Added> {
        return Router<Routes..., Added>(std::move(static_cast<Routes&>(*this))...,
                                       Added{detail::adapt_service(std::forward<F>(handler))});
    }

    template<class Request, class Selected = Match<Request>,
             class R = detail::service_result_t<typename Selected::service_type&, Request&&>,
             std::enable_if_t<std::is_same<R, result_type>::value, int> = 0>
    R operator()(Request&& request) {
        return static_cast<Selected&>(*this).service(std::forward<Request>(request));
    }

    template<class Request, class Selected = Match<Request>,
             class R = detail::service_result_t<const typename Selected::service_type&, Request&&>,
             std::enable_if_t<std::is_same<R, result_type>::value, int> = 0>
    R operator()(Request&& request) const {
        return static_cast<const Selected&>(*this).service(std::forward<Request>(request));
    }

    template<class Request>
    auto call(Request&& request) -> decltype((*this)(std::forward<Request>(request))) {
        return (*this)(std::forward<Request>(request));
    }

    template<class Request>
    auto call(Request&& request) const -> decltype((*this)(std::forward<Request>(request))) {
        return (*this)(std::forward<Request>(request));
    }
};

}  // namespace tower
