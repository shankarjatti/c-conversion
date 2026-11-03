add_executable(securecom_terr_bridge main.cpp)
target_include_directories(securecom_terr_bridge SYSTEM PRIVATE /usr/include)
target_link_libraries(securecom_terr_bridge PRIVATE backend_deps "${SYSLIB}/libgnuradio-pmt.so.3.10.1.1")
