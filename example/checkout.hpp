#pragma once

#include <tower/layer.hpp>
#include <tower/router.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <system_error>

namespace checkout {

struct RetailOrder { std::size_t sku; unsigned units; };
struct WholesaleOrder { std::size_t sku; unsigned units; };
struct Quote { std::uint64_t total_cents; };

constexpr std::size_t quote_attempts = 3;

class PriceBook {
public:
    void fail_next(unsigned count) { unavailable_calls_ = count; }
    unsigned calls() const { return calls_; }

    tower::Result<Quote> price(std::size_t sku, unsigned units, unsigned discount_percent) {
        ++calls_;
        if (sku >= unit_prices_.size() || units == 0) {
            return tower::failure(std::make_error_code(std::errc::invalid_argument));
        }
        if (unavailable_calls_ > 0) {
            --unavailable_calls_;
            return tower::failure(std::make_error_code(std::errc::resource_unavailable_try_again));
        }
        const auto gross = static_cast<std::uint64_t>(unit_prices_[sku]) * units;
        return tower::success(Quote{gross - gross * discount_percent / 100});
    }

private:
    std::array<unsigned, 4> unit_prices_{{1299, 450, 825, 2499}};
    unsigned unavailable_calls_ = 0;
    unsigned calls_ = 0;
};

// Each handler owns the rules for its own order type.
struct RetailQuote {
    PriceBook& prices;

    tower::Result<Quote> operator()(const RetailOrder& order) const {
        return prices.price(order.sku, order.units, 0);
    }
};

struct WholesaleQuote {
    PriceBook& prices;

    tower::Result<Quote> operator()(const WholesaleOrder& order) const {
        if (order.units < 10) {
            return tower::failure(std::make_error_code(std::errc::invalid_argument));
        }
        return prices.price(order.sku, order.units, 10);
    }
};

struct RetryUnavailable {
    tower::Error unavailable = std::make_error_code(std::errc::resource_unavailable_try_again);

    bool operator()(const tower::Error& error) const { return error == unavailable; }
};

struct QuoteAudit {
    unsigned accepted = 0;
    unsigned rejected = 0;
    std::uint64_t total_cents = 0;

    void operator()(const tower::Result<Quote>& result) {
        if (result) {
            ++accepted;
            total_cents += result.value().total_cents;
        } else {
            ++rejected;
        }
    }
};

inline auto make_quote_service(PriceBook& prices, QuoteAudit& audit) {
    auto routes = tower::make_router(
        tower::route<RetailOrder>(tower::make_service(RetailQuote{prices})),
        tower::route<WholesaleOrder>(tower::make_service(WholesaleQuote{prices})));

    // Put the audit outside retry so each order contributes one final outcome.
    return tower::apply_layers(routes, tower::log(std::ref(audit)),
                               tower::retry(quote_attempts, RetryUnavailable{}));
}

}  // namespace checkout
