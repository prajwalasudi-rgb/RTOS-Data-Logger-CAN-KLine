cmake_minimum_required(VERSION 3.16)
project(rtos_data_logger C)

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# FreeRTOS kernel: use a local checkout if given (-DFREERTOS_KERNEL_PATH=...),
# otherwise fetch the tagged release from GitHub.
include(FetchContent)
if(DEFINED FREERTOS_KERNEL_PATH)
  FetchContent_Declare(freertos_kernel SOURCE_DIR ${FREERTOS_KERNEL_PATH})
else()
  FetchContent_Declare(freertos_kernel
    GIT_REPOSITORY https://github.com/FreeRTOS/FreeRTOS-Kernel.git
    GIT_TAG        V11.1.0
    GIT_SHALLOW    TRUE)
endif()

# FreeRTOS reads its configuration through this interface target
add_library(freertos_config INTERFACE)
target_include_directories(freertos_config SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(freertos_config INTERFACE projCOVERAGE_TEST=0)

set(FREERTOS_PORT GCC_POSIX CACHE STRING "" FORCE)   # FreeRTOS on Linux/macOS
set(FREERTOS_HEAP 4 CACHE STRING "" FORCE)
FetchContent_MakeAvailable(freertos_kernel)

find_package(Threads REQUIRED)

add_executable(rtos_logger src/main.c src/bus.c src/sd.c src/ringbuffer.c)
target_include_directories(rtos_logger PRIVATE src include)
target_link_libraries(rtos_logger freertos_kernel freertos_config Threads::Threads)
target_compile_options(rtos_logger PRIVATE -Wall -Wextra)

# unit tests for the lock-free ring buffer (plain C + pthreads, no RTOS)
enable_testing()
add_executable(test_ringbuffer tests/test_ringbuffer.c src/ringbuffer.c)
target_include_directories(test_ringbuffer PRIVATE src)
target_link_libraries(test_ringbuffer Threads::Threads)
add_test(NAME ringbuffer COMMAND test_ringbuffer)
