include(ProcessorCount)
include(ExternalProject)
ProcessorCount(N)

set(FIO_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/fio)

include_directories(${FIO_SOURCE_DIR})
link_directories(${CMAKE_CURRENT_BINARY_DIR})

# Workaround for the centos 7 compile error, the strsep has duplicate name.
add_compile_definitions(FIO_STRSEP_LIB_H)

include_directories(${CMAKE_SOURCE_DIR})
ExternalProject_Add(fio-ext
    SOURCE_DIR ${FIO_SOURCE_DIR}
    CONFIGURE_COMMAND make clean && ./configure --prefix=${CMAKE_BINARY_DIR}
    BUILD_COMMAND make libfio -j ${N} V=1
    BUILD_IN_SOURCE 1
    INSTALL_COMMAND mv libfio_static.a ${CMAKE_CURRENT_BINARY_DIR}
    )
##################### vhost engine ##########################
add_library(vhost_fio_plugin STATIC vhost_fio_plugin.c)
add_dependencies(vhost_fio_plugin fio-ext)

##################### libiscsi engine ##########################
set(LIBISCSI_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libiscsi)
add_subdirectory(${LIBISCSI_SOURCE_DIR} EXCLUDE_FROM_ALL)

file(GLOB LIBISCSI_HEADERS ${LIBISCSI_SOURCE_DIR}/include/*.h)
file(COPY ${LIBISCSI_HEADERS} DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/libiscsi/include/iscsi)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/libiscsi/include)

add_library(iscsi_fio_plugin STATIC ${FIO_SOURCE_DIR}/engines/libiscsi.c)
add_dependencies(iscsi_fio_plugin fio-ext libiscsi_static)

add_executable(fio fio.c)
set(FIO_LIBRARY -Wl,--whole-archive vhost_fio_plugin iscsi_fio_plugin libiscsi_static fio_static obj_libvhost obj_allocator -Wl,--no-whole-archive gcrypt)
target_link_libraries(fio PRIVATE ${FIO_LIBRARY} tcmalloc numa z m pthread dl aio ibverbs rdmacm curl ssl crypto pmem pmem)

install(TARGETS fio DESTINATION bin COMPONENT fio)
