add_executable(vehicle_ecu main.cpp)
target_link_libraries(vehicle_ecu PRIVATE CabinFlow::Vehicle)
cabinflow_enable_compiler_warnings(vehicle_ecu)
