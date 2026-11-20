#include <tower/tower.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

namespace {

using tower::Result;

TEST(PipelineTest, SequencesValuesAndSkipsContinuationOnFailure) {
    int calls = 0;
    const auto denied = std::make_error_code(std::errc::permission_denied);
    const auto pipeline = tower::Tower{}.service([denied](int value) -> Result<int> {
        if (value < 0) {
            return tower::failure(denied);
        }
        return tower::success(value);
    }).next([&calls](int value) {
        ++calls;
        return std::to_string(value);
    });

    static_assert(std::is_same<decltype(pipeline.call(5)), Result<std::string>>::value,
                  "the continuation determines the result value type");
    const auto success = pipeline.call(5);
    ASSERT_TRUE(success.has_value());
    EXPECT_EQ(success.value(), "5");
    const auto error = pipeline.call(-1);
    ASSERT_TRUE(error.has_error());
    EXPECT_EQ(error.error(), denied);
    EXPECT_EQ(calls, 1);
}

TEST(PipelineTest, AcceptsVoidOwnedAndBorrowedPayloads) {
    int value = 4;
    auto pipeline = tower::Tower{}.service([&value] { ++value; })
        .next([&value] { return std::make_unique<int>(value); })
        .next([](std::unique_ptr<int> item) { return *item * 2; });
    const auto owned = pipeline.call();
    ASSERT_TRUE(owned.has_value());
    EXPECT_EQ(owned.value(), 10);

    auto borrowed = tower::Tower{}.service([&value]() -> int& { return value; })
        .next([](auto& input) -> int& {
            static_assert(std::is_same<decltype(input), int&>::value, "unwrap borrowed payloads");
            ++input;
            return input;
        });
    const auto reference = borrowed.call();
    ASSERT_TRUE(reference.has_value());
    EXPECT_EQ(&reference.value().get(), &value);
    EXPECT_EQ(value, 6);
}

TEST(PipelineTest, GuardBorrowsArgumentsAndCanRejectWithCustomError) {
    int calls = 0;
    auto endpoint = tower::Tower{}.service([&calls](std::unique_ptr<int> item, int limit) {
        ++calls;
        return *item + limit;
    });
    auto guarded = endpoint.guard([](const std::unique_ptr<int>& item, int limit) {
        return item && *item <= limit;
    }, std::errc::permission_denied);

    auto value = std::make_unique<int>(7);
    const auto rejected = guarded.call(std::move(value), 5);
    ASSERT_TRUE(rejected.has_error());
    EXPECT_EQ(rejected.error(), std::errc::permission_denied);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(calls, 0);
    const auto accepted = guarded.call(std::move(value), 9);
    ASSERT_TRUE(accepted.has_value());
    EXPECT_EQ(accepted.value(), 16);
    EXPECT_EQ(value, nullptr);
    EXPECT_EQ(calls, 1);

    const auto default_error = tower::Tower{}.service([](int input) { return input; })
        .guard([](int) { return false; }).call(2);
    ASSERT_TRUE(default_error.has_error());
    EXPECT_EQ(default_error.error(), std::errc::invalid_argument);
}

TEST(PipelineTest, PassesAnIntermediateValueThroughAGuardedService) {
    auto positive = tower::Tower{}.service([](int value) { return value * 2; })
        .guard([](int value) { return value > 0; });
    auto pipeline = tower::Tower{}.service([](int value) { return value - 5; }).next(positive);

    const auto error = pipeline.call(4);
    ASSERT_TRUE(error.has_error());
    EXPECT_EQ(error.error(), std::errc::invalid_argument);
    const auto result = pipeline.call(8);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 6);
}

TEST(PipelineTest, DispatchesBothBooleanResultsAndPreservesPriorErrors) {
    int yes_calls = 0;
    int no_calls = 0;
    const auto denied = std::make_error_code(std::errc::permission_denied);
    auto choose = tower::Tower{}.service([denied](int input) -> Result<bool> {
        if (input < 0) {
            return tower::failure(denied);
        }
        return tower::success(input == 1);
    }).dispatch([&yes_calls] { return ++yes_calls; }, [&no_calls]() -> Result<int> {
        --no_calls;
        return tower::success(no_calls);
    });

    const auto yes = choose.call(1);
    ASSERT_TRUE(yes.has_value());
    EXPECT_EQ(yes.value(), 1);
    const auto no = choose.call(0);
    ASSERT_TRUE(no.has_value());
    EXPECT_EQ(no.value(), -1);
    const auto error = choose.call(-1);
    ASSERT_TRUE(error.has_error());
    EXPECT_EQ(error.error(), denied);
    EXPECT_EQ(yes_calls, 1);
    EXPECT_EQ(no_calls, -1);

    const auto label = tower::Tower{}.service([] { return false; })
        .dispatch([] { return "review"; }, [] { return "approved"; }).call();
    ASSERT_TRUE(label.has_value());
    EXPECT_STREQ(label.value(), "approved");
}

TEST(PipelineTest, PredicateAndExplicitBooleanDispatchForwardThePayload) {
    auto choose = tower::Tower{}.service([](int value) { return std::make_unique<int>(value); })
        .dispatch([](const std::unique_ptr<int>& value) { return *value > 0; },
                  [](std::unique_ptr<int> value) { return *value + 1; },
                  [](std::unique_ptr<int> value) { return *value - 1; });
    const auto positive = choose.call(4);
    ASSERT_TRUE(positive.has_value());
    EXPECT_EQ(positive.value(), 5);
    const auto negative = choose.call(-4);
    ASSERT_TRUE(negative.has_value());
    EXPECT_EQ(negative.value(), -5);

    const bool choose_first = false;
    const auto fixed = tower::Tower{}.service([](int value) { return value; })
        .dispatch(choose_first, [](int value) { return value + 1; }, [](int value) { return value * 2; });
    const auto fixed_result = fixed.call(10);
    ASSERT_TRUE(fixed_result.has_value());
    EXPECT_EQ(fixed_result.value(), 20);
}

struct Retail { int units; };
struct Wholesale { int units; };
struct NormalizeOrder {
    Retail operator()(int units) const { return {units}; }
    Wholesale operator()(long units) const { return {static_cast<int>(units)}; }
};

TEST(PipelineTest, DispatchesIntermediateTypesThroughAConfiguredRouter) {
    const auto routes = tower::Tower{}.router()
        .route<Retail>([](Retail order) { return order.units * 10; })
        .route<Wholesale>([](Wholesale order) { return order.units * 7; })
        .build();
    const auto pipeline = tower::Tower{}.service(NormalizeOrder{}).dispatch(routes);
    const auto retail = pipeline.call(5);
    ASSERT_TRUE(retail.has_value());
    EXPECT_EQ(retail.value(), 50);
    const auto wholesale = pipeline.call(5L);
    ASSERT_TRUE(wholesale.has_value());
    EXPECT_EQ(wholesale.value(), 35);
    static_assert(!tower::is_service_for<decltype(pipeline)&, std::string>::value,
                  "unhandled input types are rejected during substitution");
}

struct Doubler : tower::Pipeline<Doubler> {
    int operator()(int value) const { return value * 2; }
};

TEST(PipelineTest, SupportsCustomCrtpCallablesAndMoveOnlyServiceState) {
    const auto custom = Doubler{}.next([](int value) { return value + 1; });
    const auto custom_result = custom.call(3);
    ASSERT_TRUE(custom_result.has_value());
    EXPECT_EQ(custom_result.value(), 7);

    auto service = tower::Tower{}.service([offset = std::make_unique<int>(2)](int value) {
        return *offset + value;
    });
    auto pipeline = std::move(service).next([](int value) { return value * 2; });
    const auto result = pipeline.call(3);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 10);
}

TEST(PipelineTest, RejectsIncompatibleBranchesPredicatesAndBorrowedResults) {
    const auto bad_branches = tower::Tower{}.service([] { return true; })
        .dispatch([] { return 1; }, [] { return std::string("no"); });
    static_assert(!tower::is_service_for<decltype(bad_branches)&>::value, "branches need equal result types");
    const auto wrong_discriminant = tower::Tower{}.service([] { return 1; })
        .dispatch([] { return 2; }, [] { return 3; });
    static_assert(!tower::is_service_for<decltype(wrong_discriminant)&>::value, "the discriminator must be bool");
    const auto bad_guard = tower::Tower{}.service([](int value) { return value; })
        .guard([](int value) -> Result<bool> { return tower::success(value > 0); });
    static_assert(!tower::is_service_for<decltype(bad_guard)&, int>::value, "guard expects a bool predicate");
    const auto dangling = tower::Tower{}.service([] { return std::string("temporary"); })
        .next([](const std::string& text) { return text.c_str(); });
    static_assert(!tower::is_service_for<decltype(dangling)&>::value, "an owned intermediate dies before return");
}

}  // namespace
