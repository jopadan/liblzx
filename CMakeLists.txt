cmake_minimum_required( VERSION 3.5 )

project( lzx VERSION 0.1 LANGUAGES CXX )
set(PROJECT_DESCRIPTION "LZX compression algorithm format C/C++ library")

include(GNUInstallDirs)

if( NOT CMAKE_BUILD_TYPE )
	set( CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE )
endif()

set( CMAKE_CXX_STANDARD 23 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )
add_compile_options(-std=gnu++26 -march=native -O3)
add_compile_options(-D_FILE_OFFSET_BITS=64)
add_compile_options(-fdata-sections)
add_compile_options(-fpermissive)
add_compile_options(-ffunction-sections)
add_compile_options(-Wall)
#add_compile_options(-Wextra)
add_compile_options(-Wpedantic)
add_link_options(-Wl,--gc-sections)
#add_link_options(-Wl,--print-gc-sections)
add_link_options(-Wl,-s)

add_library( ${PROJECT_NAME} INTERFACE ) 
target_include_directories( ${PROJECT_NAME} INTERFACE
	PUBLIC_HEADER $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
	$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

add_executable( unlzx examples/unlzx.cpp )
target_link_libraries( unlzx ${PROJECT_NAME} )

install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/lzx DESTINATION
	${CMAKE_INSTALL_INCLUDEDIR} COMPONENT headers
)

