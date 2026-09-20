#include <cabinflow/runtime/version.hpp>

int main() {
    return cabinflow::runtime::kVersion.empty() ? 1 : 0;
}
