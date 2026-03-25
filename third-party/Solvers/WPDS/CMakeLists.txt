# WPDS libraries
set(WPDS_SRCS dictionary.cpp common.cpp keys.cpp key_source.cpp)
set(WPDS_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/../.. ${CMAKE_CURRENT_SOURCE_DIR})

# Create libraries
add_library(wpds STATIC ${WPDS_SRCS})
add_library(wpds++ STATIC ${WPDS_SRCS})

# Configure libraries
foreach(target wpds wpds++)
	target_include_directories(${target} PUBLIC ${WPDS_INCLUDE_DIRS})
	if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
		target_compile_options(${target} PRIVATE -Wno-deprecated-declarations)
	endif()
endforeach()

target_compile_definitions(wpds++ PRIVATE WITNESS_TRACE=1 GRAMMATECH=1)

# Interface libraries
foreach(target ewpds wpdsplusplus_util)
	add_library(${target} INTERFACE)
	target_include_directories(${target} INTERFACE ${WPDS_INCLUDE_DIRS})
endforeach()

# Install
file(GLOB_RECURSE WPDS_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/*.h")
install(FILES ${WPDS_HEADERS} DESTINATION include/Solvers/WPDS)
install(TARGETS wpds wpds++ ewpds wpdsplusplus_util
	ARCHIVE DESTINATION lib
	LIBRARY DESTINATION lib)
