#pragma once

#include <boost/outcome/result.hpp>
#include <boost/outcome/policy/terminate.hpp>

#include <functional>
#include <system_error>
#include <type_traits>
#include <utility>

namespace tower {

namespace outcome = boost::outcome_v2;

using Error = std::error_code;

template<class T>
using Result = outcome::result<T, Error, outcome::policy::terminate>;

using outcome::failure;
using outcome::success;

namespace detail {

template<class...>
struct make_void { using type = void; };

template<class... Ts>
using void_t = typename make_void<Ts...>::type;

template<class...>
struct Types {};

template<class... Conditions>
using all = std::is_same<Types<std::true_type, std::integral_constant<bool, Conditions::value>...>,
                         Types<std::integral_constant<bool, Conditions::value>..., std::true_type>>;

template<class T>
struct is_result : std::false_type {};

template<class T, class E, class Policy>
struct is_result<outcome::basic_result<T, E, Policy>> : std::true_type {};

template<class Raw, class = void>
struct Lift;

template<class Raw>
struct Lift<Raw, std::enable_if_t<!is_result<std::decay_t<Raw>>::value &&
                                 !std::is_reference<Raw>::value &&
                                 !std::is_void<Raw>::value &&
                                 !std::is_same<std::decay_t<Raw>, Error>::value &&
                                 std::is_constructible<std::decay_t<Raw>, Raw>::value>> {
    using value_type = std::decay_t<Raw>;
    using result_type = Result<value_type>;

    template<class F, class... Args>
    static result_type call(F&& function, Args&&... args) {
        return result_type(outcome::in_place_type<value_type>,
                           std::forward<F>(function)(std::forward<Args>(args)...));
    }
};

template<class T>
struct Lift<T&, std::enable_if_t<!is_result<std::decay_t<T>>::value>> {
    using result_type = Result<std::reference_wrapper<T>>;

    template<class F, class... Args>
    static result_type call(F&& function, Args&&... args) {
        return success(std::ref(std::forward<F>(function)(std::forward<Args>(args)...)));
    }
};

template<class T>
struct Lift<T&&, std::enable_if_t<!is_result<std::decay_t<T>>::value>> : Lift<T> {};

template<class Raw>
struct Lift<Raw, std::enable_if_t<is_result<std::decay_t<Raw>>::value &&
                                 std::is_constructible<std::decay_t<Raw>, Raw>::value>> {
    using result_type = std::decay_t<Raw>;

    template<class F, class... Args>
    static result_type call(F&& function, Args&&... args) {
        return std::forward<F>(function)(std::forward<Args>(args)...);
    }
};

template<>
struct Lift<void> {
    using result_type = Result<void>;

    template<class F, class... Args>
    static result_type call(F&& function, Args&&... args) {
        std::forward<F>(function)(std::forward<Args>(args)...);
        return success();
    }
};

template<class F, class... Args>
using raw_result_t = decltype(std::declval<F>()(std::declval<Args>()...));

template<class F, class... Args>
auto call_result(F&& function, Args&&... args)
    -> typename Lift<raw_result_t<F, Args...>>::result_type {
    return Lift<raw_result_t<F, Args...>>::call(std::forward<F>(function),
                                              std::forward<Args>(args)...);
}

}  // namespace detail
}  // namespace tower
