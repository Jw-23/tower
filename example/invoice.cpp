#include "invoice.hpp"

#include <tower/tower.hpp>

#include <iostream>

int main() {
    // The wired service owns the policy registry and resolves its inputs at startup.
    auto service = tower::Tower{}
        .dependencies()
        .with(billing::TaxRules{750})
        .with(billing::ShippingRules{499, 150})
        .wire(billing::make_invoice_handler);
    if (!service) {
        std::cerr << "Could not wire invoice service " << service.error().message() << '\n';
        return 1;
    }

    const auto invoice = service.value().call(billing::InvoiceRequest{2399, 2});
    const auto rejected = service.value().call(billing::InvoiceRequest{0, 2});
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
