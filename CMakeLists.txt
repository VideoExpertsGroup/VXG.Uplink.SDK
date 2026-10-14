# vxg-proxy-client — native or cross. Preinstall libwebsockets, OpenSSL, and zlib under /install.
cmake_minimum_required(VERSION 3.10)
project(vxg-proxy-client C)

if(POLICY CMP0074)
    cmake_policy(SET CMP0074 NEW)
endif()

set(CMAKE_C_STANDARD 99)

add_executable(${PROJECT_NAME}
    vxg_proxy_client.c
    cjson/cJSON.c
)

# Root where libwebsockets, OpenSSL, and zlib are installed (lws/, ssl/, zlib/)
set(VXG_DEPS_PREFIX "/install" CACHE PATH "Prefix containing lws/, ssl/, zlib/ preinstalls")

list(APPEND CMAKE_PREFIX_PATH
    "${VXG_DEPS_PREFIX}/lws"
    "${VXG_DEPS_PREFIX}/ssl"
    "${VXG_DEPS_PREFIX}/zlib")

if(NOT OPENSSL_ROOT_DIR)
    set(OPENSSL_ROOT_DIR "${VXG_DEPS_PREFIX}/ssl" CACHE PATH "")
endif()
if(NOT ZLIB_ROOT)
    set(ZLIB_ROOT "${VXG_DEPS_PREFIX}/zlib" CACHE PATH "")
endif()

set(OPENSSL_USE_STATIC_LIBS TRUE)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

find_library(VXG_WEBSOCKETS_LIB
    NAMES websockets libwebsockets
    PATHS "${VXG_DEPS_PREFIX}/lws/lib"
    NO_DEFAULT_PATH
    REQUIRED)

target_include_directories(${PROJECT_NAME} PRIVATE "${VXG_DEPS_PREFIX}/lws/include")

target_link_libraries(${PROJECT_NAME} PRIVATE
    ${VXG_WEBSOCKETS_LIB}
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
    dl
    pthread
)

option(VXG_ENABLE_BW_PROBE "Enable bandwidth probe support in the camera agent" ON)

set(VXG_DEFAULT_API_HOST "camera.vxg.io" CACHE STRING "Default provisioning API hostname (override with -DVXG_DEFAULT_API_HOST=host at configure time)")
target_compile_definitions(${PROJECT_NAME} PRIVATE "VXG_DEFAULT_API_HOST=\"${VXG_DEFAULT_API_HOST}\"")
if(VXG_ENABLE_BW_PROBE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE VXG_ENABLE_BW_PROBE=1)
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE VXG_ENABLE_BW_PROBE=0)
endif()
