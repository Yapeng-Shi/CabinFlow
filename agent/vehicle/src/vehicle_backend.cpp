#include <cabinflow/agent/vehicle_backend.hpp>

namespace cabinflow::agent {

const char* vehicle_error_name(VehicleError error) noexcept {
    switch (error) {
        case VehicleError::none: return "none";
        case VehicleError::cancelled: return "cancelled";
        case VehicleError::unavailable: return "unavailable";
        case VehicleError::invalid_response: return "invalid_response";
        case VehicleError::rejected: return "rejected";
        case VehicleError::timeout: return "timeout";
        case VehicleError::state_unknown: return "state_unknown";
        case VehicleError::io_error: return "io_error";
    }
    return "invalid_error";
}

}  // namespace cabinflow::agent
