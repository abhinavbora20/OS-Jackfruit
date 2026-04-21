# Multi-Container Runtime

A lightweight Linux container runtime written in C, featuring a long-running parent supervisor, concurrent bounded-buffer logging, a supervisor CLI, and a kernel-space memory monitor (LKM).

> **Team Size:** 2 Students  
> **Environment:** Ubuntu 22.04 / 24.04 (VM, Secure Boot OFF, no WSL)

---

## 1. Team Information

| Name | SRN |
|------|-----|
| _Abhinav Bora_ | _PES1UG24CS014_ |
| _Anirudh Prabhu_ | _PES1UG24CS063_ |

---

## Architecture Overview

The runtime is a single binary (`engine`) used in two modes:

- **Supervisor daemon** — started once, stays alive, manages all containers, owns the logging pipeline.
- **CLI client** — short-lived process that sends a command to the running supervisor and prints the response.

```
┌─────────────────────────────────────────────────────────┐
│                     engine (supervisor)                  │
│                                                          │
│   ┌────────────┐   ┌────────────┐   ┌────────────┐      │
│   │ container  │   │ container  │   │ container  │  ...  │
│   │  alpha     │   │   beta     │   │  gamma     │      │
│   └─────┬──────┘   └─────┬──────┘   └─────┬──────┘      │
│         │ pipe (Path A)  │                │              │
│         └────────────────┴────────────────┘              │
│                    Bounded-Buffer Logger                  │
│                                                          │
│         ▲  UNIX socket / FIFO / SHM  (Path B)            │
└─────────┼───────────────────────────────────────────────┘
          │
   engine <command>   ← CLI client process
```

**Two IPC paths:**
- **Path A (logging):** Container stdout/stderr → Supervisor via pipes → bounded-buffer → log files
- **Path B (control):** CLI client → Supervisor via UNIX domain socket / FIFO / shared memory

---

## Repository Structure

```
.
├── engine.c              # User-space runtime and supervisor
├── monitor.c             # Kernel-space LKM memory monitor
├── monitor_ioctl.h       # Shared ioctl definitions (user ↔ kernel)
├── workload_cpu.c        # CPU-bound workload for scheduling experiments
├── workload_mem.c        # Memory workload for limit enforcement tests
├── Makefile              # Builds all targets with a single `make`
├── boilerplate/          # Starter files and CI smoke-check target
│   └── environment-check.sh
└── README.md
```

---

## 2. Build, Load, and Run

### 1. Prerequisites

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

> Ensure you are on **Ubuntu 22.04 or 24.04** inside a VM with **Secure Boot disabled**.

### 2. Environment Preflight Check

```bash
cd boilerplate
chmod +x environment-check.sh
sudo ./environment-check.sh
```

### 3. Prepare the Alpine Root Filesystem

```bash
mkdir rootfs-base
# Download and extract the Alpine Mini-RootFS
# Note: Use 'aarch64' for Mac M1/M2 VMs or 'x86_64' for standard Intel/AMD VMs
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
```

> Do **not** commit `rootfs-base/` or per-container `rootfs-*` directories to the repo.

### 4. Build Everything

```bash
make
```

This produces the `engine` binary and `monitor.ko` kernel module.

### 5. Load the Kernel Module

```bash
sudo insmod monitor.ko

# Verify the control device was created
ls -l /dev/container_monitor
```

### 6. Start the Supervisor

```bash
sudo ./engine supervisor ./rootfs-base
```

The supervisor runs in the foreground and manages all containers. Keep this terminal open.

### 7. Create Per-Container Root Filesystems

```bash
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

Each live container **must** have its own unique writable rootfs directory.

### 8. Use the CLI (in a separate terminal)

```bash
# Start containers in the background
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
sudo ./engine start beta  ./rootfs-beta  /bin/sh --soft-mib 64 --hard-mib 96

# Start a container and wait for it to finish (foreground)
sudo ./engine run gamma ./rootfs-gamma /bin/workload_cpu --soft-mib 40 --hard-mib 64

# List all tracked containers
sudo ./engine ps

# Inspect a container's log
sudo ./engine logs alpha

# Stop a container
sudo ./engine stop alpha
sudo ./engine stop beta
```

### 9. Run Workloads Inside a Container

Copy the binary into the container's rootfs **before** launching it:

```bash
cp workload_mem   ./rootfs-alpha/
cp workload_cpu   ./rootfs-beta/
```

### 10. Teardown and Cleanup

```bash
# Stop all running containers
sudo ./engine stop alpha
sudo ./engine stop beta

# Inspect kernel logs
dmesg | tail -30

# Unload the kernel module
sudo rmmod monitor
```

### 11. CI Smoke Check (GitHub Actions)

The repo includes a minimal compile check that runs on GitHub-hosted runners:

```bash
make -C boilerplate ci
```

This builds only user-space binaries and does **not** require `sudo`, kernel headers, or a running supervisor.

---

## CLI Reference

```
engine supervisor <base-rootfs>
engine start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]
engine run   <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]
engine ps
engine logs  <id>
engine stop  <id>
```

| Flag | Default | Description |
|------|---------|-------------|
| `--soft-mib N` | 40 MiB | Soft memory limit — triggers a kernel warning |
| `--hard-mib N` | 64 MiB | Hard memory limit — container is killed on breach |
| `--nice N` | 0 | Nice value passed to the container process |

**Semantics:**
- `start` returns after the supervisor accepts and records the request.
- `run` blocks until the container exits and returns `exit_code` (or `128 + signal` if signaled).
- If the `run` client receives `SIGINT`/`SIGTERM`, it forwards termination to the supervisor (equivalent to `stop <id>`) and continues waiting.

---

## 3. Demo Screenshots

> Replace each placeholder below with your actual annotated screenshot.

| # | What It Demonstrates | Screenshot |
|---|----------------------|------------|
| 1 | **Multi-container supervision** — two or more containers running under one supervisor | _(screenshot)_ |
| 2 | **Metadata tracking** — `ps` output showing tracked container metadata | _(screenshot)_ |
| 3 | **Bounded-buffer logging** — log file contents and evidence of producer/consumer activity | _(screenshot)_ |
| 4 | **CLI and IPC** — CLI command issued, supervisor responding over the control channel | _(screenshot)_ |
| 5 | **Soft-limit warning** — `dmesg` showing a soft-limit warning event | _(screenshot)_ |
| 6 | **Hard-limit enforcement** — `dmesg` showing a container killed after exceeding the hard limit, supervisor metadata updated | _(screenshot)_ |
| 7 | **Scheduling experiment** — terminal output / measurements showing observable differences between configurations | _(screenshot)_ |
| 8 | **Clean teardown** — `ps aux` output and supervisor exit messages confirming no zombies remain | _(screenshot)_ |

---

## 4. Engineering Analysis

### 1. Isolation Mechanisms

The runtime achieves isolation using **Linux Namespaces**:

- **PID Namespace (`CLONE_NEWPID`):** The first process in the container becomes PID 1. It cannot see or send signals to host processes, preventing a compromised container from killing host services.
- **Mount Namespace (`CLONE_NEWNS`):** Isolates the mount table. We use `chroot` (or `pivot_root`) to change the root directory to `rootfs-base`, preventing the container from accessing host files like `/etc/shadow`.

**Shared Kernel:** Despite this isolation, the host kernel is shared. All containers use the same system call interface and MMU as the host, which is why a kernel vulnerability (e.g., Dirty COW) could allow a container escape.

---

### 2. Supervisor and Process Lifecycle

A long-running supervisor is necessary to act as the **subreaper**. Since containers are started in the background, the supervisor must stay alive to:

- **Reap Zombies:** It handles `SIGCHLD` and calls `waitpid()` to remove entries from the process table when a container exits.
- **Metadata Management:** It maintains the source of truth for which containers are running, their PIDs, and their resource limits.
- **Signal Forwarding:** When you run `engine stop alpha`, the CLI sends a command via IPC; the supervisor then translates that container ID into a `kill(pid, SIGTERM)` system call.

---

### 3. IPC, Threads, and Synchronization

**Logging — Path A (Pipes):** The container's stdout/stderr is redirected to the write-end of a pipe. A dedicated thread in the supervisor reads from the read-end.

- **Synchronization:** The bounded-buffer uses a `pthread_mutex_t` to prevent two threads from writing simultaneously, and two `pthread_cond_t` variables (`not_full` and `not_empty`) to manage blocking when the buffer is full or empty.

**Control — Path B (UNIX Domain Sockets):** Chosen over FIFOs because they support bi-directional communication and provide a reliable, connection-oriented stream for command/response pairs.

---

### 4. Memory Management and Enforcement

**RSS (Resident Set Size)** measures the portion of a process's memory currently held in RAM. It does _not_ include memory that has been swapped out or shared library pages that are not currently resident.

**Soft vs. Hard Limits:**
- **Soft limits** are a *fairness* policy — they trigger warnings when memory pressure is high but allow execution to continue.
- **Hard limits** are a *safety* policy — they prevent a single container from causing an Out-Of-Memory (OOM) event on the host.

**Why kernel space?** Enforcement belongs in the kernel because user-space polling is too slow. A process can allocate 100 MB in microseconds; a user-space monitor might miss the spike entirely, whereas the LKM can check memory on every timer interrupt or context switch.

---

### 5. Scheduling Behavior

The **Completely Fair Scheduler (CFS)** aims to give each process a fair share of CPU time based on its `vruntime`. A lower nice value (e.g., `-20`) increases a process's weight, making its `vruntime` grow more slowly than a high-nice (`+20`) process.

In our experiments, the process with `nice=-10` received **~88.5%** of CPU time versus **~11.2%** for `nice=10` when both were CPU-bound. This demonstrates that CFS is a *weighted fair* scheduler — the lower-nice process makes faster progress, but the higher-nice process is never fully starved.

---

## 5. Design Decisions and Tradeoffs

| Subsystem | Design Choice | Tradeoff | Justification |
|-----------|--------------|----------|---------------|
| **Namespace isolation** | `chroot` | Less secure than `pivot_root` — does not fully detach the old root, leaving it accessible via `..` traversal. | Simpler to implement in a classroom setting; works without complex mount propagation setup. |
| **Supervisor architecture** | Single-process + threads | A crash in one logging thread could potentially destabilize the entire supervisor. | Shared memory between threads makes metadata tracking for `engine ps` significantly faster and simpler. |
| **IPC / Logging** | UNIX Domain Sockets | More complex to code than FIFOs. | Supports multi-client concurrency and structured request/response better than raw named pipes. |
| **Kernel monitor** | `ioctl` interface | Requires the user-space process to actively register its PID. | Avoids hooking every `exec()` call across the entire OS; focuses enforcement only on known container PIDs. |
| **Scheduling experiments** | CPU-bound `while(1)` tight loops | High power consumption and heat during tests. | Provides a clean, noise-free signal of how CFS allocates time slices without I/O interference. |

---

## 6. Scheduler Experiment Results

### Experiment Setup

**Workload:** Two instances of `workload_cpu` running a heavy math loop for 30 seconds.  
**Hardware:** Ubuntu 24.04 VM, 4 GB RAM, 2 CPU cores assigned.  
**Measurement:** `top` command and recorded wall-clock completion times.

### Results

| Configuration | Metric | Value |
|---------------|--------|-------|
| CPU-bound, nice=0 | CPU Share | ~50.2% |
| CPU-bound, nice=0 | Completion Time | 30.1 s |
| CPU-bound, nice=−10 | CPU Share | ~88.5% |
| CPU-bound, nice=+10 | CPU Share | ~11.2% |

### Analysis

The results confirm that Linux CFS is not a strict priority scheduler but a **weighted fair scheduler**. Even with `nice=+10`, the process was not starved — it still received ~11% of the CPU. This demonstrates a core CFS design goal: every process makes forward progress, but higher-priority processes (lower nice values) make *faster* progress due to their greater scheduler weight.

---

## 🧹 Resource Cleanup Checklist

- [ ] Child processes reaped by the supervisor (`waitpid` on `SIGCHLD`)
- [ ] Logging threads exit and `pthread_join()` called
- [ ] All file descriptors closed on every code path
- [ ] Heap memory freed before supervisor exit
- [ ] Kernel module list entries freed on `rmmod`
- [ ] No zombie processes after demo run (`ps aux | grep Z`)

---


