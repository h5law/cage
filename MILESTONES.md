# cage - Milestones

## Milestone 1 — Basic CLI & Container

- [x] Create basic `cage <rootfs> <command> [args...]` CLI
- [x] Validate that the supplied rootfs exists and is a directory
- [x] Create initial container process
- [x] Execute a command inside the container
- [x] Propagate the command's exit status
- [x] Add basic test runner and container probe
- [x] Add invalid-rootfs testing

## Milestone 2 — Namespace Lifecycle

- [x] Create container with `clone()`
- [x] Create user namespace
- [x] Create PID namespace
- [x] Create mount namespace
- [x] Create network namespace
- [x] Create IPC namespace
- [x] Establish UID mapping
- [x] Establish GID mapping
- [x] Synchronise parent and child during namespace setup
- [x] Make container mounts private
- [x] `chroot()` into the supplied rootfs
- [x] Set up `/proc`
- [x] Set up private `/tmp`
- [x] Set up private `/dev`
- [x] Forward relevant signals
- [x] Supervise the workload
- [x] Reap the container process
- [x] Terminate container descendants during cleanup
- [x] Handle parent death
- [x] Test PID namespace isolation
- [x] Test user namespace isolation
- [x] Test mount namespace isolation
- [x] Test network namespace isolation
- [x] Test IPC namespace isolation
- [x] Test `/proc`
- [x] Test `/tmp`
- [x] Test `/dev`
- [x] Test descendant cleanup
- [x] Test forced/uncooperative descendant cleanup

## Milestone 3 — OverlayFS & Filesystem Isolation

- [x] Create private runtime directory
- [x] Exclusively lock the runtime directory
- [x] Create OverlayFS upper directory
- [x] Create OverlayFS work directory
- [x] Create OverlayFS root directory
- [x] Mount supplied rootfs as OverlayFS lower layer
- [x] Mount OverlayFS as the container root
- [x] Create `oldroot` mountpoint
- [x] `pivot_root()` into the OverlayFS root
- [x] Detach the original root
- [x] Keep the OverlayFS work directory available for cleanup
- [x] Mount private `/tmp`
- [x] Construct private `/dev`
- [x] Mount container `/proc`
- [x] Clean up device mounts
- [x] Clean up `/tmp`
- [x] Clean up `/dev`
- [x] Clean up `/proc`
- [x] Clean up the detached root
- [x] Clean up OverlayFS runtime directories
- [x] Remove the private runtime directory
- [x] Drop container capabilities
- [x] Set `no_new_privs`
- [x] Close inherited file descriptors before executing the workload
- [x] Add regression test for FD inheritance
- [ ] Test that files created inside the container disappear after teardown
- [ ] Test that modifications to existing files do not modify the supplied rootfs
- [ ] Test that deleted lower-layer files remain deleted only inside the container
- [ ] Test that the supplied rootfs is unchanged after container exit
- [ ] Test OverlayFS cleanup after normal command exit
- [ ] Test OverlayFS cleanup after forced termination
- [ ] Test cleanup when container setup fails part-way through
- [ ] Audit all OverlayFS and mount error paths

## Milestone 4 — Container Hardening

- [x] Drop Linux capabilities
- [x] Drop capability bounding set
- [x] Set `no_new_privs`
- [x] Establish parent-death handling
- [x] Verify parent liveness across the clone/`prctl()` race
- [x] Close inherited descriptors before `execve()`
- [ ] Restrict remaining filesystem/device access further
- [ ] Review namespace-specific privilege boundaries
- [ ] Audit inherited process state
- [ ] Audit signal-handling edge cases
- [ ] Add hardening regression tests where appropriate

## Milestone 5 — Reliability & Edge Cases

- [x] Test normal command exit
- [x] Test non-zero command exit
- [x] Test command termination by signal
- [x] Test descendant processes
- [x] Test uncooperative descendants
- [x] Test parent death
- [x] Test command `execve()` failure
- [x] Test invalid rootfs
- [ ] Test missing OverlayFS support
- [ ] Test unusable rootfs permissions
- [ ] Test failure during mount setup
- [ ] Test failure during `pivot_root()`
- [ ] Test failure during `/dev` setup
- [ ] Test failure during `/proc` setup
- [ ] Verify cleanup after every setup failure
- [ ] Verify no runtime-directory leaks
- [ ] Verify no mount leaks
- [ ] Verify no child-process leaks

## Milestone 6 — Documentation & Release

- [ ] Complete the implementation review
- [ ] Clean up and simplify the code where appropriate
- [ ] Run the complete test suite from a clean build
- [ ] Update the README to reflect the current implementation
- [ ] Document rootfs requirements
- [ ] Document OverlayFS behaviour
- [ ] Document container lifecycle
- [ ] Document security model and limitations
- [ ] Review project structure and build instructions
- [ ] Finalize commit history
- [ ] Open the final pull request
- [ ] Tag the first release
