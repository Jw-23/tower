#include "invoice.hpp"

#include <tower/dependencies.hpp>

#include <iostream>

int main() {
    tower::Dependencies dependencies;
    if (!dependencies.insert(billing::TaxRules{750}) ||
        !dependencies.insert(billing::ShippingRules{499, 150})) {
        std::cerr << "Could not register billing policies\n";
        return 1;
    }

    // The registry owns both policies and outlives the service that borrows them.
    auto service = dependencies.invoke(billing::make_invoice_service);
    if (!service) {
        std::cerr << "Could not wire invoice service " << service.error().message() << '\n';
        return 1;
    }

    const auto invoice = service.value()(billing::InvoiceRequest{2399, 2});
    const auto rejected = service.value()(billing::InvoiceRequest{0, 2});
    if (!invoice || !rejected.has_error()) {
        std::cerr << "Unexpected invoice result\n";
        return 1;
    }
    std::cout << "Tax " << invoice.value().tax_cents << " cents\n"
              << "Shipping " << invoice.value().shipping_cents << " cents\n"
              << "Invoice total " << invoice.value().total_cents << " cents\n"
              << "Rejected invoice " << rejected.error().message() << '\n';

    return invoice.value().tax_cents == 179 && invoice.value().shipping_cents == 799 &&
        invoice.value().total_cents == 3377 && rejected.error() == std::errc::invalid_argument ? 0 : 1;
}
