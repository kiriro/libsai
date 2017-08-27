cmake_minimum_required( VERSION 3.8.0 )
project( libsai )

set( CMAKE_CXX_STANDARD 14 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )
set( CMAKE_CXX_EXTENSIONS OFF )

set( CMAKE_COLOR_MAKEFILE ON )
set( CMAKE_VERBOSE_MAKEFILE ON )
set( CMAKE_EXPORT_COMPILE_COMMANDS 1)

include_directories(
	include
)

add_library(
	${PROJECT_NAME}
	source/sai.cpp
)

add_executable(
	Decrypt
	Decrypt.cpp
)

target_link_libraries(
	Decrypt
	${PROJECT_NAME}
)