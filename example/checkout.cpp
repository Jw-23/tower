#include "checkout.hpp"

#include <iostream>

int main() {
    checkout::PriceBook prices;
    checkout::QuoteAudit audit;
    auto quote_order = checkout::make_quote_service(prices, audit);

    const auto retail = quote_order(checkout::RetailOrder{0, 2});
    prices.fail_next(1);
    const auto wholesale = quote_order(checkout::WholesaleOrder{1, 20});
    const auto rejected = quote_order(checkout::WholesaleOrder{1, 4});
    if (!retail || !wholesale || !rejected.has_error()) {
        std::cerr << "Unexpected checkout result\n";
        return 1;
    }

    std::cout << "Retail quote " << retail.value().total_cents << " cents\n"
              << "Wholesale quote " << wholesale.value().total_cents << " cents\n"
              << "Rejected order " << rejected.error().message() << '\n'
              << "Price lookups " << prices.calls() << '\n'
              << "Audit accepted " << audit.accepted << ", rejected " << audit.rejected << '\n';

    return retail.value().total_cents == 2598 && wholesale.value().total_cents == 8100 &&
        rejected.error() == std::errc::invalid_argument && prices.calls() == 3 &&
        audit.accepted == 2 && audit.rejected == 1 && audit.total_cents == 10698 ? 0 : 1;
}
