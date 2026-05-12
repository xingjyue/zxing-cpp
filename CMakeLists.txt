cmake_minimum_required(VERSION 3.10)

project(zxing)

option(BUILD_TESTING "Enable generation of test targets" OFF)

set(CMAKE_LIBRARY_PATH /opt/local/lib ${CMAKE_LIBRARY_PATH})

# Check for polluted source tree.
if(EXISTS ${CMAKE_SOURCE_DIR}/CMakeCache.txt OR
    EXISTS ${CMAKE_SOURCE_DIR}/CMakeFiles)
    message(FATAL_ERROR
        "Source directory is polluted:"
        "\n  * remove CMakeCache.txt"
        "\n  * remove CMakeFiles directory")
endif()

# Suppress in-source builds.
if(${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_BINARY_DIR})
    message(FATAL_ERROR
        "CMake generation is not allowed within the source directory:"
        "\n  * mkdir build"
        "\n  * cd build"
        "\n  * Unix-like: cmake -G \"Unix Makefiles\" .."
        "\n  * Windows: cmake -G \"Visual Studio 10\" ..")
endif()

# Adjust CMake's module path.
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake/FindModules)

# Suppress MSVC CRT warnings.
if(MSVC)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
    add_definitions(/Za)
    add_definitions(/MP) # multi-core builds
endif()

include(source_files.cmake)

if(WIN32)
    include_directories(core/lib/win32)
    set(CMAKE_DEBUG_POSTFIX -debug)
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
endif()

# OpenCV classes (optional; zxing CLI builds without it)
find_package(OpenCV QUIET)
if(OpenCV_FOUND)
    list(APPEND LIBZXING_FILES
        opencv/src/zxing/MatSource.cpp
        opencv/src/zxing/MatSource.h
    )

    include_directories(${OpenCV_INCLUDE_DIRS})
    include_directories(opencv/src)
endif()

include_directories(core/src)
add_library(libzxing ${LIBZXING_FILES})
set_target_properties(libzxing PROPERTIES PREFIX "")

find_package(Iconv)
if(ICONV_FOUND)
    include_directories(${ICONV_INCLUDE_DIR})
    target_link_libraries(libzxing ${ICONV_LIBRARIES})
else()
    add_definitions(-DNO_ICONV=1)
endif()

# Add OpenCV cli executable
if(OpenCV_FOUND)
    set(OPENCV_ZXING_FILES
        "./opencv-cli/src/main.cpp"
    )
    add_executable(zxing-cv ${OPENCV_ZXING_FILES})
    target_link_libraries(zxing-cv libzxing ${OpenCV_LIBRARIES})
endif()

# Add cli executable.
set(ZXING_FILES
    "./cli/src/main.cpp"
    "./cli/src/ImageReaderSource.cpp"
    "./cli/src/ImageReaderSource.h"
    "./cli/src/lodepng.cpp"
    "./cli/src/lodepng.h"
    "./cli/src/jpgd.cpp"
    "./cli/src/jpgd.h"
)

add_executable(zxing ${ZXING_FILES})
target_link_libraries(zxing libzxing)

# Add finder pattern marker tool (no OpenCV dependency)
set(FINDER_MARKER_FILES
    "./finder-marker/main.cpp"
    "./cli/src/lodepng.cpp"
    "./cli/src/lodepng.h"
    "./cli/src/jpgd.cpp"
    "./cli/src/jpgd.h"
)
add_executable(zxing-finder-marker ${FINDER_MARKER_FILES})
target_include_directories(zxing-finder-marker PRIVATE "./cli/src")
target_link_libraries(zxing-finder-marker libzxing)

install(TARGETS zxing libzxing EXPORT zxing-targets
	LIBRARY DESTINATION lib
	RUNTIME DESTINATION bin
	ARCHIVE DESTINATION lib
    INCLUDES DESTINATION include
)

install(EXPORT zxing-targets DESTINATION lib/zxing/cmake NAMESPACE zxing::)

install(
    DIRECTORY core/src/zxing/
    DESTINATION include/zxing
    FILES_MATCHING PATTERN "*.h"
)

if(OpenCV_FOUND)
    install(
        DIRECTORY opencv/src/zxing/
        DESTINATION include/zxing
        FILES_MATCHING PATTERN "*.h"
    )
endif()

configure_file(cmake/zxing-config.cmake.in zxing-config.cmake @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/zxing-config.cmake DESTINATION lib/zxing/cmake)

if(BUILD_TESTING)
    # Add testrunner executable.
    find_package(CPPUNIT)
    if(CPPUNIT_FOUND)
        file(GLOB_RECURSE TESTRUNNER_FILES
            "./core/tests/src/*.cpp"
            "./core/tests/src/*.h"
        )
        add_executable(testrunner ${TESTRUNNER_FILES})
        include_directories(${CPPUNIT_INCLUDE_DIR})
        target_link_libraries(testrunner libzxing  ${CPPUNIT_LIBRARIES})
    endif()
endif()
