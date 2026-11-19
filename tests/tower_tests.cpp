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

template<class, class...>
struct can_make_router : std::false_type {};

template<class... Routes>
struct can_make_router<void_t<decltype(tower::make_router(std::declval<Routes>()...))>, Routes...>
    : std::true_type {};

template<class Container, class Function, class = void>
struct can_inject : std::false_type {};

template<class Container, class Function>
struct can_inject<Container, Function, void_t<decltype(std::declval<Container>()
    .invoke(std::declval<Function>()))>> : std::true_type {};

int double_it(int value) { return value * 2; }

TEST(ServiceTest, WrapsFunctionAndChecksSignature) {
    const auto service = tower::make_service(double_it);
    static_assert(tower::is_service_for<decltype(service)&, int>::value, "wrapped function");
    static_assert(!tower::is_service_for<decltype(service)&, std::string>::value, "invalid argument");
    static_assert(!tower::is_service_for<decltype(&double_it), int>::value, "plain result type");

    const auto result = service(5);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 10);
}

TEST(ServiceTest, WrapsVoidAndReferences) {
    int calls = 0;
    ASSERT_TRUE(tower::make_service([&] { ++calls; })().has_value());
    EXPECT_EQ(calls, 1);

    auto ref = tower::make_service([&]() -> int& { return calls; })();
    ASSERT_TRUE(ref.has_value());
    ref.value().get() = 42;
    EXPECT_EQ(calls, 42);
}

TEST(ServiceTest, PreservesExistingResultAndError) {
    const auto service = tower::make_service([]() -> Result<int> {
        return tower::failure(denied);
    });
    static_assert(std::is_same<decltype(service()), Result<int>>::value, "no nesting");

    const auto result = service();
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(result.error(), denied);
}

TEST(ServiceTest, AcceptsMoveOnlyStateAndReturnsMoveOnlyValues) {
    auto service = tower::make_service([value = std::make_unique<int>(3)](int count) mutable {
        return std::make_unique<int>(*value += count);
    });
    const auto first = service(4);
    ASSERT_TRUE(first.has_value());
    ASSERT_NE(first.value(), nullptr);
    EXPECT_EQ(*first.value(), 7);

    const auto second = service(1);
    ASSERT_TRUE(second.has_value());
    ASSERT_NE(second.value(), nullptr);
    EXPECT_EQ(*second.value(), 8);
}

TEST(LayerTest, LogsOnceOrPerAttemptDependingOnLayerOrder) {
    std::ostringstream stream;
    int attempts = 0;
    auto fallible = tower::make_service([&](std::string&& request) -> Result<std::size_t> {
        ++attempts;
        EXPECT_EQ(request, "request");
        std::string consumed = std::move(request);
        if (attempts < 3) {
            return tower::failure(unavailable);
        }
        return tower::success(consumed.size());
    });
    auto retry = tower::retry(3, [](const tower::Error& error) { return error == unavailable; });
    static_assert(tower::is_layer_for<decltype(retry)&, decltype(fallible)&, std::string>::value,
                  "retry wraps a service");

    auto log_final = tower::apply_layers(fallible, tower::log_to(stream), retry);
    const auto final_only = log_final(std::string("request"));
    ASSERT_TRUE(final_only.has_value());
    EXPECT_EQ(final_only.value(), 7u);
    EXPECT_EQ(attempts, 3);
    EXPECT_EQ(stream.str(), "tower: success\n");

    attempts = 0;
    stream.str("");
    auto log_every = tower::apply_layers(fallible, retry, tower::log_to(stream));
    const auto every_attempt = log_every(std::string("request"));
    ASSERT_TRUE(every_attempt.has_value());
    EXPECT_EQ(every_attempt.value(), 7u);
    EXPECT_EQ(attempts, 3);
    EXPECT_EQ(stream.str(), "tower: failure\ntower: failure\ntower: success\n");
}

TEST(LayerTest, HonorsRetryPredicateAndAttemptLimit) {
    int attempts = 0;
    auto fail = tower::make_service([&]() -> Result<void> {
        ++attempts;
        return tower::failure(denied);
    });
    auto retry = tower::retry(3, [](const tower::Error& error) { return error == unavailable; });

    const auto rejected = retry(fail)();
    ASSERT_TRUE(rejected.has_error());
    EXPECT_EQ(rejected.error(), denied);
    EXPECT_EQ(attempts, 1);

    const auto once = tower::retry(0)(fail)();
    ASSERT_TRUE(once.has_error());
    EXPECT_EQ(once.error(), denied);
    EXPECT_EQ(attempts, 2);

    const auto exhausted = tower::retry(2)(fail)();
    ASSERT_TRUE(exhausted.has_error());
    EXPECT_EQ(exhausted.error(), denied);
    EXPECT_EQ(attempts, 4);
}

TEST(LayerTest, ComposesCustomLayers) {
    auto increase = tower::make_layer([](auto inner) {
        return tower::make_service([inner = std::move(inner)](int value) mutable {
            return inner(value + 1);
        });
    });
    auto identity = tower::make_layer([](auto&& inner) -> decltype(auto) {
        return std::forward<decltype(inner)>(inner);
    });
    const auto result = tower::apply_layers(tower::make_service(double_it), identity, increase)(3);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 8);
}

TEST(LayerTest, SupportsMoveOnlyLogSinkAndBorrowedRetryArgument) {
    auto consuming = tower::retry(2)(tower::make_service([](std::unique_ptr<int> value) {
        return *value;
    }));
    static_assert(!tower::is_service_for<decltype(consuming)&, std::unique_ptr<int>>::value,
                  "retry cannot replay a move-only rvalue");
    auto by_ref = tower::retry(2)(tower::make_service([](const std::unique_ptr<int>& value) {
        return *value;
    }));
    auto pointer = std::make_unique<int>(4);
    const auto borrowed = by_ref(pointer);
    ASSERT_TRUE(borrowed.has_value());
    EXPECT_EQ(borrowed.value(), 4);

    int recorded = 0;
    auto sink = tower::log([count = std::make_unique<int>(0), &recorded](const Result<int>& result) {
        EXPECT_TRUE(result.has_value());
        recorded = ++*count;
    });
    const auto logged = std::move(sink)(tower::make_service(double_it))(1);
    ASSERT_TRUE(logged.has_value());
    EXPECT_EQ(logged.value(), 2);
    EXPECT_EQ(recorded, 1);
}

TEST(RouterTest, SelectsExactRequestTypeAndChecksResultTypes) {
    auto integer = tower::route<int>(tower::make_service(double_it));
    auto string = tower::route<std::string>(tower::make_service([offset = std::make_unique<int>(1)]
        (const std::string& value) { return *offset + static_cast<int>(value.size()); }));
    auto inconsistent = tower::route<double>(tower::make_service([](double value) { return value; }));
    static_assert(!can_make_router<void, decltype(integer), decltype(inconsistent)>::value,
                  "different responses rejected by SFINAE");
    static_assert(!can_make_router<void, decltype(integer), decltype(integer)>::value,
                  "duplicate request rejected by SFINAE");
    static_assert(!can_make_router<void>::value, "empty router rejected by SFINAE");
    auto routes = tower::make_router(integer, std::move(string));
    static_assert(!tower::is_service_for<decltype(routes)&, short>::value, "no implicit routing");

    const auto number = routes(5);
    ASSERT_TRUE(number.has_value());
    EXPECT_EQ(number.value(), 10);
    const std::string word = "abcd";
    const auto text = routes(word);
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(text.value(), 5);
}

TEST(RouterTest, ForwardsReferencesAndMoveOnlyRequests) {
    auto mutable_route = tower::make_router(tower::route<int>(tower::make_service([](int& value) {
        return ++value;
    })));
    static_assert(!tower::is_service_for<decltype(mutable_route)&, int>::value, "request category");
    int value = 2;
    const auto mutated = mutable_route(value);
    ASSERT_TRUE(mutated.has_value());
    EXPECT_EQ(mutated.value(), 3);
    EXPECT_EQ(value, 3);

    auto pointer_route = tower::make_router(tower::route<std::unique_ptr<int>>(
        tower::make_service([](std::unique_ptr<int> input) { return *input; })));
    const auto consumed = pointer_route(std::make_unique<int>(19));
    ASSERT_TRUE(consumed.has_value());
    EXPECT_EQ(consumed.value(), 19);

    const auto temporary = tower::make_router(tower::route<int>(
        tower::make_service([count = 1](int input) mutable { return count += input; })))(2);
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

}  // namespace
