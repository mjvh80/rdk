include(FetchContent)
FetchContent_Declare(rdk_freerdp
    URL https://github.com/FreeRDP/FreeRDP/archive/refs/tags/3.26.0.tar.gz
    URL_HASH SHA512=8ac48097de3b976e830e6f613de1b91f3003997856538742350ec6aee29156d0ec926fffd5c2fb2453904a889ada51dbaa7ce87c5f289606eaa20a2158c55005
    SOURCE_SUBDIR rdk-protocol-only
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(rdk_freerdp)
set(RDK_CAMERA_PROTOCOL_DIR "${rdk_freerdp_SOURCE_DIR}/channels/rdpecam/client")
add_library(rdk_camera_protocol STATIC
    "${RDK_CAMERA_PROTOCOL_DIR}/camera_device_enum_main.c"
    "${RDK_CAMERA_PROTOCOL_DIR}/camera_device_main.c"
    "${CMAKE_CURRENT_LIST_DIR}/../src/rdk_camera_codec.c")
target_include_directories(rdk_camera_protocol PUBLIC "${RDK_CAMERA_PROTOCOL_DIR}")
target_include_directories(rdk_camera_protocol PRIVATE "${RDK_CAMERA_PROTOCOL_DIR}/../common")
target_link_libraries(rdk_camera_protocol PUBLIC freerdp freerdp-client winpr)
target_compile_definitions(rdk_camera_protocol PRIVATE UNICODE _UNICODE)
target_compile_options(rdk_camera_protocol PRIVATE "/FI${CMAKE_CURRENT_LIST_DIR}/../src/rdk_camera_compat.h")
enable_language(CXX)
add_library(rdk_camera STATIC "${CMAKE_CURRENT_LIST_DIR}/../src/rdk_camera.cpp")
target_compile_features(rdk_camera PRIVATE cxx_std_17)
target_link_libraries(rdk_camera PUBLIC rdk_camera_protocol mfplat mfreadwrite mfuuid mf ole32)
target_include_directories(rdk_camera PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../src")
target_compile_definitions(rdk_camera PRIVATE UNICODE _UNICODE NOMINMAX)