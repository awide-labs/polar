# Copyright (c) 2015, VMware, Inc. or its affiliates. All Rights Reserved.

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${PROJECT_SOURCE_DIR}/libgpopt/include)
include_directories(${PROJECT_SOURCE_DIR}/libgpdbcost/include)
# for the generated config.h file.
include_directories(${PROJECT_BINARY_DIR}/libgpos/include/)

file(GLOB_RECURSE hdrs ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h)
file(GLOB_RECURSE srcs ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

# Add headers to make them visible in some IDEs (Clion, VS, Xcode)
list(APPEND srcs ${hdrs})

# pg15_px: TODO: maybe it's worth to delete these files?
# Remove unused files, which compiles with errors (like references to removed
# constants and others) from build tree to fix broken test compilation
set(ignore_patterns
  "CDXLPhysicalDynamicIndexScan\.(h|cpp)"
  "CParseHandlerDynamicIndexScan\.(h|cpp)"
  "CParseHandlerDynamicTableScan\.(h|cpp)"
  "CParseHandlerPhysicalDynamicBitmapTableScan\.(h|cpp)"
  "CDXLPhysicalDynamicTableScan\.(h|cpp)"
)

include(ListFilter)
list_filter_by_patterns("${srcs}" "${ignore_patterns}" srcs)

add_library(naucrates ${srcs})

target_link_libraries(naucrates
                      ${CMAKE_DL_LIBS}
                      gpos
                      ${XERCES_LIBRARIES})

# Prevent linker errors from undefined symbols on OSX.
if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
  set_target_properties(naucrates PROPERTIES
                        LINK_FLAGS "-undefined dynamic_lookup")
endif()
