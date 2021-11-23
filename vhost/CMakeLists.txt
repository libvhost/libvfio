include_directories(.)
set(libvhost_SOURCES
    conn.c
    utils.c
    memory.c
    virtqueue.c
    io.c
    blk.c
    scsi.c
    vhost_user.c
)
file(GLOB liballocator_SOURCES
allocator/*.c
)
include_directories(allocator/include)
add_library(obj_allocator STATIC ${liballocator_SOURCES})
add_library(obj_libvhost STATIC ${libvhost_SOURCES})

# project targets
add_executable(test_vhost main.c)
target_link_libraries(test_vhost PRIVATE obj_libvhost obj_allocator)
INSTALL(TARGETS
    test_vhost
    DESTINATION bin
    )

INSTALL(TARGETS
    obj_allocator
    obj_libvhost
    DESTINATION lib
    )
