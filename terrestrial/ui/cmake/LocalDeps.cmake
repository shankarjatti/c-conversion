set(DEVROOT "$ENV{HOME}/Downloads/tutorials/cpp_ui_devroot/prefix" CACHE PATH "unpacked -dev packages (or / for a normal install)")
set(QTINC "${DEVROOT}/usr/include/x86_64-linux-gnu/qt5")
set(SYSLIB "/lib/x86_64-linux-gnu")
foreach(f IN ITEMS "${QTINC}/QtWidgets/QApplication" "${DEVROOT}/usr/include/zmq.h" "${DEVROOT}/usr/include/pulse/simple.h")
  if(NOT EXISTS "${f}")
    message(FATAL_ERROR "missing ${f}: unpack the -dev packages into DEVROOT (without root: apt-get download the -dev packages and dpkg -x them into DEVROOT; or install them and use -DDEVROOT=/)")
  endif()
endforeach()

add_library(secuecom_deps INTERFACE)
target_include_directories(secuecom_deps SYSTEM INTERFACE
  "${QTINC}" "${QTINC}/QtCore" "${QTINC}/QtGui" "${QTINC}/QtWidgets" "${QTINC}/QtNetwork" "${DEVROOT}/usr/include")
target_link_libraries(secuecom_deps INTERFACE
  "${SYSLIB}/libQt5Widgets.so.5" "${SYSLIB}/libQt5Gui.so.5" "${SYSLIB}/libQt5Network.so.5" "${SYSLIB}/libQt5Core.so.5"
  "${SYSLIB}/libzmq.so.5" "${SYSLIB}/libpulse-simple.so.0" "${SYSLIB}/libpulse.so.0")

target_compile_options(secuecom_deps INTERFACE -fPIC)

# moc: only needed for classes with Q_OBJECT (the real UI); found from the system's qtbase5-dev-tools
find_program(MOC_EXECUTABLE NAMES moc PATHS /usr/lib/qt5/bin /usr/bin REQUIRED)
function(secuecom_moc out)   # secuecom_moc(<var> header1.h header2.h ...)
  set(gen "")
  foreach(h IN LISTS ARGN)
    get_filename_component(n "${h}" NAME_WE)
    set(o "${CMAKE_CURRENT_BINARY_DIR}/moc_${n}.cpp")
    add_custom_command(OUTPUT "${o}" COMMAND "${MOC_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/${h}" -o "${o}"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${h}" VERBATIM)
    list(APPEND gen "${o}")
  endforeach()
  set(${out} "${gen}" PARENT_SCOPE)
endfunction()
