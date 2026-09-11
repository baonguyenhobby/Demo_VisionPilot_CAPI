cmake_minimum_required(VERSION 3.22.1)

add_library(ap_runtime
        src/ap_runtime.cpp
)

target_include_directories(ap_runtime
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# ap::capi, not ap_generated_common: ap_runtime uses ara::core / ara::exec /
# ara::phm only. It has no service interfaces of its own, so it must not drag
# the generated binding sources into every process that links it.
target_link_libraries(ap_runtime
        PUBLIC
        ap::capi
)

target_compile_features(ap_runtime PUBLIC cxx_std_17)
