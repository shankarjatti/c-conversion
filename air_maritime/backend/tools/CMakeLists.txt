add_executable(adsb_file_test adsb_file_test.cpp)
target_include_directories(adsb_file_test PRIVATE ../air_decoder)
target_compile_options(adsb_file_test PRIVATE -Wall -Wextra)
