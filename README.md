
## 1. Team Information

**Team Members:**

* Prarthana Shivakumar — PES1UG24CS339
* Prema P Kotur PES1UG24CS343

---

## 2. Build, Load, and Run Instructions

This project was developed and tested on Ubuntu 22.04/24.04 VM with Secure Boot disabled.

### Step 1: Build the project

```bash
make
```

### Step 2: Load the kernel module

```bash
sudo insmod monitor.ko
```

Verify device:

```bash
ls -l /dev/container_monitor
```

### Step 3: Start the supervisor

```bash
sudo ./engine supervisor ./rootfs-base
```

### Step 4: Prepare container root filesystems

```bash
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

### Step 5: Start containers

```bash
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
sudo ./engine start beta ./rootfs-beta /bin/sh --soft-mib 64 --hard-mib 96
```

### Step 6: View running containers

```bash
sudo ./engine ps
```

### Step 7: View logs

```bash
sudo ./engine logs alpha
```

### Step 8: Run scheduling experiments

```bash
time sudo ./engine run alpha ./rootfs-alpha /bin/sh --nice -10
time sudo ./engine run beta ./rootfs-beta /bin/sh --nice 10
```

### Step 9: Stop containers

```bash
sudo ./engine stop alpha
sudo ./engine stop beta
```

### Step 10: Shutdown supervisor

Press `Ctrl + C`

### Step 11: Inspect kernel logs

```bash
dmesg | tail
```

### Step 12: Unload module

```bash
sudo rmmod monitor
```

---

## 3. Demo with Screenshots

| # | Requirement                 | Description                                            | Screenshot   |
| - | --------------------------- | ------------------------------------------------------ | ------------ |
| 1 | Multi-container supervision | Two containers running under a single supervisor       | *screenshot1* |
| 2 | Metadata tracking           | `engine ps` showing container states and metadata      | *screenshot2_1, screenshot2_2* |
| 3 | Bounded-buffer logging      | Logs captured from container output                    | *screenshot3_1, screenshot3_2,screenshot3_3* |
| 4 | CLI and IPC                 | CLI command interacting with supervisor                | *screenshot4* |
| 5 | Soft-limit warning          | Kernel log showing container registration / monitoring | *screenshot5* |
| 6 | Hard-limit enforcement      | Kernel integration and monitoring evidence             | *screenshot6* |
| 7 | Scheduling experiment       | Execution time difference using nice values            | *screenshot7* |
| 8 | Clean teardown              | Supervisor shutdown and no leftover processes          | *screenshot8* |

---
## 4. Engineering Analysis

### Isolation Mechanisms

Containers in this project are implemented using Linux namespaces, which allow a process to have its own isolated view of system resources. We used PID, UTS, and mount namespaces through the `clone()` system call.

The PID namespace ensures that processes inside the container see their own process hierarchy starting from PID 1, even though the host maintains a different PID mapping. This prevents containers from interfering with or even observing processes outside their namespace.

The UTS namespace allows each container to have its own hostname, which is set using `sethostname()`. This is mainly for identification and does not affect the host system.

Mount namespaces combined with `chroot()` provide filesystem isolation. Each container operates within its own root filesystem, so it cannot access files outside its assigned directory. However, this is not complete security isolation, since the underlying kernel is still shared.

Overall, namespaces isolate the *view* of the system, not the actual hardware resources.

---

### Supervisor and Process Lifecycle

The supervisor is a long-running process responsible for managing all containers. This design simplifies lifecycle management because every container is a child of the supervisor.

Containers are created using `clone()`, and their execution is tracked using metadata stored in a shared structure. When a container exits, the supervisor receives a `SIGCHLD` signal and calls `waitpid()` to clean up the process. This prevents zombie processes from accumulating.

A `stop_requested` flag is used before sending termination signals. This helps distinguish between containers that are intentionally stopped and those that are killed due to errors or limits.

Without a persistent supervisor, process cleanup would be unreliable and resource leaks could occur.

---

### IPC, Threads, and Synchronization

Two separate IPC mechanisms are used in this project.

The control path uses UNIX domain sockets to communicate between the CLI and the supervisor. This allows structured request-response communication for commands like start, stop, ps, and logs.

The logging path uses pipes to capture container output (stdout and stderr). Each container has a producer thread that reads from these pipes and pushes data into a bounded buffer.

The bounded buffer follows a producer-consumer model. Multiple producer threads insert log data, while a single consumer thread writes the data to log files. A mutex protects shared buffer access, and condition variables ensure that threads block instead of busy-waiting when the buffer is full or empty.

This design avoids race conditions and ensures that log data is not lost even under high output rates.

---

### Memory Management and Enforcement

Memory usage is monitored in kernel space because user-space processes cannot reliably enforce limits on themselves. The kernel module registers container processes using their host PIDs and tracks their memory usage.

RSS (Resident Set Size) is used as an approximation of actual physical memory usage. While it is not perfect (it does not include swapped-out pages or shared memory precisely), it is sufficient for enforcing limits in this context.

Soft limits act as warning thresholds, allowing the system to log when memory usage is approaching critical levels. Hard limits act as enforcement thresholds, where the container is terminated if the limit is exceeded.

This two-level approach provides both observability and control over resource usage.

---

### Scheduling Behavior

The Linux scheduler used in this project is the Completely Fair Scheduler (CFS), which allocates CPU time based on process weights derived from nice values.

Processes with lower nice values are given higher priority and receive more CPU time, while those with higher nice values receive less. This does not mean lower-priority processes are starved, but rather that CPU time is distributed proportionally.

In practice, this results in faster execution for high-priority containers and slower execution for low-priority ones, especially when multiple CPU-bound processes are competing.

---

## 5. Design Decisions and Tradeoffs

### Namespace Isolation

We used PID, UTS, and mount namespaces along with `chroot()` for simplicity. While this provides sufficient isolation for demonstration purposes, it is not as secure as using `pivot_root()` or additional namespaces like network namespaces. The tradeoff here is simplicity versus completeness.

---

### Supervisor Architecture

A single supervisor process manages all containers. This makes it easier to maintain global state and handle logging and lifecycle events. However, it introduces a single point of failure — if the supervisor crashes, all container management is affected.

---

### Logging System

The bounded-buffer logging system ensures that container output is handled efficiently without data loss. The tradeoff is increased complexity due to synchronization using mutexes and condition variables. A simpler design would be easier to implement but less reliable under load.

---

### IPC Mechanism

UNIX domain sockets were chosen for communication between the CLI and supervisor because they are fast and suitable for local communication. The limitation is that they cannot be used for remote communication without additional setup.

---

### Kernel Monitor

Implementing memory monitoring in the kernel provides reliable enforcement, since the kernel has direct access to process memory information. The tradeoff is increased complexity and the need to write kernel-level code, which is harder to debug compared to user-space programs.

---

## 6. Scheduler Experiment Results

### Experiment: Effect of Nice Values

Two containers were run with different nice values to observe scheduling behavior.

| Container | Nice Value | Observation                       |
| --------- | ---------- | --------------------------------- |
| alpha     | -10        | Higher priority, completed faster |
| beta      | 10         | Lower priority, slower execution  |

The results show that the Linux scheduler distributes CPU time based on priority. Processes with lower nice values receive more CPU time and therefore complete their tasks faster.

This demonstrates how the Completely Fair Scheduler balances fairness while still respecting process priority.


## Conclusion

This project demonstrates the core concepts behind container runtimes, including process isolation, inter-process communication, kernel interaction, and scheduling. It provides a simplified view of how modern container systems operate.

