cmake_minimum_required(VERSION 3.1.3)

message("mjpeg_omx_decoder generating Makefile")
project(mjpeg_omx_decoder)

find_package(PkgConfig REQUIRED)

find_file(BCM_HOST bcm_host.h /opt/vc/include)
if(BCM_HOST)
	set(ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_PATH}:/opt/vc/lib/pkgconfig")
	pkg_check_modules(BCMHOST bcm_host REQUIRED)
	link_directories( ${BCMHOST_LIBRARY_DIRS} ) # need to upper of add_executable
	
	link_directories( /opt/vc/src/hello_pi/libs/ilclient ) # need to upper of add_executable
endif()

add_library(mjpeg_omx_decoder MODULE
	mjpeg_omx_decoder.cc
)

if(APPLE)
	message("OSX")
elseif(UNIX)
	message("UNIX or LINUX")
	#check raspberry pi
	if(BCM_HOST)
		message("RASPI")
		set( USE_GLES ON )
		add_definitions(-DBCM_HOST)
		add_definitions(-DOMX_SKIP64BIT) # this would be not need in the future
		include_directories( ${BCMHOST_INCLUDE_DIRS} )
		target_link_libraries(mjpeg_omx_decoder ${BCMHOST_LIBRARIES} )
		
		include_directories( /opt/vc/src/hello_pi/libs/ilclient )
		target_link_libraries(mjpeg_omx_decoder ilclient openmaxil )
	endif()
elseif(WIN32)
	message("WINDOWS")
endif()

set_target_properties(mjpeg_omx_decoder PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS ON # gnu11
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO # c++11
    PREFIX ""
)
	
include_directories(
	../../
	../../include
)
link_directories(
)

target_link_libraries(mjpeg_omx_decoder
	pthread
	dl
)

#post build
add_custom_command(TARGET mjpeg_omx_decoder POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:mjpeg_omx_decoder> ${CMAKE_CURRENT_SOURCE_DIR}/../
)