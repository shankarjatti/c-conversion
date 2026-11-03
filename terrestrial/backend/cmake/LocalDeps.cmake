# Backend dependencies (no Qt): libzmq (C API) and nlohmann/json (header only). Same DEVROOT scheme as the console's:
# an unpacked copy of the -dev packages; with -DDEVROOT=/ a normal system install works too.
set(DEVROOT "$ENV{HOME}/Downloads/tutorials/cpp_ui_devroot/prefix" CACHE PATH "unpacked -dev packages (or / for a normal install)")
set(SYSLIB "/lib/x86_64-linux-gnu")
foreach(f IN ITEMS "${DEVROOT}/usr/include/zmq.h" "${DEVROOT}/usr/include/nlohmann/json.hpp")
  if(NOT EXISTS "${f}")
    message(FATAL_ERROR "missing ${f}: unpack libzmq3-dev / nlohmann-json3-dev into DEVROOT (without root: apt-get download the -dev packages and dpkg -x them into DEVROOT; or install them and use -DDEVROOT=/)")
  endif()
endforeach()
add_library(backend_deps INTERFACE)
target_include_directories(backend_deps SYSTEM INTERFACE "${DEVROOT}/usr/include")
target_link_libraries(backend_deps INTERFACE "${SYSLIB}/libzmq.so.5" pthread)
target_compile_options(backend_deps INTERFACE -Wall -Wextra -fPIC)
