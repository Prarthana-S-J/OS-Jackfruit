<img width="940" height="565" alt="image" src="https://github.com/user-attachments/assets/82ac1ac8-74d7-40d5-967b-6b121ad7c164" /># Multi-Container Runtime with Kernel Memory Monitor

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
| 1 | Multi-container supervision | Two containers running under a single supervisor       | *(Add here)* |
| 2 | Metadata tracking           | `engine ps` showing container states and metadata      | *(Add here)* |
| 3 | Bounded-buffer logging      | Logs captured from container output                    | *(Add here)* |
| 4 | CLI and IPC                 | CLI command interacting with supervisor                | *(Add here)* |
| 5 | Soft-limit warning          | Kernel log showing container registration / monitoring | *(Add here)* |
| 6 | Hard-limit enforcement      | Kernel integration and monitoring evidence             | *(Add here)* |
| 7 | Scheduling experiment       | Execution time difference using nice values            | *(Add here)* |
| 8 | Clean teardown              | Supervisor shutdown and no leftover processes          | *(Add here)* |

---
## 4. Engineering Analysis

### Isolation Mechanisms

Each container is created using Linux namespaces (PID, UTS, and mount). This ensures that processes inside a container cannot see or interfere with processes outside it. Filesystem isolation is achieved using `chroot`, which restricts the container to its own root filesystem. However, all containers still share the same kernel, which is why kernel-level enforcement is required.

### Supervisor and Process Lifecycle

A long-running supervisor manages all containers and maintains their metadata. Containers are created using `clone()`, and the supervisor tracks their lifecycle. When a container exits, it is reaped using `waitpid()` to prevent zombie processes. Signal handling ensures proper shutdown and cleanup.

### IPC, Threads, and Synchronization

Two IPC paths are used:

* Control path using UNIX domain sockets
* Logging path using pipes

The logging system uses a bounded buffer with producer-consumer design. Producer threads read container output and insert it into the buffer, while a consumer thread writes it to log files. Mutexes and condition variables ensure safe synchronization.

### Memory Management and Enforcement

The kernel module monitors container processes using their host PIDs. RSS is used to track memory usage. A soft limit triggers a warning, while a hard limit results in termination. This logic is implemented in kernel space for accuracy and reliability.

### Scheduling Behavior

Different `nice` values were used to observe scheduling differences. Containers with lower nice values completed faster, while those with higher values took longer. This demonstrates how the Linux scheduler prioritizes processes.

---

## 5. Design Decisions and Tradeoffs

### Namespace Isolation

Namespaces provide strong isolation with low overhead. However, full isolation would require additional namespaces such as network namespaces.

### Supervisor Architecture

A centralized supervisor simplifies management and logging but introduces a single point of failure.

### Logging System

The bounded-buffer design ensures no data loss and smooth logging, but increases complexity due to synchronization.

### IPC Mechanism

UNIX domain sockets were chosen for simplicity and efficiency, though they are limited to local communication.

### Kernel Monitor

Kernel-level monitoring ensures reliable enforcement of limits but adds complexity compared to user-space solutions.

---

## 6. Scheduler Experiment Results

| Configuration | Execution Time |
| ------------- | -------------- |
| nice = -10    | Faster         |
| nice = 10     | Slower         |

The results show that higher-priority processes receive more CPU time and complete faster, reflecting the behavior of the Linux scheduler.

---

## Conclusion

This project demonstrates the core concepts behind container runtimes, including process isolation, inter-process communication, kernel interaction, and scheduling. It provides a simplified view of how modern container systems operate.

