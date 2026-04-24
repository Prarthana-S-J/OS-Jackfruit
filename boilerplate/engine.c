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
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>

#include "monitor_ioctl.h"

#define STACK_SIZE        (1024 * 1024)
#define CONTAINER_ID_LEN  32
#define CONTROL_PATH      "/tmp/mini_runtime.sock"
#define LOG_DIR           "logs"
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE    4096
#define LOG_BUFFER_CAPACITY 16

/* ------------------------------------------------------------------ */
/* Command kinds                                                        */
/* ------------------------------------------------------------------ */
typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

/* ------------------------------------------------------------------ */
/* Container state                                                      */
/* ------------------------------------------------------------------ */
typedef enum {
    STATE_STARTING = 0,
    STATE_RUNNING,
    STATE_STOPPED,
    STATE_KILLED,
    STATE_HARD_LIMIT_KILLED
} container_state_t;

static const char *state_str(container_state_t s) {
    switch (s) {
        case STATE_STARTING:         return "starting";
        case STATE_RUNNING:          return "running";
        case STATE_STOPPED:          return "stopped";
        case STATE_KILLED:           return "killed";
        case STATE_HARD_LIMIT_KILLED:return "hard_limit_killed";
        default:                     return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* Bounded-buffer logging                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    char   container_id[CONTAINER_ID_LEN];
    size_t length;
    char   data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t     head, tail, count;
    int        shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} bounded_buffer_t;

/* ------------------------------------------------------------------ */
/* Container record (full metadata)                                    */
/* ------------------------------------------------------------------ */
typedef struct container_record {
    char   id[CONTAINER_ID_LEN];
    pid_t  host_pid;
    time_t start_time;
    container_state_t state;
    int    stop_requested;      /* set before sending SIGTERM/SIGKILL */
    int    exit_status;         /* raw waitpid status, -1 if running  */
    int    exit_signal;         /* 0 if clean exit                     */
    size_t soft_limit_bytes;
    size_t hard_limit_bytes;
    char   log_path[PATH_MAX];
    int    stdout_pipe[2];      /* [0]=read in supervisor, [1]=write in child */
    int    stderr_pipe[2];
    pthread_t producer_thread;  /* reads pipes, pushes to log buffer  */
    struct container_record *next;
} container_record_t;

/* ------------------------------------------------------------------ */
/* Control request / response                                          */
/* ------------------------------------------------------------------ */
typedef struct {
    command_kind_t kind;
    char   container_id[CONTAINER_ID_LEN];
    char   rootfs[PATH_MAX];
    char   command[CHILD_COMMAND_LEN];
    size_t soft_limit_bytes;   /* 0 → use default */
    size_t hard_limit_bytes;
    int    nice_val;
} control_request_t;

#define RESPONSE_LEN 4096
typedef struct {
    int  status;               /* 0 = ok, non-zero = error */
    char message[RESPONSE_LEN];
} control_response_t;

/* ------------------------------------------------------------------ */
/* Globals                                                             */
/* ------------------------------------------------------------------ */
static container_record_t *container_list = NULL;
static pthread_mutex_t     container_list_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t stop_flag = 0;
static bounded_buffer_t    log_buffer;
static int                 monitor_fd = -1;

/* ------------------------------------------------------------------ */
/* Signal handlers                                                     */
/* ------------------------------------------------------------------ */
static void sigchld_handler(int sig) {
    (void)sig;
    int saved = errno;
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&container_list_mutex);
        container_record_t *c = container_list;
        while (c) {
            if (c->host_pid == pid) {
                c->exit_status = status;
                if (WIFSIGNALED(status)) {
                    c->exit_signal = WTERMSIG(status);
                    if (c->stop_requested) {
                        c->state = STATE_STOPPED;
                    } else if (c->exit_signal == SIGKILL) {
                        c->state = STATE_HARD_LIMIT_KILLED;
                    } else {
                        c->state = STATE_KILLED;
                    }
                } else {
                    c->state = STATE_STOPPED;
                }
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&container_list_mutex);
    }
    errno = saved;
}

static void handle_sigint(int sig) {
    (void)sig;
    stop_flag = 1;
}

/* ------------------------------------------------------------------ */
/* Bounded buffer                                                      */
/* ------------------------------------------------------------------ */
static int bounded_buffer_push(bounded_buffer_t *buf, const log_item_t *item) {
    pthread_mutex_lock(&buf->mutex);
    while (buf->count == LOG_BUFFER_CAPACITY && !buf->shutting_down)
        pthread_cond_wait(&buf->not_full, &buf->mutex);
    if (buf->shutting_down) {
        pthread_mutex_unlock(&buf->mutex);
        return -1;
    }
    buf->items[buf->tail] = *item;
    buf->tail = (buf->tail + 1) % LOG_BUFFER_CAPACITY;
    buf->count++;
    pthread_cond_signal(&buf->not_empty);
    pthread_mutex_unlock(&buf->mutex);
    return 0;
}

static int bounded_buffer_pop(bounded_buffer_t *buf, log_item_t *item) {
    pthread_mutex_lock(&buf->mutex);
    while (buf->count == 0 && !buf->shutting_down)
        pthread_cond_wait(&buf->not_empty, &buf->mutex);
    if (buf->count == 0 && buf->shutting_down) {
        pthread_mutex_unlock(&buf->mutex);
        return -1;
    }
    *item = buf->items[buf->head];
    buf->head = (buf->head + 1) % LOG_BUFFER_CAPACITY;
    buf->count--;
    pthread_cond_signal(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Consumer (logger) thread                                            */
/* ------------------------------------------------------------------ */
static void *logging_thread(void *arg) {
    (void)arg;
    log_item_t item;
    mkdir(LOG_DIR, 0755);
    while (bounded_buffer_pop(&log_buffer, &item) == 0) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);
        FILE *f = fopen(path, "a");
        if (!f) continue;
        fwrite(item.data, 1, item.length, f);
        fclose(f);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Producer thread: reads stdout+stderr pipes for one container        */
/* ------------------------------------------------------------------ */
typedef struct {
    container_record_t *rec;
} producer_arg_t;

static void *producer_thread(void *arg) {
    producer_arg_t     *parg = (producer_arg_t *)arg;
    container_record_t *rec  = parg->rec;
    free(parg);

    /* Close the write ends in the supervisor (child owns them) */
    close(rec->stdout_pipe[1]);
    close(rec->stderr_pipe[1]);

    int fds[2] = { rec->stdout_pipe[0], rec->stderr_pipe[0] };
    int active  = 2;

    while (active > 0) {
        fd_set rset;
        FD_ZERO(&rset);
        int maxfd = -1;
        for (int i = 0; i < 2; i++) {
            if (fds[i] >= 0) {
                FD_SET(fds[i], &rset);
                if (fds[i] > maxfd) maxfd = fds[i];
            }
        }
        if (maxfd < 0) break;

        struct timeval tv = {1, 0};
        int r = select(maxfd + 1, &rset, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) continue;

        for (int i = 0; i < 2; i++) {
            if (fds[i] < 0 || !FD_ISSET(fds[i], &rset)) continue;
            log_item_t item;
            memset(&item, 0, sizeof(item));
            snprintf(item.container_id, CONTAINER_ID_LEN, "%s", rec->id);
            ssize_t n = read(fds[i], item.data, LOG_CHUNK_SIZE);
            if (n <= 0) {
                close(fds[i]);
                fds[i] = -1;
                active--;
            } else {
                item.length = (size_t)n;
                bounded_buffer_push(&log_buffer, &item);
            }
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Child fn (runs inside the new namespaces)                           */
/* ------------------------------------------------------------------ */
typedef struct {
    control_request_t req;
    int stdout_pipe_write;
    int stderr_pipe_write;
} child_arg_t;

static int child_fn(void *arg) {
    child_arg_t *carg = (child_arg_t *)arg;
    control_request_t *req  = &carg->req;

    /* Redirect stdout/stderr into the pipes */
    dup2(carg->stdout_pipe_write, STDOUT_FILENO);
    dup2(carg->stderr_pipe_write, STDERR_FILENO);
    close(carg->stdout_pipe_write);
    close(carg->stderr_pipe_write);

    sethostname(req->container_id, strlen(req->container_id));
    chroot(req->rootfs);
    chdir("/");
    mount("proc", "/proc", "proc", 0, NULL);

    if (req->nice_val != 0)
        setpriority(PRIO_PROCESS, 0, req->nice_val);
        
    free(carg);

    execl(req->command, req->command, NULL);
    perror("exec failed");
    return 1;
}

/* ------------------------------------------------------------------ */
/* Helper: find container by id (caller must hold container_list_mutex)*/
/* ------------------------------------------------------------------ */
static container_record_t *find_container(const char *id) {
    container_record_t *c = container_list;
    while (c) {
        if (strncmp(c->id, id, CONTAINER_ID_LEN) == 0) return c;
        c = c->next;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Handle CMD_START / CMD_RUN                                          */
/* ------------------------------------------------------------------ */
static void handle_start(const control_request_t *req,
                         control_response_t *resp,
                         int client_fd)
{
    /* Check for duplicate ID */
    pthread_mutex_lock(&container_list_mutex);
    if (find_container(req->container_id)) {
        pthread_mutex_unlock(&container_list_mutex);
        resp->status = 1;
        snprintf(resp->message, RESPONSE_LEN,
                 "container '%s' already exists\n", req->container_id);
        return;
    }
    pthread_mutex_unlock(&container_list_mutex);

    container_record_t *c = calloc(1, sizeof(*c));
    if (!c) { resp->status = 1; snprintf(resp->message, RESPONSE_LEN, "OOM\n"); return; }

    snprintf(c->id,       CONTAINER_ID_LEN, "%s", req->container_id);
    snprintf(c->log_path, PATH_MAX,         "%s/%s.log", LOG_DIR, req->container_id);
    c->start_time        = time(NULL);
    c->state             = STATE_STARTING;
    c->exit_status       = -1;
    c->soft_limit_bytes  = req->soft_limit_bytes ? req->soft_limit_bytes : 40 * 1024 * 1024;
    c->hard_limit_bytes  = req->hard_limit_bytes ? req->hard_limit_bytes : 64 * 1024 * 1024;

    if (pipe(c->stdout_pipe) < 0 || pipe(c->stderr_pipe) < 0) {
        free(c);
        resp->status = 1;
        snprintf(resp->message, RESPONSE_LEN, "pipe() failed: %s\n", strerror(errno));
        return;
    }

    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        free(c);
        resp->status = 1;
        snprintf(resp->message, RESPONSE_LEN, "stack malloc failed\n");
        return;
    }

    /* Build child args on stack of this function — safe because we wait below
       or the child starts fast enough (clone is synchronous in the kernel). */
    child_arg_t *carg = malloc(sizeof(*carg));
if (!carg) {
    free(stack);
    free(c);
    resp->status = 1;
    snprintf(resp->message, RESPONSE_LEN, "malloc failed\n");
    return;
}

carg->req               = *req;
carg->stdout_pipe_write = c->stdout_pipe[1];
carg->stderr_pipe_write = c->stderr_pipe[1];

pid_t pid = clone(child_fn, stack + STACK_SIZE,
                  CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                  carg);
    if (pid < 0) {
        free(stack);
        free(c);
        resp->status = 1;
        snprintf(resp->message, RESPONSE_LEN, "clone() failed: %s\n", strerror(errno));
        return;
    }

    c->host_pid = pid;
    c->state    = STATE_RUNNING;

    /* Register with kernel monitor */
    if (monitor_fd >= 0) {
        struct monitor_request mreq;
        memset(&mreq, 0, sizeof(mreq));
        mreq.pid              = pid;
        mreq.soft_limit_bytes = c->soft_limit_bytes;
        mreq.hard_limit_bytes = c->hard_limit_bytes;
        snprintf(mreq.container_id, sizeof(mreq.container_id), "%s", c->id);
        ioctl(monitor_fd, MONITOR_REGISTER, &mreq);
    }

    /* Start producer thread for this container */
    producer_arg_t *parg = malloc(sizeof(*parg));
    parg->rec = c;
    pthread_create(&c->producer_thread, NULL, producer_thread, parg);

    /* Add to list */
    pthread_mutex_lock(&container_list_mutex);
    c->next        = container_list;
    container_list = c;
    pthread_mutex_unlock(&container_list_mutex);

    printf("Started %s (PID %d)\n", c->id, pid);

    snprintf(resp->message, RESPONSE_LEN, "started %s pid=%d\n", c->id, pid);
    resp->status = 0;

    /* CMD_RUN: keep client_fd open and block until container exits */
    if (req->kind == CMD_RUN && client_fd >= 0) {
        /* Write initial response so client knows it started */
        write(client_fd, resp, sizeof(*resp));

        /* Wait for child */
        int wstatus;
        waitpid(pid, &wstatus, 0);

        /* Update metadata */
        pthread_mutex_lock(&container_list_mutex);
        container_record_t *rec = find_container(req->container_id);
        if (rec) {
            rec->exit_status = wstatus;
            if (WIFSIGNALED(wstatus)) {
                rec->exit_signal = WTERMSIG(wstatus);
                rec->state = rec->stop_requested ? STATE_STOPPED : STATE_HARD_LIMIT_KILLED;
            } else {
                rec->state = STATE_STOPPED;
            }
        }
        pthread_mutex_unlock(&container_list_mutex);

        /* Final response with exit info */
        int code = WIFEXITED(wstatus)   ? WEXITSTATUS(wstatus)
                 : WIFSIGNALED(wstatus) ? 128 + WTERMSIG(wstatus)
                 : -1;
        snprintf(resp->message, RESPONSE_LEN, "run finished exit_code=%d\n", code);
        resp->status = code;
        write(client_fd, resp, sizeof(*resp));
        return;
    }

    /* For CMD_START the caller writes the response */
}

/* ------------------------------------------------------------------ */
/* Handle CMD_STOP                                                     */
/* ------------------------------------------------------------------ */
static void handle_stop(const char *id, control_response_t *resp) {
    pthread_mutex_lock(&container_list_mutex);
    container_record_t *c = find_container(id);
    if (!c) {
        pthread_mutex_unlock(&container_list_mutex);
        resp->status = 1;
        snprintf(resp->message, RESPONSE_LEN, "container '%s' not found\n", id);
        return;
    }
    /* Allow stop even if state is slightly out of sync */
if (kill(c->host_pid, 0) != 0) {
    pthread_mutex_unlock(&container_list_mutex);
    resp->status = 1;
    snprintf(resp->message, RESPONSE_LEN, "container '%s' is not running\n", id);
    return;
}

c->stop_requested = 1;
pid_t pid = c->host_pid;
pthread_mutex_unlock(&container_list_mutex);

kill(pid, SIGTERM);

    

    /* Give it 3 s to exit gracefully, then SIGKILL */
    struct timespec ts = {0, 100 * 1000 * 1000}; /* 100 ms */
    for (int i = 0; i < 30; i++) {
        nanosleep(&ts, NULL);
        pthread_mutex_lock(&container_list_mutex);
        container_record_t *cur = find_container(id);
        int still_running = cur && cur->state == STATE_RUNNING;
        pthread_mutex_unlock(&container_list_mutex);
        if (!still_running) goto done;
    }
    kill(pid, SIGKILL);

done:
    /* Unregister from kernel monitor */
    if (monitor_fd >= 0) {
        struct monitor_request mreq;
        memset(&mreq, 0, sizeof(mreq));
        mreq.pid = pid;
        ioctl(monitor_fd, MONITOR_UNREGISTER, &mreq);
    }

    resp->status = 0;
    snprintf(resp->message, RESPONSE_LEN, "stopped %s\n", id);
}

/* ------------------------------------------------------------------ */
/* Handle CMD_PS                                                       */
/* ------------------------------------------------------------------ */
static void handle_ps(control_response_t *resp) {
    char buf[RESPONSE_LEN];
    int  off = 0;
    off += snprintf(buf + off, RESPONSE_LEN - off,
                    "%-16s %-8s %-22s %-20s %s\n",
                    "ID", "PID", "STARTED", "STATE", "LOG");
    pthread_mutex_lock(&container_list_mutex);
    container_record_t *c = container_list;
    while (c && off < RESPONSE_LEN - 128) {
        char tbuf[32];
        struct tm *tm = localtime(&c->start_time);
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", tm);
        off += snprintf(buf + off, RESPONSE_LEN - off,
                        "%-16s %-8d %-22s %-20s %s\n",
                        c->id, c->host_pid, tbuf,
                        state_str(c->state), c->log_path);
        c = c->next;
    }
    pthread_mutex_unlock(&container_list_mutex);
    resp->status = 0;
    snprintf(resp->message, RESPONSE_LEN, "%s", buf);
}

/* ------------------------------------------------------------------ */
/* Handle CMD_LOGS                                                     */
/* ------------------------------------------------------------------ */
static void handle_logs(const char *id, control_response_t *resp) {
    pthread_mutex_lock(&container_list_mutex);
    container_record_t *c = find_container(id);
    char path[PATH_MAX];
    if (c) snprintf(path, PATH_MAX, "%s", c->log_path);
    pthread_mutex_unlock(&container_list_mutex);

    if (!c) {
        resp->status = 1;
        snprintf(resp->message, RESPONSE_LEN, "container '%s' not found\n", id);
        return;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        resp->status = 1;
        snprintf(resp->message, RESPONSE_LEN, "log not found: %s\n", path);
        return;
    }
    size_t n = fread(resp->message, 1, RESPONSE_LEN - 1, f);
    resp->message[n] = '\0';
    fclose(f);
    resp->status = 0;
}

/* ------------------------------------------------------------------ */
/* Supervisor main loop                                                */
/* ------------------------------------------------------------------ */
static int run_supervisor(void) {
    monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (monitor_fd < 0) perror("monitor open (non-fatal)");

    /* Init log buffer */
    memset(&log_buffer, 0, sizeof(log_buffer));
    pthread_mutex_init(&log_buffer.mutex,     NULL);
    pthread_cond_init (&log_buffer.not_empty, NULL);
    pthread_cond_init (&log_buffer.not_full,  NULL);

    /* Start consumer thread */
    pthread_t logger_tid;
    pthread_create(&logger_tid, NULL, logging_thread, NULL);

    /* Set up control socket */
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    unlink(CONTROL_PATH);
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", CONTROL_PATH);
    bind  (server_fd, (struct sockaddr*)&addr, sizeof(addr));
    chmod (CONTROL_PATH, 0777);
    listen(server_fd, 16);

    printf("Supervisor running (pid=%d)...\n", getpid());
    mkdir(LOG_DIR, 0755);

    while (!stop_flag) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(server_fd, &fds);
        struct timeval tv = {1, 0};
        int ret = select(server_fd + 1, &fds, NULL, NULL, &tv);
        if (ret < 0) { if (errno == EINTR) continue; break; }
        if (ret == 0) continue;

        int client = accept(server_fd, NULL, NULL);
        if (client < 0) continue;

        control_request_t  req;
        control_response_t resp;
        memset(&resp, 0, sizeof(resp));

        if (read(client, &req, sizeof(req)) <= 0) {
            close(client);
            continue;
        }

        if (req.kind == CMD_START) {
            handle_start(&req, &resp, -1);
            write(client, &resp, sizeof(resp));
        } else if (req.kind == CMD_RUN) {
            /* handle_start writes its own responses and blocks */
            handle_start(&req, &resp, client);
        } else if (req.kind == CMD_PS) {
            handle_ps(&resp);
            write(client, &resp, sizeof(resp));
        } else if (req.kind == CMD_LOGS) {
            handle_logs(req.container_id, &resp);
            write(client, &resp, sizeof(resp));
        } else if (req.kind == CMD_STOP) {
            handle_stop(req.container_id, &resp);
            write(client, &resp, sizeof(resp));
        }

        close(client);
    }

    /* ---- Graceful shutdown ---- */
    printf("Supervisor shutting down...\n");

    /* Stop all running containers */
    pthread_mutex_lock(&container_list_mutex);
    container_record_t *c = container_list;
    while (c) {
        if (c->state == STATE_RUNNING) {
            c->stop_requested = 1;
            kill(c->host_pid, SIGTERM);
        }
        c = c->next;
    }
    pthread_mutex_unlock(&container_list_mutex);

    /* Drain children */
    while (waitpid(-1, NULL, WNOHANG) >= 0);
    sleep(1);
    while (waitpid(-1, NULL, WNOHANG) >= 0);

    /* Join producer threads */
    pthread_mutex_lock(&container_list_mutex);
    c = container_list;
    while (c) {
        pthread_join(c->producer_thread, NULL);
        c = c->next;
    }
    pthread_mutex_unlock(&container_list_mutex);

    /* Signal log buffer to drain and shut down */
    pthread_mutex_lock(&log_buffer.mutex);
    log_buffer.shutting_down = 1;
    pthread_cond_broadcast(&log_buffer.not_empty);
    pthread_cond_broadcast(&log_buffer.not_full);
    pthread_mutex_unlock(&log_buffer.mutex);
    pthread_join(logger_tid, NULL);

    /* Free container list */
    pthread_mutex_lock(&container_list_mutex);
    c = container_list;
    while (c) {
        container_record_t *next = c->next;
        free(c);
        c = next;
    }
    container_list = NULL;
    pthread_mutex_unlock(&container_list_mutex);

    close(server_fd);
    unlink(CONTROL_PATH);
    if (monitor_fd >= 0) close(monitor_fd);
    printf("Supervisor exited cleanly.\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Client helper                                                       */
/* ------------------------------------------------------------------ */
static int send_and_receive(const control_request_t *req) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", CONTROL_PATH);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return 1;
    }
    if (write(fd, req, sizeof(*req)) <= 0) {
        perror("write");
        close(fd);
        return 1;
    }

    /* For CMD_RUN there may be two responses; read until EOF */
    control_response_t resp;
    ssize_t n;
    while ((n = read(fd, &resp, sizeof(resp))) > 0) {
        printf("%s", resp.message);
        if (req->kind != CMD_RUN) break;
    }
    int rc = (req->kind == CMD_RUN) ? resp.status : 0;
    close(fd);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Argument parsing helpers                                            */
/* ------------------------------------------------------------------ */
static size_t parse_mib(const char *s) {
    return (size_t)atol(s) * 1024 * 1024;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[]) {
    signal(SIGCHLD, sigchld_handler);
    signal(SIGINT,  handle_sigint);
    signal(SIGTERM, handle_sigint);

    if (argc < 2) {
        fprintf(stderr,
            "Usage:\n"
            "  engine supervisor <base-rootfs>\n"
            "  engine start  <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  engine run    <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  engine ps\n"
            "  engine logs   <id>\n"
            "  engine stop   <id>\n");
        return 1;
    }

    if (!strcmp(argv[1], "supervisor"))
        return run_supervisor();

    if (!strcmp(argv[1], "start") || !strcmp(argv[1], "run")) {
        if (argc < 5) { fprintf(stderr, "start/run needs <id> <rootfs> <cmd>\n"); return 1; }
        control_request_t req;
        memset(&req, 0, sizeof(req));
        req.kind = (!strcmp(argv[1], "run")) ? CMD_RUN : CMD_START;
        snprintf(req.container_id, CONTAINER_ID_LEN, "%s", argv[2]);
        snprintf(req.rootfs,       PATH_MAX,         "%s", argv[3]);
        snprintf(req.command,      CHILD_COMMAND_LEN,"%s", argv[4]);
        /* Optional flags */
        for (int i = 5; i < argc - 1; i++) {
            if (!strcmp(argv[i], "--soft-mib"))  req.soft_limit_bytes = parse_mib(argv[++i]);
            else if (!strcmp(argv[i], "--hard-mib")) req.hard_limit_bytes = parse_mib(argv[++i]);
            else if (!strcmp(argv[i], "--nice"))  req.nice_val = atoi(argv[++i]);
        }
        return send_and_receive(&req);
    }

    if (!strcmp(argv[1], "ps")) {
        control_request_t req = {0};
        req.kind = CMD_PS;
        return send_and_receive(&req);
    }

    if (!strcmp(argv[1], "logs")) {
        if (argc < 3) { fprintf(stderr, "logs needs <id>\n"); return 1; }
        control_request_t req = {0};
        req.kind = CMD_LOGS;
        snprintf(req.container_id, CONTAINER_ID_LEN, "%s", argv[2]);
        return send_and_receive(&req);
    }

    if (!strcmp(argv[1], "stop")) {
        if (argc < 3) { fprintf(stderr, "stop needs <id>\n"); return 1; }
        control_request_t req = {0};
        req.kind = CMD_STOP;
        snprintf(req.container_id, CONTAINER_ID_LEN, "%s", argv[2]);
        return send_and_receive(&req);
    }

    fprintf(stderr, "Unknown command: %s\n", argv[1]);
    return 1;
}
