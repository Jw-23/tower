#pragma once

#include <tower/service.hpp>

#include <cstddef>
#include <ostream>

namespace tower {

namespace detail {

template<class, class L, class S, class... Args>
struct is_layer_for : std::false_type {};

template<class L, class S, class... Args>
struct is_layer_for<void_t<raw_result_t<L, S>>, L, S, Args...>
    : tower::is_service_for<std::remove_reference_t<raw_result_t<L, S>>&, Args...> {};

template<class... Layers>
struct LayerStack;

template<>
struct LayerStack<> {
    template<class S>
    static S&& apply(S&& service) {
        return std::forward<S>(service);
    }
};

template<class First, class... Rest>
struct LayerStack<First, Rest...> {
    template<class S, class Applied = decltype(std::declval<First>()(
                 LayerStack<Rest...>::apply(std::declval<S>(), std::declval<Rest>()...))),
             std::enable_if_t<std::is_constructible<std::decay_t<Applied>, Applied>::value, int> = 0>
    static auto apply(S&& service, First&& first, Rest&&... rest) -> std::decay_t<Applied> {
        return std::forward<First>(first)(
            LayerStack<Rest...>::apply(std::forward<S>(service), std::forward<Rest>(rest)...));
    }
};

}  // namespace detail

template<class L, class S, class... Args>
using is_layer_for = detail::is_layer_for<void, L, S, Args...>;

template<class F>
class Layer {
public:
    template<class Fn, std::enable_if_t<std::is_constructible<F, Fn&&>::value, int> = 0>
    explicit Layer(Fn&& factory) : factory_(std::forward<Fn>(factory)) {}

    template<class S>
    auto operator()(S&& service) & -> detail::raw_result_t<F&, S&&> {
        return factory_(std::forward<S>(service));
    }

    template<class S>
    auto operator()(S&& service) const& -> detail::raw_result_t<const F&, S&&> {
        return factory_(std::forward<S>(service));
    }

    template<class S>
    auto operator()(S&& service) && -> detail::raw_result_t<F&&, S&&> {
        return std::move(factory_)(std::forward<S>(service));
    }

private:
    F factory_;
};

template<class F, std::enable_if_t<std::is_constructible<std::decay_t<F>, F&&>::value, int> = 0>
auto make_layer(F&& factory) -> Layer<std::decay_t<F>> {
    return Layer<std::decay_t<F>>(std::forward<F>(factory));
}

// The first layer is outermost. The last layer wraps the base service first.
template<class S, class... Layers,
         class Applied = decltype(detail::LayerStack<Layers...>::apply(
             std::declval<S>(), std::declval<Layers>()...)),
         std::enable_if_t<std::is_constructible<std::decay_t<Applied>, Applied>::value, int> = 0>
auto apply_layers(S&& service, Layers&&... layers) -> std::decay_t<Applied> {
    return detail::LayerStack<Layers...>::apply(std::forward<S>(service),
                                               std::forward<Layers>(layers)...);
}

namespace detail {

template<class S, class Sink>
struct Logged {
    S service;
    Sink sink;

    template<class... Args, class R = service_result_t<S&, Args&&...>,
             class = raw_result_t<Sink&, const R&>>
    R operator()(Args&&... args) {
        auto result = service(std::forward<Args>(args)...);
        sink(static_cast<const R&>(result));
        return result;
    }
};

struct StreamLogger {
    std::ostream* stream;

    template<class R>
    void operator()(const R& result) const {
        *stream << (result.has_value() ? "tower: success\n" : "tower: failure\n");
    }
};

struct RetryEveryError {
    template<class E>
    bool operator()(const E&) const { return true; }
};

template<class Arg>
using can_replay = std::integral_constant<bool, std::is_lvalue_reference<Arg>::value ||
    std::is_constructible<std::remove_reference_t<Arg>, const std::remove_reference_t<Arg>&>::value>;

template<class Arg, std::enable_if_t<std::is_lvalue_reference<Arg>::value, int> = 0>
Arg replay(std::remove_reference_t<Arg>& request) { return request; }

template<class Arg, std::enable_if_t<!std::is_reference<Arg>::value, int> = 0>
Arg replay(const Arg& request) { return Arg(request); }

template<class S, class Predicate>
struct Retried {
    S service;
    Predicate predicate;
    std::size_t max_attempts;

    template<class... Args, class R = service_result_t<S&, Args&&...>,
             class = decltype(static_cast<bool>(std::declval<Predicate&>()(
                 std::declval<const R&>().assume_error()))),
             std::enable_if_t<all<can_replay<Args>...>::value, int> = 0>
    R operator()(Args&&... args) {
        for (std::size_t remaining = max_attempts; remaining > 1; --remaining) {
            auto result = service(replay<Args>(args)...);
            if (!result.has_error() || !predicate(static_cast<const R&>(result).assume_error())) {
                return result;
            }
        }
        return service(std::forward<Args>(args)...);
    }
};

}  // namespace detail

template<class Sink>
class Log {
public:
    explicit Log(Sink sink) : sink_(std::move(sink)) {}

    template<class S, class T = Sink,
             std::enable_if_t<std::is_constructible<std::decay_t<S>, S&&>::value &&
                              std::is_copy_constructible<T>::value, int> = 0>
    auto operator()(S&& service) const& -> detail::Logged<std::decay_t<S>, Sink> {
        return {std::forward<S>(service), sink_};
    }

    template<class S, std::enable_if_t<std::is_constructible<std::decay_t<S>, S&&>::value, int> = 0>
    auto operator()(S&& service) && -> detail::Logged<std::decay_t<S>, Sink> {
        return {std::forward<S>(service), std::move(sink_)};
    }

private:
    Sink sink_;
};

template<class Sink>
auto log(Sink&& sink) -> Log<std::decay_t<Sink>> {
    return Log<std::decay_t<Sink>>(std::forward<Sink>(sink));
}

inline auto log_to(std::ostream& stream) -> Log<detail::StreamLogger> {
    return log(detail::StreamLogger{&stream});
}

template<class Predicate = detail::RetryEveryError>
class Retry {
public:
    explicit Retry(std::size_t max_attempts, Predicate predicate = Predicate{})
        : predicate_(std::move(predicate)), max_attempts_(max_attempts == 0 ? 1 : max_attempts) {}

    template<class S, class P = Predicate,
             std::enable_if_t<std::is_constructible<std::decay_t<S>, S&&>::value &&
                              std::is_copy_constructible<P>::value, int> = 0>
    auto operator()(S&& service) const& -> detail::Retried<std::decay_t<S>, Predicate> {
        return {std::forward<S>(service), predicate_, max_attempts_};
    }

    template<class S, std::enable_if_t<std::is_constructible<std::decay_t<S>, S&&>::value, int> = 0>
    auto operator()(S&& service) && -> detail::Retried<std::decay_t<S>, Predicate> {
        return {std::forward<S>(service), std::move(predicate_), max_attempts_};
    }

private:
    Predicate predicate_;
    std::size_t max_attempts_;
};

template<class Predicate = detail::RetryEveryError>
auto retry(std::size_t max_attempts, Predicate&& predicate = Predicate{})
    -> Retry<std::decay_t<Predicate>> {
    return Retry<std::decay_t<Predicate>>(max_attempts, std::forward<Predicate>(predicate));
}

}  // namespace tower
