#include <tower/tower.hpp>

#include <iostream>
#include <system_error>

namespace {

struct DeliveryOrder { unsigned units; };
struct PickupOrder { unsigned units; };
struct Parcel { unsigned units; };
struct CounterPickup { unsigned units; };

struct NormalizeOrder {
    Parcel operator()(DeliveryOrder order) const { return {order.units}; }
    CounterPickup operator()(PickupOrder order) const { return {order.units}; }
};

struct Quote { unsigned subtotal_cents; };
struct Invoice { unsigned total_cents; };
struct Receipt { unsigned total_cents; bool requires_review; };

}  // namespace

int main() {
    const auto rates = tower::Tower{}.router()
        .route<Parcel>([](Parcel request) { return Quote{700 * request.units + 500}; })
        .route<CounterPickup>([](CounterPickup request) { return Quote{700 * request.units}; })
        .build();

    // Validate the order before pricing, then review high-value invoices.
    const auto fulfill = tower::Tower{}.service(NormalizeOrder{})
        .guard([](const auto& order) { return order.units > 0 && order.units <= 100; })
        .dispatch(rates)
        .next([](Quote quote) { return Invoice{quote.subtotal_cents + quote.subtotal_cents / 20}; })
        .dispatch([](const Invoice& invoice) { return invoice.total_cents >= 3000; },
                  [](Invoice invoice) { return Receipt{invoice.total_cents, true}; },
                  [](Invoice invoice) { return Receipt{invoice.total_cents, false}; });

    const auto delivery = fulfill.call(DeliveryOrder{4});
    const auto pickup = fulfill.call(PickupOrder{2});
    const auto invalid = fulfill.call(PickupOrder{0});
    if (!delivery || !pickup || !invalid.has_error()) {
        std::cerr << "Unexpected fulfillment result\n";
        return 1;
    }

    std::cout << "Delivery " << delivery.value().total_cents << " cents, review "
              << std::boolalpha << delivery.value().requires_review << '\n'
              << "Pickup " << pickup.value().total_cents << " cents, review "
              << pickup.value().requires_review << '\n'
              << "Invalid order " << invalid.error().message() << '\n';

    return delivery.value().total_cents == 3465 && delivery.value().requires_review &&
        pickup.value().total_cents == 1470 && !pickup.value().requires_review &&
        invalid.error() == std::errc::invalid_argument ? 0 : 1;
}
