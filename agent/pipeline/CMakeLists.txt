add_library(cabinflow_voice_pipeline src/voice_pipeline.cpp)
add_library(CabinFlow::VoicePipeline ALIAS cabinflow_voice_pipeline)
target_include_directories(cabinflow_voice_pipeline PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../inference/include)
target_link_libraries(cabinflow_voice_pipeline PUBLIC
    CabinFlow::Runtime CabinFlow::Gateway CabinFlow::FakeVehicle
    PRIVATE CabinFlow::AgentProtocol
    CabinFlow::DialogueTextNode CabinFlow::TransportInMemory)
target_compile_features(cabinflow_voice_pipeline PUBLIC cxx_std_17)
cabinflow_enable_compiler_warnings(cabinflow_voice_pipeline)
