cmake_minimum_required(VERSION 3.2)
project(ImageViewer)

add_executable(ImageViewer main.cpp)

find_package(OpenGL REQUIRED)

include(ExternalProject)
ExternalProject_Add(glfw
	GIT_REPOSITORY "https://github.com/glfw/glfw.git"
	GIT_TAG "master"

	SOURCE_DIR "${CMAKE_SOURCE_DIR}/third-party/glfw"
	CMAKE_ARGS -DGLFW_BUILD_DOCS=OFF
		-DGLFW_BUILD_TESTS=OFF
		-DGLFW_BUILD_EXAMPLES=OFF
		-DCMAKE_INSTALL_PREFIX=${CMAKE_SOURCE_DIR}/install/glfw

	TEST_COMMAND ""
	UPDATE_COMMAND ""
)

add_dependencies(ImageViewer glfw)

include_directories(ImageViewer ${OPENGL_INCLUDE_DIR} "${CMAKE_SOURCE_DIR}/install/glfw/include/")
find_library(GLFW3_LIBRARY glfw3 HINTS "${CMAKE_SOURCE_DIR}/install/glfw/lib/")

target_link_libraries(ImageViewer ${OPENGL_gl_LIBRARY} ${OPENGL_glu_LIBRARY} ${GLFW3_LIBRARY})

