set(ENV{FIO} "/root/fio-master")
message("variable is $ENV{FIO}")

include_directories($ENV{FIO})
include_directories(${CMAKE_SOURCE_DIR})
add_library(vhost_fio_plugin SHARED vhost_fio_plugin.c)
target_link_libraries(vhost_fio_plugin PRIVATE obj_libvhost obj_allocator)

install(TARGETS vhost_fio_plugin DESTINATION lib)