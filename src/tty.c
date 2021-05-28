/* Finit TTY handling
 *
 * Copyright (c) 2013       Mattias Walström <lazzer@gmail.com>
 * Copyright (c) 2013-2021  Joachim Wiberg <troglobit@gmail.com>
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

#include <ctype.h>		/* isdigit() */
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <lite/lite.h>

#include "config.h"		/* Generated by configure script */
#include "finit.h"
#include "conf.h"
#include "helpers.h"
#include "service.h"
#include "tty.h"
#include "util.h"
#include "utmp-api.h"

char *tty_canonicalize(char *dev)
{
	static char path[80];
	struct stat st;

	if (!dev)
		return NULL;
	if (tty_isatcon(dev))
		return dev;

	strlcpy(path, dev, sizeof(path));
	if (stat(path, &st)) {
		if (!strncmp(path, _PATH_DEV, strlen(_PATH_DEV))) {
		unavailable:
			_d("TTY %s not available at the moment, registering anyway.", path);
			return path;
		}

		snprintf(path, sizeof(path), "%s%s", _PATH_DEV, dev);
		if (stat(path, &st))
			goto unavailable;
	}

	if (!S_ISCHR(st.st_mode))
		return NULL;

	return path;
}

/*
 * The @console syntax is a wildcard to match the system console(s) given
 * on the kernel cmdline.  As such it can match multiple, or even none.
 */
int tty_isatcon(char *dev)
{
	return dev && !strcmp(dev, "@console");
}

/*
 * Returns console TTYs known by the kernel, from cmdline
 */
char *tty_atcon(void)
{
	static char *buf = NULL;
	static char *save;
	char *ptr = NULL;
	char *dev;

	if (!buf) {
		FILE *fp;

		fp = fopen("/sys/class/tty/console/active", "r");
		if (!fp) {
			_e("Cannot find system console, is sysfs not mounted?");
			errno = ENOENT;
			return NULL;
		}

		buf = malloc(512);
		if (!buf) {
			_pe("Failed allocating memory for @console");
			fclose(fp);
			return NULL;
		}

		if (!fgets(buf, 512, fp)) {
			fclose(fp);
			goto done;
		}
		fclose(fp);

		ptr = chomp(buf);
		_d("consoles: %s", ptr);
	}

	dev = strtok_r(ptr, " \t", &save);
	if (!dev) {
	done:
		free(buf);
		return buf = NULL;
	}

	return dev;
}

/**
 * tty_parse_args - Parse cmdline args for a tty
 * @cmd: command or tty
 *
 * A Finit tty line can use the internal getty implementation or an
 * external one, like the BusyBox getty for instance.  This function
 * determines which one to use based on a leading '/dev' prefix.  If
 * a leading '/dev' is encountered the remaining options must be in
 * the following sequence:
 *
 *     tty [!1-9,S] <DEV> [BAUD[,BAUD,...]] [noclear] [nowait] [TERM]
 *
 * Otherwise the leading prefix must be the full path to an existing
 * getty implementation, with it's arguments following:
 *
 *     tty [!1-9,S] </path/to/getty> [ARGS] [noclear] [nowait]
 *
 * Different getty implementations prefer the TTY device argument in
 * different order, so take care to investigate this first.
 */
int tty_parse_args(char *cmd, struct tty *tty)
{
	char  *dev = NULL;
	size_t i;

	do {
		if (!strcmp(cmd, "noclear"))
			tty->noclear = 1;
		else if (!strcmp(cmd, "nowait"))
			tty->nowait  = 1;
		else if (!strcmp(cmd, "nologin"))
			tty->nologin = 1;
		else if (!strcmp(cmd, "notty"))
			tty->notty = 1;		/* for board bringup */
		else if (!strcmp(cmd, "rescue"))
			tty->rescue = 1;	/* for rescue shells */
		else if (whichp(cmd))		/* in $PATH? */
			tty->cmd = cmd;
		else
			tty->args[tty->num++] = cmd;

		cmd = strtok(NULL, " \t");
	} while (cmd && tty->num < NELEMS(tty->args));

	/* rescue shells are always notty */
	if (tty->rescue)
		tty->notty = 1;

	/* skip /dev probe, we just want a bríngup shell */
	if (tty->notty)
		return 0;

	/* Iterate over all args */
	for (i = 0; i < tty->num; i++) {
		/* 
		 * First, figure out if built-in or external getty
		 * tty [12345] /dev/ttyAMA0 115200 noclear vt220		# built-in
		 * tty [12345] /sbin/getty -L 115200 @console vt100 noclear	# external
		 */
		if (!dev) {
			if (!strcmp(tty->args[i], "@console"))
				dev = tty->args[i];
			if (!strncmp(tty->args[i], "/dev", 4))
				dev = tty->args[i];
			if (!strncmp(tty->args[i], "tty", 3) || !strcmp(tty->args[i], "console"))
				dev = tty->args[i];

			/* The first arg must be one of the above */
			continue;
		}

		/* Built-in getty args */
		if (!tty->cmd && dev) {
			if (isdigit(tty->args[i][0])) {
				tty->baud = tty->args[i];
				continue;
			}

			/*
			 * Last arg, if not anything else, is the value
			 * to be used for the TERM environment variable.
			 */
			if (i + 1 == tty->num)
				tty->term = tty->args[i];
		}
	}

	tty->dev = tty_canonicalize(dev);
	if (!tty->dev) {
		_e("Incomplete or non-existing TTY device given, cannot register.");
		return errno = EINVAL;
	}

	_d("Registering %s getty on TTY %s at %s baud with term %s", tty->cmd ? "external" : "built-in",
	   tty->dev, tty->baud ?: "0", tty->term ?: "N/A");

	return 0;
}

int tty_exists(char *dev)
{
	struct termios c;
	int fd, rc;

	fd = open(dev, O_RDWR | O_NOCTTY);
	if (-1 == fd)
		return 0;

	/* XXX: Add check for errno == EIO? */
	rc = tcgetattr(fd, &c);
	close(fd);

	return rc == 0;
}

int tty_exec(svc_t *svc)
{
	char *args[MAX_NUM_SVC_ARGS];
	char *dev;
	int i, j;

	/* try to protect system with sulogin, fall back to root shell */
	if (svc->notty) {
		if (svc->rescue) {
			/* check if bundled one is available */
			if (whichp(_PATH_SULOGIN))
				return execl(_PATH_SULOGIN, _PATH_SULOGIN, NULL);

			/* util-linux or busybox, no args for compat */
			if (whichp("sulogin"))
				return execlp("sulogin", "sulogin", NULL);
		}

		/*
		 * Become session leader and set controlling TTY
		 * to enable Ctrl-C and job control in shell.
		 */
		setsid();
		ioctl(STDIN_FILENO, TIOCSCTTY, 1);

		prctl(PR_SET_NAME, "finitsh", 0, 0, 0);
		return execl(_PATH_BSHELL, "-sh", NULL);
	}

	dev = tty_canonicalize(svc->dev);
	if (!dev) {
		_d("%s: Cannot find TTY device: %s", svc->dev, strerror(errno));
		return EX_CONFIG;
	}

	if (!tty_exists(dev)) {
		_d("%s: Not a valid TTY: %s", dev, strerror(errno));
		return EX_OSFILE;
	}

	if (svc->nologin) {
		_d("%s: Starting /bin/sh ...", dev);
		return run_sh(dev, svc->noclear, svc->nowait, svc->rlimit);
	}

	_d("%s: Starting %sgetty ...", dev, !svc->cmd ? "built-in " : "");
	if (!strcmp(svc->cmd, "tty"))
		return run_getty(dev, svc->baud, svc->term, svc->noclear, svc->nowait, svc->rlimit);

	for (i = 1, j = 0; i < MAX_NUM_SVC_ARGS; i++) {
		if (!svc->args[i][0])
			break;

		args[j++] = svc->args[i];
	}
	args[j++] = NULL;

	return run_getty2(dev, svc->cmd, args, svc->noclear, svc->nowait, svc->rlimit);
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
