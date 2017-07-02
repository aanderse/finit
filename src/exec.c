/* Functions for exec'ing processes
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

#include <ctype.h>		/* isdigit() */
#include <dirent.h>
#include <stdarg.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <lite/lite.h>

#include "finit.h"
#include "helpers.h"
#include "sig.h"
#include "utmp-api.h"

#define NUM_ARGS    16

int getty(char *tty, char *baud, char *term, char *user);


/* Wait for process completion, returns status of waitpid(2) syscall */
int complete(char *cmd, int pid)
{
	int status = 0;

	if (waitpid(pid, &status, 0) == -1) {
		if (errno == EINTR)
			_e("Caught unblocked signal waiting for %s, aborting", cmd);
		else if (errno == ECHILD)
			_e("Caught SIGCHLD waiting for %s, aborting", cmd);
		else
			_e("Failed starting %s, error %d: %s", cmd, errno, strerror (errno));

		return -1;
	}

	return status;
}

int run(char *cmd)
{
	int status, result, i = 0;
	char *args[NUM_ARGS + 1], *arg, *backup;
	pid_t pid;

	/* We must create a copy that is possible to modify. */
	backup = arg = strdup(cmd);
	if (!arg)
		return 1; /* Failed allocating a string to be modified. */

	/* Split command line into tokens of an argv[] array. */
	args[i++] = strsep(&arg, "\t ");
	while (arg && i < NUM_ARGS) {
		/* Handle run("su -c \"dbus-daemon --system\" messagebus");
		 *   => "su", "-c", "\"dbus-daemon --system\"", "messagebus" */
		if (*arg == '\'' || *arg == '"') {
			char *p, delim[2] = " ";

			delim[0]  = arg[0];
			args[i++] = arg++;
			strsep(&arg, delim);
			 p     = arg - 1;
			*p     = *delim;
			*arg++ = 0;
		} else {
			args[i++] = strsep(&arg, "\t ");
		}
	}
	args[i] = NULL;

	if (i == NUM_ARGS && arg) {
		_e("Command too long: %s", cmd);
		free(backup);
		errno = EOVERFLOW;
		return 1;
	}

	pid = fork();
	if (0 == pid) {
		FILE *fp;

		/* Reset signal handlers that were set by the parent process */
		sig_unblock();
		setsid();

		/* Always redirect stdio for run() */
		fp = fopen("/dev/null", "w");
		if (fp) {
			int fd = fileno(fp);

			dup2(fd, STDIN_FILENO);
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
		}

		sig_unblock();
		execvp(args[0], args);

		_exit(1); /* Only if execv() fails. */
	} else if (-1 == pid) {
		_pe("%s", args[0]);
		free(backup);

		return -1;
	}

	status = complete(args[0], pid);
	if (-1 == status) {
		free(backup);
		return 1;
	}

	result = WEXITSTATUS(status);
	if (WIFEXITED(status)) {
		_d("Started %s and ended OK: %d", args[0], result);
	} else if (WIFSIGNALED(status)) {
		_d("Process %s terminated by signal %d", args[0], WTERMSIG(status));
		if (!result)
			result = 1; /* Must alert callee that the command did complete successfully.
				     * This is necessary since not all programs trap signals and
				     * change their return code accordingly. --Jocke */
	}

	free(backup);

	return result;
}

int run_interactive(char *cmd, char *fmt, ...)
{
	int status, oldout = 1, olderr = 2;
	char line[LINE_SIZE];
	va_list ap;
	FILE *fp;

	if (!cmd) {
		errno = EINVAL;
		return 1;
	}

	if (fmt) {
		va_start(ap, fmt);
		vsnprintf(line, sizeof(line), fmt, ap);
		va_end(ap);

		print_desc("", line);
	}

	/* Redirect output from cmd to a tempfile */
	fp = tempfile();
	if (fp && !debug) {
		oldout = dup(STDOUT_FILENO);
		olderr = dup(STDERR_FILENO);
		dup2(fileno(fp), STDOUT_FILENO);
		dup2(fileno(fp), STDERR_FILENO);
	}

	/* Run cmd ... */
	status = run(cmd);

	/* Restore stderr/stdout */
	if (fp && !debug) {
		if (oldout >= 0) {
			dup2(oldout, STDOUT_FILENO);
			close(oldout);
		}
		if (olderr >= 0) {
			dup2(olderr, STDERR_FILENO);
			close(olderr);
		}
	}

	if (fmt)
		print_result(status);

	/* Dump any results of cmd on stderr after we've printed [ OK ] or [FAIL]  */
	if (fp && !debug) {
		size_t len, written;

		rewind(fp);
		do {
			len     = fread(line, 1, sizeof(line), fp);
			written = fwrite(line, len, sizeof(char), stderr);
		} while (len > 0 && written == len);
	}

	if (fp)
		fclose(fp);

	return status;
}

pid_t run_getty(char *tty, char *speed, char *term, int console)
{
	pid_t pid;

	pid = fork();
	if (!pid) {
		/* Reset signal handlers that were set by the parent process */
		sig_unblock();
		setsid();

		/* Set INIT_PROCESS UTMP entry */
		utmp_set_init(tty, 0);

		if (console)
			prctl(PR_SET_NAME, "console", 0, 0, 0);
		else
			prctl(PR_SET_NAME, "finit-getty", 0, 0, 0);

		_exit(getty(tty, speed, term, NULL));
	}

	return pid;
}


int run_parts(char *dir, char *cmd)
{
	struct dirent **e;
	int i, num;

	num = scandir(dir, &e, NULL, alphasort);
	if (num < 0) {
		_d("No files found in %s, skipping ...", dir);
		return -1;
	}

	for (i = 0; i < num; i++) {
		int j = 0;
		pid_t pid = 0;
		mode_t mode;
		char *args[NUM_ARGS];
		char *name = e[i]->d_name;
		char path[CMD_SIZE];

		snprintf(path, sizeof(path), "%s/%s", dir, name);
		mode = fmode(path);
		if (!S_ISEXEC(mode) || S_ISDIR(mode)) {
			_d("Skipping %s ...", path);
			continue;
		}

		/* Fill in args[], starting with full path to executable */
		args[j++] = path;

		/* If the callee didn't supply a run_parts() argument */
		if (!cmd) {
			/* Check if S<NUM>service or K<NUM>service notation is used */
			_d("Checking if %s is a sysvinit startstop script ...", name);
			if (name[0] == 'S' && isdigit(name[1])) {
				args[j++] = "start";
			} else if (name[0] == 'K' && isdigit(name[1])) {
				args[j++] = "stop";
			}
		} else {
			args[j++] = cmd;
		}
		args[j++] = NULL;

		pid = fork();
		if (!pid) {
			_d("Calling %s ...", path);
			sig_unblock();
			execv(path, args);
			exit(0);
		}

                complete(path, pid);
	}

	while (num--)
		free(e[num]);
	free(e);

	return 0;
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
