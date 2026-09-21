add_library(cabinflow_dialogue_text_node
    src/dialogue_text_node.cpp
)
add_library(CabinFlow::DialogueTextNode ALIAS cabinflow_dialogue_text_node)

target_include_directories(cabinflow_dialogue_text_node
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(cabinflow_dialogue_text_node
    PUBLIC
        CabinFlow::Runtime
    PRIVATE
        CabinFlow::AgentProtocol
)

target_compile_features(cabinflow_dialogue_text_node PUBLIC cxx_std_17)
cabinflow_enable_compiler_warnings(cabinflow_dialogue_text_node)
