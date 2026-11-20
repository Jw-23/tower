#pragma once

#include <tower/detail/service.hpp>

#include <functional>
#include <system_error>
#include <type_traits>
#include <utility>

namespace tower {
namespace detail {

template<class F, class... Args>
using lifted_result_t = decltype(call_result(std::declval<F>(), std::declval<Args>()...));

template<class T>
using read_only_t = const std::remove_reference_t<T>&;

template<class P, class... A>
using bool_predicate_t = std::enable_if_t<
    std::is_same<std::decay_t<raw_result_t<P, A...>>, bool>::value>;

template<class T>
T&& unwrap_payload(T&& value) { return std::forward<T>(value); }

template<class T>
T& unwrap_payload(std::reference_wrapper<T> value) { return value.get(); }

template<class F, class R, std::enable_if_t<!std::is_void<typename std::decay_t<R>::value_type>::value, int> = 0>
auto pass_payload(F&& continuation, R&& previous)
    -> lifted_result_t<F, decltype(unwrap_payload(std::forward<R>(previous).assume_value()))> {
    return call_result(std::forward<F>(continuation), unwrap_payload(std::forward<R>(previous).assume_value()));
}

template<class F, class R, std::enable_if_t<std::is_void<typename std::decay_t<R>::value_type>::value, int> = 0>
auto pass_payload(F&& continuation, R&&) -> lifted_result_t<F> {
    return call_result(std::forward<F>(continuation));
}

template<class Yes, class No>
struct DispatchBool;

template<class Continuation>
struct ForwardsPayload : std::true_type {};

// The boolean selector reads the input and invokes a nullary branch.
template<class Yes, class No>
struct ForwardsPayload<DispatchBool<Yes, No>> : std::false_type {};

template<class Continuation, class Prior, class Output>
using safe_pipeline_value = std::integral_constant<bool,
    !ForwardsPayload<std::decay_t<Continuation>>::value ||
    std::is_void<typename Prior::value_type>::value ||
    IsBorrowedValue<std::decay_t<typename Prior::value_type>>::value ||
    !IsBorrowedValue<std::decay_t<typename Output::value_type>>::value>;

template<class Stage>
struct ForwardPipelineCall {
    template<class... A, class D = Stage>
    auto operator()(A&&... args) & -> decltype(D::invoke(std::declval<D&>(), std::declval<A>()...)) {
        return D::invoke(static_cast<D&>(*this), std::forward<A>(args)...);
    }

    template<class... A, class D = Stage>
    auto operator()(A&&... args) const&
        -> decltype(D::invoke(std::declval<const D&>(), std::declval<A>()...)) {
        return D::invoke(static_cast<const D&>(*this), std::forward<A>(args)...);
    }

    template<class... A, class D = Stage>
    auto operator()(A&&... args) && -> decltype(D::invoke(std::declval<D>(), std::declval<A>()...)) {
        return D::invoke(static_cast<D&&>(*this), std::forward<A>(args)...);
    }

private:
    friend Stage;
    ForwardPipelineCall() = default;
};

template<class Upstream, class Continuation>
struct Sequence : ForwardPipelineCall<Sequence<Upstream, Continuation>> {
    template<class U, class C>
    Sequence(U&& upstream, C&& continuation)
        : upstream(std::forward<U>(upstream)), continuation(std::forward<C>(continuation)) {}

    Upstream upstream;
    Continuation continuation;

    template<class Self, class... Args,
             class First = decltype((std::declval<Self>().upstream)),
             class Next = decltype((std::declval<Self>().continuation)),
             class Prior = lifted_result_t<First, Args&&...>,
             class Output = decltype(pass_payload(std::declval<Next>(), std::declval<Prior>())),
             std::enable_if_t<!std::is_void<typename Prior::error_type>::value, int> = 0,
             std::enable_if_t<all<safe_pipeline_value<Next, Prior, Output>, std::is_constructible<Output,
                 decltype(std::declval<Prior>().as_failure())>>::value, int> = 0>
    static Output invoke(Self&& self, Args&&... args) {
        auto& first = self.upstream;
        auto& next = self.continuation;
        auto previous = call_result(std::forward<First>(first), std::forward<Args>(args)...);
        if (!previous) {
            return Output(std::move(previous).as_failure());
        }
        return pass_payload(std::forward<Next>(next), std::move(previous));
    }
};

inline auto guard_failure(std::errc error) { return failure(std::make_error_code(error)); }

template<class E>
using guard_failure_t = decltype(failure(std::declval<const E&>()));

template<class E>
auto guard_failure(const E& error) -> guard_failure_t<E> { return failure(error); }

template<class Inner, class Predicate, class Rejection>
struct Guarded : ForwardPipelineCall<Guarded<Inner, Predicate, Rejection>> {
    template<class I, class P, class E>
    Guarded(I&& inner, P&& predicate, E&& rejected)
        : inner(std::forward<I>(inner)), predicate(std::forward<P>(predicate)),
          rejected(std::forward<E>(rejected)) {}

    Inner inner;
    Predicate predicate;
    Rejection rejected;

    template<class Self, class... Args,
             class ServiceRef = decltype((std::declval<Self>().inner)),
             class PredicateRef = decltype((std::declval<Self>().predicate)),
             class Output = lifted_result_t<ServiceRef, Args&&...>,
             class = bool_predicate_t<PredicateRef, read_only_t<Args>...>,
             std::enable_if_t<std::is_constructible<Output, decltype(guard_failure(
                 std::declval<const Rejection&>()))>::value, int> = 0>
    static Output invoke(Self&& self, Args&&... args) {
        auto& predicate = self.predicate;
        auto& inner = self.inner;
        if (!std::forward<PredicateRef>(predicate)(static_cast<read_only_t<Args>>(args)...)) {
            return Output(guard_failure(self.rejected));
        }
        return call_result(std::forward<ServiceRef>(inner), std::forward<Args>(args)...);
    }
};

template<class Y, class N, class... Args, class R = lifted_result_t<Y, Args&&...>,
         std::enable_if_t<std::is_same<R, lifted_result_t<N, Args&&...>>::value, int> = 0>
R choose_branch(bool select, Y&& yes, N&& no, Args&&... args) {
    if (select) {
        return call_result(std::forward<Y>(yes), std::forward<Args>(args)...);
    }
    return call_result(std::forward<N>(no), std::forward<Args>(args)...);
}

template<class Yes, class No>
struct DispatchBool : ForwardPipelineCall<DispatchBool<Yes, No>> {
    template<class Y, class N>
    DispatchBool(Y&& yes, N&& no) : yes(std::forward<Y>(yes)), no(std::forward<N>(no)) {}

    Yes yes;
    No no;

    template<class Self, class Flag, std::enable_if_t<std::is_same<std::decay_t<Flag>, bool>::value, int> = 0,
             class Y = decltype((std::declval<Self>().yes)), class N = decltype((std::declval<Self>().no))>
    static auto invoke(Self&& self, Flag&& flag) -> decltype(choose_branch(
        flag, std::declval<Y>(), std::declval<N>())) {
        return choose_branch(flag, std::forward<Y>(self.yes), std::forward<N>(self.no));
    }
};

template<class P, class V, std::enable_if_t<std::is_same<std::decay_t<P>, bool>::value, int> = 0>
bool select_branch(P&& selected, const V&) { return selected; }

template<class P, class V, class = bool_predicate_t<P, const V&>>
bool select_branch(P&& predicate, const V& value) {
    return std::forward<P>(predicate)(value);
}

template<class Predicate, class Yes, class No>
struct DispatchWhen : ForwardPipelineCall<DispatchWhen<Predicate, Yes, No>> {
    template<class P, class Y, class N>
    DispatchWhen(P&& predicate, Y&& yes, N&& no)
        : predicate(std::forward<P>(predicate)), yes(std::forward<Y>(yes)), no(std::forward<N>(no)) {}

    Predicate predicate;
    Yes yes;
    No no;

    template<class Self, class Value,
             class P = decltype((std::declval<Self>().predicate)),
             class Y = decltype((std::declval<Self>().yes)), class N = decltype((std::declval<Self>().no)),
             class = decltype(select_branch(std::declval<P>(), std::declval<read_only_t<Value>>()))>
    static auto invoke(Self&& self, Value&& value) -> decltype(choose_branch(
        false, std::declval<Y>(), std::declval<N>(), std::declval<Value>())) {
        const bool selected = select_branch(std::forward<P>(self.predicate), static_cast<read_only_t<Value>>(value));
        return choose_branch(selected, std::forward<Y>(self.yes), std::forward<N>(self.no),
                             std::forward<Value>(value));
    }
};

template<template<class...> class Node, class... Parts>
using pipeline_node_t = std::enable_if_t<all<std::is_constructible<std::decay_t<Parts>, Parts>...>::value,
                                       Node<std::decay_t<Parts>...>>;

template<template<class...> class Node, class... Parts, class Stored = pipeline_node_t<Node, Parts&&...>>
auto extend_pipeline(Parts&&... parts) -> Service<Stored> {
    return Service<Stored>(Stored(std::forward<Parts>(parts)...));
}

}  // namespace detail

template<class Derived>
class Pipeline {
public:
    template<class F, class D = Derived, class Node = detail::pipeline_node_t<detail::Sequence, const D&, F&&>,
             std::enable_if_t<detail::is_service_source<F>::value, int> = 0>
    auto next(F&& continuation) const& -> Service<Node> {
        return detail::extend_pipeline<detail::Sequence>(derived(), std::forward<F>(continuation));
    }

    template<class F, class D = Derived, class Node = detail::pipeline_node_t<detail::Sequence, D&&, F&&>,
             std::enable_if_t<detail::is_service_source<F>::value, int> = 0>
    auto next(F&& continuation) && -> Service<Node> {
        return detail::extend_pipeline<detail::Sequence>(std::move(*this).derived(), std::forward<F>(continuation));
    }

    template<class P, class E = std::errc, class D = Derived,
             class Node = detail::pipeline_node_t<detail::Guarded, const D&, P&&, E&&>>
    auto guard(P&& predicate, E error = std::errc::invalid_argument) const& -> Service<Node> {
        return detail::extend_pipeline<detail::Guarded>(derived(), std::forward<P>(predicate), std::move(error));
    }

    template<class P, class E = std::errc, class D = Derived,
             class Node = detail::pipeline_node_t<detail::Guarded, D&&, P&&, E&&>>
    auto guard(P&& predicate, E error = std::errc::invalid_argument) && -> Service<Node> {
        return detail::extend_pipeline<detail::Guarded>(std::move(*this).derived(),
            std::forward<P>(predicate), std::move(error));
    }

    template<class Router>
    auto dispatch(Router&& router) const& -> decltype(next(std::forward<Router>(router))) {
        return next(std::forward<Router>(router));
    }

    template<class Router>
    auto dispatch(Router&& router) && -> decltype(std::move(*this).next(std::forward<Router>(router))) {
        return std::move(*this).next(std::forward<Router>(router));
    }

    template<class Y, class N, class Branch = detail::pipeline_node_t<detail::DispatchBool, Y&&, N&&>>
    auto dispatch(Y&& yes, N&& no) const& -> decltype(next(std::declval<Branch>())) {
        return next(Branch(std::forward<Y>(yes), std::forward<N>(no)));
    }

    template<class Y, class N, class Branch = detail::pipeline_node_t<detail::DispatchBool, Y&&, N&&>>
    auto dispatch(Y&& yes, N&& no) && -> decltype(std::move(*this).next(std::declval<Branch>())) {
        return std::move(*this).next(Branch(std::forward<Y>(yes), std::forward<N>(no)));
    }

    template<class P, class Y, class N, class Branch = detail::pipeline_node_t<detail::DispatchWhen, P&&, Y&&, N&&>>
    auto dispatch(P&& predicate, Y&& yes, N&& no) const& -> decltype(next(std::declval<Branch>())) {
        return next(Branch(std::forward<P>(predicate), std::forward<Y>(yes), std::forward<N>(no)));
    }

    template<class P, class Y, class N, class Branch = detail::pipeline_node_t<detail::DispatchWhen, P&&, Y&&, N&&>>
    auto dispatch(P&& predicate, Y&& yes, N&& no) && -> decltype(std::move(*this).next(std::declval<Branch>())) {
        return std::move(*this).next(Branch(std::forward<P>(predicate), std::forward<Y>(yes), std::forward<N>(no)));
    }

private:
    friend Derived;
    Pipeline() = default;
    Pipeline(const Pipeline&) = default;
    Pipeline(Pipeline&&) = default;
    Pipeline& operator=(const Pipeline&) = default;
    Pipeline& operator=(Pipeline&&) = default;
    ~Pipeline() = default;

    const Derived& derived() const& { return static_cast<const Derived&>(*this); }
    Derived&& derived() && { return static_cast<Derived&&>(*this); }
};

}  // namespace tower
