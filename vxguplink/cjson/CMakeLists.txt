add_library(cjson STATIC cJSON.c)
set_target_properties(cjson PROPERTIES POSITION_INDEPENDENT_CODE ON)
