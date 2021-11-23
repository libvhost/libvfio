cmake_minimum_required(VERSION 3.0)
project(libvfio C CXX)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

#set(ENABLE_SANITIZERS true CACHE BOOL "Enable Sanitizers")

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

# example: https://github.com/amzn/hawktracer/blob/master/CMakeLists.txt
# https://github.com/soedinglab/hh-suite/blob/master/CMakeLists.txt

set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
#if (ENABLE_SANITIZERS)
    include(FindUBSan)
    include(FindASan)
    include(FindMSan)
    include(FindTSan)
#endif()

add_subdirectory(vhost)
add_subdirectory(examples)
###################################### RPM ##############################
#set(CPACK_GENERATOR "DEB;TGZ;ZIP;RPM")
set(CPACK_GENERATOR "RPM")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Feng Li")

set(CPACK_SOURCE_IGNORE_FILES
        "/rpm/"
        "^${PROJECT_SOURCE_DIR}/build/"
        "^${PROJECT_SOURCE_DIR}/.git/"
        "^${PROJECT_SOURCE_DIR}/.gitignore"
        "^${PROJECT_SOURCE_DIR}/.gitattributes"
        )
set(LIBVFIO_VERSION_MAJOR 0)
set(LIBVFIO_VERSION_MINOR 0)
set(LIBVFIO_VERSION_PATCH 1)
set(CPACK_PACKAGE_VERSION_MAJOR "${LIBVFIO_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${LIBVFIO_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${LIBVFIO_VERSION_PATCH}")
include(CPack)
