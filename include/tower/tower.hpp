#pragma once

#include <tower/dependencies.hpp>
#include <tower/layer.hpp>
#include <tower/router.hpp>

#include <tuple>

namespace tower {
namespace detail {

template<std::size_t Index, std::size_t End>
struct ApplyLayers {
    template<class S, class Tuple, class Outer = decltype(std::get<Index>(std::declval<Tuple>())),
             class Inner = decltype(ApplyLayers<Index + 1, End>{}(
                 std::declval<S>(), std::declval<Tuple>())),
             class Applied = layer_result_t<Outer, Inner>>
    auto operator()(S&& service, Tuple&& layers) const -> adapted_service_t<Applied> {
        auto& outer = std::get<Index>(layers);
        auto&& inner = ApplyLayers<Index + 1, End>{}(std::forward<S>(service), std::forward<Tuple>(layers));
        return adapt_service(std::forward<Outer>(outer).layer(std::forward<decltype(inner)>(inner)));
    }
};

template<std::size_t End>
struct ApplyLayers<End, End> {
    template<class S, class Tuple>
    S&& operator()(S&& service, Tuple&&) const {
        return std::forward<S>(service);
    }
};

template<class F, class Tuple, class S = adapted_service_t<F&&>,
         class Apply = ApplyLayers<0, std::tuple_size<std::decay_t<Tuple>>::value>,
         class Applied = decltype(Apply{}(std::declval<S>(), std::declval<Tuple>()))>
auto finish_service(F&& handler, Tuple&& layers) -> adapted_service_t<Applied> {
    return adapt_service(Apply{}(adapt_service(std::forward<F>(handler)), std::forward<Tuple>(layers)));
}

struct NoRouter {};

template<class Builder>
class DependencyScope;

template<class RouteState, class... Layers>
class TowerBuilder {
    template<class, class...>
    friend class TowerBuilder;

    using LayersTuple = std::tuple<Layers...>;

    TowerBuilder(RouteState router, LayersTuple layers)
        : router_(std::move(router)), layers_(std::move(layers)) {}

public:
    TowerBuilder() = default;

    // The first registered layer wraps all later layers.
    template<class L, class Stored = std::decay_t<L>,
             std::enable_if_t<std::is_constructible<Stored, L&&>::value, int> = 0>
    auto next(L&& layer) && -> TowerBuilder<RouteState, Layers..., Stored> {
        return {std::move(router_), std::tuple_cat(std::move(layers_),
                                                  std::tuple<Stored>(std::forward<L>(layer)))};
    }

    template<class L, class Stored = std::decay_t<L>,
             std::enable_if_t<all<std::is_constructible<Stored, L&&>,
                                  std::is_copy_constructible<RouteState>,
                                  std::is_copy_constructible<Layers>...>::value, int> = 0>
    auto next(L&& layer) const& -> TowerBuilder<RouteState, Layers..., Stored> {
        return {router_, std::tuple_cat(layers_, std::tuple<Stored>(std::forward<L>(layer)))};
    }

    template<class Sink>
    auto log(Sink&& sink) && {
        return std::move(*this).next(Log<std::decay_t<Sink>>(std::forward<Sink>(sink)));
    }

    template<class Sink>
    auto log(Sink&& sink) const& {
        return next(Log<std::decay_t<Sink>>(std::forward<Sink>(sink)));
    }

    auto log_to(std::ostream& stream) && {
        return std::move(*this).log(StreamLogger{&stream});
    }

    auto log_to(std::ostream& stream) const& {
        return log(StreamLogger{&stream});
    }

    template<class Predicate = RetryEveryError>
    auto retry(std::size_t max_attempts, Predicate&& predicate = Predicate{}) && {
        return std::move(*this).next(Retry<std::decay_t<Predicate>>(
            max_attempts, std::forward<Predicate>(predicate)));
    }

    template<class Predicate = RetryEveryError>
    auto retry(std::size_t max_attempts, Predicate&& predicate = Predicate{}) const& {
        return next(Retry<std::decay_t<Predicate>>(max_attempts, std::forward<Predicate>(predicate)));
    }

    template<class F>
    auto layer(F&& factory) && {
        return std::move(*this).next(Layer<std::decay_t<F>>(std::forward<F>(factory)));
    }

    template<class F>
    auto layer(F&& factory) const& {
        return next(Layer<std::decay_t<F>>(std::forward<F>(factory)));
    }

    template<class F, class State = RouteState,
             std::enable_if_t<std::is_same<State, NoRouter>::value, int> = 0>
    auto service(F&& handler) && -> decltype(finish_service(std::declval<F>(), std::declval<LayersTuple>())) {
        return finish_service(std::forward<F>(handler), std::move(layers_));
    }

    template<class F, class State = RouteState,
             std::enable_if_t<std::is_same<State, NoRouter>::value, int> = 0>
    auto service(F&& handler) const&
        -> decltype(finish_service(std::declval<F>(), std::declval<const LayersTuple&>())) {
        return finish_service(std::forward<F>(handler), layers_);
    }

    template<class State = RouteState, std::enable_if_t<std::is_same<State, NoRouter>::value, int> = 0>
    auto router() && -> TowerBuilder<Router<>, Layers...> {
        return {Router<>{}, std::move(layers_)};
    }

    template<class State = RouteState,
             std::enable_if_t<all<std::is_same<State, NoRouter>,
                                  std::is_copy_constructible<Layers>...>::value, int> = 0>
    auto router() const& -> TowerBuilder<Router<>, Layers...> {
        return {Router<>{}, layers_};
    }

    template<class Request, class F, class State = RouteState,
             class R = decltype(std::declval<State&&>().template route<Request>(std::declval<F>()))>
    auto route(F&& handler) && -> TowerBuilder<R, Layers...> {
        return {std::move(router_).template route<Request>(std::forward<F>(handler)), std::move(layers_)};
    }

    template<class Request, class F, class State = RouteState,
             class R = decltype(std::declval<const State&>().template route<Request>(std::declval<F>())),
             std::enable_if_t<all<std::is_copy_constructible<Layers>...,
                                  std::is_move_constructible<R>>::value, int> = 0>
    auto route(F&& handler) const& -> TowerBuilder<R, Layers...> {
        return {router_.template route<Request>(std::forward<F>(handler)), layers_};
    }

    template<class State = RouteState,
             std::enable_if_t<!std::is_void<typename State::result_type>::value, int> = 0>
    auto build() && -> decltype(finish_service(std::declval<State>(), std::declval<LayersTuple>())) {
        return finish_service(std::move(router_), std::move(layers_));
    }

    template<class State = RouteState,
             std::enable_if_t<!std::is_void<typename State::result_type>::value, int> = 0>
    auto build() const& -> decltype(finish_service(std::declval<const State&>(), std::declval<const LayersTuple&>())) {
        return finish_service(router_, layers_);
    }

    template<class State = RouteState, std::enable_if_t<std::is_same<State, NoRouter>::value, int> = 0>
    auto dependencies() && -> DependencyScope<TowerBuilder> {
        return DependencyScope<TowerBuilder>(std::move(*this));
    }

    template<class State = RouteState,
             std::enable_if_t<all<std::is_same<State, NoRouter>,
                                  std::is_copy_constructible<Layers>...>::value, int> = 0>
    auto dependencies() const& -> DependencyScope<TowerBuilder> {
        return DependencyScope<TowerBuilder>(*this);
    }

private:
    RouteState router_;
    LayersTuple layers_;
};

template<class S>
class OwnedService {
public:
    OwnedService(Dependencies&& dependencies, S&& service)
        : dependencies_(std::move(dependencies)), service_(std::move(service)) {}

    OwnedService(OwnedService&&) = default;

    template<class... Args>
    auto operator()(Args&&... args) & -> raw_result_t<S&, Args&&...> {
        return service_(std::forward<Args>(args)...);
    }

    template<class... Args>
    auto operator()(Args&&... args) const& -> raw_result_t<const S&, Args&&...> {
        return service_(std::forward<Args>(args)...);
    }

    template<class... Args>
    auto operator()(Args&&... args) && -> raw_result_t<S&&, Args&&...> {
        return std::move(service_)(std::forward<Args>(args)...);
    }

private:
    // The service must be destroyed before the objects it borrows.
    Dependencies dependencies_;
    S service_;
};

template<class Builder>
class DependencyScope {
    template<class Resolved,
             class Built = decltype(std::declval<Builder&&>().service(std::declval<Resolved&&>().assume_value())),
             class Owned = Service<OwnedService<Built>>>
    auto bind(Resolved&& handler) && -> typename std::decay_t<Resolved>::template rebind<Owned> {
        if (!handler) {
            return std::forward<Resolved>(handler).as_failure();
        }
        auto service = std::move(builder_).service(std::forward<Resolved>(handler).assume_value());
        return success(Owned(OwnedService<Built>(std::move(dependencies_), std::move(service))));
    }

public:
    explicit DependencyScope(Builder builder) : builder_(std::move(builder)) {}

    template<class T, class... Args,
             class = decltype(std::declval<Dependencies&>().template emplace<T>(std::declval<Args>()...))>
    DependencyScope& emplace(Args&&... args) & {
        if (!error_) {
            auto inserted = dependencies_.template emplace<T>(std::forward<Args>(args)...);
            if (!inserted) {
                error_ = inserted.assume_error();
            }
        }
        return *this;
    }

    template<class T, class... Args,
             class = decltype(std::declval<Dependencies&>().template emplace<T>(std::declval<Args>()...))>
    DependencyScope&& emplace(Args&&... args) && {
        return std::move(emplace<T>(std::forward<Args>(args)...));
    }

    template<class T>
    auto with(T&& value) & -> decltype(emplace<std::decay_t<T>>(std::forward<T>(value))) {
        return emplace<std::decay_t<T>>(std::forward<T>(value));
    }

    template<class T>
    auto with(T&& value) &&
        -> decltype(std::move(*this).template emplace<std::decay_t<T>>(std::forward<T>(value))) {
        return std::move(*this).template emplace<std::decay_t<T>>(std::forward<T>(value));
    }

    template<class T, class R = decltype(std::declval<Dependencies&>().template get<T>())>
    R get() & {
        if (error_) {
            return failure(error_);
        }
        return dependencies_.template get<T>();
    }

    template<class F, class R = decltype(std::declval<Dependencies&>().invoke(std::declval<F>()))>
    R invoke(F&& function) & {
        if (error_) {
            return failure(typename R::error_type(error_));
        }
        return dependencies_.invoke(std::forward<F>(function));
    }

    template<class F, class R = decltype(std::declval<Dependencies&>().invoke(std::declval<F>())),
             std::enable_if_t<!IsBorrowedValue<std::decay_t<typename R::value_type>>::value, int> = 0>
    R invoke(F&& function) && {
        return invoke(std::forward<F>(function));
    }

    template<class... Args, class F, class R = decltype(std::declval<Dependencies&>()
                 .template invoke_as<Args...>(std::declval<F>()))>
    R invoke_as(F&& function) & {
        if (error_) {
            return failure(typename R::error_type(error_));
        }
        return dependencies_.template invoke_as<Args...>(std::forward<F>(function));
    }

    template<class... Args, class F, class R = decltype(std::declval<Dependencies&>()
                 .template invoke_as<Args...>(std::declval<F>())),
             std::enable_if_t<!IsBorrowedValue<std::decay_t<typename R::value_type>>::value, int> = 0>
    R invoke_as(F&& function) && {
        return invoke_as<Args...>(std::forward<F>(function));
    }

    template<class F>
    auto wire(F&& factory) && -> decltype(std::move(*this).bind(invoke(std::forward<F>(factory)))) {
        return std::move(*this).bind(invoke(std::forward<F>(factory)));
    }

    template<class... Args, class F>
    auto wire_as(F&& factory) &&
        -> decltype(std::move(*this).bind(invoke_as<Args...>(std::forward<F>(factory)))) {
        return std::move(*this).bind(invoke_as<Args...>(std::forward<F>(factory)));
    }

private:
    Builder builder_;
    Dependencies dependencies_;
    Error error_;
};

}  // namespace detail

using Tower = detail::TowerBuilder<detail::NoRouter>;

}  // namespace tower
