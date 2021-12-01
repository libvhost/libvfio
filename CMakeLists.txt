cmake_minimum_required(VERSION 3.0)
project(libvfio LANGUAGES C CXX VERSION 3.27.211)
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
SET(CPACK_RPM_COMPONENT_INSTALL 1)
SET(CPACK_RPM_fio_PACKAGE_NAME fio)
SET(CPACK_RPM_fio_FILE_NAME fio-${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}-g29463.rpm)
#set(CPACK_GENERATOR "DEB;TGZ;ZIP;RPM")
set(CPACK_GENERATOR "RPM")
set(CPACK_RPM_PACKAGE_MAINTAINER "Feng Li")

set(CPACK_SOURCE_IGNORE_FILES
        "/rpm/"
        "^${PROJECT_SOURCE_DIR}/build/"
        "^${PROJECT_SOURCE_DIR}/.git/"
        "^${PROJECT_SOURCE_DIR}/.gitignore"
        "^${PROJECT_SOURCE_DIR}/.gitattributes"
        )
include(CPack)
