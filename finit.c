/* Finit - Fast /sbin/init replacement w/ I/O, hook & service plugins
 *
 * Copyright (c) 2008-2010  Claudio Matsuoka <cmatsuoka@gmail.com>
 * Copyright (c) 2008-2015  Joachim Nilsson <troglobit@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "config.h"		/* Generated by configure script */

#include <ctype.h>
#include <glob.h>
#include <dirent.h>
#include <fstab.h>
#include <mntent.h>
#include <sys/mount.h>
#include <sys/stat.h>		/* umask(), mkdir() */
#include <lite/lite.h>

#include "finit.h"
#include "conf.h"
#include "helpers.h"
#include "private.h"
#include "plugin.h"
#include "service.h"
#include "sig.h"
#include "tty.h"
#include "sm.h"

int   debug     = 0;
int   quiet     = QUIET_MODE;	/* Delayed disable of silent mode. */
int   silent    = SILENT_MODE;	/* Completely silent, including boot */
int   runlevel  = 0;		/* Bootstrap 'S' */
int   cfglevel  = RUNLEVEL;	/* Fallback if no configured runlevel */
int   prevlevel = -1;
char *sdown     = NULL;
char *network   = NULL;
char *username  = NULL;
char *hostname  = NULL;
char *rcsd      = FINIT_RCSD;
char *runparts  = NULL;
char *console   = NULL;

uev_ctx_t *ctx  = NULL;		/* Main loop context */

static int banner(void)
{
	char buf[42] = INIT_HEADING;
	const char separator[] = "========================================================================";

	if (silent)
		return 0;

	fprintf(stderr, "\e[2K\e[1m%s %.*s\e[0m\n", buf, 66 - (int)strlen(buf), separator);

	return 0;
}

/*
 * Check all filesystems in /etc/fstab with a fs_passno > 0
 */
static int fsck(int pass)
{
	struct fstab *fs;

	if (!setfsent()) {
		_pe("Failed opening fstab");
		return 1;
	}

	while ((fs = getfsent())) {
		char cmd[80];
		struct stat st;

		if (fs->fs_passno < pass)
			continue;

		errno = 0;
		if (stat(fs->fs_spec, &st) || !S_ISBLK(st.st_mode)) {
			_d("Cannot fsck %s, not a block device: %s", fs->fs_spec, strerror(errno));
			continue;
		}

		snprintf(cmd, sizeof(cmd), "/sbin/fsck -C -a %s", fs->fs_spec);
		run_interactive(cmd, "Checking file system %s", fs->fs_spec);
	}

	endfsent();

	return 0;
}

static void networking(void)
{
	size_t i;
	glob_t gl;
	FILE *fp;
	char buf[160];

	/* Setup kernel specific settings, e.g. allow broadcast ping, etc. */
	glob("/run/sysctl.d/*.conf",           0, NULL, &gl);
	glob("/etc/sysctl.d/*.conf",           GLOB_APPEND, NULL, &gl);
	glob("/usr/local/lib/sysctl.d/*.conf", GLOB_APPEND, NULL, &gl);
	glob("/usr/lib/sysctl.d/*.conf",       GLOB_APPEND, NULL, &gl);
	glob("/lib/sysctl.d/*.conf",           GLOB_APPEND, NULL, &gl);
	glob("/mnt/sysctl.d/*.conf",           GLOB_APPEND, NULL, &gl);
	glob("/etc/sysctl.conf",               GLOB_APPEND, NULL, &gl);
	if (gl.gl_pathc > 0) {
		for (i = 0; i < gl.gl_pathc; i++) {
			snprintf(buf, sizeof(buf), "/sbin/sysctl -e -p %s >/dev/null", gl.gl_pathv[i]);
			run(buf);
		}
		globfree(&gl);
	}

	/* Run user network start script if enabled */
	if (network) {
		run_interactive(network, "Starting networking: %s", network);
		return;
	}

	/* Debian/Ubuntu/Busybox interfaces file */
	fp = fopen("/etc/network/interfaces", "r");
	if (fp) {
		i = 0;

		/* Bring up all 'auto' interfaces */
		while (fgets(buf, sizeof(buf), fp)) {
			char cmd[80];
			char *line, *ifname = NULL;

			chomp(buf);
			line = strip_line(buf);

			if (!strncmp(line, "auto", 4))
				ifname = &line[5];
 			if (!strncmp(line, "allow-hotplug", 13))
				ifname = &line[14];

			if (!ifname)
				continue;

			snprintf(cmd, 80, "/sbin/ifup %s", ifname);
			run_interactive(cmd, "Bringing up interface %s", ifname);
			i++;
		}

		fclose(fp);
		if (i)
			return;
	}

	/* Fall back to bring up at least loopback */
	ifconfig("lo", "127.0.0.1", "255.0.0.0", 1);
}

/* Requires /proc to be mounted */
static int fismnt(char *dir)
{
	FILE *fp;
	int found = 0;
	struct mntent *mnt;

	fp = setmntent("/proc/mounts", "r");
	if (!fp)
		return 0;	/* Dunno, maybe not */

	while ((mnt = getmntent(fp))) {
		if (!strcmp(mnt->mnt_dir, dir)) {
			found = 1;
			break;
		}
	}
	endmntent(fp);

	return found;
}

int main(int argc, char* argv[])
{
	int err;
	uev_ctx_t loop;

	/*
	 * finit/init/telinit client tool uses /dev/initctl pipe
	 * for compatibility but initctl client tool uses socket
	 */
	if (getpid() != 1)
		return client(argc, argv);

	/*
	 * Hello world.
	 */
	banner();

	/*
	 * Initial setup of signals, ignore all until we're up.
	 */
	sig_init();

	/*
	 * Initalize event context.
	 */
	uev_init(&loop);
	ctx = &loop;

	/*
	 * Mount base file system, kernel is assumed to run devtmpfs for /dev
	 */
	chdir("/");
	umask(0);
	mount("none", "/proc", "proc", 0, NULL);
	mount("none", "/proc/bus/usb", "usbfs", 0, NULL);
	mount("none", "/sys", "sysfs", 0, NULL);

#ifndef EMBEDDED_SYSTEM
	if (!fismnt("/dev"))
		mount("udev", "/dev", "devtmpfs", MS_RELATIME, "size=10%,nr_inodes=61156,mode=755");
	else
		run_interactive("/sbin/udevadm info --cleanup-db", "Cleaning up udev db");
#endif

	/* Some systems use /dev/pts */
	makedir("/dev/pts", 0755);
	mount("devpts", "/dev/pts", "devpts", 0, "gid=5,mode=620");

	makedir("/dev/shm", 0755);
	if (!fismnt("/dev/shm"))
		mount("shm", "/dev/shm", "tmpfs", 0, NULL);

	/*
	 * New tmpfs based /run for volatile runtime data
	 * For details, see http://lwn.net/Articles/436012/
	 */
	if (fisdir("/run") && !fismnt("/run"))
		mount("tmpfs", "/run", "tmpfs", MS_NODEV, "mode=0755,size=10%");
	umask(022);

	/*
	 * Parse kernel parameters
	 */
	conf_parse_cmdline();

	/*
	 * Populate /dev and prepare for runtime events from kernel.
	 */
#ifdef EMBEDDED_SYSTEM
	if (debug)
		touch("/dev/mdev.log");
#endif
	run_interactive(SETUP_DEVFS, "Populating device tree");

	/*
	 * Check file filesystems in /etc/fstab
	 */
	for (int pass = 1; pass < 10; pass++) {
		if (fsck(pass))
			break;
	}

	/*
	 * Load plugins first, finit.conf may contain references to
	 * features implemented by plugins.
	 */
	plugin_init(&loop, PLUGIN_PATH);

	/*
	 * Parse /etc/finit.conf, main configuration file
	 */
	conf_parse_config();

	/* Set hostname as soon as possible, for syslog et al. */
	set_hostname(&hostname);

	/* Set default PATH, for uid 0 */
	setenv("PATH", _PATH_STDPATH, 1);

	/*
	 * Mount filesystems
	 */
#ifdef REMOUNT_ROOTFS
	run("/bin/mount -n -o remount,rw /");
#endif
#ifdef SYSROOT
	mount(SYSROOT, "/", NULL, MS_MOVE, NULL);
#endif

#ifndef EMBEDDED_SYSTEM
	run_interactive("/lib/udev/udev-finish", "Finalizing udev");
#endif

	_d("Root FS up, calling hooks ...");
	plugin_run_hooks(HOOK_ROOTFS_UP);

	umask(0);
	print_desc("Mounting filesystems", NULL);

	err = run("/bin/mount -na");
	print_result(err);
	if (err)
		plugin_run_hooks(HOOK_MOUNT_ERROR);

	run("/sbin/swapon -ea");
	umask(0022);

	/* Cleanup of stale files, if any still linger on. */
	run_interactive("rm -rf /tmp/* /var/run/* /var/lock/*", "Cleaning up temporary directories");

	/* Base FS up, enable standard SysV init signals */
	sig_setup(&loop);

	_d("Base FS up, calling hooks ...");
	plugin_run_hooks(HOOK_BASEFS_UP);

	/*
	 * Initalize finit state machine and start all bootstrap tasks, no network available!
	 */
	sm_init(&sm);
	sm_step(&sm);

	/*
	 * Network stuff
	 */
	networking();
	umask(022);

	/* Hooks that rely on loopback, or basic networking being up. */
	plugin_run_hooks(HOOK_NETWORK_UP);

	/*
	 * Start all tasks/services in the configured runlevel
	 */
	service_runlevel(cfglevel);

	_d("Running svc up hooks ...");
	plugin_run_hooks(HOOK_SVC_UP);

	/*
	 * Run startup scripts in the runparts directory, if any.
	 */
	if (runparts && fisdir(runparts)) {
		_d("Running startup scripts in %s ...", runparts);
		run_parts(runparts, NULL);
	}

	/* Convenient SysV compat for when you just don't care ... */
	if (!access(FINIT_RC_LOCAL, X_OK))
		run_interactive(FINIT_RC_LOCAL, "Calling %s", FINIT_RC_LOCAL);

	/* Hooks that should run at the very end */
	plugin_run_hooks(HOOK_SYSTEM_UP);

	/* Start TTYs */
	tty_runlevel(runlevel);

	/* Enable silent mode, if selected */
	if (quiet && !debug)
		silent = 1;

	/* Start new initctl API responder */
	api_init(&loop);

	/*
	 * Enter main loop to monior /dev/initctl and services
	 */
	return uev_run(&loop, 0);
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
