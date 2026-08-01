# cmake/Android toolchain usage:
# cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26
cmake_minimum_required(VERSION 3.16)
project(opencl_loader C)

add_library(OpenCL STATIC src/opencl_loader.c)
target_include_directories(OpenCL PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(OpenCL PUBLIC CL_TARGET_OPENCL_VERSION=120)
target_compile_features(OpenCL PRIVATE c_std_11)
target_link_libraries(OpenCL PUBLIC dl)
set_target_properties(OpenCL PROPERTIES OUTPUT_NAME OpenCL POSITION_INDEPENDENT_CODE ON)

install(TARGETS OpenCL ARCHIVE DESTINATION lib)
install(DIRECTORY include/CL DESTINATION include)
