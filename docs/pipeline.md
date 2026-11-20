# Service pipelines

Every `tower::Service<F>` inherits `tower::Pipeline<tower::Service<F>>` through the Curiously Recurring Template Pattern (CRTP). Composition stores concrete callables by value and produces another `Service`, so the resulting pipeline can be called, extended, passed to a layer, or registered as a route. Include `<tower/tower.hpp>` to use the builder and router, or `<tower/pipeline.hpp>` for the mixin and `Service`.

## Sequence and validation

```cpp
auto total = tower::Tower{}
    .service([](int units, int unit_price) { return units * unit_price; })
    .guard([](const int& units, const int& unit_price) {
        return units > 0 && units <= 100 && unit_price >= 0 && unit_price <= 1000;
    })
    .next([](int subtotal) { return subtotal + 500; });

const auto quote = total.call(3, 200);  // Result<int> containing 1100
```

- `service.next(continuation)` runs the continuation on success. It forwards an owned value as an rvalue, unwraps a `std::reference_wrapper<T>` to `T&`, or calls the continuation with zero arguments after a successful `void` result. The continuation determines the final value type. Its result must be able to carry an error from the preceding service. The first error skips the rest of the pipeline.
- `service.guard(predicate, error)` checks the call arguments of the service it wraps. The predicate gets const lvalue references before arguments are forwarded to the service. It returns `bool`. Rejection skips the wrapped service and returns `std::errc::invalid_argument` by default. You can supply a different `std::errc` or compatible error code. To validate an intermediate value, pass an already guarded service to `.next(...)`.
- Return owned output from a continuation that consumes an owned intermediate. Direct pointers and reference wrappers in that output are rejected at compile time because the intermediate is destroyed at the end of the step. Keep the source data alive when composing services that deliberately return borrowed values.

`Tower{}.next(layer)` registers middleware on a builder. After `.service(...)` or `.build()`, `Service::next(...)` sequences an operation after that service. A named service can be copied into a new pipeline. Use `std::move(service)` to transfer a service with move-only state.

## Dispatch

| Form | Selection | What the selected service receives |
| --- | --- | --- |
| `.dispatch(on_true, on_false)` | The previous service returns `bool` or `Result<bool>` | Zero arguments |
| `.dispatch(predicate, on_true, on_false)` | A `bool` predicate reads the previous value by const reference | The previous value, with the same forwarding rules as `.next(...)` |
| `.dispatch(flag, on_true, on_false)` | A supplied `bool` selects a branch when composing the pipeline | The previous value |
| `.dispatch(router)` | The configured router selects a handler by the C++ type of the previous value | The previous value |

Both branches must have the same complete result type after adapting plain callables. Only the selected branch runs. A successful boolean value of `false` runs the second branch. An upstream error skips both branches. Type dispatch uses the existing `Tower{}.router().route<T>(...).build()` API and requires one compatible result type across its routes.

```cpp
auto notice = tower::Tower{}.service([](int total) { return total >= 3000; })
    .dispatch([] { return "review"; }, [] { return "approved"; });

const auto decision = notice.call(3465);  // "review"
```

## Extend a custom callable

Derive a custom callable from `Pipeline<YourType>` to add the same composition methods to it. The mixin uses the derived type's `operator()` as the starting operation.

```cpp
struct Double : tower::Pipeline<Double> {
    int operator()(int value) const { return value * 2; }
};

auto pipeline = Double{}.next([](int value) { return value + 1; });
const auto result = pipeline.call(3);  // Result<int> containing 7
```

## Run the fulfillment example

The [fulfillment example](../example/pipeline.cpp) validates both order types, dispatches each normalized request to the matching pricing service, computes the invoice, and picks a review outcome with a boolean predicate.

```sh
cmake --preset debug
cmake --build build/debug --target tower_example_pipeline
./build/debug/example/tower_example_pipeline
```
