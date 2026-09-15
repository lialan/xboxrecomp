cmake_minimum_required(VERSION 3.20)
project(wma_decoder_test C)
set(CMAKE_C_STANDARD 11)

add_executable(wma_decoder_test
    test_main.c
    ../../src/audio/wma_decoder.c
)
target_include_directories(wma_decoder_test PRIVATE ../../src/audio)
if(WIN32)
    target_link_libraries(wma_decoder_test PRIVATE mfplat mfreadwrite mfuuid mf ole32)
endif()

enable_testing()
add_test(NAME wma_decoder_test COMMAND wma_decoder_test)
