secuecom_moc(TERR_MOC ../common/link.h plots.h audio.h mainwindow.h)
add_executable(terr_ui main.cpp ../common/link.cpp plots.cpp audio.cpp mainwindow.cpp ${TERR_MOC})
target_include_directories(terr_ui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_link_libraries(terr_ui PRIVATE secuecom_deps)
