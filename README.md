# cage

A small, Linux-native container runtime written in C.

`cage` is a deliberately minimal container runtime built around Linux namespaces, `pivot_root`, OverlayFS, capability dropping, and a small configuration file.

It is designed to make the mechanics of Linux containers explicit rather than hide them behind a large abstraction layer.

> **Status:** early development — not production hardened.

## What cage does

`cage` creates an isolated Linux process environment with:

* PID namespace isolation
* mount namespace isolation
* UTS namespace isolation
* IPC namespace isolation
* network namespace isolation
* a private `/dev`
* a private `/proc`
* an OverlayFS-backed writable root
* configurable bind mounts
* dropped Linux capabilities
* a restricted capability bounding set
* `no_new_privs`
* parent-death handling
* inherited file descriptor cleanup

The container runs a normal Linux executable. There is no daemon, image format, registry, or container service involved.

## Usage

```text
cage [options] <executable> [arguments...]
```

Options:

```text
--config <path>  use the specified configuration file
-h, --help       show this help message
```

For example:

```sh
cage --config cage.toml /bin/sh
```

If `--config` is omitted, `cage` looks for:

```text
cage.toml
```

in the current working directory.

The configuration file defines the root filesystem and any additional mounts.

## Configuration

A minimal configuration is:

```toml
rootfs = "./rootfs"
```

A configuration with additional mounts:

```toml
rootfs = "./rootfs"

[[mounts]]
source = "/tmp/cage-mount"
target = "/data"

[[mounts]]
source = "/home/user/project"
target = "/workspace"
readonly = true
```

### Root filesystem

`rootfs` specifies the directory used as the container's supplied root filesystem.

The path must refer to an existing directory.

The supplied root filesystem is used as the **lower layer** of an OverlayFS mount. `cage` does not modify this directory during normal container execution.

### Mounts

Additional host paths can be exposed inside the container with `[[mounts]]`.

Each mount has:

```toml
[[mounts]]
source = "/host/path"
target = "/container/path"
readonly = false
```

`source` is a path on the host.

`target` is an absolute path inside the container.

`readonly` controls whether the configured bind mount is writable from inside the container. It defaults to `false`.

For example:

```toml
[[mounts]]
source = "/tmp/cage-data"
target = "/data"
```

makes the host directory available as:

```text
/data
```

inside the container.

A read-only mount can be configured with:

```toml
[[mounts]]
source = "/home/user/project"
target = "/workspace"
readonly = true
```

The configuration parser deliberately implements only the subset of TOML required by `cage`. It is not intended to be a general-purpose TOML implementation.

Currently supported configuration values are:

* `rootfs`
* `[[mounts]]`
* `source`
* `target`
* `readonly`

Unknown settings and duplicate fields are rejected.

## Root filesystem

`cage` requires a supplied root filesystem.

For example:

```text
rootfs/
├── bin/
├── etc/
├── lib/
├── lib64/
├── proc/
├── sbin/
├── tmp/
├── usr/
└── var/
```

The root filesystem can be produced by any mechanism capable of providing a usable Linux filesystem tree.

`cage` does not manage images or distributions.

A root filesystem is simply a directory supplied to the runtime.

## Rootfs and OverlayFS

The supplied root filesystem acts as the immutable lower layer of an OverlayFS filesystem.

Conceptually:

```text
             supplied rootfs
              lower layer
                   │
                   ▼
             ┌───────────┐
             │ OverlayFS │
             └─────┬─────┘
                   │
                   ▼
             container root
```

The writable OverlayFS upper and work directories are created in a temporary runtime directory.

They exist only for the lifetime of the container.

This provides two important properties:

1. The supplied root filesystem remains unchanged.
2. Normal filesystem changes made by the container disappear when the container is destroyed.

## Filesystem changes

Suppose the container starts with:

```text
rootfs/
└── etc/
    └── config
```

Inside the container:

```sh
echo changed > /etc/config
touch /tmp/example
rm /etc/config
```

These operations affect the OverlayFS upper layer rather than the supplied root filesystem.

When the container exits, the temporary upper layer is removed.

The original rootfs therefore remains:

```text
rootfs/
└── etc/
    └── config
```

with its original contents.

### New files

Files created inside the normal container filesystem are stored in the temporary OverlayFS upper layer.

They disappear when the container is torn down.

### Modifications

Changes to files originating from the supplied rootfs are represented in the OverlayFS upper layer.

They do not modify the supplied rootfs.

### Deletions

Deleting a file from the container creates the appropriate OverlayFS whiteout state in the upper layer.

The lower-layer file therefore appears deleted from inside the container without being removed from the supplied rootfs.

## Configured mounts are different

Configured bind mounts intentionally bypass the disposable OverlayFS layer.

For example:

```toml
[[mounts]]
source = "/tmp/cage-data"
target = "/data"
```

means that `/data` is backed directly by:

```text
/tmp/cage-data
```

on the host.

Therefore:

```sh
echo hello > /data/file
```

can modify the host filesystem.

This is intentional.

### Persistence model

| Filesystem location                   | Backing storage | Persists after exit |
| ------------------------------------- | --------------- | ------------------- |
| `/etc`                                | OverlayFS       | No                  |
| `/tmp`                                | OverlayFS       | No                  |
| `/usr`                                | OverlayFS       | No                  |
| `/data` configured as bind mount      | Host path       | Yes                 |
| `/workspace` configured as bind mount | Host path       | Yes                 |

Configured mounts should therefore be treated as explicit persistence or host-integration points.

Use `readonly = true` when the container should be able to access a host path without modifying it.

## Filesystem lifecycle

The filesystem setup broadly follows:

```text
supplied rootfs
      │
      ▼
create temporary runtime directory
      │
      ├── upper/
      └── work/
      │
      ▼
mount OverlayFS
      │
      ▼
private container root
      │
      ├── /proc
      ├── /dev
      └── configured bind mounts
      │
      ▼
pivot_root
      │
      ▼
execute process
      │
      ▼
container exits
      │
      ▼
unmount / cleanup
      │
      ▼
remove temporary OverlayFS state
```

The supplied root filesystem is never used as the writable container layer.

## Architecture

The runtime is intentionally small.

```text
                  cage
                   │
                   ▼
            parse configuration
                   │
                   ▼
             create container
                   │
        ┌──────────┴──────────┐
        │                     │
        ▼                     ▼
    namespaces            filesystem
        │                     │
        │              ┌──────┴──────┐
        │              │             │
        │           OverlayFS    bind mounts
        │              │             │
        │              └──────┬──────┘
        │                     │
        └──────────┬──────────┘
                   ▼
               pivot_root
                   │
                   ▼
             security setup
                   │
                   ▼
                execve
```

The implementation is divided into a small number of components:

```text
include/
├── cage.h
├── config.h
└── container.h

src/
├── cage.c
├── config.c
└── container.c

tests/
└── test_runner.c
```

## Container lifecycle

A container follows this general lifecycle:

```text
CLI
 │
 ▼
load configuration
 │
 ▼
validate rootfs
 │
 ▼
clone child
 │
 ▼
create namespaces
 │
 ▼
configure hostname
 │
 ▼
mount OverlayFS
 │
 ▼
prepare /dev and /proc
 │
 ▼
pivot_root
 │
 ▼
mount configured bind mounts
 │
 ▼
drop privileges
 │
 ▼
close inherited file descriptors
 │
 ▼
execve
 │
 ▼
process exits
 │
 ▼
parent cleans up
```

The parent process owns the lifecycle of the container and performs cleanup after the child exits.

## Isolation

### PID namespace

The container receives its own PID namespace.

The container's init process becomes PID 1 inside that namespace.

Processes created inside the container therefore have a separate PID view from the host.

### Mount namespace

The container receives a private mount namespace.

Mount operations performed inside the container do not directly alter the host mount namespace.

### UTS namespace

The container receives an independent hostname and domain-name namespace.

### IPC namespace

System V IPC and POSIX message queue state is isolated from the host.

### Network namespace

The container receives its own network namespace.

No network interfaces are configured by `cage` itself.

### `/dev`

A private `/dev` is created for the container.

Only the devices explicitly provided by the runtime are made available.

### `/proc`

A private proc filesystem is mounted inside the container.

It reflects the container's PID namespace rather than exposing the host's process tree directly.

## Security model

`cage` applies several basic privilege-reduction mechanisms before executing the container process.

### Capabilities

Linux capabilities are dropped from the container process.

The capability bounding set is also restricted so that dropped capabilities cannot simply be regained through later execution.

### `no_new_privs`

`PR_SET_NO_NEW_PRIVS` is enabled before execution.

This prevents the process and its descendants from gaining additional privileges through mechanisms such as set-user-ID and set-group-ID executables or file capabilities.

### Parent death handling

The container process establishes parent-death handling so that it does not remain running independently if its supervising parent disappears.

The implementation also accounts for the race between creating the child and configuring parent-death behaviour by checking that the expected parent remains alive.

### Inherited file descriptors

File descriptors inherited from the launching process are closed before `execve`.

This prevents unrelated host descriptors from accidentally becoming available to the container process.

## Failure model

Container setup consists of multiple operations that can fail:

```text
clone
  │
  ├── namespace setup
  ├── mount setup
  ├── OverlayFS setup
  ├── /dev setup
  ├── /proc setup
  ├── pivot_root
  ├── configured mounts
  └── security setup
          │
          ▼
        execve
```

Failures during setup must not leave persistent container state behind.

Temporary OverlayFS directories and mounts belong to the container lifecycle and are cleaned up when setup or execution fails.

The runtime treats setup failure as a container creation failure rather than allowing a partially configured environment to continue running.

## Design principles

### Small

The implementation should remain small enough to understand by reading the source.

### Linux-native

`cage` uses Linux primitives directly rather than recreating them behind a large abstraction layer.

### Explicit

Container behaviour should be visible in the code and configuration.

### Disposable

The normal container filesystem is ephemeral.

### Host integration is explicit

Host filesystem access is only introduced through explicitly configured bind mounts.

### No daemon

There is no background service managing containers.

### No image management

`cage` consumes an existing root filesystem. Creating, downloading, versioning, and distributing root filesystems are outside its scope.

## Core invariants

The following properties are fundamental to the design:

1. The supplied rootfs is never used as the writable container layer.
2. Normal container filesystem changes are stored in a temporary OverlayFS upper layer.
3. Normal container filesystem changes disappear after teardown.
4. Configured bind mounts are explicit exceptions and may modify host-backed storage.
5. The container has its own mount namespace.
6. The container has its own PID namespace.
7. Container processes cannot retain arbitrary inherited file descriptors.
8. Container privileges are reduced before execution.
9. Temporary runtime state belongs to the container lifecycle.
10. Container setup failure must not leave persistent temporary state behind.

## Process model

The host process acts as the container supervisor.

Conceptually:

```text
host
 │
 └── cage
      │
      └── container process
           │
           ├── PID namespace
           ├── mount namespace
           ├── UTS namespace
           ├── IPC namespace
           └── network namespace
```

The container process eventually replaces itself with the requested executable using `execve`.

There is no long-running runtime daemon.

## Filesystem model

The filesystem model can be reduced to three layers:

```text
             Host filesystem
                   │
          ┌────────┴────────┐
          │                 │
          ▼                 ▼
      supplied rootfs   configured mounts
       lower layer       host-backed paths
          │                 │
          ▼                 │
       OverlayFS            │
          │                 │
          ▼                 │
    temporary upper         │
       + work               │
          │                 │
          └────────┬────────┘
                   ▼
             container root
```

The important distinction is that the OverlayFS upper layer is disposable, while configured bind mounts refer directly to host storage.

## Complete model

A useful way to think about `cage` is:

```text
                   configuration
                         │
                         ▼
                    rootfs path
                         │
                         ▼
                  supplied rootfs
                         │
                         ▼
                    OverlayFS
                  ┌──────┴──────┐
                  │             │
               lower          upper
                  │          temporary
                  │             │
                  └──────┬──────┘
                         │
                         ▼
                  container root
                         │
             ┌───────────┼───────────┐
             │           │           │
           /proc        /dev     bind mounts
             │           │           │
             └───────────┼───────────┘
                         │
                         ▼
                    pivot_root
                         │
                         ▼
                  security setup
                         │
                         ▼
                       exec
```

Most of what happens inside the container therefore exists only for the lifetime of that process.

The exception is anything intentionally exposed through a host-backed configured mount.

## What cage is not

`cage` intentionally does **not** provide:

* images
* registries
* image distribution
* container orchestration
* networking management
* persistent container storage
* daemon management
* a virtual machine
* a general-purpose TOML implementation

It does provide a small configuration file because filesystem and mount configuration are part of the runtime's explicit interface.

## Project status

`cage` is being developed incrementally through milestones.

The current implementation covers the core container lifecycle, filesystem isolation, configuration-driven rootfs and mounts, and initial privilege reduction.

Further hardening and reliability work remains before the runtime should be considered production-ready.

## Building

`cage` is built with the provided `Makefile`.

```sh
make
```

The test suite can be built and run with:

```sh
make test
```

A compiler with support for the required Linux interfaces is required.

## Requirements

`cage` currently targets Linux.

It relies on Linux-specific functionality including:

* namespaces
* `clone`
* `pivot_root`
* mount namespaces
* OverlayFS
* Linux capabilities
* `prctl`
* procfs
* Linux device management

It is therefore not intended to be portable to non-Linux operating systems.

## Philosophy

Containers are not magic.

They are processes combined with a collection of kernel primitives:

```text
namespaces
    +
mounts
    +
OverlayFS
    +
pivot_root
    +
capabilities
    +
process lifecycle
    =
container
```

`cage` exists to keep that relationship visible.

The goal is not to compete with mature container engines.

The goal is to understand, implement, and provide a small executable embodiment of the primitives that make Linux containers possible.
