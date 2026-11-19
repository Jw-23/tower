#include <tower/tower.hpp>

#include "dependency_fixture.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <sstream>
#include <string>

namespace {

using tower::Result;

const auto unavailable = std::make_error_code(std::errc::resource_unavailable_try_again);
const auto denied = std::make_error_code(std::errc::permission_denied);

template<class...>
using void_t = void;

template<class Builder, class Request, class Handler, class = void>
struct can_route : std::false_type {};

template<class Builder, class Request, class Handler>
struct can_route<Builder, Request, Handler, void_t<decltype(std::declval<Builder>()
    .template route<Request>(std::declval<Handler>()))>> : std::true_type {};

template<class Builder, class = void>
struct can_build : std::false_type {};

template<class Builder>
struct can_build<Builder, void_t<decltype(std::declval<Builder>().build())>>
    : std::true_type {};

template<class Container, class Function, class = void>
struct can_inject : std::false_type {};

template<class Container, class Function>
struct can_inject<Container, Function, void_t<decltype(std::declval<Container>()
    .invoke(std::declval<Function>()))>> : std::true_type {};

int double_it(int value) { return value * 2; }

struct AddOneLayer {
    template<class S>
    auto layer(S inner) const {
        return [inner = std::move(inner)](int value) mutable {
            return inner.call(value).value() + 1;
        };
    }
};

TEST(ServiceTest, WrapsFunctionAndChecksSignature) {
    const auto service = tower::Tower{}.service(double_it);
    static_assert(tower::is_service_for<decltype(service)&, int>::value, "wrapped function");
    static_assert(!tower::is_service_for<decltype(service)&, std::string>::value, "invalid argument");
    static_assert(!tower::is_service_for<decltype(&double_it), int>::value, "plain result type");

    const auto result = service.call(5);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 10);
}

TEST(ServiceTest, WrapsVoidAndReferences) {
    int calls = 0;
    ASSERT_TRUE(tower::Tower{}.service([&] { ++calls; }).call().has_value());
    EXPECT_EQ(calls, 1);

    auto ref = tower::Tower{}.service([&]() -> int& { return calls; }).call();
    ASSERT_TRUE(ref.has_value());
    ref.value().get() = 42;
    EXPECT_EQ(calls, 42);
}

TEST(ServiceTest, PreservesExistingResultAndError) {
    const auto service = tower::Tower{}.service([]() -> Result<int> {
        return tower::failure(denied);
    });
    static_assert(std::is_same<decltype(service.call()), Result<int>>::value, "no nesting");

    const auto result = service.call();
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(result.error(), denied);
}

TEST(ServiceTest, AcceptsMoveOnlyStateAndReturnsMoveOnlyValues) {
    auto service = tower::Tower{}.service([value = std::make_unique<int>(3)](int count) mutable {
        return std::make_unique<int>(*value += count);
    });
    const auto first = service.call(4);
    ASSERT_TRUE(first.has_value());
    ASSERT_NE(first.value(), nullptr);
    EXPECT_EQ(*first.value(), 7);

    const auto second = service.call(1);
    ASSERT_TRUE(second.has_value());
    ASSERT_NE(second.value(), nullptr);
    EXPECT_EQ(*second.value(), 8);
}

TEST(LayerTest, LogsOnceOrPerAttemptDependingOnLayerOrder) {
    std::ostringstream stream;
    int attempts = 0;
    auto fallible = tower::Tower{}.service([&](std::string&& request) -> Result<std::size_t> {
        ++attempts;
        EXPECT_EQ(request, "request");
        std::string consumed = std::move(request);
        if (attempts < 3) {
            return tower::failure(unavailable);
        }
        return tower::success(consumed.size());
    });
    auto retryable = [](const tower::Error& error) { return error == unavailable; };
    auto retry = tower::Retry<decltype(retryable)>{3, retryable};
    static_assert(tower::is_layer_for<decltype(retry)&, decltype(fallible)&, std::string>::value,
                  "retry wraps a service");

    auto log_final = tower::Tower{}.log_to(stream).next(retry).service(fallible);
    const auto final_only = log_final.call(std::string("request"));
    ASSERT_TRUE(final_only.has_value());
    EXPECT_EQ(final_only.value(), 7u);
    EXPECT_EQ(attempts, 3);
    EXPECT_EQ(stream.str(), "tower: success\n");

    attempts = 0;
    stream.str("");
    auto log_every = tower::Tower{}.next(retry).log_to(stream).service(fallible);
    const auto every_attempt = log_every.call(std::string("request"));
    ASSERT_TRUE(every_attempt.has_value());
    EXPECT_EQ(every_attempt.value(), 7u);
    EXPECT_EQ(attempts, 3);
    EXPECT_EQ(stream.str(), "tower: failure\ntower: failure\ntower: success\n");
}

TEST(LayerTest, HonorsRetryPredicateAndAttemptLimit) {
    int attempts = 0;
    auto fail = tower::Tower{}.service([&]() -> Result<void> {
        ++attempts;
        return tower::failure(denied);
    });
    auto retry = [](const tower::Error& error) { return error == unavailable; };
    const auto rejected = tower::Tower{}.retry(3, retry).service(fail).call();
    ASSERT_TRUE(rejected.has_error());
    EXPECT_EQ(rejected.error(), denied);
    EXPECT_EQ(attempts, 1);

    const auto once = tower::Retry<>{0}.layer(fail).call();
    ASSERT_TRUE(once.has_error());
    EXPECT_EQ(once.error(), denied);
    EXPECT_EQ(attempts, 2);

    const auto exhausted = tower::Retry<>{2}.layer(fail).call();
    ASSERT_TRUE(exhausted.has_error());
    EXPECT_EQ(exhausted.error(), denied);
    EXPECT_EQ(attempts, 4);
}

TEST(LayerTest, ComposesCustomLayers) {
    auto increase = [](auto inner) {
        return tower::Tower{}.service([inner = std::move(inner)](int value) mutable {
            return inner.call(value + 1);
        });
    };
    auto identity = [](auto&& inner) -> decltype(auto) {
        return std::forward<decltype(inner)>(inner);
    };
    const auto result = tower::Tower{}
        .next(tower::Layer<decltype(identity)>{identity})
        .layer(increase)
        .service(double_it)
        .call(3);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 8);

    const auto direct = tower::Layer<decltype(increase)>{increase}.layer(tower::Tower{}.service(double_it)).call(4);
    ASSERT_TRUE(direct.has_value());
    EXPECT_EQ(direct.value(), 10);
}

TEST(LayerTest, AdaptsPlainCallablesAndReusesNamedBuilder) {
    const auto builder = tower::Tower{}.next(AddOneLayer{});
    auto first = builder.service(double_it);
    EXPECT_EQ(first.call(3).value(), 7);

    int logged = 0;
    auto second = tower::Tower{}
        .log([&logged](const Result<int>& result) { logged = result.value(); })
        .next(AddOneLayer{})
        .service(double_it);
    const auto result = second.call(4);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 9);
    EXPECT_EQ(logged, 9);
}

TEST(LayerTest, ReusesBorrowedRequestAndSkipsPredicateOnLastAttempt) {
    auto operation = tower::Tower{}.service([](int& request) -> Result<int> {
        ++request;
        return tower::failure(unavailable);
    });
    int predicate_calls = 0;
    auto service = tower::Tower{}.retry(3, [&predicate_calls](const tower::Error& error) {
        ++predicate_calls;
        return error == unavailable;
    }).service(operation);
    int request = 0;
    const auto result = service.call(request);

    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(result.error(), unavailable);
    EXPECT_EQ(request, 3);
    EXPECT_EQ(predicate_calls, 2);
}

TEST(LayerTest, SupportsMoveOnlyLogSinkAndBorrowedRetryArgument) {
    auto consuming = tower::Tower{}.retry(2).service([](std::unique_ptr<int> value) {
        return *value;
    });
    static_assert(!tower::is_service_for<decltype(consuming)&, std::unique_ptr<int>>::value,
                  "retry cannot replay a move-only rvalue");
    auto by_ref = tower::Tower{}.retry(2).service([](const std::unique_ptr<int>& value) {
        return *value;
    });
    auto pointer = std::make_unique<int>(4);
    const auto borrowed = by_ref.call(pointer);
    ASSERT_TRUE(borrowed.has_value());
    EXPECT_EQ(borrowed.value(), 4);

    int recorded = 0;
    auto sink = [count = std::make_unique<int>(0), &recorded](const Result<int>& result) {
        EXPECT_TRUE(result.has_value());
        recorded = ++*count;
    };
    const auto logged = tower::Tower{}
        .next(tower::Log<decltype(sink)>{std::move(sink)})
        .service(double_it)
        .call(1);
    ASSERT_TRUE(logged.has_value());
    EXPECT_EQ(logged.value(), 2);
    EXPECT_EQ(recorded, 1);
}

TEST(RouterTest, SelectsExactRequestTypeAndChecksResultTypes) {
    auto empty = tower::Tower{}.router();
    auto integer = empty.route<int>(double_it);
    auto inconsistent = [](double value) { return value; };
    static_assert(!can_route<decltype(integer), double, decltype(inconsistent)>::value,
                  "different responses rejected by SFINAE");
    static_assert(!can_route<decltype(integer), int, decltype(&double_it)>::value,
                  "duplicate request rejected by SFINAE");
    static_assert(!can_build<decltype(empty)>::value, "empty router rejected by SFINAE");
    auto routes = integer.route<std::string>([offset = std::make_unique<int>(1)](const std::string& value) {
        return *offset + static_cast<int>(value.size());
    }).build();
    static_assert(!tower::is_service_for<decltype(routes)&, short>::value, "no implicit routing");

    const auto number = routes.call(5);
    ASSERT_TRUE(number.has_value());
    EXPECT_EQ(number.value(), 10);
    const std::string word = "abcd";
    const auto text = routes.call(word);
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(text.value(), 5);
}

TEST(RouterTest, ForwardsReferencesAndMoveOnlyRequests) {
    auto mutable_route = tower::Tower{}.router().route<int>([](int& value) {
        return ++value;
    }).build();
    static_assert(!tower::is_service_for<decltype(mutable_route)&, int>::value, "request category");
    int value = 2;
    const auto mutated = mutable_route.call(value);
    ASSERT_TRUE(mutated.has_value());
    EXPECT_EQ(mutated.value(), 3);
    EXPECT_EQ(value, 3);

    auto pointer_route = tower::Router<>{}.route<std::unique_ptr<int>>(
        [](std::unique_ptr<int> input) { return *input; });
    const auto consumed = pointer_route.call(std::make_unique<int>(19));
    ASSERT_TRUE(consumed.has_value());
    EXPECT_EQ(consumed.value(), 19);

    const auto temporary = tower::Tower{}.router()
        .route<int>([count = 1](int input) mutable { return count += input; }).build().call(2);
    ASSERT_TRUE(temporary.has_value());
    EXPECT_EQ(temporary.value(), 3);
}

struct Immovable {
    explicit Immovable(int value) : value(value) {}
    Immovable(const Immovable&) = delete;
    Immovable(Immovable&&) = delete;
    int value;
};

struct alignas(128) OverAligned { int value = 7; };

int sum(const std::string& name, int& count, const Immovable& item,
        const std::unique_ptr<int>& pointer, SharedDependency shared, const int (&array)[2]) {
    ++count;
    return static_cast<int>(name.size()) + count + item.value + *pointer + shared.value + array[1];
}

TEST(DependenciesTest, InjectsSixHeterogeneousParametersAcrossTranslationUnits) {
    tower::Dependencies dependencies;
    ASSERT_TRUE(dependencies.insert(std::string("abc")).has_value());
    ASSERT_TRUE(dependencies.emplace<int>(3).has_value());
    ASSERT_TRUE(dependencies.emplace<Immovable>(6).has_value());
    ASSERT_TRUE(dependencies.insert(std::make_unique<int>(8)).has_value());
    ASSERT_TRUE(dependencies.emplace<int[2]>(9, 10).has_value());
    ASSERT_TRUE(add_shared(dependencies).has_value());

    const auto shared = dependencies.get<SharedDependency>();
    ASSERT_TRUE(shared.has_value());
    EXPECT_EQ(shared.value().get().value, 29);
    const auto result = dependencies.invoke(sum);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 60);
    const auto count = dependencies.get<int>();
    ASSERT_TRUE(count.has_value());
    EXPECT_EQ(count.value().get(), 4);
}

TEST(DependenciesTest, StoresOverAlignedInstances) {
    tower::Dependencies dependencies;
    const auto aligned = dependencies.emplace<OverAligned>();
    ASSERT_TRUE(aligned.has_value());
    const auto address = reinterpret_cast<std::uintptr_t>(&aligned.value().get());
    EXPECT_EQ(address % alignof(OverAligned), 0u);

    const auto result = dependencies.invoke([](const OverAligned& input) { return input.value; });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 7);
}

TEST(DependenciesTest, ReportsDuplicateAndMissingDependencies) {
    tower::Dependencies dependencies;
    ASSERT_TRUE(dependencies.emplace<int>(4).has_value());
    const auto duplicate = dependencies.emplace<int>(55);
    ASSERT_TRUE(duplicate.has_error());
    EXPECT_EQ(duplicate.error(), tower::make_error_code(tower::errc::duplicate_dependency));
    const auto retained = dependencies.get<int>();
    ASSERT_TRUE(retained.has_value());
    EXPECT_EQ(retained.value().get(), 4);

    const auto missing = tower::make_error_code(tower::errc::missing_dependency);
    const auto lookup = dependencies.get<double>();
    ASSERT_TRUE(lookup.has_error());
    EXPECT_EQ(lookup.error(), missing);
    bool called = false;
    const auto invocation = dependencies.invoke([&](int, double) { called = true; });
    ASSERT_TRUE(invocation.has_error());
    EXPECT_EQ(invocation.error(), missing);
    EXPECT_FALSE(called);
}

TEST(DependenciesTest, InvokesZeroArgumentAndFallibleFunctions) {
    tower::Dependencies dependencies;
    const auto zero = dependencies.invoke([] { return 5; });
    ASSERT_TRUE(zero.has_value());
    EXPECT_EQ(zero.value(), 5);

    ASSERT_TRUE(dependencies.emplace<int>(4).has_value());
    const auto failed = dependencies.invoke([](int) -> Result<int> { return tower::failure(denied); });
    ASSERT_TRUE(failed.has_error());
    EXPECT_EQ(failed.error(), denied);
}

TEST(DependenciesTest, InjectsExplicitSignaturesRepeatedTypesAndConstReferences) {
    tower::Dependencies dependencies;
    ASSERT_TRUE(dependencies.emplace<int>(4).has_value());
    ASSERT_TRUE(dependencies.emplace<std::string>("abc").has_value());
    const auto explicit_args = dependencies.invoke_as<int&, const std::string&>(
        [](auto& count, const auto& name) { return count + static_cast<int>(name.size()); });
    ASSERT_TRUE(explicit_args.has_value());
    EXPECT_EQ(explicit_args.value(), 7);

    const auto repeated = dependencies.invoke([](int a, const int& b) { return a + b; });
    ASSERT_TRUE(repeated.has_value());
    EXPECT_EQ(repeated.value(), 8);
    const auto alias = dependencies.invoke([](int& input) -> int& { return input; });
    ASSERT_TRUE(alias.has_value());
    alias.value().get() = 6;

    const auto& frozen = dependencies;
    const auto constant = frozen.invoke([](const int& input) { return input; });
    ASSERT_TRUE(constant.has_value());
    EXPECT_EQ(constant.value(), 6);
    const auto lookup = frozen.get<int>();
    ASSERT_TRUE(lookup.has_value());
    EXPECT_EQ(lookup.value().get(), 6);

    static_assert(!can_inject<const tower::Dependencies&, decltype(&sum)>::value,
                  "const registry does not provide mutable references");
    const auto rvalue = [](std::unique_ptr<int>&&) {};
    static_assert(!can_inject<tower::Dependencies&, decltype(rvalue)>::value,
                  "injection does not move a dependency out of its registry");
}

TEST(DependenciesTest, WiresPolicyArgumentsAndLayerWithOwnedLifetime) {
    int observed = 0;
    auto wired = tower::Tower{}
        .log([&observed](const Result<int>& result) { observed = result.value(); })
        .dependencies()
        .emplace<Immovable>(40)
        .with(0)
        .wire([](const Immovable& policy, int& count) {
            return [&policy, &count](int amount) { return policy.value + ++count + amount; };
        });
    ASSERT_TRUE(wired.has_value());

    auto service = std::move(wired).value();
    const auto first = service.call(2);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first.value(), 43);
    EXPECT_EQ(observed, 43);
    const auto second = service.call(3);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second.value(), 45);
    EXPECT_EQ(observed, 45);
}

TEST(DependenciesTest, FluentScopeReportsRegistrationAndFactoryFailures) {
    bool called = false;
    const auto duplicate = tower::Tower{}.dependencies().with(1).with(2).wire([&called](int& value) {
        called = true;
        return [&value] { return value; };
    });
    ASSERT_TRUE(duplicate.has_error());
    EXPECT_EQ(duplicate.error(), tower::make_error_code(tower::errc::duplicate_dependency));
    EXPECT_FALSE(called);

    const auto missing = tower::Tower{}.dependencies().with(0).wire([](const std::string& value) {
        return [&value] { return value.size(); };
    });
    ASSERT_TRUE(missing.has_error());
    EXPECT_EQ(missing.error(), tower::make_error_code(tower::errc::missing_dependency));

    const auto failure = tower::Tower{}.dependencies().wire([]() -> Result<decltype(&double_it)> {
        return tower::failure(denied);
    });
    ASSERT_TRUE(failure.has_error());
    EXPECT_EQ(failure.error(), denied);
}

TEST(DependenciesTest, FluentInvokeSupportsValuesAndExplicitSignatures) {
    const auto temporary = tower::Tower{}.dependencies().with(20).with(22L).invoke_as<int, long>(
        [](auto first, auto second) { return first + second; });
    ASSERT_TRUE(temporary.has_value());
    EXPECT_EQ(temporary.value(), 42L);

    auto scope = tower::Tower{}.dependencies().with(42);
    auto borrow = [](int& value) -> int& { return value; };
    static_assert(!can_inject<decltype(scope)&&, decltype(borrow)>::value,
                  "a temporary scope cannot return a reference");
    auto alias = scope.invoke(borrow);
    ASSERT_TRUE(alias.has_value());
    alias.value().get() = 10;
    const auto value = scope.get<int>();
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(value.value().get(), 10);

    auto wired = std::move(scope).wire_as<int&>([](auto& input) {
        return [&input] { return input; };
    });
    ASSERT_TRUE(wired.has_value());
    EXPECT_EQ(wired.value().call().value(), 10);
}

}  // namespace
