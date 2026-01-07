Finit provides three different cgroup directives for controlling resource allocation:

 1. **Top-level cgroup definition**: `cgroup NAME settings`
    - Defines a top-level cgroup (e.g., `init`, `system`, `user`) with default settings
    - Space-separated syntax
    - Example: `cgroup system cpu.weight:9700`

 2. **Global cgroup selector**: `cgroup.NAME[,options]` (standalone directive)
    - Sets the default cgroup for subsequent services in a `.conf` file
    - Dot-separated with optional comma-separated options
    - Example: `cgroup.maint` or `cgroup.system,delegate`

 3. **Per-service cgroup option**: `cgroup.NAME[,options]` or `cgroup:options`
    - Overrides the cgroup for a specific service
    - Part of the service directive line
    - Example: `service [...] cgroup.maint,mem.max:1G /path/to/cmd`

> [!NOTE]
> Linux cgroups and details surrounding values are not explained in the
> Finit documentation.  The Linux admin-guide covers this well:
> <https://www.kernel.org/doc/html/latest/admin-guide/cgroup-v2.html>

Top-level Cgroup Definition
----------------------------

**Syntax:** `cgroup NAME settings`

    # Top-level cgroups and their default settings.  All groups mandatory
    # but more can be added, max 8 groups in total currently.  The cgroup
    # 'root' is also available, reserved for RT processes.  Settings are
    # as-is, only one shorthand 'mem.' exists, other than that it's the
    # cgroup v2 controller default names.
    cgroup init   cpu.weight:100
    cgroup user   cpu.weight:100
    cgroup system cpu.weight:9800

Adding an extra cgroup `maint/` will require you to adjust the weight of
the above three.  We leave `init/` and `user/` as-is reducing weight of
`system/` to 9700.

    cgroup system cpu.weight:9700

    # Example extra cgroup 'maint'
    cgroup maint  cpu.weight:100

By default, the `system/` cgroup is selected for almost everything.  The
`init/` cgroup is reserved for PID 1 itself and its closest relatives.
The `user/` cgroup is for local TTY logins spawned by getty.

Global Cgroup Selector
----------------------

**Syntax:** `cgroup.NAME[,options]` (standalone directive)

To select a different top-level cgroup, e.g. `maint/`, for a group of
run/task/service directives in a `.conf` file, use the `cgroup.NAME`
directive as a standalone line:

    cgroup.maint
    service [...] <...> /path/to/foo args -- description
    service [...] <...> /path/to/bar args -- description

Both services will run in the `maint/` cgroup.

You can also include options with the global selector:

    cgroup.system,delegate
    service [...] <...> /path/to/foo args -- description

Per-Service Cgroup Option
--------------------------

**Syntax:** `cgroup.NAME[,options]` or `cgroup:options` (service option)

To override the cgroup for a specific service, use the `cgroup.NAME`
option within the service directive:

    service [...] <...> cgroup.maint /path/to/foo args -- description

This form also allows per-service limits. Two syntaxes are supported:

**New comma-separated syntax (recommended):**

    service [...] <...> cgroup.maint,cpu.max:10000,mem.max:655360 /path/to/foo args -- description

**Old colon-separated syntax (legacy):**

    service [...] <...> cgroup.maint:cpu.max:10000,mem.max:655360 /path/to/foo args -- description

You can also apply options to the current default cgroup (without changing it)
using the `cgroup:options` syntax:

    service [...] <...> cgroup:cpu.max:10000,mem.max:655360 /path/to/foo args -- description

Both syntaxes work identically. The new comma-separated syntax is recommended
as it's more consistent with other option parsing in Finit.

Note the `mem.` exception to the rule: every cgroup setting maps directly to
cgroup v2 syntax. I.e., `cpu.max` maps to the file `/sys/fs/cgroup/maint/foo/cpu.max`.
There is no filtering, except for expanding the shorthand `mem.` to `memory.`.
If the file is not available, either the cgroup controller is not available
in your Linux kernel, or the name is misspelled.

### Overriding Cgroup Leaf Names

By default, the cgroup leaf directory name is derived from the service
configuration filename (without the `.conf` extension). For example, a
service defined in `system/10-hotplug.conf` would create a cgroup at
`/sys/fs/cgroup/system/10-hotplug/` by default.

To use a more descriptive name (recommended for clarity), you can specify
`name:` in the cgroup directive:

    service [...] <...> cgroup.system,name:udevd /lib/systemd/systemd-udevd -- Device event daemon

This creates the cgroup at `/sys/fs/cgroup/system/udevd/` instead.

The syntax supports combining the name override with other options:

    service [...] <...> cgroup.system,name:udevd,cpu.max:10000 /lib/systemd/systemd-udevd -- Device event daemon

Or with delegation:

    service [2345] @podman:podman \
        cgroup.containers,name:podman,delegate,mem.max:4G \
        /usr/bin/podman system service -- Podman API

A daemon using `SCHED_RR` currently needs to run outside the default cgroups.

    service [...] <...> cgroup.root /path/to/daemon arg -- Real-Time process

Cgroup Delegation
-----------------

For services that need to create their own child cgroups (container runtimes
like Docker, Podman, systemd-nspawn, LXC), use the `delegate` option:

    service [2345] @dockerd:dockerd \
        cgroup.system,delegate /usr/bin/dockerd -- Docker daemon

Or with the old colon syntax:

    service [2345] @dockerd:dockerd \
        cgroup.system:delegate /usr/bin/dockerd -- Docker daemon

This allows the container runtime to:

- Create child cgroups for containers
- Manage controller settings for containers
- Move processes between cgroups

When delegation is enabled, Finit:

1. Creates the service cgroup as a **domain group** (not a leaf)
2. Enables all available controllers in `cgroup.subtree_control`
3. Changes ownership of delegation files to the service user
4. Moves the service process to the cgroup root
5. Lets the container runtime manage its own subdirectories

**Requirements:**

- The service should specify `@user:group` for proper ownership
- Controllers are delegated from the parent cgroup

**Example with additional config (new syntax):**

    service [2345] @podman:podman \
        cgroup.containers,delegate,mem.max:4G \
        /usr/bin/podman system service -- Podman API

**Or with old syntax:**

    service [2345] @podman:podman \
        cgroup.containers:delegate,mem.max:4G \
        /usr/bin/podman system service -- Podman API

Both examples delegate the cgroup while also setting a 4GB memory limit.

**Container template example:**

Here's a real-world example from [Infix OS](https://github.com/kernelkit/infix)
for running rootful podman container instances using delegation:

    sysv log:prio:local1,tag:%i kill:30 pid:!/run/container:%i.pid \
        pre:0,/usr/sbin/container cleanup:0,/usr/sbin/container \
        cgroup.system,delegate                                   \
        [2345] <!> :%i container -n %i -- container %i

This template uses `sysv` type with delegation, demonstrating that cgroup
delegation works with different service types, not just `service`.

**Cgroup structure with delegation:**

Initially, the service process runs directly in the cgroup root:

    /sys/fs/cgroup/system/container@web/
    ├── cgroup.procs            (service PID - owned by service user)
    ├── cgroup.subtree_control  (+cpu +memory +io - owned by service user)
    └── (container children will be created here)

Once the container runtime creates child cgroups (e.g., `libpod-*/`), cgroups v2
enforces the "no internal processes" rule. When Finit detects this (`EBUSY` error),
it automatically creates an `supervisor/` subdirectory and moves service-related
processes there:

    /sys/fs/cgroup/system/container@web/
    ├── cgroup.procs            (empty)
    ├── cgroup.subtree_control  (+cpu +memory +io)
    ├── supervisor/             (service processes)
    │   └── cgroup.procs        (conmon PIDs, etc.)
    └── libpod-$HASH/           (container processes)
        └── cgroup.procs        (container PIDs)

This happens automatically - no configuration needed. Without delegation, the
cgroup would be a leaf and the container runtime could not create child cgroups.
