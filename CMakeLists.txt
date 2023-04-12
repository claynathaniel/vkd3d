project(vkd3d)
cmake_minimum_required(VERSION 3.25)

set(CMAKE_C_STANDARD 99 )

find_program( CMAKE_WIDL NAMES widl )

if( NOT CMAKE_WIDL )
    message( FATAL_ERROR "widl not found" )
endif()

find_program(FLEX NAMES flex)
if(NOT FLEX)
    message(FATAL_ERROR "flex not found")
endif()

find_program(BISON NAMES bison)
if(NOT BISON)
    message(FATAL_ERROR "bison not found")
endif()

