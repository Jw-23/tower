# tower

一个 C++14 同步、纯头文件的 Service 和 Layer 库。公共入口是 `<tower/tower.hpp>`，CMake 目标是 `tower::tower`。依赖 Boost.Outcome 的头文件。

## 服务与中间件

```cpp
#include <tower/tower.hpp>

#include <iostream>
#include <string>

int main() {
    auto service = tower::Tower{}
        .log_to(std::clog)
        .next(tower::Retry<>{3})
        .router()
        .route<int>([](int n) { return n * 2; })
        .route<std::string>([](const std::string& s) -> tower::Result<int> {
            if (s.empty()) {
                return tower::failure(std::make_error_code(std::errc::invalid_argument));
            }
            return tower::success(static_cast<int>(s.size()));
        })
        .build();

    auto result = service.call(std::string("tower"));
    if (result) {
        std::cout << result.value() << '\n';
    }
    return result ? 0 : 1;
}
```

- 从 `Tower{}` 开始，通过 `.next(layer)` 依次注册中间件，先注册的在外层。`.service(handler)` 直接生成可 `.call(...)` 的服务。普通值、`void`、左值引用分别包装成 `Result<T>`、`Result<void>`、`Result<std::reference_wrapper<T>>`。已有的 Boost.Outcome `basic_result` 保留原类型。
- `Layer::layer(inner)` 包装内层服务。自定义层可以实现这个方法，也可以用 builder 的 `.layer(factory)` 传入接受内层服务的工厂。`.log(sink)` 向回调传递结果的常量引用，`.log_to(stream)` 输出成功或失败。
- `.retry(n, predicate)` 最多尝试 `n` 次，含首次调用，`0` 按 `1` 处理。省略谓词时重试所有错误。左值参数沿用同一引用，右值在最后一次之前使用副本。仅可移动的请求可通过左值引用交给支持该签名的服务。右值调用的结果应拥有其数据，或者只引用生命周期更长的对象。
- `.router().route<T>(handler)... .build()` 逐个注册路由并生成可调用的服务。路由仍按请求的 `std::decay` 类型在编译期分发。每个请求类型只能注册一次，所有服务的完整 `Result` 类型必须相同。`is_service_for<S, Args...>` 和 `is_layer_for<L, S, Args...>` 可用于检测指定调用签名。
- `tower::Result<T>` 使用 `std::error_code`。回调用 `tower::failure(error)` 返回失败，调用者先检查 `Result` 再读取值或错误。对错误分支调用 `.value()` 会终止进程。

## 依赖注入

```cpp
auto wired = tower::Tower{}
    .dependencies()
    .with(10)
    .emplace<std::string>("jobs")
    .wire([](int& count, const std::string& label) {
        return [&count, &label](int amount) {
            count += amount;
            return label + "=" + std::to_string(count);
        };
    });
if (wired) {
    auto result = wired.value().call(2);
}
```

容器按具体类型持有一个实例。`.with(value)` 按 `std::decay` 后的类型存储传入值，`.emplace<T>(args...)` 可直接构造不可移动对象或定长数组。`.wire(factory)` 在组装时解析依赖，返回的服务拥有容器，并应用 `.dependencies()` 之前注册的中间件。`.invoke(function)` 直接注入参数并调用函数，命名的作用域也支持 `.get<T>()` 和返回依赖的借用引用。泛型 lambda 和重载函数对象可以用 `.wire_as<Args...>(factory)` 或 `.invoke_as<Args...>(function)` 指定参数类型。借用依赖的结果应由命名作用域持有依赖的生命周期。缺失和重复注册分别返回 `tower::errc::missing_dependency` 和 `tower::errc::duplicate_dependency`。

## 业务示例

```sh
cmake --preset debug
cmake --build build/debug --target tower_example_checkout tower_example_invoice
./build/debug/example/tower_example_checkout
./build/debug/example/tower_example_invoice
```

- [结账报价](example/checkout.hpp)把零售和批发规则放在各自的处理器里，用 `.router().route<T>(...)` 按订单类型注册处理器。`make_quote_service` 集中声明重试和审计，审计包在重试外层，因此每笔订单只记一次最终结果。
- [账单计算](example/invoice.cpp)用 `.dependencies().with(...).wire(...)` 在启动时解析税率和运费规则。服务持有规则，业务调用只接收账单请求。

## 构建与检查

手动配置需要 CMake 3.14 或以上。下面的预设命令使用 CMake 3.28 或以上及 Ninja。

```sh
cmake --preset debug
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
clang-tidy -p build/debug tests/tower_tests.cpp tests/dependency_fixture.cpp
```

`debug` 会构建 GoogleTest 单元测试并生成 `build/debug/compile_commands.json`。CTest 会逐项列出通过 `gtest_discover_tests` 注册的测试。使用 `ctest --test-dir build/debug -R RouterTest` 可以只运行路由测试。`.clangd` 从编译数据库读取实际参数，`.clang-tidy` 提供静态分析、缺陷、性能、可移植性及兼容 C++14 的基础风格检查。

## 基准测试

```sh
cmake --preset benchmark
cmake --build build/benchmark --target tower_benchmarks
./build/benchmark/tower_benchmarks --benchmark_repetitions=5
```

`benchmark` preset 使用 Release 优化并开启 `TOWER_BUILD_BENCHMARKS`。Google Benchmark 对比直接调用、`Service`、路由和无 I/O 的日志回调，并测量重试首轮与第二轮成功，以及六参数注入全部命中、首个缺失和末个缺失的耗时。可以用 `--benchmark_filter=BM_InjectSix` 筛选注入场景，用 `--benchmark_out=results.json --benchmark_out_format=json` 保存结果。基准源码的 clangd 编译数据库由 `benchmark` preset 生成在 `build/benchmark`。

首次启用测试或基准时，CMake 会下载 GoogleTest 1.16.0 或 Google Benchmark 1.9.0 并校验 SHA-256。这两个版本支持 C++14。已有 CMake 工程可以通过 `add_subdirectory` 引入后链接 `tower::tower`。将 `BUILD_TESTING`、`TOWER_BUILD_BENCHMARKS` 和 `TOWER_BUILD_EXAMPLES` 设为 `OFF` 即可只使用头文件库。

### 与手写实现比较

```sh
./build/benchmark/tower_benchmarks \
    --benchmark_filter='BM_(DirectCall|ServiceCall|RouterCall|Checkout|Invoice)' \
    --benchmark_min_time=0.08s --benchmark_repetitions=7 \
    --benchmark_enable_random_interleaving=true --benchmark_report_aggregates_only=true \
    --benchmark_out=build/benchmark/business-results.json --benchmark_out_format=json
```

[对照基准](benchmarks/business_benchmarks.cpp)共享示例中的业务规则和 `Result` 类型。结账基线手写订单分发、重试循环和审计调用。账单基线分别测量直接调用已绑定规则，以及每次请求手动查找规则。计时前校验金额、错误、重试次数和审计计数。组装服务和注册依赖在计时循环外完成，审计在两组中都使用内存计数。

本机 Apple Silicon、Apple Clang 21、C++14 Release 下随机交错跑 7 轮，每次调用的 CPU 时间中位数如下。系统报告线程亲和性不可用。

| 场景 | 手写（ns） | Tower（ns） |
| --- | ---: | ---: |
| 简单调用 / `Service` | 0.688 | 0.689 |
| 简单调用 / 类型路由 | 0.688 | 0.689 |
| 零售报价成功 | 1.98 | 1.19 |
| 批发报价重试一次后成功 | 2.42 | 2.88 |
| 报价因无效 SKU 失败 | 1.89 | 0.85 |
| 批发报价重试耗尽 | 2.44 | 3.15 |
| 账单，启动时绑定规则 | 0.747 | 0.750 |
| 账单，每次请求查找规则 | 14.50 | 14.40 |

批发报价重试耗尽的差距为 0.71 ns。账单示例采用启动时绑定，把类型容器查找移到了请求处理之前。
