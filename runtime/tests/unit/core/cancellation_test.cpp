#include <iostream>
#include <string_view>

#include <cabinflow/runtime/cancellation.hpp>

namespace {

bool expect(bool condition, std::string_view name) {
    if (condition) {
        return true;
    }

    std::cerr << "expectation failed: " << name << '\n';
    return false;
}

}  // namespace

int main() {
    cabinflow::runtime::CancellationRegistry cancellations;
    const auto invalid = cancellations.token_for("", "route-work");
    const auto driver_route =
        cancellations.token_for("driver-session", "route-work");
    const auto driver_media =
        cancellations.token_for("driver-session", "media-work");
    const auto passenger_route =
        cancellations.token_for("passenger-session", "route-work");

    if (!expect(!invalid.valid(), "invalid scope rejected") ||
        !expect(driver_route.valid(), "valid work token") ||
        !expect(!driver_route.cancelled(), "initially active") ||
        !expect(cancellations.cancel_work("driver-session", "route-work"),
                "work cancelled") ||
        !expect(driver_route.cancelled(), "matching work observes cancellation") ||
        !expect(!driver_media.cancelled(), "other work stays active") ||
        !expect(!passenger_route.cancelled(), "other session stays active") ||
        !expect(!cancellations.cancel_work("driver-session", "route-work"),
                "cancellation is idempotent") ||
        !expect(cancellations.cancel_session("driver-session"),
                "session cancelled") ||
        !expect(driver_media.cancelled(), "session cancellation fans out") ||
        !expect(!passenger_route.cancelled(), "session isolation")) {
        return 1;
    }

    const auto late_token =
        cancellations.token_for("driver-session", "late-work");
    if (!expect(late_token.cancelled(), "late token sees session cancellation")) {
        return 1;
    }

    return 0;
}
