#pragma once

#include <tower/service.hpp>

#include <boost/config.hpp>

#include <cstddef>
#include <ostream>

namespace tower {

namespace detail {

template<class L, class S>
using layer_result_t = decltype(std::declval<L>().layer(std::declval<S>()));

template<class, class L, class S, class... Args>
struct is_layer_for : std::false_type {};

template<class L, class S, class... Args>
struct is_layer_for<void_t<layer_result_t<L, S>>, L, S, Args...>
    : tower::is_service_for<std::remove_reference_t<layer_result_t<L, S>>&, Args...> {};

}  // namespace detail

template<class L, class S, class... Args>
using is_layer_for = detail::is_layer_for<void, L, S, Args...>;

template<class F>
class Layer {
public:
    template<class Fn, std::enable_if_t<std::is_constructible<F, Fn&&>::value, int> = 0>
    explicit Layer(Fn&& factory) : factory_(std::forward<Fn>(factory)) {}

    template<class S>
    auto layer(S&& service) & -> detail::adapted_service_t<detail::raw_result_t<F&, S&&>> {
        return detail::adapt_service(factory_(std::forward<S>(service)));
    }

    template<class S>
    auto layer(S&& service) const& -> detail::adapted_service_t<detail::raw_result_t<const F&, S&&>> {
        return detail::adapt_service(factory_(std::forward<S>(service)));
    }

    template<class S>
    auto layer(S&& service) && -> detail::adapted_service_t<detail::raw_result_t<F&&, S&&>> {
        return detail::adapt_service(std::move(factory_)(std::forward<S>(service)));
    }

private:
    F factory_;
};

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

template<class Predicate, class R>
using retry_predicate_t = decltype(static_cast<bool>(std::declval<Predicate&>()(
    std::declval<const R&>().assume_error())));

template<class S, class Predicate>
struct Retried {
    S service;
    Predicate predicate;
    std::size_t max_attempts;

    // Inline borrowed retries so the outer layer can consume the final result directly.
    template<class... Args, class R = service_result_t<S&, Args&&...>,
             class = retry_predicate_t<Predicate, R>,
             std::enable_if_t<all<std::is_lvalue_reference<Args>...>::value, int> = 0>
    BOOST_FORCEINLINE R operator()(Args&&... args) {
        // Borrowed arguments can use the same call site for every attempt.
        for (std::size_t attempt = 1;; ++attempt) {
            auto result = service(args...);
            if (attempt == max_attempts || !should_retry(result)) {
                return result;
            }
        }
    }

    template<class... Args, class R = service_result_t<S&, Args&&...>,
             class = retry_predicate_t<Predicate, R>,
             std::enable_if_t<!all<std::is_lvalue_reference<Args>...>::value &&
                               all<can_replay<Args>...>::value, int> = 0>
    R operator()(Args&&... args) {
        for (std::size_t remaining = max_attempts; remaining > 1; --remaining) {
            auto result = service(replay<Args>(args)...);
            if (!should_retry(result)) {
                return result;
            }
        }
        return service(std::forward<Args>(args)...);
    }

private:
    template<class R>
    bool should_retry(const R& result) {
        return result.has_error() && static_cast<bool>(predicate(result.assume_error()));
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
    auto layer(S&& service) const& -> Service<detail::Logged<std::decay_t<S>, Sink>> {
        using Wrapped = detail::Logged<std::decay_t<S>, Sink>;
        return Service<Wrapped>(Wrapped{std::forward<S>(service), sink_});
    }

    template<class S, std::enable_if_t<std::is_constructible<std::decay_t<S>, S&&>::value, int> = 0>
    auto layer(S&& service) && -> Service<detail::Logged<std::decay_t<S>, Sink>> {
        using Wrapped = detail::Logged<std::decay_t<S>, Sink>;
        return Service<Wrapped>(Wrapped{std::forward<S>(service), std::move(sink_)});
    }

private:
    Sink sink_;
};

template<class Predicate = detail::RetryEveryError>
class Retry {
public:
    explicit Retry(std::size_t max_attempts, Predicate predicate = Predicate{})
        : predicate_(std::move(predicate)), max_attempts_(max_attempts == 0 ? 1 : max_attempts) {}

    template<class S, class P = Predicate,
             std::enable_if_t<std::is_constructible<std::decay_t<S>, S&&>::value &&
                              std::is_copy_constructible<P>::value, int> = 0>
    auto layer(S&& service) const& -> Service<detail::Retried<std::decay_t<S>, Predicate>> {
        using Wrapped = detail::Retried<std::decay_t<S>, Predicate>;
        return Service<Wrapped>(Wrapped{std::forward<S>(service), predicate_, max_attempts_});
    }

    template<class S, std::enable_if_t<std::is_constructible<std::decay_t<S>, S&&>::value, int> = 0>
    auto layer(S&& service) && -> Service<detail::Retried<std::decay_t<S>, Predicate>> {
        using Wrapped = detail::Retried<std::decay_t<S>, Predicate>;
        return Service<Wrapped>(Wrapped{std::forward<S>(service), std::move(predicate_), max_attempts_});
    }

private:
    Predicate predicate_;
    std::size_t max_attempts_;
};

}  // namespace tower
