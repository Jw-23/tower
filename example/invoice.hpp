#pragma once

#include <tower/service.hpp>

#include <cstdint>
#include <system_error>

namespace billing {

struct TaxRules { unsigned basis_points; };
struct ShippingRules { unsigned base_cents; unsigned per_parcel_cents; };

struct InvoiceRequest {
    unsigned subtotal_cents;
    unsigned parcels;
};

struct Invoice {
    std::uint64_t tax_cents;
    std::uint64_t shipping_cents;
    std::uint64_t total_cents;
};

struct CalculateInvoice {
    const TaxRules& tax;
    const ShippingRules& shipping;

    tower::Result<Invoice> operator()(const InvoiceRequest& request) const {
        if (request.subtotal_cents == 0 || request.parcels == 0 || request.parcels > 100) {
            return tower::failure(std::make_error_code(std::errc::invalid_argument));
        }
        const auto tax_cents = static_cast<std::uint64_t>(request.subtotal_cents) * tax.basis_points / 10000;
        const auto shipping_cents = shipping.base_cents +
            static_cast<std::uint64_t>(shipping.per_parcel_cents) * request.parcels;
        return tower::success(Invoice{tax_cents, shipping_cents,
                                      request.subtotal_cents + tax_cents + shipping_cents});
    }
};

// Resolve these policy references once when wiring the invoice service.
inline auto make_invoice_service(const TaxRules& tax, const ShippingRules& shipping) {
    return tower::make_service(CalculateInvoice{tax, shipping});
}

}  // namespace billing
