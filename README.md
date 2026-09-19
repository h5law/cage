# cage

A small Linux container runtime written in C.

`cage` uses Linux namespaces, OverlayFS, `pivot_root`, mounts, and Linux security primitives to provide a simple isolated environment for running a process.

It intentionally avoids images, registries, daemons, and orchestration.

> **Status:** early development. Not production hardened.

## Features

* PID, mount, UTS, IPC, and network namespaces
* Isolated `/proc` and `/dev`
* OverlayFS-backed writable root filesystem
* Configurable bind mounts
* Dropped Linux capabilities
* Restricted capability bounding set
* `no_new_privs`
* Parent-death handling
* Inherited file-descriptor cleanup
* Process-state sanitisation
* Container-wide descendant cleanup
* Configuration-driven runtime

## Usage

```text
cage [options] <executable> [arguments...]
```

Options:

```text
--config <path>  use a configuration file
-h, --help       show help
```

Example:

```sh
cage --config cage.toml /bin/sh
```

When `--config` is omitted, `cage` looks for `cage.toml` in the current directory.

## Configuration

A minimal configuration:

```toml
rootfs = "./rootfs"
```

Additional host paths can be exposed with bind mounts:

```toml
rootfs = "./rootfs"

[[mounts]]
source = "/tmp/cage-data"
target = "/data"

[[mounts]]
source = "/home/user/project"
target = "/workspace"
readonly = true
```

Supported settings are:

* `rootfs`
* `[[mounts]]`
* `source`
* `target`
* `readonly`

Unknown settings and duplicate fields are rejected.

The configuration parser implements only the TOML features required by `cage`; it is not a general-purpose TOML parser.

## Root filesystem

`cage` requires an existing directory as its root filesystem.

For example:

```text
rootfs/
├── bin/
├── etc/
├── lib/
├── lib64/
├── sbin/
├── usr/
└── var/
```

The root filesystem is used as the **lower layer** of an OverlayFS mount.

The writable upper and work directories are created in a temporary runtime directory.

Therefore, normal changes made inside the container do not modify the supplied rootfs.

For example:

```sh
echo changed > /etc/config
touch /tmp/example
rm /etc/config
```

These changes exist only in the temporary OverlayFS layer and disappear when the container is destroyed.

### Bind mounts

Configured bind mounts are different.

```toml
[[mounts]]
source = "/tmp/cage-data"
target = "/data"
```

`/data` is backed directly by `/tmp/cage-data` on the host.

Changes made through the mount therefore persist after the container exits.

Use:

```toml
readonly = true
```

when the container should be able to access a host path without modifying it.

| Location                    | Storage             | Persists          |
| --------------------------- | ------------------- | ----------------- |
| Normal container filesystem | Temporary OverlayFS | No                |
| Configured writable mount   | Host filesystem     | Yes               |
| Configured read-only mount  | Host filesystem     | Host changes only |

## Container lifecycle

The runtime follows this general sequence:

```text
load configuration
        │
        ▼
validate rootfs
        │
        ▼
create container
        │
        ▼
create namespaces
        │
        ▼
prepare OverlayFS
        │
        ▼
prepare /proc and /dev
        │
        ▼
pivot_root
        │
        ▼
configure bind mounts
        │
        ▼
drop privileges
        │
        ▼
sanitise process state
        │
        ▼
exec requested process
        │
        ▼
wait and reap descendants
        │
        ▼
cleanup mounts and runtime state
```

The host process supervises the container lifecycle.

The container's init process is PID 1 inside its PID namespace and is responsible for reaping descendants.

During teardown, remaining descendants are terminated and forcibly killed if they do not exit within the grace period.

## Isolation

### PID namespace

The container has its own PID namespace.

Its init process is PID 1 and processes inside the container have a separate PID view from the host.

### Mount namespace

The container has a private mount namespace.

Mount operations performed inside the container do not directly modify the host mount namespace.

### UTS namespace

The container has an independent hostname and domain-name namespace.

### IPC namespace

System V IPC and POSIX message queues are isolated from the host.

### Network namespace

The container has its own network namespace.

`cage` does not configure networking itself.

### `/proc`

A private proc filesystem is mounted inside the container and reflects the container's PID namespace.

### `/dev`

A private `/dev` is provided with the basic devices required by the container.

## Security

`cage` applies several privilege and state restrictions before executing the workload.

### Capabilities

Linux capabilities are dropped and the capability bounding set is restricted.

### `no_new_privs`

`PR_SET_NO_NEW_PRIVS` is enabled before execution, preventing the workload and its descendants from gaining additional privileges through mechanisms such as set-user-ID executables and file capabilities.

### Parent death

The container establishes parent-death handling so it cannot continue independently after its supervisor disappears.

The implementation also checks parent liveness to handle the race between child creation and parent-death configuration.

### File descriptors

Inherited file descriptors from the launching process are closed before execution.

Standard input, output, and error remain available.

### Process state

The workload does not inherit cage's supervisor state.

Before execution, `cage`:

* resets the inherited signal mask
* resets cage-installed signal handlers
* establishes a `022` umask
* changes the working directory to `/`
* clears the inherited environment
* establishes a minimal container environment

## Failure handling

Container creation consists of several operations that can fail.

If setup fails, `cage` cleans up the resources created for that container rather than leaving a partially configured environment running.

This includes:

* mounts
* OverlayFS state
* temporary runtime directories
* child processes

The test suite includes failure-path and teardown regression tests for these cases.

## Project structure

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
├── container_probe.c
└── test_runner.c
```

The implementation deliberately keeps the number of components small.

## Building

Build:

```sh
make
```

Run the test suite:

```sh
make test
```

`cage` currently targets Linux and relies on Linux-specific interfaces including:

* namespaces
* `clone`
* `pivot_root`
* mount namespaces
* OverlayFS
* Linux capabilities
* `prctl`
* procfs

## Development

Development is organised into implementation milestones.

The current work covers:

* basic container and namespace isolation
* filesystem isolation
* OverlayFS
* configuration and bind mounts
* privilege reduction
* lifecycle handling
* failure cleanup
* security/property testing
* inherited process-state sanitisation

The next milestone focuses on resource limits, lifecycle races, filesystem hardening, and stress testing.

See [`MILESTONES.md`](MILESTONES.md) for the current roadmap.

## What cage is not

`cage` does not provide:

* container images
* registries
* image distribution
* orchestration
* networking management
* persistent container storage
* a container daemon
* virtual machines

A root filesystem is simply a directory supplied to the runtime.

Creating, downloading, versioning, and distributing root filesystems are outside the scope of `cage`.

## Design

The core idea is deliberately simple:

```text
namespaces
    +
mounts
    +
OverlayFS
    +
pivot_root
    +
privilege reduction
    +
process lifecycle
    =
container
```

`cage` exists to make these Linux primitives explicit.

The goal is not to replace mature container engines. The goal is to provide a small, understandable implementation of the mechanisms that make Linux containers possible.
