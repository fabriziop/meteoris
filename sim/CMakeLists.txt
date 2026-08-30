find_package(SoapySDR CONFIG REQUIRED)

SOAPY_SDR_MODULE_UTIL(
    TARGET meteorisSim
    SOURCES SoapyMeteorisSim.cpp
)

# Keep the freshly built plugin in a predictable directory. run_sim.sh uses
# this path so simulator tests cannot silently pick up an older installed
# meteoris_sim module.
set_target_properties(meteorisSim PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/soapy-modules"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(meteorisSim PRIVATE -Wall -Wextra -Wpedantic)
endif()
