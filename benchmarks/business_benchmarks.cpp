#include "checkout.hpp"
#include "invoice.hpp"

#include <tower/dependencies.hpp>

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <system_error>

namespace {

struct ManualCheckout {
    checkout::PriceBook& prices;
    checkout::QuoteAudit& audit;
    checkout::RetryUnavailable retryable{};

    auto operator()(const checkout::RetailOrder& order) -> tower::Result<checkout::Quote> {
        return quote(checkout::RetailQuote{prices}, order);
    }

    auto operator()(const checkout::WholesaleOrder& order) -> tower::Result<checkout::Quote> {
        return quote(checkout::WholesaleQuote{prices}, order);
    }

private:
    template<class Handler, class Order>
    auto quote(Handler handler, const Order& order) -> tower::Result<checkout::Quote> {
        for (std::size_t attempt = 1;; ++attempt) {
            auto result = handler(order);
            if (attempt == checkout::quote_attempts || !result.has_error() ||
                !retryable(result.error())) {
                audit(result);
                return result;
            }
        }
    }
};

struct QuoteCase {
    unsigned outages;
    unsigned expected_calls;
    std::uint64_t expected_cents;
    std::errc expected_error{};
};

template<class Factory, class Order>
void measure_checkout(benchmark::State& state, Factory make_service, const Order& order,
                      const QuoteCase& scenario) {
    checkout::PriceBook prices;
    checkout::QuoteAudit audit;
    auto quote = make_service(prices, audit);

    prices.fail_next(scenario.outages);
    const auto sample = quote(order);
    const bool expect_success = scenario.expected_error == std::errc{};
    const bool matches = expect_success ?
        sample.has_value() && sample.value().total_cents == scenario.expected_cents :
        sample.has_error() && sample.error() == std::make_error_code(scenario.expected_error);
    if (!matches || prices.calls() != scenario.expected_calls ||
        audit.accepted != static_cast<unsigned>(expect_success) ||
        audit.rejected != static_cast<unsigned>(!expect_success) ||
        audit.total_cents != scenario.expected_cents) {
        state.SkipWithError("unexpected checkout result, attempts or audit totals");
        return;
    }

    for (auto _ : state) {
        auto request = order;
        prices.fail_next(scenario.outages);
        benchmark::DoNotOptimize(request);
        benchmark::DoNotOptimize(prices);
        auto result = quote(request);
        benchmark::DoNotOptimize(result);
        benchmark::DoNotOptimize(audit);
    }
}

template<class Order>
void BM_CheckoutManual(benchmark::State& state, const Order& order, const QuoteCase& scenario) {
    measure_checkout(state, [](checkout::PriceBook& prices, checkout::QuoteAudit& audit) {
        return ManualCheckout{prices, audit};
    }, order, scenario);
}

template<class Order>
void BM_CheckoutTower(benchmark::State& state, const Order& order, const QuoteCase& scenario) {
    measure_checkout(state, checkout::make_quote_service, order, scenario);
}

auto make_manual_invoice(const tower::Dependencies& dependencies)
    -> tower::Result<billing::CalculateInvoice> {
    const auto tax = dependencies.get<billing::TaxRules>();
    if (!tax) {
        return tax.as_failure();
    }
    const auto shipping = dependencies.get<billing::ShippingRules>();
    if (!shipping) {
        return shipping.as_failure();
    }
    return tower::success(billing::CalculateInvoice{tax.value().get(), shipping.value().get()});
}

auto make_wired_invoice(const tower::Dependencies& dependencies) {
    return dependencies.invoke(billing::make_invoice_service);
}

struct ManualInvoiceLookup {
    const tower::Dependencies& dependencies;

    auto operator()(const billing::InvoiceRequest& request) const -> tower::Result<billing::Invoice> {
        const auto invoice = make_manual_invoice(dependencies);
        if (!invoice) {
            return invoice.as_failure();
        }
        return invoice.value()(request);
    }
};

struct TowerInvoiceLookup {
    const tower::Dependencies& dependencies;

    auto operator()(const billing::InvoiceRequest& request) const -> tower::Result<billing::Invoice> {
        return dependencies.invoke([&request](const billing::TaxRules& tax,
                                               const billing::ShippingRules& shipping) {
            return billing::CalculateInvoice{tax, shipping}(request);
        });
    }
};

template<class Lookup>
auto make_invoice_lookup(const tower::Dependencies& dependencies) -> tower::Result<Lookup> {
    return tower::success(Lookup{dependencies});
}

template<class Factory>
void BM_Invoice(benchmark::State& state, Factory make_service) {
    tower::Dependencies dependencies;
    if (!dependencies.insert(billing::TaxRules{750}) ||
        !dependencies.insert(billing::ShippingRules{499, 150})) {
        state.SkipWithError("could not register billing policies");
        return;
    }
    auto service = make_service(dependencies);
    if (!service) {
        state.SkipWithError("could not wire invoice service");
        return;
    }

    auto& invoice = service.value();
    const auto sample = invoice(billing::InvoiceRequest{2399, 2});
    const auto invalid = invoice(billing::InvoiceRequest{0, 2});
    if (!sample || sample.value().tax_cents != 179 || sample.value().shipping_cents != 799 ||
        sample.value().total_cents != 3377 || !invalid.has_error() ||
        invalid.error() != std::errc::invalid_argument) {
        state.SkipWithError("unexpected invoice result or validation error");
        return;
    }

    for (auto _ : state) {
        billing::InvoiceRequest request{2399, 2};
        benchmark::DoNotOptimize(request);
        auto result = invoice(request);
        benchmark::DoNotOptimize(result);
    }
}

constexpr checkout::RetailOrder retail_order{1, 3};
constexpr checkout::RetailOrder invalid_order{99, 3};
constexpr checkout::WholesaleOrder wholesale_order{1, 20};

constexpr QuoteCase retail_success{0, 1, 1350};
constexpr QuoteCase wholesale_retry{1, 2, 8100};
constexpr QuoteCase invalid_sku{0, 1, 0, std::errc::invalid_argument};
constexpr QuoteCase exhausted{3, 3, 0, std::errc::resource_unavailable_try_again};

BENCHMARK_CAPTURE(BM_CheckoutManual<checkout::RetailOrder>, RetailSuccess, retail_order, retail_success);
BENCHMARK_CAPTURE(BM_CheckoutTower<checkout::RetailOrder>, RetailSuccess, retail_order, retail_success);
BENCHMARK_CAPTURE(BM_CheckoutManual<checkout::WholesaleOrder>, WholesaleOneRetry, wholesale_order,
                  wholesale_retry);
BENCHMARK_CAPTURE(BM_CheckoutTower<checkout::WholesaleOrder>, WholesaleOneRetry, wholesale_order,
                  wholesale_retry);
BENCHMARK_CAPTURE(BM_CheckoutManual<checkout::RetailOrder>, InvalidSku, invalid_order, invalid_sku);
BENCHMARK_CAPTURE(BM_CheckoutTower<checkout::RetailOrder>, InvalidSku, invalid_order, invalid_sku);
BENCHMARK_CAPTURE(BM_CheckoutManual<checkout::WholesaleOrder>, RetryExhausted, wholesale_order, exhausted);
BENCHMARK_CAPTURE(BM_CheckoutTower<checkout::WholesaleOrder>, RetryExhausted, wholesale_order, exhausted);

BENCHMARK_CAPTURE(BM_Invoice, ManualDirect, make_manual_invoice);
BENCHMARK_CAPTURE(BM_Invoice, TowerWired, make_wired_invoice);
BENCHMARK_CAPTURE(BM_Invoice, ManualLookupPerCall, make_invoice_lookup<ManualInvoiceLookup>);
BENCHMARK_CAPTURE(BM_Invoice, TowerLookupPerCall, make_invoice_lookup<TowerInvoiceLookup>);

}  // namespace
