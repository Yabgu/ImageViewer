find_package(OpenMP)
if (OPENMP_FOUND)
    set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${OpenMP_C_FLAGS}")
    set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}")

    if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
        set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -openmp:llvm -openmp:experimental")
    endif()

    set (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${OpenMP_EXE_LINKER_FLAGS}")
endif()

find_package(JPEG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)

add_library(ui)

target_sources(
    ui PRIVATE
    FILE_SET ui_modules TYPE CXX_MODULES FILES
        View.ixx
)

target_link_libraries(ui PUBLIC media)
target_include_directories(ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(ui PUBLIC cxx_std_23)
