option(CABINFLOW_ENABLE_ASAN "Build with AddressSanitizer" OFF)

if(CABINFLOW_ENABLE_ASAN)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "CABINFLOW_ENABLE_ASAN requires GCC or Clang")
    endif()

    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()
