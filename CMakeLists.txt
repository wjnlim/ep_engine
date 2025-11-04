cmake_minimum_required(VERSION 3.15)

if ("${CMAKE_BINARY_DIR}" STREQUAL "${CMAKE_SOURCE_DIR}")
  message(FATAL_ERROR "In-source builds are not allowed.
    Please create a subfolder and use `cmake ..` inside it.
    NOTE: cmake creates CMakeCache.txt and CMakeFiles/*.
          Remove them, or cmake will refuse to work.")
endif()

project(Ep_engine C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

include(FetchContent)
# TODO: change to fetch from github repo
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
    Fetchcontent_Declare(
        Utils
        GIT_REPOSITORY https://github.com/wjnlim/utils.git
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(Utils)

else()
    Fetchcontent_Declare(
        Utils
        GIT_REPOSITORY https://github.com/wjnlim/utils.git
    )
    FetchContent_GetProperties(Utils)
    if(NOT Utils_POPULATED)
        FetchContent_Populate(Utils)
        add_subdirectory(${Utils_SOURCE_DIR} ${Utils_BINARY_DIR} EXCLUDE_FROM_ALL)
        # add_subdirectory(${Utils_SOURCE_DIR} ${Utils_BINARY_DIR})
    endif()
endif()

# Fetchcontent_Declare(
#     Utils
#     GIT_REPOSITORY https://github.com/wjnlim/utils.git
# )
# FetchContent_MakeAvailable(Utils)

add_library(ep_engine_objs OBJECT 
    src/epoll_event_engine.c
    src/ev_cb_thread_pool.c
)
target_include_directories(ep_engine_objs 
    PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)
target_compile_options(ep_engine_objs
    PRIVATE
        $<$<CONFIG:Debug>: -D DEBUG_ENGINE -O0 -g>
)

find_library(PTHREAD_LIB pthread REQUIRED)
if (PTHREAD_LIB)
    target_link_libraries(ep_engine_objs PUBLIC ${PTHREAD_LIB})
endif()

if(DEBUG_ENGINE)
    target_compile_definitions(ep_engine_objs PRIVATE DEBUG_ENGINE)
endif()
# link to utils_objs utils_ds_objs to get usage requirements
target_link_libraries(ep_engine_objs PUBLIC utils_objs utils_ds_objs)

add_library(ep_engine
    # $<TARGET_OBJECTS:ep_engine_objs>
    # $<TARGET_OBJECTS:utils_objs>
)
# target_link_libraries(ep_engine PRIVATE ep_engine_objs utils_objs)
target_link_libraries(ep_engine PUBLIC ep_engine_objs utils_objs utils_ds_objs)
install(DIRECTORY include/ep_engine DESTINATION include)
install(TARGETS ep_engine
        ARCHIVE DESTINATION lib)
