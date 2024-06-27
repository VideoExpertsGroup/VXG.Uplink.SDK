# Specify the minimum version
cmake_minimum_required(VERSION 3.9)

# CMake options
set(LWS_LIB "" CACHE FILEPATH "Path to LWS library file")
set(LWS_INCLUDE "" CACHE PATH "Path to LWS include directory")
set(SSL_LIB "" CACHE FILEPATH "Path to OpenSSL library file")
set(CRYPTO_LIB "" CACHE FILEPATH "Path to OpenSSL library file")
set(SSL_INCLUDE "" CACHE PATH "Path to OpenSSL include directory")

# Specify the project name
project(vxg-proxy-client)
# Declare the project target
add_executable(${PROJECT_NAME}
    vxg_proxy_client.cc
)
# Add uplink library
add_subdirectory(vxguplink)

# Libraries to link
target_link_libraries(${PROJECT_NAME} vxguplink)

target_include_directories(${PROJECT_NAME} PUBLIC
                            "${PROJECT_BINARY_DIR}"
                            "${PROJECT_SOURCE_DIR}/vxguplink"
                            )