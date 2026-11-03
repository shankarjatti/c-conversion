secuecom_moc(AIR_MOC ../common/link.h airspec.h mapwidget.h mainwindow.h)
add_executable(air_ui main.cpp ../common/link.cpp tiles.cpp airspec.cpp mapwidget.cpp mainwindow.cpp ${AIR_MOC})
target_include_directories(air_ui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_link_libraries(air_ui PRIVATE secuecom_deps "${SYSLIB}/libsqlite3.so.0")
