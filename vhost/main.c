#include "conn.h"
#include "io.h"
#include "memory.h"
#include "utils.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void random_buf(char *buf, size_t size) {
    int rnd = open("/dev/urandom", O_RDONLY);
    read(rnd, buf, size);
    close(rnd);
}

int my_memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = s1;
    const uint8_t *p2 = s2;
    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            printf("i: %d [0x%x] != [0x%x]\n", i, p1[i], p2[i]);
            return p1[i] - p2[i];
        }
    }
    return 0;
}

int test_sync_io(VhostConn *conn) {
    int i;
    int ret = 0;
    int buf_size = 4 << 10;
    char *rbuf;
    char *wbuf;
    wbuf = (char *) vhost_malloc(buf_size);
    rbuf = (char *) vhost_malloc(buf_size);

    for (i = 0; i < (1 << 16) + 10; ++i) {
        printf("============== %d ==================\n", i);
        random_buf(wbuf, buf_size);
        libvhost_write(conn, 0, i << 9, wbuf, buf_size);
        libvhost_read(conn, 0, i << 9, rbuf, buf_size);
        if (0 != my_memcmp(wbuf, rbuf, buf_size)) {
            printf("miscompare failed: %d\n", memcmp(wbuf, rbuf, buf_size));
            ret = -1;
            printf("wbuf: \n");
            DumpHex((void *) wbuf, 16);
            printf("rbuf: \n");
            DumpHex((void *) rbuf, 16);
            break;
        }
    }
fail:
    vhost_free(wbuf);
    vhost_free(rbuf);
    return ret;
}

struct test_iov {
    struct iovec iov;
    char *buf;
};

int test_async_io(VhostConn *conn) {
    int round;
    int idx;
    int ret = 0;
    int buf_size = 1024;
    const int depth = 128;
    const int max_round = 100;
    struct test_iov r_iov[depth];
    struct test_iov w_iov[depth];
    VhostEvent events[depth];
    for (idx = 0; idx < depth; ++idx) {
        w_iov[idx].buf = (char *) vhost_malloc(buf_size);
        r_iov[idx].buf = (char *) vhost_malloc(buf_size);

        w_iov[idx].iov.iov_base = w_iov[idx].buf;
        w_iov[idx].iov.iov_len = buf_size;
        r_iov[idx].iov.iov_base = r_iov[idx].buf;
        r_iov[idx].iov.iov_len = buf_size;
    }

    for (round = 0; round < max_round; ++round) {
        printf("============== %d ==================\n", round);
        for (idx = 0; idx < depth; ++idx) {
            random_buf(w_iov[idx].buf, buf_size);
            libvhost_submit(conn, 0, (round * depth + idx) << 10, &w_iov[idx].iov, 1,
                            true, NULL);
        }
        libvhost_getevents(conn, 0, depth, events);
        for (idx = 0; idx < depth; ++idx) {
            libvhost_submit(conn, 0, (round * depth + idx) << 10, &r_iov[idx].iov, 1,
                            false, NULL);
        }
        libvhost_getevents(conn, 0, depth, events);
        for (idx = 0; idx < depth; ++idx) {
            if (0 != my_memcmp(w_iov[idx].buf, r_iov[idx].buf, buf_size)) {
                printf("req %d miscompare failed\n", idx);
                ret = -1;
                printf("wbuf: \n");
                DumpHex((void *) w_iov[idx].buf, 520);
                printf("rbuf: \n");
                DumpHex((void *) r_iov[idx].buf, 520);
                break;
            }
        }
    }
    for (idx = 0; idx < depth; ++idx) {
        vhost_free(w_iov[idx].buf);
        vhost_free(r_iov[idx].buf);
    }
    return ret;
}

int main(int argc, char **argv) {
    int ret = 0;
    int i;
    if (argc != 2) {
        printf("Usage: %s <socket_path>\n", argv[0]);
        return 1;
    }
    init_hp_mem();
    ret = atexit(free_hp_mem);
    if (ret != 0) {
        fprintf(stderr, "cannot set exit function\n");
        exit(EXIT_FAILURE);
    }

    VhostConn *conn = create_vhost_conn(argv[1]);
    if (!conn) {
        goto fail_conn;
    }
    ret = vhost_connect(conn);
    if (ret != 0) {
        printf("vhost_connect failed: %d\n", ret);
        goto fail_conn;
    }
    ret = vhost_setup(conn);
    if (ret != 0) {
        printf("vhost_setup failed: %d\n", ret);
        goto fail_conn;
    }

    ret = vhost_conn_add_vq(conn, 1024);
    if (ret != 0) {
        printf("vhost_conn_add_vq failed: %d\n", ret);
        goto fail_conn;
    }
    // ret = vhost_conn_add_vq(conn, 32);
    // if (ret != 0) {
    //   printf("vhost_conn_add_vq failed: %d\n", ret);
    //   return -1;
    // }

    // ret = test_sync_io(conn);
    // if (ret != 0) {
    //   printf("test_sync_io failed: %d\n", ret);
    //   goto fail_conn;
    // }

    ret = test_async_io(conn);
    if (ret != 0) {
        printf("test_async_io failed: %d\n", ret);
        goto fail_conn;
    }

fail_conn:
    destroy_vhost_conn(conn);
    return ret;
}