# cage — Implementation Milestones

This document tracks the implementation of `cage` from a minimal container launcher towards a small, hardened Linux container runtime.

The milestones are ordered so that each stage builds on the previous one.

## Milestone 1 — Basic Container

Build the smallest useful container around Linux namespaces.

* [x] Implement basic CLI
* [x] Accept an executable and arguments
* [x] Create a child process with `clone`
* [x] Create PID namespace
* [x] Create mount namespace
* [x] Create UTS namespace
* [x] Create IPC namespace
* [x] Create network namespace
* [x] Configure a private hostname
* [x] Establish basic mount isolation
* [x] Implement `execve` of the requested process
* [x] Add basic error handling
* [x] Add initial test runner
* [x] Add container probe executable

## Milestone 2 — Namespace Identity & Process Isolation

Make the container's process and namespace identity explicit and testable.

* [x] Verify container process is PID 1
* [x] Verify host and container have different PID namespaces
* [x] Verify hostname isolation
* [x] Verify mount namespace isolation
* [x] Verify IPC namespace isolation
* [x] Verify network namespace isolation
* [x] Verify namespace membership through `/proc`
* [x] Ensure child lifecycle is correctly supervised
* [x] Handle child exit status
* [x] Handle child termination by signal
* [x] Add namespace identity regression tests

## Milestone 3 — Filesystem Isolation

Replace the host filesystem view with an isolated container filesystem.

### Root filesystem

* [x] Require a supplied rootfs
* [x] Validate that the rootfs exists
* [x] Validate that the rootfs is a directory
* [x] Prepare a private runtime directory
* [x] Mount the supplied rootfs through OverlayFS
* [x] Configure OverlayFS lower, upper, and work directories
* [x] Perform `pivot_root`
* [x] Detach the old root
* [x] Remove the old root mount
* [x] Clean up temporary runtime directories

### `/proc`

* [x] Mount a private proc filesystem
* [x] Mount `/proc` after entering the container root
* [x] Ensure `/proc` reflects the container PID namespace

### `/dev`

* [x] Create a private `/dev`
* [x] Mount a private devtmpfs
* [x] Provide required basic devices
* [x] Mount `/dev/null`
* [x] Mount `/dev/zero`
* [x] Mount `/dev/random`
* [x] Mount `/dev/urandom`
* [x] Mount `/dev/tty`

### OverlayFS behaviour

* [ ] Test files created inside the container disappear after teardown
* [ ] Test modifications do not alter the supplied rootfs
* [ ] Test deletions remain confined to the container
* [ ] Verify the supplied rootfs is unchanged after normal execution
* [ ] Test OverlayFS cleanup after normal exit
* [ ] Test OverlayFS cleanup after forced termination
* [ ] Test cleanup when filesystem setup fails
* [ ] Audit OverlayFS and mount error paths

## Milestone 4 — Configuration & Mounts

Move runtime configuration out of positional CLI arguments and into a configuration file.

### Configuration

* [x] Define configuration structure
* [x] Implement configuration file loading
* [x] Implement default `cage.toml` discovery
* [x] Implement `--config <path>`
* [x] Validate required `rootfs`
* [x] Reject missing required fields
* [x] Reject duplicate fields
* [x] Reject unknown configuration settings
* [x] Reject invalid mount tables
* [x] Implement configuration cleanup
* [x] Add example configuration

### Configured mounts

* [x] Define `[[mounts]]`
* [x] Support `source`
* [x] Support `target`
* [x] Support `readonly`
* [x] Validate absolute mount targets
* [x] Create mount targets where required
* [x] Bind mount configured host paths
* [x] Support read-only bind mounts
* [x] Mount configured paths on top of the OverlayFS root

### Testing

* [x] Add configuration parser property tests
* [x] Test missing configuration files
* [x] Test malformed configuration
* [x] Test invalid rootfs configuration
* [x] Test invalid mount configuration
* [x] Test configured writable mounts
* [x] Test configured read-only mounts
* [x] Test configured mounts survive container teardown
* [x] Audit configured-mount failure cleanup

### Documentation

* [x] Update README for configuration-driven usage
* [x] Document rootfs configuration
* [x] Document configured bind mounts
* [x] Document read-only mounts
* [x] Document OverlayFS persistence semantics
* [x] Document host-backed mount persistence

## Milestone 5 — Reliability & Hardening

Make container setup and teardown robust against failures and hostile conditions.

### Capability and privilege reduction

* [x] Drop Linux capabilities
* [x] Drop capability bounding set
* [x] Set `no_new_privs`
* [x] Establish parent-death handling
* [x] Verify parent liveness across the `clone`/`prctl` race
* [x] Close inherited descriptors before `execve`

### Filesystem and runtime hardening

* [x] Add property-based security testing
* [x] Exercise filesystem path traversal and escape surfaces
* [x] Exercise procfs and sysfs access surfaces
* [x] Exercise namespace-related escape surfaces
* [x] Exercise mount and mount-namespace operations
* [x] Exercise device access surfaces
* [x] Exercise capability and privilege-related operations
* [x] Exercise process-control surfaces such as `ptrace` and `kill`
* [ ] Restrict remaining filesystem/device access further
* [ ] Review namespace-specific privilege boundaries
* [ ] Audit inherited process state
* [ ] Audit signal-handling edge cases
* [ ] Audit mount propagation behaviour
* [ ] Verify no host mounts are unintentionally exposed
* [x] Verify configured read-only mounts cannot be written from the container

### Failure handling

* [ ] Handle missing OverlayFS support
* [ ] Handle unusable rootfs permissions
* [x] Handle failure during mount setup
* [x] Handle failure during `pivot_root`
* [x] Handle failure during `/dev` setup
* [x] Handle failure during `/proc` setup
* [x] Handle failure during configured mount setup
* [x] Verify cleanup after every setup failure
* [x] Verify no runtime-directory leaks
* [x] Verify no mount leaks
* [x] Verify no child-process leaks

### Regression & property testing

* [x] Add configuration property tests
* [x] Add configuration formatting-invariance tests
* [x] Add configuration invalid-input tests
* [x] Add configuration whitespace tests
* [x] Add configuration literal-value tests
* [x] Add property-based escape testing
* [x] Add generated path traversal coverage
* [x] Add generated namespace and proc/sys path coverage
* [x] Add timeout handling for hostile or hanging security probes
* [x] Add configured mount failure cleanup regression test
* [ ] Add hardening regression tests where appropriate
* [ ] Add failure-path regression tests
* [ ] Add teardown regression tests
* [x] Verify repeated container creation and teardown
* [x] Verify abnormal child termination is cleaned up correctly

## Milestone 6 — Review & Release

Bring the implementation and documentation into a coherent first release.

### Implementation review

* [ ] Complete implementation review
* [ ] Remove unnecessary complexity
* [ ] Simplify duplicated logic
* [ ] Review resource ownership
* [ ] Review error propagation
* [ ] Review cleanup paths
* [ ] Review public headers
* [ ] Review compiler warnings
* [ ] Review static-analysis findings

### Testing

* [ ] Run the complete clean test suite
* [ ] Run tests from a clean build
* [ ] Verify tests do not depend on the developer's environment
* [ ] Verify temporary files and mounts are cleaned up
* [x] Verify configured mounts behave as documented
* [ ] Verify supplied rootfs remains unchanged
* [ ] Verify ordinary container changes are ephemeral

### Documentation

* [x] Keep README current with configuration usage
* [x] Document rootfs requirements
* [x] Document OverlayFS behaviour
* [x] Document configured mount behaviour
* [x] Document container lifecycle
* [x] Document security model and limitations
* [x] Document project structure
* [x] Document build and test commands
* [ ] Review documentation against final implementation

### Release

* [ ] Finalise commit history
* [ ] Prepare final pull request
* [ ] Review and merge Milestone 6
* [ ] Create first release tag
* [ ] Publish first release