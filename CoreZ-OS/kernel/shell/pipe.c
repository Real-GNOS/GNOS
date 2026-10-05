#include "kernel/shell/pipe.h"
#include "drivers/char/ioqueue.h"
#include "kernel/fs/file.h"
#include "kernel/init/pit/pit.h"
#include "kernel/sched/sync.h"
#include "kernel/asm_func.h"
#include "kernel/mm/pool/pool.h"
#include "kernel/sched/thread.h"
static struct FILE *pipe_file(uint32_t local_fd) {
    struct FILE *file = file_get(fd_local2global(local_fd));
    if (file == NULL || file->fd_inode == NULL) {
        return NULL;
    }
    if (file->fd_flag != PIPE_FLAG && file->fd_flag != PIPE_RD_FLAG) {
        return NULL;
    }
    return file;
}

int32_t is_pipe(uint32_t local_fd) {
    return pipe_file(local_fd) != NULL;
}

int32_t pipe_is_read_end(uint32_t local_fd) {
    struct FILE *file = pipe_file(local_fd);
    return file != NULL && file->fd_flag == PIPE_RD_FLAG;
}

uint32_t pipe_buffered(uint32_t local_fd) {
    struct FILE *file = pipe_file(local_fd);
    if (file == NULL) {
        return 0;
    }
    return ioq_length((struct TTY_IOQUEUE *)file->fd_inode);
}

int32_t sys_pipe(int32_t pipefd[2]) {
    int32_t rd_g = file_table_alloc_slot();
    int32_t wr_g;
    struct FILE *rf;
    struct FILE *wf;
    void *buf;
    if (rd_g == -1) {
        return -1;
    }
    wr_g = file_table_alloc_slot();
    if (wr_g == -1) {
        file_table_free_slot(rd_g);
        return -1;
    }
    buf = get_kernel_pages(1);
    if (buf == NULL) {
        file_table_free_slot(rd_g);
        file_table_free_slot(wr_g);
        return -1;
    }
    ioq_init((struct TTY_IOQUEUE *)buf);
    ((struct TTY_IOQUEUE *)buf)->ends = 2;
    rf = file_get((uint32_t)rd_g);
    wf = file_get((uint32_t)wr_g);
    rf->fd_flag = PIPE_RD_FLAG;
    rf->ref_cnt = 1;
    rf->fd_inode = (struct FS_INODE *)buf;
    rf->fd_pos = 0;
    rf->proc_aux = (uint32_t)wr_g;
    wf->fd_flag = PIPE_FLAG;
    wf->ref_cnt = 1;
    wf->fd_inode = (struct FS_INODE *)buf;
    wf->fd_pos = 0;
    wf->proc_aux = (uint32_t)rd_g;
    pipefd[0] = fd_install(rd_g);
    pipefd[1] = fd_install(wr_g);
    if (pipefd[0] == -1 || pipefd[1] == -1) {
        if (pipefd[0] != -1)
            fd_release((uint32_t)pipefd[0]);
        if (pipefd[1] != -1)
            fd_release((uint32_t)pipefd[1]);
        file_table_free_slot(rd_g);
        file_table_free_slot(wr_g);
        free_kernel_page((uint32_t)buf);
        return -1;
    }
    return 0;
}

static int pipe_task_live(struct TASK *task) {
    if (!task->slot_used) {
        return 0;
    }
    if (task->status == TASK_DIED || task->status == TASK_HANGING) {
        return 0;
    }
    return 1;
}

int32_t pipe_end_alive(uint32_t global_fd) {
    for (uint32_t t = 0; t < MAX_TASKS; t++) {
        struct TASK *task = &task_table[t];
        if (!pipe_task_live(task)) {
            continue;
        }
        for (uint32_t fd = 0; fd < MAX_FILES_OPEN_PER_PROC; fd++) {
            if (task->fd_table[fd] == global_fd) {
                return 1;
            }
        }
    }
    return 0;
}

uint32_t pipe_read(int32_t fd, void *buf, uint32_t count) {
    struct FILE *file = pipe_file((uint32_t)fd);
    struct TTY_IOQUEUE *ioq;
    uint32_t ioq_len;
    uint32_t size;
    char *buffer = (char *)buf;
    uint32_t bytes_read = 0;
    if (file == NULL || file->fd_flag != PIPE_RD_FLAG) {
        return (uint32_t)-1;
    }
    ioq = (struct TTY_IOQUEUE *)file->fd_inode;
    ioq_len = ioq_length(ioq);
    size = (ioq_len > count) ? count : ioq_len;
    asm_cli();
    while (bytes_read < size) {
        buffer[bytes_read] = ioq_getchar(ioq);
        ++bytes_read;
    }
    asm_sti();
    return bytes_read;
}

uint32_t pipe_write(int32_t fd, const void *buf, uint32_t count) {
    struct FILE *file = pipe_file((uint32_t)fd);
    struct TTY_IOQUEUE *ioq;
    uint32_t peer;
    const char *buffer = (const char *)buf;
    uint32_t bytes_write = 0;
    if (file == NULL || file->fd_flag != PIPE_FLAG) {
        return (uint32_t)-1;
    }
    ioq = (struct TTY_IOQUEUE *)file->fd_inode;
    peer = file->proc_aux;
    while (bytes_write < count) {
        if (ioq_length(ioq) >= BUFSIZE - 1) {
            if (!pipe_end_alive(peer)) {
                return bytes_write ? bytes_write : (uint32_t)-1;
            }
            mtime_sleep(1);
            continue;
        }
        asm_cli();
        if (ioq_length(ioq) < BUFSIZE - 1) {
            ioq_putchar(ioq, buffer[bytes_write]);
            ++bytes_write;
        }
        asm_sti();
    }
    return bytes_write;
}

void sys_fd_redirect(uint32_t old_local_fd, uint32_t new_local_fd) {
    struct TASK *cur = current;
    if (new_local_fd < 3) {
        cur->fd_table[old_local_fd] = new_local_fd;
    } else {
        uint32_t new_global_fd = cur->fd_table[new_local_fd];
        cur->fd_table[old_local_fd] = new_global_fd;
    }
}
