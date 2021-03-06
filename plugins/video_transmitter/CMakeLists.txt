cmake_minimum_required(VERSION 3.1.3)

message("video_reciever generating Makefile")
project(video_transmitter)

find_package(PkgConfig REQUIRED)

add_library(video_transmitter MODULE
	video_transmitter.c
)

set_target_properties(video_transmitter PROPERTIES
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

target_link_libraries(video_transmitter
	${CMAKE_CURRENT_SOURCE_DIR}/../../libs/picam360-common/libpicam360-common.a
	pthread
	dl
)

if(APPLE)
	set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -lc++")
endif()

#post build
add_custom_command(TARGET video_transmitter POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:video_transmitter> ${CMAKE_CURRENT_SOURCE_DIR}/../
)