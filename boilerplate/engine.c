#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

// --- Configuration ---
#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16

// --- Types & Metadata ---
typedef enum { CMD_SUPERVISOR = 0, CMD_START, CMD_RUN, CMD_PS, CMD_LOGS, CMD_STOP } command_kind_t;
typedef enum { STATE_RUNNING, STATE_STOPPED, STATE_LIMIT_KILLED, STATE_EXITED } container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    container_state_t state;
    int stop_requested; // Task 4: Attribution flag
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head, tail, count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty, not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit; // Task 4
    unsigned long hard_limit; // Task 4
} control_request_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd; // Task 4: /dev/container_monitor
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    container_record_t *containers;
} supervisor_ctx_t;

// --- Global Signal Flag ---
volatile sig_atomic_t keep_running = 1;
void handle_sigint(int sig) { keep_running = 0; }

// --- Bounded Buffer Logic (Task 3) ---
void bounded_buffer_init(bounded_buffer_t *b) {
    memset(b, 0, sizeof(*b));
    pthread_mutex_init(&b->mutex, NULL);
    pthread_cond_init(&b->not_empty, NULL);
    pthread_cond_init(&b->not_full, NULL);
}

void bounded_buffer_push(bounded_buffer_t *b, const log_item_t *item) {
    pthread_mutex_lock(&b->mutex);
    while (b->count == LOG_BUFFER_CAPACITY && !b->shutting_down)
        pthread_cond_wait(&b->not_full, &b->mutex);
    if (!b->shutting_down) {
        b->items[b->tail] = *item;
        b->tail = (b->tail + 1) % LOG_BUFFER_CAPACITY;
        b->count++;
        pthread_cond_signal(&b->not_empty);
    }
    pthread_mutex_unlock(&b->mutex);
}

int bounded_buffer_pop(bounded_buffer_t *b, log_item_t *item) {
    pthread_mutex_lock(&b->mutex);
    while (b->count == 0 && !b->shutting_down)
        pthread_cond_wait(&b->not_empty, &b->mutex);
    if (b->count == 0 && b->shutting_down) { pthread_mutex_unlock(&b->mutex); return -1; }
    *item = b->items[b->head];
    b->head = (b->head + 1) % LOG_BUFFER_CAPACITY;
    b->count--;
    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

// --- Task 3: Consumer Thread ---
void *logging_thread_func(void *arg) {
    bounded_buffer_t *buffer = (bounded_buffer_t *)arg;
    log_item_t item;
    while (bounded_buffer_pop(buffer, &item) == 0) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);
        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) { write(fd, item.data, item.length); close(fd); }
    }
    return NULL;
}

// --- Task 1: Namespaces & Chroot ---
int child_fn(void *arg) {
    child_config_t *config = (child_config_t *)arg;
    sethostname(config->id, strlen(config->id));
    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
    if (chroot(config->rootfs) != 0 || chdir("/") != 0) return 1;
    mount("proc", "/proc", "proc", 0, NULL);
    dup2(config->log_write_fd, STDOUT_FILENO);
    dup2(config->log_write_fd, STDERR_FILENO);
    close(config->log_write_fd);
    char *args[] = {"/bin/sh", "-c", config->command, NULL};
    execvp(args[0], args);
    return 1;
}

// --- Supervisor Core ---
int run_supervisor(const char *rootfs_base) {
    supervisor_ctx_t ctx = {0};
    mkdir(LOG_DIR, 0755);
    bounded_buffer_init(&ctx.log_buffer);

    // Task 4: Open Kernel Monitor Device
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0) perror("Warning: Kernel monitor device not found");

    struct sigaction sa = {.sa_handler = handle_sigint};
    sigaction(SIGINT, &sa, NULL);

    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    unlink(CONTROL_PATH);
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path)-1);
    bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(ctx.server_fd, 5);

    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(ctx.server_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    pthread_create(&ctx.logger_thread, NULL, logging_thread_func, &ctx.log_buffer);

    printf("== OS-Jackfruit Supervisor Online ==\n");

    while (keep_running) {
        // Task 2 & 4: Reap Zombies and Classify Termination
        int status; pid_t reaped;
        while ((reaped = waitpid(-1, &status, WNOHANG)) > 0) {
            // Logic to find container by PID would go here to update state
            if (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL) {
                printf("[ALERT] PID %d was terminated by SIGKILL (Potential Memory Limit)\n", reaped);
            } else {
                printf("[INFO] PID %d exited normally\n", reaped);
            }
        }

        int cfd = accept(ctx.server_fd, NULL, NULL);
        if (cfd < 0) continue;

        control_request_t req;
        if (read(cfd, &req, sizeof(req)) > 0 && req.kind == CMD_START) {
            int p_fds[2]; pipe(p_fds);
            child_config_t *c_cfg = malloc(sizeof(child_config_t));
            strncpy(c_cfg->id, req.container_id, CONTAINER_ID_LEN);
            strncpy(c_cfg->rootfs, req.rootfs, PATH_MAX);
            strncpy(c_cfg->command, req.command, CHILD_COMMAND_LEN);
            c_cfg->log_write_fd = p_fds[1];

            char *stack = malloc(STACK_SIZE);
            pid_t pid = clone(child_fn, stack+STACK_SIZE, CLONE_NEWPID|CLONE_NEWNS|CLONE_NEWUTS|SIGCHLD, c_cfg);
            
            if (pid > 0) {
                // Task 4: Register with Kernel
                if (ctx.monitor_fd >= 0) {
                    struct monitor_request m_req = { .pid = pid, .soft_limit_bytes = req.soft_limit, .hard_limit_bytes = req.hard_limit };
                    strncpy(m_req.container_id, req.container_id, CONTAINER_ID_LEN);
                    ioctl(ctx.monitor_fd, MONITOR_REGISTER, &m_req);
                    printf("[KERNEL] Registered %s (PID: %d) with Limits: %lu/%lu\n", req.container_id, pid, req.soft_limit, req.hard_limit);
                }
                close(p_fds[1]);
                char buf[LOG_CHUNK_SIZE]; ssize_t n;
                while ((n = read(p_fds[0], buf, sizeof(buf))) > 0) {
                    log_item_t item; strncpy(item.container_id, req.container_id, CONTAINER_ID_LEN);
                    item.length = n; memcpy(item.data, buf, n);
                    bounded_buffer_push(&ctx.log_buffer, &item);
                }
                close(p_fds[0]);
            }
        }
        close(cfd);
    }

    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    unlink(CONTROL_PATH);
    return 0;
}

// --- Main CLI ---
int main(int argc, char *argv[]) {
    if (geteuid() != 0) { fprintf(stderr, "Run as sudo\n"); return 1; }
    if (argc < 2) return 1;
    if (strcmp(argv[1], "supervisor") == 0) return run_supervisor(argv[2]);

    control_request_t req = {0};
    if (strcmp(argv[1], "start") == 0 && argc >= 5) {
        req.kind = CMD_START;
        strncpy(req.container_id, argv[2], 31);
        strncpy(req.rootfs, argv[3], PATH_MAX-1);
        strncpy(req.command, argv[4], CHILD_COMMAND_LEN-1);
        // Task 4: Set example limits (Hardcode or parse from argv)
        req.soft_limit = 40 * 1024 * 1024; // 40MB
        req.hard_limit = 80 * 1024 * 1024; // 80MB
        
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un addr = {.sun_family = AF_UNIX};
        strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path)-1);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            write(fd, &req, sizeof(req)); close(fd);
            printf("Container start request sent.\n");
        }
    } else if (strcmp(argv[1], "logs") == 0 && argc >= 3) {
        char cmd[256]; snprintf(cmd, sizeof(cmd), "cat %s/%s.log", LOG_DIR, argv[2]);
        return system(cmd);
    }
    return 0;
}
