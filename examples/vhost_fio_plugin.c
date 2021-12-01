/*
 * vhost-user engine
 *
 * this engine read/write vhost-user device.
 */
#include "fio.h"
#include "optgroup.h"

#include "vhost/io.h"
#include "vhost/memory.h"
#include <poll.h>
#include <stdlib.h>

struct vhost_task {
  struct iovec iov;
  struct io_u *io_u;
  struct fio_vhost *fio_vhost;
  struct flist_head entry;
};

struct fio_vhost {
  VhostConn *device;
  struct vhost_info *info; // parent
  int block_size;
  uint64_t num_blocks;

  unsigned int queued;
  unsigned int events;
  unsigned long queued_bytes;

  struct flist_head tasks;
};

struct vhost_info {
  struct fio_vhost **vhosts;
  int nr_vhosts;

  struct vhost_task **cpl_tasks;
  int nr_events;
};

struct vhost_options {
  void *pad;
  unsigned int io_num_queue;
};

static struct fio_option options[] = {
    {
        .name = "io_num_queue",
        .lname = "vhost io queue number",
        .type = FIO_OPT_STR_SET,
        .off1 = offsetof(struct vhost_options, io_num_queue),
        .help = "Set the io queue number",
        .category = FIO_OPT_C_ENGINE,
        .group = FIO_OPT_G_INVALID,
    },
    {
        .name = NULL,
    },
};

static VhostConn *fio_create_conn(const char *file) {
  VhostConn *conn;
  int ret;
  init_hp_mem();
  ret = atexit(free_hp_mem);
  if (ret != 0) {
    fprintf(stderr, "cannot set exit function\n");
    exit(EXIT_FAILURE);
  }
  conn = create_vhost_conn(file);
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

  ret = vhost_conn_add_vq(conn, VIRTIO_MAX_IODEPTH * 4);
  if (ret != 0) {
    printf("vhost_conn_add_vq failed: %d\n", ret);
    goto fail_conn;
  }
  return conn;

fail_conn:
  destroy_vhost_conn(conn);
  return NULL;
}

static int fio_vhost_setup(struct thread_data *td) {
  int i;
  struct vhost_info *vhost_info;
  struct fio_file *f;
  VhostConn *conn;
  int ret;

  vhost_info = malloc(sizeof(struct vhost_info));
  vhost_info->nr_vhosts = td->o.nr_files;
  vhost_info->vhosts =
      calloc(vhost_info->nr_vhosts, sizeof(struct fio_vhost *));
  vhost_info->cpl_tasks = malloc(td->o.iodepth * sizeof(struct vhost_task));
  td->io_ops_data = vhost_info;

  for_each_file(td, f, i) {
    conn = create_vhost_conn(f->file_name);
    if (!conn) {
      return -1;
    }
    ret = vhost_connect(conn);
    if (ret != 0) {
      printf("vhost_connect failed: %d\n", ret);
      destroy_vhost_conn(conn);
      return ret;
    }
    f->real_file_size =
        vhost_conn_get_blocksize(conn) * vhost_conn_get_numblocks(conn);
    destroy_vhost_conn(conn);
    printf("pid: %d capacity: %" PRIu64 "\n", getpid(), f->real_file_size);
  }
  return 0;
}

static void fio_vhost_cleanup(struct thread_data *td) {
  struct vhost_info *vhost_info = td->io_ops_data;
  int i;
  for (i = 0; i < vhost_info->nr_vhosts; i++) {
    destroy_vhost_conn(vhost_info->vhosts[i]->device);
  }
  free(vhost_info->vhosts);
  free(vhost_info->cpl_tasks);
  free(vhost_info);
}

static int fio_vhost_init_one(struct thread_data *td,
                              struct vhost_info *vhost_info, struct fio_file *f,
                              int d_idx) {
  struct fio_vhost *fio_vhost = NULL;
  struct vhost_task *task;
  int i;
  fio_vhost = calloc(1, sizeof(*fio_vhost));
  fio_vhost->device = fio_create_conn(f->file_name);
  fio_vhost->info = vhost_info;
  vhost_info->vhosts[d_idx] = fio_vhost;

  fio_vhost->block_size = vhost_conn_get_blocksize(fio_vhost->device);
  fio_vhost->num_blocks = vhost_conn_get_numblocks(fio_vhost->device);

  f->real_file_size = fio_vhost->num_blocks * fio_vhost->block_size;
  f->engine_data = fio_vhost;
  printf("pid: %d block size: %d num blocks: %" PRIu64 "\n", getpid(),
         fio_vhost->block_size, fio_vhost->num_blocks);
  // memset(fio_vhost->device->qps[1]->sq.queue + 64, 0xfe, 64);
  INIT_FLIST_HEAD(&fio_vhost->tasks);
  for (i = 0; i < td->o.iodepth; ++i) {
    task = calloc(1, sizeof(*task));
    INIT_FLIST_HEAD(&task->entry);
    flist_add_tail(&task->entry, &fio_vhost->tasks);
  }
  return 0;
}

static int fio_vhost_init(struct thread_data *td) {
  struct vhost_info *vhost_info = td->io_ops_data;
  int ret = 0;
  struct fio_file *f;
  int i;

  for_each_file(td, f, i) {
    ret = fio_vhost_init_one(td, vhost_info, f, i);
    if (ret < 0)
      break;
  }

  return ret;
}

static void vhost_cb(void *opaque, int status) {
  struct vhost_task *vhost_task = (struct vhost_task *)opaque;
  struct fio_vhost *fio_vhost = vhost_task->fio_vhost;
  struct vhost_info *vhost_info = fio_vhost->info;
  struct io_u *io_u = vhost_task->io_u;

  if (status == 0) {
    io_u->error = 0;
  } else {
    log_err("vhost: request failed with error %d.\n", status);
    io_u->error = 1;
    io_u->resid = io_u->xfer_buflen;
  }

  vhost_info->cpl_tasks[vhost_info->nr_events++] = vhost_task;
}

static enum fio_q_status fio_vhost_queue(struct thread_data *td,
                                         struct io_u *io_u) {
  struct fio_vhost *fio_vhost = io_u->file->engine_data;
  struct vhost_task *task;
  int q_idx;
  if (fio_vhost->queued == td->o.iodepth) {
    printf("fio_vhost: reach max depth %d empty: %d\n", fio_vhost->queued,
           flist_empty(&fio_vhost->tasks));
    return FIO_Q_BUSY;
  }

  task = flist_first_entry(&fio_vhost->tasks, struct vhost_task, entry);
  flist_del(&task->entry);

  task->fio_vhost = fio_vhost;
  task->io_u = io_u;
  task->iov.iov_len = io_u->xfer_buflen;
  task->iov.iov_base = io_u->xfer_buf;
  // q_idx = fio_vhost->queued % (fio_vhost->device->qp_cnt - 1) + 1;
  // printf("pid: %d io_u offset: %llu len: %llu queued: %d\n", getpid(),
  //        io_u->offset, io_u->xfer_buflen, fio_vhost->queued);
  fio_vhost->queued++;
  libvhost_submit(fio_vhost->device, 0, io_u->offset, &task->iov, 1,
                  io_u->ddir == DDIR_WRITE, task);
  return FIO_Q_QUEUED;
}

// return the io done num, then the .event will get the idx to get the io_u.
static int fio_vhost_getevents(struct thread_data *td, unsigned int min,
                               unsigned int max, const struct timespec *t) {
  struct vhost_info *vhost_info = td->io_ops_data;
  int nr = 0;
  VhostEvent events[256];
  int i;

  vhost_info->nr_events = 0;
  while (nr < min) {
    for (int i = 0; i < vhost_info->nr_vhosts; i++) {
      // printf("min: %d, vhost_info->nr_events: %d nr :%d device: %d \n", min,
      //        vhost_info->nr_events, nr, i);
      nr +=
          libvhost_getevents(vhost_info->vhosts[i]->device, 0, 1, &events[nr]);
    }
  }

  for (i = 0; i < nr; i++) {
    vhost_cb(events[i].data, events[i].res);
  }
  return nr;
}

static struct io_u *fio_vhost_event(struct thread_data *td, int event) {
  struct vhost_info *vhost_info = (struct vhost_info *)td->io_ops_data;
  struct vhost_task *task = vhost_info->cpl_tasks[event];
  task->fio_vhost->queued--;
  flist_add_tail(&task->entry, &task->fio_vhost->tasks);
  return task->io_u;
}

static int fio_vhost_open_file(struct thread_data *td, struct fio_file *f) {
  return 0;
}

static int fio_vhost_close_file(struct thread_data *td, struct fio_file *f) {
  return 0;
}

static int fio_vhost_prep(struct thread_data *td, struct io_u *io_u) {
  // printf("pid %d: prep...\n", getpid());
  return 0;
}

// static int fio_vhost_commit(struct thread_data *td)
// {
// 	if (!ld->queued)
// 		return 0;
// }

// In the fork child process.
// static 	int fio_vhost_init (struct thread_data *) {
//   printf("pid %d: init ...\n", getpid());
//   return 0;
// }

int fio_vhost_iomem_alloc(struct thread_data *td, size_t total_mem) {
  printf("fio_vhost_iomem_alloc total_mem: %" PRIu64 "\n", total_mem);
  td->orig_buffer = vhost_malloc(total_mem);
  if (td->orig_buffer == NULL)
    return 1;
  return 0;
}

void fio_vhost_iomem_free(struct thread_data *td) {
  printf("fio_vhost_iomem_free \n");
}

struct ioengine_ops ioengine = {
    .name = "vhost",
    .version = FIO_IOOPS_VERSION,
    .flags = FIO_SYNCIO | FIO_DISKLESSIO | FIO_NODISKUTIL,
    .setup = fio_vhost_setup,
    .cleanup = fio_vhost_cleanup,
    .init = fio_vhost_init,
    .queue = fio_vhost_queue,
    // .commit = fio_vhost_commit,
    .getevents = fio_vhost_getevents,
    .event = fio_vhost_event,
    .open_file = fio_vhost_open_file,
    .close_file = fio_vhost_close_file,
    .prep = fio_vhost_prep,
    .iomem_alloc = fio_vhost_iomem_alloc,
    .iomem_free = fio_vhost_iomem_free,
    .option_struct_size = sizeof(struct vhost_options),
    .options = options,
};

static void fio_init fio_vhost_register(void) { register_ioengine(&ioengine); }

static void fio_exit fio_vhost_unregister(void) {
  unregister_ioengine(&ioengine);
}
