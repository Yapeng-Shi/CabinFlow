add_executable(fake_voice_demo main.cpp)
target_link_libraries(fake_voice_demo PRIVATE
    CabinFlow::Runtime
    CabinFlow::Gateway
    CabinFlow::AgentProtocol
    CabinFlow::DialogueTextNode
    CabinFlow::TransportInMemory
)
target_compile_features(fake_voice_demo PRIVATE cxx_std_17)
cabinflow_enable_compiler_warnings(fake_voice_demo)
