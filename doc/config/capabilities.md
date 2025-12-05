Linux Capabilities
==================

Finit supports Linux capabilities, allowing services to run with minimal
required privileges instead of running as root. This significantly improves
system security by following the principle of least privilege.

## Overview

Linux capabilities divide the traditional root privileges into distinct units
that can be independently granted to processes. For example, a web server only
needs the capability to bind to privileged ports (< 1024), not full root access.

Finit uses the modern IAB (Inheritable, Ambient, Bounding) API from libcap,
which is the same approach used by other modern service managers like dinit.

## Basic Usage

Capabilities are specified using the `caps:` directive in service configuration:

```conf
service [2345] name:nginx \
        @www-data:www-data \
        caps:^cap_net_bind_service \
        /usr/sbin/nginx -g 'daemon off;' \
        -- Web server
```

This example allows nginx to bind to privileged ports (like 80 and 443) while
running as the unprivileged `www-data` user.

## IAB Format

The capability string uses the IAB (Inheritable, Ambient, Bounding) format
with the following prefixes:

- `^` **Ambient** (and Inheritable) - **Recommended for most use cases**
  - Capabilities survive across `exec()` calls
  - Automatically raised to effective after exec
  - Example: `^cap_net_bind_service`

- `%` **Inheritable** only
  - Requires the executed binary to have matching file capabilities
  - Less common, more complex setup
  - Example: `%cap_net_admin`

- `!` **Bounding** - Block capability from bounding set
  - Prevents the service from ever acquiring this capability
  - Useful for security hardening
  - Example: `!cap_sys_admin`

Multiple capabilities can be specified as a comma-separated list:

```conf
caps:^cap_net_raw,^cap_net_admin,^cap_net_bind_service
```

## Common Use Cases

### Web Server (Privileged Ports)

Allow a web server to bind to ports 80 and 443 without running as root:

```conf
service [2345] name:webserver \
        @www-data:www-data \
        caps:^cap_net_bind_service \
        /usr/sbin/nginx -g 'daemon off;'
```

### Network Monitoring (Raw Sockets)

Allow packet capture without root privileges:

```conf
service [2345] name:tcpdump \
        @tcpdump \
        caps:^cap_net_raw,^cap_net_admin \
        /usr/sbin/tcpdump -i eth0 -w /var/log/capture.pcap
```

### NTP Daemon (System Time)

Allow time synchronization without full root:

```conf
service [2345] name:ntpd \
        @ntp \
        caps:^cap_sys_time,^cap_sys_nice \
        /usr/sbin/ntpd -n
```

## Available Capabilities

Common capabilities include (see `man 7 capabilities` for the complete list):

- `cap_chown` - Make arbitrary changes to file UIDs and GIDs
- `cap_dac_override` - Bypass file read, write, and execute permission checks
- `cap_dac_read_search` - Bypass file read permission checks
- `cap_fowner` - Bypass permission checks on operations that normally require filesystem UID
- `cap_kill` - Bypass permission checks for sending signals
- `cap_net_admin` - Perform various network-related operations
- `cap_net_bind_service` - Bind to privileged ports (< 1024)
- `cap_net_raw` - Use RAW and PACKET sockets
- `cap_setgid` - Make arbitrary manipulations of process GIDs
- `cap_setuid` - Make arbitrary manipulations of process UIDs
- `cap_sys_admin` - Perform system administration operations (very powerful!)
- `cap_sys_module` - Load and unload kernel modules
- `cap_sys_nice` - Raise process nice value and change scheduling
- `cap_sys_time` - Set system clock

## Security Best Practices

1. **Use the minimum required capabilities**
   - Only grant what the service actually needs
   - Don't grant `cap_sys_admin` unless absolutely necessary

2. **Always specify a user**
   - Always use `@user` to drop to a non-root user
   - Capabilities work best when combined with user separation

3. **Use ambient capabilities (`^`)**
   - The `^` prefix ensures capabilities survive exec()
   - Simpler than setting file capabilities on binaries

4. **Block dangerous capabilities**
   - Use `!` to explicitly block capabilities you don't want
   - Example: `!cap_sys_admin,!cap_sys_module`

5. **Test with `getpcaps`**
   - After starting a service, verify its capabilities:
     ```bash
     getpcaps $(pidof nginx)
     ```
   - Should show only the capabilities you granted

## Verification

After configuring a service with capabilities, verify it works correctly:

```bash
# Start the service
initctl start webserver

# Check the process capabilities
getpcaps $(pidof nginx)

# Should show something like:
# 12345: cap_net_bind_service=eip

# Verify the user
ps -o user,pid,cmd -p $(pidof nginx)

# Should show the service running as the specified user
```

## Requirements

- Linux kernel 4.3+ (for ambient capabilities support)
- libcap library installed
- Finit built with `--enable-libcap`

## Limitations

- Capabilities are only applied when both `@user` and `caps:` are specified
- The service must drop to a non-root user for capabilities to be effective
- Some very old binaries may not work correctly with ambient capabilities
- File system capabilities are not managed by Finit (use `setcap` for that)

## See Also

- `man 7 capabilities` - Linux capabilities overview
- `man 3 cap_iab` - IAB capability API documentation
- `man 8 setcap` - Set file capabilities
- `man 8 getcap` - Query file capabilities
- `man 1 capsh` - Capability shell wrapper
