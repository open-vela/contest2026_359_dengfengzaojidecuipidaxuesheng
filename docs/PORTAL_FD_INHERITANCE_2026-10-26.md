# Portal descriptor inheritance

The deployed candidate-3 reported `listener=-1`, `running=1` and repeated
`EADDRINUSE` after HTTP activity through the recovery AP. This audit did not use
the serial port or change the deployed or frozen candidates.

## Confirmed mechanism

The actual candidate-3 NuttX configuration has `FDCLONE_DISABLE=n` and
`FDCLONE_STDIO=n`. Its `task_create()` calls `nxthread_create()`, then
`nxtask_init()` / `task_setup()`, which calls
`group_setuptaskfiles(tcb, actions, true)`. `fdlist_copy()` duplicates every
parent descriptor except those marked close-on-exec. Pthreads share their
existing task group; the DHCP server is a separate task with a copied table.

The recovery worker starts the portal before `dhcpd_start()` creates that task.
Previously, both `socket(AF_INET, SOCK_STREAM, 0)` and plain `accept()` produced
portal descriptors without close-on-exec. The DHCP task therefore retained the
listener and any clients open at task creation. Closing the portal group's copy
did not release the listener's binding or the accepted client's connection.
`SO_REUSEADDR` does not let a replacement listener take a port still owned by
that live listening socket.

The production change sets `SOCK_CLOEXEC` atomically in `socket()` and
`accept4()`. The exact target's `fs/socket/socket.c` and `fs/socket/accept.c`
translate this flag into `O_CLOEXEC` before descriptor allocation. No global
descriptor-cloning policy, DHCP task ownership or Bluetooth ownership changes.
There is no open-to-fcntl inheritance race.

## Executed checks

`tools/test_portal_fd_inheritance.py` runs the actual portal worker on native
TCP sockets and executes the exact `fdlist_copy()` function extracted from the
supplied NuttX tree. Native `dup()` / `close()` adapt the file-table primitives;
the kernel cloning loop is unchanged.

- The fixed source permits immediate listener rebind after the parent closes
  its descriptor, and closing an accepted client produces peer EOF.
- Removing only listener close-on-exec reproduces bind error 98 while the child
  retains its copy. This is an expected failing control.
- Removing only client close-on-exec prevents peer EOF. This is a separate
  expected failing control.
- An unrelated descriptor without close-on-exec is still cloned, while an
  unrelated close-on-exec descriptor is skipped.
- AddressSanitizer and UndefinedBehaviorSanitizer checks pass for the fixed
  case; the complete portal translation unit compiles with P4 GCC 14 and the
  actual target configuration.
- The existing native portal HTTP regression passes all 45 routes.

The runner records source hashes and rejects a source modification during its
run. Its latest artifacts are under
`/home/flash/vela-p4/portal-fd-inheritance-20261026/`; the existing route check is
under `/home/flash/vela-p4/portal-cloexec-routes-20261026/`.

## Limits

This establishes the retained-binding defect and its host reproduction. It
does not establish which initial target poll/accept event caused the old worker
to replace the listener, nor prove recovery on a newly flashed board. Root's
separate listener-error handling and target diagnostics address that trigger.
The currently running candidate retains descriptors created before this fix;
changing source does not release those live references.
