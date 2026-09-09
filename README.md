# cage

A tiny, secure, single-command Linux container.

`cage` runs one command inside an isolated Linux environment built from a **provided root filesystem**, supervises its entire lifetime, and destroys the environment when the command exits.

```text
cage <rootfs> <command> [args...]
```

The design is deliberately narrow:

```text
rootfs → isolate → execute → supervise → destroy
```

`cage` does **not** create, populate, install, or manage root filesystems. The supplied rootfs defines what exists inside the container; `cage` defines how that environment is isolated and how long it lives.

It is not a container platform. There are no images, registries, daemons, orchestration layers, persistent containers, or configuration files.

---

## Motivation

Linux already provides the primitives required to isolate a process.

Namespaces isolate kernel-visible resources. User namespaces isolate credentials. Mount namespaces isolate filesystems. PID namespaces isolate process trees. Network namespaces isolate networking. Capabilities and `no_new_privs` restrict privilege.

`cage` exists to combine these primitives into one small, deterministic program.

The goal is not to reproduce Docker.

The goal is to provide the smallest useful abstraction around:

> **Run one command in a disposable container and guarantee that nothing from the container survives it.**

The root filesystem is deliberately provided by the caller.

This keeps root filesystem construction outside the scope of `cage`.

---

## Root filesystem

A **root filesystem**, or **rootfs**, is a directory tree that provides the initial userspace for the container.

For example:

```text
rootfs/
├── bin/
│   └── sh
├── lib/
│   ├── libc.so.6
│   └── ...
├── lib64/
│   └── ld-linux-x86-64.so.2
├── etc/
├── usr/
└── ...
```

The rootfs is **required input** to `cage`:

```bash
cage <rootfs> <command> [args...]
```

For example:

```bash
cage ./rootfs /bin/sh
```

`cage` does not create, populate, install, modify, or otherwise manage root filesystems.

The supplied rootfs defines the initial contents of `/` inside the container.

---

## Rootfs and OverlayFS

The supplied rootfs is treated as an **immutable base filesystem**.

`cage` must never allow the container to write directly to it.

Instead, the rootfs becomes the read-only **lower layer** of an OverlayFS mount. A temporary writable **upper layer** is created for the lifetime of the container.

```text
                    PROVIDED ROOTFS
                    read-only lower
                          │
                          ▼
                    ┌─────────────┐
                    │   lowerdir  │
                    └──────┬──────┘
                           │
                           │
                    ┌──────▼──────┐
                    │  OverlayFS  │
                    │             │
                    │ lowerdir    │
                    │ upperdir    │
                    │ workdir     │
                    └──────┬──────┘
                           │
                           ▼
                     CONTAINER /
                           │
              ┌────────────┼────────────┐
              │            │            │
            read         modify       create
              │            │            │
              ▼            ▼            ▼
           lower         upper        upper
                           │
                           │
                       ephemeral
```

The lower layer contains the files supplied by the caller.

The upper layer contains every filesystem change made by the container.

The container therefore sees a normal writable filesystem:

```text
/
├── bin/       ← lower
├── lib/       ← lower
├── etc/       ← lower
├── usr/       ← lower
├── new-file   ← upper
└── modified   ← upper
```

The distinction is invisible to the command.

---

## The writable layer

The OverlayFS upper and work directories are created in temporary storage belonging to the container.

Conceptually:

```text
cage private state
│
└── writable/
    ├── upper/
    └── work/
```

These directories exist only for the lifetime of the cage.

The exact storage mechanism is an implementation detail, but it must be disposable and must not cause modifications to the supplied rootfs.

The intended model is:

```text
lowerdir = supplied rootfs
upperdir = temporary
workdir  = temporary
```

The upper and work directories are never reused between cage invocations.

---

## Filesystem changes

All filesystem modifications made by the command are directed into the upper layer.

For example, if the supplied rootfs contains:

```text
/etc/config
```

and the command executes:

```bash
echo changed > /etc/config
```

the original file remains untouched:

```text
supplied rootfs
└── etc/
    └── config          ← original
```

while OverlayFS stores the modified version in the upper layer.

The container sees:

```text
/etc/config             ← modified upper version
```

but the caller's rootfs still contains the original.

Likewise, creating:

```bash
touch /new-file
```

creates `/new-file` only in the upper layer.

It does not appear in the supplied rootfs.

---

## Deletions

OverlayFS also ensures that deleting a file does not modify the lower layer.

If the supplied rootfs contains:

```text
/etc/config
```

and the command executes:

```bash
rm /etc/config
```

OverlayFS records the deletion in the upper layer using its whiteout mechanism.

The container sees:

```text
/etc/config       ← absent
```

while the supplied rootfs remains:

```text
/etc/config       ← still present
```

This means creation, modification and deletion all remain disposable.

---

## Temporary mounts

The OverlayFS root is supplemented with filesystems that are inherently private to the container:

```text
/
├── supplied rootfs       ← OverlayFS lower layer
│
├── /tmp                  ← private tmpfs
├── /proc                 ← container procfs
└── /dev                  ← private device filesystem
```

These mounts are created inside the container's private mount namespace.

They are therefore not visible to the host mount namespace and are destroyed when the container is torn down.

---

## Rootfs lifetime

The rootfs is **not the container**.

It is persistent input used to construct the container's filesystem view.

```text
                 PROVIDED ROOTFS
                       │
                       │ read-only
                       ▼
                  OverlayFS
                       │
                 ephemeral upper
                       │
                       ▼
                   CONTAINER
                       │
                    COMMAND
                       │
                      exit
                       │
                       ▼
                destroy upper layer
                       │
                       ▼
               destroy container
                       │
                       ▼
                 ROOTFS REMAINS
                   UNCHANGED
```

This distinction is fundamental to `cage`.

The caller owns the rootfs.

`cage` owns the temporary writable layer.

The container owns neither beyond its lifetime.

---

## Filesystem lifecycle

The filesystem portion of the container is therefore:

```text
cage <rootfs> <command>
 │
 ├─ validate rootfs
 │
 ├─ create temporary writable storage
 │
 ├─ create OverlayFS
 │    ├── lowerdir = rootfs
 │    ├── upperdir = temporary
 │    └── workdir  = temporary
 │
 ├─ make OverlayFS the container's /
 │
 ├─ mount private /tmp
 │
 ├─ mount container /proc
 │
 ├─ construct private /dev
 │
 ├─ execute command
 │
 ├─ terminate remaining processes
 │
 ├─ unmount /proc, /dev and /tmp
 │
 ├─ unmount OverlayFS
 │
 ├─ destroy upper/work directories
 │
 └─ leave supplied rootfs untouched
```

The resulting invariant is:

> **Every filesystem change made by a container is temporary, while the supplied rootfs remains unchanged.**

---

# Architecture

```text
┌─────────────────────────────────────────────────────────────────────────────┐
│                                  cage                                       │
│                    tiny, secure, single-command container                   │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
                                      │ cage <rootfs> <command> [args...]
                                      │
                    ┌─────────────────┴─────────────────┐
                    │                                   │
                    ▼                                   ▼
             PROVIDED ROOTFS                     HOST SUPERVISOR
                    │                                   │
                    │ defines userspace                 │ owns lifetime
                    │                                   │ owns lock
                    │                                   │ owns cleanup
                    └───────────────┬───────────────────┘
                                    │
                                    │ clone() / namespaces
                                    ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                         ISOLATED CONTAINER                                  │
│                                                                             │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │                           NAMESPACES                                 │   │
│   │                                                                     │   │
│   │  USER       isolated UID/GID mapping                                │   │
│   │  PID        private process tree                                    │   │
│   │  MOUNT      private filesystem                                      │   │
│   │  NET        no network interfaces by default                        │   │
│   │  IPC        private IPC objects                                     │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
│   ┌───────────────────────────┐       ┌─────────────────────────────────┐   │
│   │       SECURITY            │       │          FILESYSTEM             │   │
│   │                           │       │                                 │   │
│   │  no_new_privs             │       │  supplied rootfs → lower        │   │
│   │  capabilities dropped     │       │  temporary upper → writable    │   │
│   │  seccomp                  │       │  OverlayFS → /                  │   │
│   │  inherited FDs closed     │       │  private /tmp                   │   │
│   │  parent-death handling    │       │  controlled /dev                 │   │
│   └───────────────────────────┘       │  container /proc                 │   │
│                                       └─────────────────────────────────┘   │
│                                                                             │
│                            PID 1: cage-init                                │
│                                  │                                          │
│                                  ▼                                          │
│                         ┌──────────────┐                                   │
│                         │   COMMAND    │                                   │
│                         └──────┬───────┘                                   │
│                                │                                            │
│                         ┌──────┴──────┐                                     │
│                         ▼             ▼                                     │
│                      child         child ...                                │
│                                                                             │
│                  PID 1 reaps all descendants                               │
└──────────────────────────────┬──────────────────────────────────────────────┘
                               │
                               │ command exits
                               ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                              CLEANUP                                        │
│                                                                             │
│   terminate remaining processes                                            │
│              ↓                                                              │
│   reap namespace                                                           │
│              ↓                                                              │
│   unmount temporary filesystems                                            │
│              ↓                                                              │
│   release lock                                                             │
│              ↓                                                              │
│   remove private directory                                                 │
│              ↓                                                              │
│   return command's exit status                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

# Design

`cage` consists of three conceptual parts:

```text
                 cage
                   │
       ┌───────────┼───────────┐
       │           │           │
   namespaces   filesystem   security
       │           │           │
   ┌───┴────┐      │       ┌────┴─────┐
   │ USER   │      │       │ caps     │
   │ PID    │   rootfs     │ no_new_  │
   │ MOUNT  │      │       │ privs    │
   │ NET    │      │       │ seccomp  │
   │ IPC    │      │       └──────────┘
   └────────┘      │
                   │
            supplied by caller
```

Each mechanism has one clear purpose:

| Mechanism         | Purpose                                   |
| ----------------- | ----------------------------------------- |
| Provided rootfs   | Defines container userspace               |
| User namespace    | Isolates credentials                      |
| PID namespace     | Isolates and contains processes           |
| Mount namespace   | Isolates the filesystem                   |
| Network namespace | Removes network access                    |
| IPC namespace     | Isolates IPC resources                    |
| OverlayFS         | Provides disposable filesystem changes    |
| Capabilities      | Removes kernel privileges                 |
| `no_new_privs`    | Prevents privilege acquisition            |
| Seccomp           | Restricts available system calls          |
| Private `/tmp`    | Provides ephemeral temporary storage      |
| Container `/proc` | Provides the container's process view     |
| Private `/dev`    | Exposes only explicitly permitted devices |
| Exclusive lock    | Establishes ownership of the cage         |
| `pidfd`           | Provides race-free process identity       |
| Supervisor        | Owns the container lifetime               |

---

# Lifecycle

The entire operation is intentionally linear.

```text
cage <rootfs> <command> [args...]
 │
 ├─ validate rootfs
 │
 ├─ create random private directory
 │
 ├─ open + exclusively lock directory
 │
 ├─ clone container process
 │
 ├─ create namespaces
 │    ├── USER
 │    ├── MOUNT
 │    ├── PID
 │    ├── NET
 │    └── IPC
 │
 ├─ establish UID/GID mapping
 │
 ├─ make mounts private
 │
 ├─ create temporary OverlayFS upper/work storage
 │
 ├─ mount supplied rootfs as read-only lower layer
 │
 ├─ mount OverlayFS as container root
 │
 ├─ pivot_root()
 │
 ├─ remove old root
 │
 ├─ mount private /tmp
 │
 ├─ mount container /proc
 │
 ├─ construct private /dev
 │
 ├─ close inherited FDs
 │
 ├─ drop capabilities
 │
 ├─ set no_new_privs
 │
 ├─ establish parent-death handling
 │
 ├─ start cage-init as PID 1
 │
 ├─ exec COMMAND
 │
 ├─ supervise
 │
 ├─ terminate descendants
 │
 ├─ reap namespace
 │
 ├─ collect exit status
 │
 ├─ destroy temporary mounts
 │
 ├─ destroy OverlayFS upper/work storage
 │
 ├─ release lock
 │
 ├─ remove private directory
 │
 └─ exit with COMMAND's status
```

Every operation has a defined owner and a defined failure path.

If a setup operation fails, `cage` does not continue with a partially isolated environment.

If supervision fails, the safe response is to terminate the container rather than leave it running.

If the command exits, the environment is destroyed before `cage` returns.

---

# Failure model

Failure handling is part of the design rather than an afterthought.

Every resource has one owner.

```text
resource                  owner
────────────────────────────────────────
supplied rootfs             caller
private directory            cage parent
lock                         cage parent
container namespaces         kernel / container
temporary mounts             cage
OverlayFS upper/work         cage
command                     cage-init
descendants                 cage-init
exit status                 cage parent
```

Setup proceeds forwards:

```text
validate
  ↓
create
  ↓
lock
  ↓
isolate
  ↓
mount
  ↓
pivot
  ↓
prepare
  ↓
execute
  ↓
supervise
```

Cleanup proceeds backwards:

```text
supervise
  ↓
terminate
  ↓
reap
  ↓
unmount
  ↓
destroy writable layer
  ↓
remove temporary state
  ↓
unlock
```

If setup fails halfway through, every successfully-created resource is cleaned up before returning.

If execution fails, the container is destroyed.

If the command crashes, the container is destroyed.

If the command forks, descendants remain contained.

If the command exits while descendants remain, those descendants are terminated.

If the supervisor loses the command, the container is destroyed.

If the supervisor itself unexpectedly disappears, parent-death handling prevents the isolated process from becoming an unmanaged container.

The preferred failure mode is therefore:

> **Kill and clean up rather than continue with an uncertain security state.**

---

# Security guarantees

Under the assumptions of the Linux kernel and the supplied rootfs, `cage` aims to guarantee:

```text
✓ isolated UID/GID namespace
✓ isolated process namespace
✓ isolated mount namespace
✓ isolated network namespace
✓ isolated IPC namespace
✓ immutable supplied rootfs
✓ disposable writable filesystem layer
✓ no host users or groups created
✓ no host network configuration required
✓ no inherited file descriptors by default
✓ capabilities removed
✓ no_new_privs enabled
✓ private filesystem root
✓ private /tmp
✓ private /dev
✓ container-specific /proc
✓ supervised process tree
✓ race-resistant process identification
✓ deterministic cleanup
✓ exclusive ownership of temporary state
✓ no persistent container state
✓ no daemon
✓ supplied rootfs remains untouched
```

These mechanisms are layered deliberately.

No single mechanism is treated as sufficient isolation.

In particular:

```text
chroot       ≠ container
UID change   ≠ container
namespace    ≠ complete security policy
seccomp      ≠ filesystem isolation
rootfs       ≠ container
```

The container is the combination of these mechanisms.

---

# What cage is not

`cage` intentionally does not provide:

```text
✗ rootfs creation
✗ rootfs population
✗ package installation
✗ dependency resolution
✗ container images
✗ image registries
✗ OCI compatibility
✗ persistent containers
✗ container networking
✗ volume management
✗ orchestration
✗ service discovery
✗ container daemons
✗ configuration files
✗ global container state
✗ host user/group management
✗ privileged helper processes
```

A rootfs is an input to `cage`, not something `cage` manages.

If a feature does not directly contribute to:

```text
isolate → execute → supervise → destroy
```

it probably does not belong in `cage`.

---

# Design principles

### One operation

`cage` performs one operation:

> Run one command inside one disposable container.

### Rootfs is supplied

`cage` does not build userspace.

The caller provides the filesystem.

### Immutable base

The supplied rootfs is treated as read-only.

All container filesystem changes occur in a disposable OverlayFS upper layer.

### No daemon

The process invoking `cage` owns the operation.

There is no background service responsible for containers.

### No persistent state

When `cage` exits, its container and writable filesystem layer are gone.

### No global mutation

`cage` should not create system users, modify system configuration, or configure the host network.

### Kernel primitives over abstractions

Where Linux already provides a primitive, `cage` should use it directly.

### Explicit ownership

Every resource should have one obvious owner.

### Deterministic cleanup

Cleanup is part of normal execution, not an optional maintenance operation.

### Fail closed

An uncertain security state is a failed operation.

The command should not be executed if the requested isolation cannot be established.

### Small attack surface

Every additional feature increases the code, state and security surface.

`cage` therefore prefers a small amount of exact functionality over a large amount of configurable functionality.

---

# Core invariants

The implementation should preserve these invariants throughout its lifetime:

```text
1. Only cage owns the container environment.

2. The supplied rootfs is never modified or destroyed by cage.

3. The supplied rootfs is the immutable lower filesystem layer.

4. All filesystem changes made by the container occur in the
   disposable upper layer.

5. The lock exists for the lifetime of cage.

6. The supervised process cannot outlive its container.

7. Processes inside the cage cannot access the host process namespace.

8. Processes inside the cage cannot access the host filesystem
   unless something is explicitly provided to them.

9. No network access exists by default.

10. No inherited host file descriptor crosses the boundary accidentally.

11. The command receives no unnecessary capabilities.

12. The container has no persistent host user or group.

13. Temporary container state exists only for the invocation.

14. Cleanup completes before cage returns.

15. A failed isolation step prevents command execution.

16. An unexpected supervisor failure cannot leave an unmanaged
    container running.
```

---

# Process model

```text
HOST
│
└── cage
     │
     └── isolated namespace
          │
          └── cage-init                 PID 1
               │
               └── COMMAND              PID 2
                    ├── child           PID 3
                    ├── child           PID 4
                    └── ...
```

The host supervisor owns the container.

`cage-init` owns the process tree.

The command owns nothing outside its own process resources.

This separation is intentional.

---

# Filesystem model

```text
HOST
│
├── normal filesystem
│
└── supplied rootfs
     │
     ├── bin/
     ├── lib/
     ├── etc/
     ├── usr/
     └── ...
              │
              │ read-only lower layer
              ▼
          OverlayFS
              │
              │ ephemeral upper layer
              ▼
          CONTAINER /
              │
              ├── supplied rootfs
              ├── container changes
              │
              ├── /tmp  → private tmpfs
              ├── /proc → container proc
              └── /dev  → private tmpfs
```

The supplied rootfs survives the container.

The OverlayFS upper and work directories do not.

Temporary mounts do not.

---

# Complete model

```text
                         ┌─────────────┐
                         │    cage     │
                         └──────┬──────┘
                                │
                   ROOTFS + COMMAND
                                │
              ┌─────────────────┴─────────────────┐
              │                                   │
              ▼                                   ▼
        ┌─────────────┐                    ┌─────────────┐
        │   ROOTFS    │                    │ SUPERVISOR  │
        │             │                    │             │
        │ read-only   │                    │ lifetime    │
        │ userspace   │                    │ lock        │
        │ binaries    │                    │ pidfds      │
        │ libraries   │                    │ cleanup     │
        └──────┬──────┘                    └──────┬──────┘
               │                                  │
               │ lower layer                      │
               └────────────────┬─────────────────┘
                                │
                            OverlayFS
                                │
                    ephemeral upper/work
                                │
                                ▼
              ┌─────────────────────────────────┐
              │       ISOLATED CONTAINER        │
              │                                 │
              │ USER ───── credentials          │
              │ PID ────── processes            │
              │ MOUNT ──── filesystem           │
              │ NET ────── networking            │
              │ IPC ────── IPC                  │
              │                                 │
              │ capabilities ─── dropped        │
              │ no_new_privs ─── enabled        │
              │ seccomp ──────── restricted     │
              │                                 │
              │          PID 1                  │
              │       cage-init                 │
              │           │                     │
              │           ▼                     │
              │        COMMAND                  │
              │           │                     │
              │       descendants               │
              └───────────┬─────────────────────┘
                          │
                     command exits
                          │
                          ▼
                 terminate descendants
                          │
                          ▼
                       reap
                          │
                          ▼
                  destroy temporary state
                          │
                          ▼
                     release lock
                          │
                          ▼
                  return exit status
```

---

# Philosophy

`cage` is intentionally boring.

There is no container lifecycle to manage because the process lifecycle **is** the container lifecycle.

There is no user database because a user namespace is sufficient.

There is no rootfs builder because the caller provides the userspace.

There is no network manager because the default network is empty.

There is no cleanup daemon because the supervisor owns cleanup.

There is no persistent state because the environment is disposable.

There is no complicated IPC protocol because the kernel already provides process descriptors, signals, namespaces and locks.

The entire program should be understandable as one chain:

```text
rootfs
  ↓
validate
  ↓
lock
  ↓
isolate
  ↓
mount
  ↓
overlay
  ↓
pivot
  ↓
execute
  ↓
supervise
  ↓
destroy
```

The division of responsibility is equally simple:

```text
ROOTFS  → defines what exists
CAGE    → defines isolation
COMMAND → defines what runs
SUPERVISOR → defines how long it lives
```

**Nothing more. Nothing less.**
