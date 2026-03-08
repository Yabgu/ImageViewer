
cmake_minimum_required(VERSION 3.30)
project(wasmedge_glue)

add_executable(wasmedge_glue wasmedge_glue.cpp)

# Prefer Wasmtime if available, otherwise try to find headers/libs manually.
find_package(Wasmtime CONFIG QUIET)
if (Wasmtime_FOUND)
	target_link_libraries(wasmedge_glue PRIVATE Wasmtime::wasmtime)
else()
	# Prefer using the bundled third-party wasmtime CMake if it exists
	set(WASMTIME_THIRD_PARTY_DIR ${CMAKE_SOURCE_DIR}/third-party/wasmtime/crates/c-api)
	if(EXISTS ${WASMTIME_THIRD_PARTY_DIR}/CMakeLists.txt)
		message(STATUS "Including third-party Wasmtime CMake from ${WASMTIME_THIRD_PARTY_DIR}")
		add_subdirectory(${WASMTIME_THIRD_PARTY_DIR} ${CMAKE_BINARY_DIR}/third-party/wasmtime)
		# The third-party CMake defines an INTERFACE library named `wasmtime`
		target_link_libraries(wasmedge_glue PRIVATE wasmtime-cpp)
	else()
		# Fall back to searching for headers/libs on the system
		find_path(WASMTIME_INCLUDE_DIR wasmtime.h)
		find_library(WASMTIME_LIBRARY NAMES wasmtime wasmtime_c)
		if (WASMTIME_INCLUDE_DIR)
			target_include_directories(wasmedge_glue PRIVATE ${WASMTIME_INCLUDE_DIR})
		endif()
		if (WASMTIME_LIBRARY)
			target_link_libraries(wasmedge_glue PRIVATE ${WASMTIME_LIBRARY})
		endif()
	endif()
endif()

target_include_directories(wasmedge_glue PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
