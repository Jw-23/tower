#include <tower/tower.hpp>

#include <benchmark/benchmark.h>

namespace {

struct Increment {
    int operator()(int value) const { return value + 1; }
};

template<class Call>
void measure_transform(benchmark::State& state, Call call) {
    for (auto _ : state) {
        int request = 42;
        benchmark::DoNotOptimize(request);
        auto response = call(request);
        benchmark::DoNotOptimize(response);
    }
}

void BM_DirectCall(benchmark::State& state) {
    measure_transform(state, Increment{});
}

void BM_ServiceCall(benchmark::State& state) {
    auto service = tower::make_service(Increment{});
    measure_transform(state, [&service](int request) { return service(request).value(); });
}

void BM_RouterCall(benchmark::State& state) {
    auto router = tower::make_router(
        tower::route<long>(tower::make_service([](long value) { return value > 0 ? 1 : 0; })),
        tower::route<int>(tower::make_service(Increment{})));
    measure_transform(state, [&router](int request) { return router(request).value(); });
}

void BM_LogSink(benchmark::State& state) {
    int recorded = 0;
    auto service = tower::log([&recorded](const tower::Result<int>& result) {
        recorded = result.value();
        benchmark::DoNotOptimize(recorded);
    })(tower::make_service(Increment{}));
    measure_transform(state, [&service](int request) { return service(request).value(); });
}

struct RetryRequest {
    int value;
    int remaining_failures;
};

void BM_Retry(benchmark::State& state, int failures) {
    const auto unavailable = std::make_error_code(std::errc::resource_unavailable_try_again);
    auto service = tower::retry(2)(tower::make_service([unavailable](RetryRequest& request)
        -> tower::Result<int> {
        if (request.remaining_failures > 0) {
            --request.remaining_failures;
            return tower::failure(unavailable);
        }
        return tower::success(Increment{}(request.value));
    }));

    for (auto _ : state) {
        RetryRequest request{42, failures};
        benchmark::DoNotOptimize(request);
        auto result = service(request);
        benchmark::DoNotOptimize(result);
    }
}

template<int Index>
struct Dependency {
    explicit Dependency(int value) : value(value) {}
    int value;
};

template<int Index>
bool store_dependency(tower::Dependencies& dependencies, int missing_index) {
    if (Index == missing_index) {
        // Keep the number of entries consistent between success and failure runs.
        return dependencies.emplace<Dependency<6>>(Index).has_value();
    }
    return dependencies.emplace<Dependency<Index>>(Index).has_value();
}

int sum(const Dependency<0>& first, const Dependency<1>& second, const Dependency<2>& third,
        const Dependency<3>& fourth, const Dependency<4>& fifth, const Dependency<5>& sixth) {
    return first.value + second.value + third.value + fourth.value + fifth.value + sixth.value;
}

void BM_InjectSix(benchmark::State& state, int missing_index) {
    tower::Dependencies dependencies;
    if (!(store_dependency<0>(dependencies, missing_index) &&
          store_dependency<1>(dependencies, missing_index) &&
          store_dependency<2>(dependencies, missing_index) &&
          store_dependency<3>(dependencies, missing_index) &&
          store_dependency<4>(dependencies, missing_index) &&
          store_dependency<5>(dependencies, missing_index))) {
        state.SkipWithError("could not initialize dependencies");
        return;
    }

    for (auto _ : state) {
        benchmark::DoNotOptimize(dependencies);
        auto result = dependencies.invoke(sum);
        benchmark::DoNotOptimize(result);
    }
}

BENCHMARK(BM_DirectCall);
BENCHMARK(BM_ServiceCall);
BENCHMARK(BM_RouterCall);
BENCHMARK(BM_LogSink);
BENCHMARK_CAPTURE(BM_Retry, FirstAttemptSucceeds, 0);
BENCHMARK_CAPTURE(BM_Retry, SecondAttemptSucceeds, 1);
BENCHMARK_CAPTURE(BM_InjectSix, AllPresent, -1);
BENCHMARK_CAPTURE(BM_InjectSix, FirstMissing, 0);
BENCHMARK_CAPTURE(BM_InjectSix, LastMissing, 5);

}  // namespace
