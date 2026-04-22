#define _GNU_SOURCE   // Enables advanced Linux features like clone()

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>      // clone()
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>  // kernel communication
#include <sys/mount.h>  // mount /proc
#include <sys/resource.h> // nice value
#include <sys/socket.h> // IPC
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>

#include "monitor_ioctl.h"

/* ---------------- CONFIG ---------------- */
#define STACK_SIZE (1024 * 1024)   // stack for clone()
#define CONTROL_PATH "/tmp/mini_runtime.sock" // UNIX socket path
#define LOG_DIR "logs"

/* ---------------- COMMAND TYPES ---------------- */
// What CLI can ask supervisor to do
typedef enum {
    CMD_SUPERVISOR,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

/* ---------------- CONTAINER STATE ---------------- */
// Lifecycle of container
typedef enum {
    STATE_STARTING,
    STATE_RUNNING,
    STATE_STOPPED,
    STATE_KILLED,
    STATE_HARD_LIMIT_KILLED
} container_state_t;

/* ---------------- CONTAINER STRUCT ---------------- */
// Stores everything about a container
typedef struct container_record {
    char id[32];              // container name
    pid_t host_pid;           // PID in host
    time_t start_time;        // start time
    container_state_t state;  // current state

    int stop_requested;       // user requested stop?
    int exit_status;
    int exit_signal;

    size_t soft_limit_bytes;  // memory warning
    size_t hard_limit_bytes;  // memory kill

    char log_path[PATH_MAX];  // where logs stored

    int stdout_pipe[2];       // capture stdout
    int stderr_pipe[2];       // capture stderr

    pthread_t producer_thread; // log reader thread

    struct container_record *next; // linked list
} container_record_t;

/* ---------------- GLOBALS ---------------- */
static container_record_t *container_list = NULL;
static pthread_mutex_t container_list_mutex = PTHREAD_MUTEX_INITIALIZER;
static int monitor_fd = -1;

/* ---------------- CHILD FUNCTION ---------------- */
// This runs inside container (after clone)
static int child_fn(void *arg) {

    char **args = (char **)arg;

    // Set hostname (UTS namespace)
    sethostname("container", strlen("container"));

    // Change root filesystem
    chroot(args[1]);
    chdir("/");

    // Mount /proc so commands like ps work
    mount("proc", "/proc", "proc", 0, NULL);

    // Run actual program
    execvp(args[2], &args[2]);

    // If exec fails
    perror("exec failed");
    return 1;
}

/* ---------------- START CONTAINER ---------------- */
static void start_container(const char *id, const char *rootfs, const char *cmd) {

    // Allocate stack for clone
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        perror("malloc");
        return;
    }

    // Arguments passed to child
    char *args[] = {(char *)id, (char *)rootfs, (char *)cmd, NULL};

    // Create container using clone()
    pid_t pid = clone(child_fn, stack + STACK_SIZE,
        CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
        args);

    if (pid < 0) {
        perror("clone");
        return;
    }

    printf("Started container %s (PID %d)\n", id, pid);

    // Register with kernel module
    if (monitor_fd >= 0) {
        struct monitor_request req;
        req.pid = pid;
        snprintf(req.container_id, sizeof(req.container_id), "%s", id);
        req.soft_limit_bytes = 40 * 1024 * 1024;
        req.hard_limit_bytes = 64 * 1024 * 1024;

        ioctl(monitor_fd, MONITOR_REGISTER, &req);
    }
}

/* ---------------- STOP CONTAINER ---------------- */
static void stop_container(pid_t pid) {
    kill(pid, SIGTERM); // try graceful stop
    sleep(1);
    kill(pid, SIGKILL); // force kill if needed
}

/* ---------------- SUPERVISOR ---------------- */
static int run_supervisor(void) {

    // Open kernel module device
    monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (monitor_fd < 0)
        perror("monitor open");

    // Create UNIX socket
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, CONTROL_PATH);

    unlink(CONTROL_PATH); // remove old socket
    bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 5);

    printf("Supervisor running...\n");

    while (1) {
        int client = accept(server_fd, NULL, NULL);

        char buffer[256];
        read(client, buffer, sizeof(buffer));

        printf("Received: %s\n", buffer);

        close(client);
    }

    return 0;
}

/* ---------------- MAIN ---------------- */
int main(int argc, char *argv[]) {

    if (argc < 2) {
        printf("Usage: engine <command>\n");
        return 1;
    }

    // Supervisor mode
    if (strcmp(argv[1], "supervisor") == 0) {
        return run_supervisor();
    }

    // Start container
    if (strcmp(argv[1], "start") == 0) {
        if (argc < 5) {
            printf("Usage: start <id> <rootfs> <cmd>\n");
            return 1;
        }

        start_container(argv[2], argv[3], argv[4]);
        return 0;
    }

    printf("Unknown command\n");
    return 1;
}
