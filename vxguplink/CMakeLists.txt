add_library(vxguplink SHARED Proxy.cpp)
add_subdirectory(cjson)

# set libraries to link
set(link_libs cjson)
if(LWS_LIB)
    set(link_libs ${LWS_LIB} ${link_libs})
    message("Added LWS_LIB to link_libs: ${link_libs}")
else()
    set(link_libs websockets ${link_libs})
endif(LWS_LIB)

if(SSL_LIB)
    set(link_libs ${SSL_LIB} ${link_libs})
    message("Added SSL_LIB to link_libs: ${link_libs}")
endif(SSL_LIB)

if(CRYPTO_LIB)
    set(link_libs ${CRYPTO_LIB} ${link_libs})
    message("Added CRYPTO_LIB to link_libs: ${link_libs}")
endif(CRYPTO_LIB)

message("link_libs: ${link_libs}")
target_link_libraries(vxguplink ${link_libs})

# set directories to include
set(inc_dirs "/cjson")
if(LWS_INCLUDE)
    set(inc_dirs ${LWS_INCLUDE} ${inc_dirs})
    message("Added LWS_INCLUDE to inc_dirs: ${inc_dirs}")
endif(LWS_INCLUDE)

if(SSL_INCLUDE)
    set(inc_dirs ${SSL_INCLUDE} ${inc_dirs})
    message("Added SSL_INCLUDE to inc_dirs: ${inc_dirs}")
endif(SSL_INCLUDE)

target_include_directories(vxguplink PUBLIC ${inc_dirs})

set(installable_libs vxguplink cjson)
install(TARGETS ${installable_libs} DESTINATION lib)
install(FILES Proxy.h DESTINATION include)
install(FILES cjson/cJSON.h DESTINATION include/cjson)