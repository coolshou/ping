cmake_minimum_required(VERSION 3.1)
project(cping C)

add_executable(cping src/ping.c)
target_sources(cping PRIVATE src/cping.rc)
set_target_properties(cping PROPERTIES C_STANDARD 90)
#Static start
#set_target_properties(cping PROPERTIES LINK_SEARCH_START_STATIC 1)
#set_target_properties(cping PROPERTIES LINK_SEARCH_END_STATIC 1)
#set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")

#Set Linker flags
#set(CMAKE_EXE_LINKER_FLAGS "-static-libgcc -static-libstdc++")

#target_compile_definitions(cping PRIVATE "$<$<CONFIG:Debug>:DEBUG>")

if(WIN32)
    target_link_libraries(cping ws2_32)
endif()
