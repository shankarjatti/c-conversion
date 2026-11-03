add_executable(securecom_air_bridge main.cpp)
target_include_directories(securecom_air_bridge PRIVATE ../common)
target_link_libraries(securecom_air_bridge PRIVATE backend_deps "${SYSLIB}/libgnuradio-pmt.so.3.10.1.1")
