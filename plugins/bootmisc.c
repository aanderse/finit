/* Setup necessary system files for, e.g. UTMP (tracking logins)
 *
 * Copyright (c) 2012-2022  Joachim Wiberg <troglobit@gmail.com>
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

#include <ftw.h>
#ifdef HAVE_MNTENT_H
#include <mntent.h>
#endif
#include <string.h>
#ifdef _LIBITE_LITE
# include <libite/lite.h>
#else
# include <lite/lite.h>
#endif

#include "config.h"
#include "finit.h"
#include "helpers.h"
#include "plugin.h"
#include "tmpfiles.h"
#include "util.h"
#include "utmp-api.h"

static int is_tmpfs(char *path)
{
	struct mntent mount;
	struct mntent *mnt;
	char buf[256];
	int tmpfs = 0;
	FILE *fp;
	char *dir;

	/* If path is a symlink, check what it resolves to */
	dir = realpath(path, NULL);
	if (!dir)
		return 0;	/* Outlook not so good */

	fp = setmntent("/proc/mounts", "r");
	if (!fp) {
		free(dir);
		return 0;	/* Dunno, maybe not */
	}

	while ((mnt = getmntent_r(fp, &mount, buf, sizeof(buf)))) {
		if (!strcmp(dir, mnt->mnt_dir))
			break;
	}

	if (mnt && !strcmp("tmpfs", mnt->mnt_type))
		tmpfs = 1;

	endmntent(fp);
	free(dir);

	return tmpfs;
}

static int bootclean(const char *fpath, const struct stat *sb, int tflag, struct FTW *ftw)
{
	if (ftw->level == 0)
		return 1;

	dbg("Removing %s ...", fpath);
	(void)remove(fpath);

	return 0;
}

/*
 * Cleanup stale files from previous boot, if any still linger on.
 * Some systems, e.g. Alpine Linux, still have a persistent /run and
 * /tmp, i.e. not tmpfs.
 *
 * We can safely skip tmpfs, nothing to clean there.
 */
static void clean(void *arg)
{
	char *dir[] = {
		"/tmp/",
		"/var/run/",
		"/var/lock/",
		NULL
	};

	for (int i = 0; dir[i]; i++) {
		if (is_tmpfs(dir[i]))
			continue;

		nftw(dir[i], bootclean, 20, FTW_DEPTH);
	}
}

/* Kernel defines the following compulsory and recommended links
 * https://github.com/torvalds/linux/blob/v5.18/Documentation/admin-guide/devices.rst#compulsory-links
 */
static void kernel_links(void)
{
	struct {
		char *tgt, *lnk;
		int  optional;
	} k[] = {
		{ "/proc/self/fd", "/dev/fd", 0 },
		{ "fd/0", "/dev/stdin",  0 },
		{ "fd/1", "/dev/stdout", 0 },
		{ "fd/2", "/dev/stderr", 0 },
		{ "socksys", "/dev/nfsd", 0 },
		{ "null", "/dev/X0R", 0 },
		{ "/proc/kcore", "/dev/core", 1 },
		{ "ram0", "/dev/ramdisk", 1 },
		{ "qft0", "/dev/ftape", 1 },
		{ "video0", "/dev/bttv0", 1 },
		{ "radio0", "/dev/radio", 1 },
	};
	char *fn, buf[80];
	size_t i;

	for (i = 0; i < NELEMS(k); i++) {
		fn = k[i].tgt;

		if (k[i].optional) {
			if (fn[0] != '/') {
				snprintf(buf, sizeof(buf), "/dev/%s", k[i].tgt);
				fn = buf;
			}
			if (!fexist(fn))
				continue;
		}
		ln(k[i].tgt, k[i].lnk);
	}
}

/*
 * Setup standard FHS 2.3 structure in /var, and write runlevel to UTMP
 */
static void setup(void *arg)
{
	mode_t prev;

	prev = umask(0);

	/* Kernel symlinks, e.g. /proc/self/fd -> /dev/fd */
	kernel_links();

	/* Create all system tmpfiles.d(5) */
	tmpfilesd();

	/* Set BOOT_TIME UTMP entry */
	utmp_set_boot();

	umask(prev);
}

static plugin_t plugin = {
	.name = __FILE__,
	.hook[HOOK_MOUNT_POST] = { .cb = clean },
	.hook[HOOK_BASEFS_UP]  = { .cb = setup },
	.depends = { "pidfile" },
};

PLUGIN_INIT(plugin_init)
{
	plugin_register(&plugin);
}

PLUGIN_EXIT(plugin_exit)
{
	plugin_unregister(&plugin);
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
