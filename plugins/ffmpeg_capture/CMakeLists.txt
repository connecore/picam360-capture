cmake_minimum_required(VERSION 3.1.3)

message("ffmpeg_capture generating Makefile")
project(ffmpeg_capture)

add_library(ffmpeg_capture MODULE
	ffmpeg_capture.c
)
set_target_properties(ffmpeg_capture PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS ON # gnu11
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO # c++11
    PREFIX ""
)
	
include_directories(
	../../include
)
link_directories(
)

target_link_libraries(ffmpeg_capture
	pthread
	dl
)

#packages
find_package(PkgConfig REQUIRED)

#opengl
find_package(OpenGL REQUIRED)
find_package(GLEW REQUIRED)
pkg_check_modules(GLFW glfw3 REQUIRED)
find_package(Freetype REQUIRED)
	
include_directories( ${OPENGL_INCLUDE_DIRS} ${GLEW_INCLUDE_DIRS} ${GLFW_INCLUDE_DIRS} ${FREETYPE_INCLUDE_DIRS} )
target_link_libraries(ffmpeg_capture ${OPENGL_LIBRARIES} ${GLEW_LIBRARY} ${GLFW_LIBRARIES} ${FREETYPE_LIBRARIES})


#post build
add_custom_command(TARGET ffmpeg_capture POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:ffmpeg_capture> ${CMAKE_CURRENT_SOURCE_DIR}/../
)