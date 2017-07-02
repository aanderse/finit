/* Finit service monitor, task starter and generic API for managing svc_t
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

#include <ctype.h>		/* isblank() */
#include <string.h>
#include <sys/wait.h>
#include <net/if.h>
#include <lite/lite.h>

#include "conf.h"
#include "cond.h"
#include "finit.h"
#include "helpers.h"
#include "inetd.h"
#include "private.h"
#include "sig.h"
#include "service.h"
#include "sm.h"
#include "tty.h"
#include "utmp-api.h"

#define RESPAWN_MAX    10	/* Prevent endless respawn of faulty services. */


/**
 * service_enabled - Should the service run?
 * @svc: Pointer to &svc_t object
 *
 * Returns:
 * 1, if the service is allowed to run in the current runlevel and the
 * user has not manually requested that this service should not run. 0
 * otherwise.
 */
int service_enabled(svc_t *svc)
{
	if (!svc)
		return 0;

	if (!svc_in_runlevel(svc, runlevel))
		return 0;

	if (svc_is_removed(svc) || svc_is_blocked(svc))
		return 0;

	return 1;
}

/**
 * service_timeout_cb - libuev callback wrapper for service timeouts
 * @w:      Watcher
 * @arg:    Callback argument, from init
 * @events: Error, or ready to read/write (N/A for relative timers)
 *
 * Calls the callback registered with the call to
 * service_timeout_after().
 */
static void service_timeout_cb(uev_t *UNUSED(w), void *arg, int UNUSED(events))
{
	svc_t *svc = arg;

	svc->timer_cb(svc);
}

/**
 * service_timeout_after - Call a function after some time has elapsed
 * @param svc      Service to use as argument to the callback
 * @param timeout  Timeout, in milliseconds
 * @param cb       Callback function
 *
 * After @param timeout milliseconds has elapsed, calls @param cb with
 * @param svc as the argument.
 *
 * @return 0 on success, non-zero on error.
 */
static int service_timeout_after(svc_t *svc, int timeout, void (*cb)(svc_t *svc))
{
	if (svc->timer_cb)
		return -EBUSY;

	svc->timer_cb = cb;
	return uev_timer_init(ctx, &svc->timer, service_timeout_cb, svc, timeout, 0);
}

/**
 * service_timeout_cancel - Cancel timeout associated with service
 * @param svc      Service whose timeout to cancel
 *
 * If a timeout is associated with @param svc, cancel it.
 *
 * @return 0 on success, non-zero on error.
 */
static int service_timeout_cancel(svc_t *svc)
{
	int err;

	if (!svc->timer_cb)
		return 0;

	err = uev_timer_stop(&svc->timer);
	svc->timer_cb = NULL;

	return err;
}

static int is_norespawn(void)
{
	return  sig_stopped()            ||
		fexist("/mnt/norespawn") ||
		fexist("/tmp/norespawn");
}

/**
 * service_start - Start service
 * @svc: Service to start
 *
 * Returns:
 * 0 if the service was successfully started. Non-zero otherwise. 
 */
static int service_start(svc_t *svc)
{
	int i, result = 0;
	pid_t pid;
	sigset_t nmask, omask;

	if (!svc)
		return 1;

	/* Don't try and start service if it doesn't exist. */
	if (!fexist(svc->cmd) && !svc->inetd.cmd) {
		print(1, "Service %s does not exist!", svc->cmd);
		svc_missing(svc);
		return 1;
	}

	/* Ignore if finit is SIGSTOP'ed */
	if (is_norespawn())
		return 1;

	if (svc_is_daemon(svc) || svc_is_inetd(svc))
		print_desc("Starting ", svc->desc);
	else
		print_desc("", svc->desc);

#ifdef INETD_ENABLED
	if (svc_is_inetd(svc))
		return print_result(inetd_start(&svc->inetd));
#endif

	/* Declare we're waiting for svc to create its pidfile */
	svc_starting(svc);

	/* Block SIGCHLD while forking.  */
	sigemptyset(&nmask);
	sigaddset(&nmask, SIGCHLD);
	sigprocmask(SIG_BLOCK, &nmask, &omask);

	pid = fork();
	if (pid == 0) {
		int status;
		char *home = NULL;
#ifdef ENABLE_STATIC
		int uid = 0; /* XXX: Fix better warning that dropprivs is disabled. */
#else
		int uid = getuser(svc->username, &home);
#endif
		char *args[MAX_NUM_SVC_ARGS];

		/* Set desired user */
		if (uid >= 0) {
			setuid(uid);

			/* Set default path for regular users */
			if (uid > 0)
				setenv("PATH", _PATH_DEFPATH, 1);
			if (home) {
				setenv("HOME", home, 1);
				chdir(home);
			}
		}

		/* Serve copy of args to process in case it modifies them. */
		for (i = 0; i < (MAX_NUM_SVC_ARGS - 1) && svc->args[i][0] != 0; i++)
			args[i] = svc->args[i];
		args[i] = NULL;

		/* Redirect inetd socket to stdin for connection */
#ifdef INETD_ENABLED
		if (svc_is_inetd_conn(svc)) {
			dup2(svc->stdin_fd, STDIN_FILENO);
			close(svc->stdin_fd);
			dup2(STDIN_FILENO, STDOUT_FILENO);
			dup2(STDIN_FILENO, STDERR_FILENO);
		} else
#endif

		if (svc->log) {
			int fd;

			/*
			 * Open PTY to connect to logger.  A pty isn't buffered
			 * like a pipe, and it eats newlines so they aren't logged
			 */
			fd = posix_openpt(O_RDWR);
			if (fd == -1) {
				svc->log = 0;
				goto logger_err;
			}
			if (grantpt(fd) == -1 || unlockpt(fd) == -1) {
				close(fd);
				svc->log = 0;
				goto logger_err;
			}

			/* SIGCHLD is still blocked for grantpt() and fork() */
			sigprocmask(SIG_BLOCK, &nmask, NULL);
			pid = fork();
			if (pid == 0) {
				int fds = open(ptsname(fd), O_RDONLY);

				close(fd);
				if (fds == -1)
					_exit(0);
				dup2(fds, STDIN_FILENO);

				/* Reset signals */
				sig_unblock();

				execlp("logger", "logger", "-t", svc->cmd, "-p", "daemon.info", NULL);
				_exit(0);
			}

			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			close(fd);
		} else if (debug) {
			int fd;

			fd = open(CONSOLE, O_WRONLY | O_APPEND);
			if (-1 != fd) {
				dup2(fd, STDOUT_FILENO);
				dup2(fd, STDERR_FILENO);
				close(fd);
			}
		}
	logger_err:
		sig_unblock();

		if (svc->inetd.cmd)
			status = svc->inetd.cmd(svc->inetd.type);
		else
			status = execv(svc->cmd, args); /* XXX: Maybe use execve() to be able to launch scripts? */

#ifdef INETD_ENABLED
		if (svc_is_inetd_conn(svc)) {
			if (svc->inetd.type == SOCK_STREAM) {
				close(STDIN_FILENO);
				close(STDOUT_FILENO);
				close(STDERR_FILENO);
			}
		} else
#endif
		if (svc->log)
			waitpid(pid, NULL, 0);

		exit(status);
	} else if (debug) {
		char buf[CMD_SIZE] = "";

		for (i = 0; i < (MAX_NUM_SVC_ARGS - 1) && svc->args[i][0] != 0; i++) {
			char arg[MAX_ARG_LEN];

			snprintf(arg, sizeof(arg), "%s ", svc->args[i]);
			if (strlen(arg) < (sizeof(buf) - strlen(buf)))
				strcat(buf, arg);
		}
		_d("Starting %s: %s", svc->cmd, buf);
	}

	svc->pid = pid;

#ifdef INETD_ENABLED
	if (svc_is_inetd_conn(svc) && svc->inetd.type == SOCK_STREAM)
		close(svc->stdin_fd);
#endif

	plugin_run_hook(HOOK_SVC_START, (void *)(uintptr_t)pid);

	if (SVC_TYPE_RUN == svc->type) {
		result = WEXITSTATUS(complete(svc->cmd, pid));
		svc->pid = 0;
	}
	
	sigprocmask(SIG_SETMASK, &omask, NULL);

	return print_result(result);
}

/**
 * service_kill - Forcefully terminate a service
 * @param svc  Service to kill
 *
 * Called when a service refuses to terminate gracefully.
 */
static void service_kill(svc_t *svc)
{
	service_timeout_cancel(svc);

	if (runlevel != 1)
		print_desc("Killing ", svc->desc);

	_d("Sending SIGKILL to pid:%d name:%s", svc->pid, pid_get_name(svc->pid, NULL, 0));
	kill(svc->pid, SIGKILL);

	/* Let SIGKILLs stand out, show result as [WARN] */
	if (runlevel != 1)
		print(2, NULL);
}

/**
 * service_stop - Stop service
 * @svc: Service to stop
 *
 * Returns:
 * 0 if the service was successfully stopped. Non-zero otherwise. 
 */
static int service_stop(svc_t *svc)
{
	int res = 0;

	if (!svc)
		return 1;

#ifdef INETD_ENABLED
	if (svc_is_inetd(svc)) {
		int do_print = runlevel != 1 && !svc_is_busy(svc);

		if (do_print)
			print_desc("Stopping ", svc->desc);

		inetd_stop(&svc->inetd);

		if (do_print)
			print_result(0);
		return 0;
	}
#endif

	if (svc->pid <= 1) {
		_d("Bad PID %d for %s, SIGTERM", svc->pid, svc->desc);
		return 1;
	}

	if (SVC_TYPE_SERVICE != svc->type)
		return 0;

	if (runlevel != 1)
		print_desc("Stopping ", svc->desc);

	_d("Sending SIGTERM to pid:%d name:%s", svc->pid, pid_get_name(svc->pid, NULL, 0));
	res = kill(svc->pid, SIGTERM);

	if (runlevel != 1)
		print_result(res);

	return res;
}

/**
 * service_restart - Restart a service by sending %SIGHUP
 * @svc: Service to reload
 *
 * This function does some basic checks of the runtime state of Finit
 * and a sanity check of the @svc before sending %SIGHUP.
 * 
 * Returns:
 * POSIX OK(0) or non-zero on error.
 */
static int service_restart(svc_t *svc)
{
	/* Ignore if finit is SIGSTOP'ed */
	if (is_norespawn())
		return 1;

	if (!svc || !svc->sighup)
		return 1;

	if (svc->pid <= 1) {
		_d("Bad PID %d for %s, SIGHUP", svc->pid, svc->cmd);
		svc->pid = 0;
		return 1;
	}

	print_desc("Restarting ", svc->desc);

	/* Declare we're waiting for svc to re-assert/touch its pidfile */
	svc_starting(svc);

	_d("Sending SIGHUP to PID %d", svc->pid);
	return print_result(kill(svc->pid, SIGHUP));
}

/**
 * service_reload_dynamic - Called on SIGHUP, 'init q' or 'initctl reload'
 *
 * This function is called when Finit has recieved SIGHUP to reload
 * .conf files in /etc/finit.d.  It is responsible for starting,
 * stopping and reloading (forwarding SIGHUP) to processes affected.
 */
void service_reload_dynamic(void)
{
	sm_set_reload(&sm);
	sm_step(&sm);
}

/**
 * service_runlevel - Change to a new runlevel
 * @newlevel: New runlevel to activate
 *
 * Stops all services not in @newlevel and starts, or lets continue to run,
 * those in @newlevel.  Also updates @prevlevel and active @runlevel.
 */
void service_runlevel(int newlevel)
{
	sm_set_runlevel(&sm, newlevel);
	sm_step(&sm);
}

/**
 * service_register - Register service, task or run commands
 * @type:     %SVC_TYPE_SERVICE(0), %SVC_TYPE_TASK(1), %SVC_TYPE_RUN(2)
 * @line:     A complete command line with -- separated description text
 * @mtime:    The modification time if service is loaded from /etc/finit.d
 * @username: Optional username to run service as, or %NULL to run as root
 *
 * This function is used to register commands to be run on different
 * system runlevels with optional username.  The @type argument details
 * if it's service to bo monitored/respawned (daemon), a one-shot task
 * or a command that must run in sequence and not in parallell, like
 * service and task commands do.
 *
 * The @line can optionally start with a username, denoted by an @
 * character. Like this:
 *
 *     service @username [!0-6,S] <!COND> /path/to/daemon arg -- Description
 *     task @username [!0-6,S] /path/to/task arg              -- Description
 *     run  @username [!0-6,S] /path/to/cmd arg               -- Description
 *     inetd tcp/ssh nowait [2345] @root:root /sbin/sshd -i   -- Description
 *
 * If the username is left out the command is started as root.  The []
 * brackets denote the allowed runlevels, if left out the default for a
 * service is set to [2-5].  Allowed runlevels mimic that of SysV init
 * with the addition of the 'S' runlevel, which is only run once at
 * startup.  It can be seen as the system bootstrap.  If a task or run
 * command is listed in more than the [S] runlevel they will be called
 * when changing runlevel.
 *
 * Services (daemons, not inetd services) also support an optional <!EV>
 * argument.  This is for services that, e.g., require a system gateway
 * or interface to be up before they are started.  Or restarted, or even
 * SIGHUP'ed, when the gateway changes or interfaces come and go.  The
 * special case when a service is declared with <!> means it does not
 * support SIGHUP but must be STOP/START'ed at system reconfiguration.
 *
 * Service conditions can be: svc/<PATH> for PID files, net/<IFNAME>/up
 * and net/<IFNAME>/exists.  The condition handling is further described
 * doc/conditions.md.
 *
 * For multiple instances of the same command, e.g. multiple DHCP
 * clients, the user must enter an ID, using the :ID syntax.
 *
 *     service :1 /sbin/udhcpc -i eth1
 *     service :2 /sbin/udhcpc -i eth2
 *
 * Without the :ID syntax Finit will overwrite the first service line
 * with the contents of the second.  The :ID must be [1,MAXINT].
 *
 * Returns:
 * POSIX OK(0) on success, or non-zero errno exit status on failure.
 */
int service_register(int type, char *line, struct timeval *mtime, char *username)
{
	int i = 0;
	int id = 1;		/* Default to ID:1 */
#ifdef INETD_ENABLED
	int forking = 0;
#endif
	int log = 0;
	char *service = NULL, *proto = NULL, *ifaces = NULL;
	char *cmd, *desc, *runlevels = NULL, *cond = NULL;
	svc_t *svc;
	plugin_t *plugin = NULL;

	if (!line) {
		_e("Invalid input argument");
		return errno = EINVAL;
	}

	desc = strstr(line, "-- ");
	if (desc) {
		*desc = 0;
		desc += 3;

		while (*desc && isblank(*desc))
			desc++;
	}

	cmd = strtok(line, " ");
	if (!cmd) {
	incomplete:
		_e("Incomplete service, cannot register");
		return errno = ENOENT;
	}

	while (cmd) {
		     if (cmd[0] == '@')	/* @username[:group] */
			username = &cmd[1];
		else if (cmd[0] == '[')	/* [runlevels] */
			runlevels = &cmd[0];
		else if (cmd[0] == '<')	/* <[!][cond][,cond..]> */
			cond = &cmd[1];
		else if (cmd[0] == ':')	/* :ID */
			id = atoi(&cmd[1]);
#ifdef INETD_ENABLED
		else if (!strncasecmp(cmd, "nowait", 6))
			forking = 1;
		else if (!strncasecmp(cmd, "wait", 4))
			forking = 0;
#endif
		else if (cmd[0] != '/' && strchr(cmd, '/'))
			service = cmd;   /* inetd service/proto */
		else if (!strncasecmp(cmd, "log", 3))
			log = 1;
		else
			break;

		/* Check if valid command follows... */
		cmd = strtok(NULL, " ");
		if (!cmd)
			goto incomplete;
	}

	/* Example: inetd ssh/tcp@eth0,eth1 or 222/tcp@eth2 */
	if (service) {
		ifaces = strchr(service, '@');
		if (ifaces)
			*ifaces++ = 0;

		proto = strchr(service, '/');
		if (!proto)
			goto incomplete;
		*proto++ = 0;
	}

#ifdef INETD_ENABLED
	/* Find plugin that provides a callback for this inetd service */
	if (type == SVC_TYPE_INETD) {
		if (!strncasecmp(cmd, "internal", 8)) {
			char *ptr, *ps = service;

			/* internal.service */
			ptr = strchr(cmd, '.');
			if (ptr) {
				*ptr++ = 0;
				ps = ptr;
			}

			plugin = plugin_find(ps);
			if (!plugin || !plugin->inetd.cmd) {
				_w("Inetd service %s has no internal plugin, skipping ...", service);
				return errno = ENOENT;
			}
		}

		/* Check if known inetd, then add ifnames for filtering only. */
		svc = inetd_find_svc(cmd, service, proto);
		if (svc)
			goto inetd_setup;

		id = svc_next_id(cmd);
	}
recreate:
#endif

	svc = svc_find(cmd, id);
	if (!svc) {
		_d("Creating new svc for %s id #%d type %d", cmd, id, type);
		svc = svc_new(cmd, id, type);
		if (!svc) {
			_e("Out of memory, cannot register service %s", cmd);
			return errno = ENOMEM;
		}
	}
#ifdef INETD_ENABLED
	else {
		if (svc_is_inetd(svc) && type != SVC_TYPE_INETD) {
			_d("Service was previously inetd, deregistering ...");
			inetd_del(&svc->inetd);
			svc_del(svc);
			goto recreate;
		}
	}
#endif

	svc->log = log;
	if (desc)
		strlcpy(svc->desc, desc, sizeof(svc->desc));

	if (username) {
		char *ptr = strchr(username, ':');

		if (ptr) {
			*ptr++ = 0;
			strlcpy(svc->group, ptr, sizeof(svc->group));
		}
		strlcpy(svc->username, username, sizeof(svc->username));
	}

	if (plugin) {
		/* Internal plugin provides this service */
		svc->inetd.cmd = plugin->inetd.cmd;
		svc->inetd.builtin = 1;
	} else {
		strlcpy(svc->args[i++], cmd, sizeof(svc->args[0]));
		while ((cmd = strtok(NULL, " ")))
			strlcpy(svc->args[i++], cmd, sizeof(svc->args[0]));
		svc->args[i][0] = 0;
	}

	svc->runlevels = conf_parse_runlevels(runlevels);
	_d("Service %s runlevel 0x%2x", svc->cmd, svc->runlevels);

	conf_parse_cond(svc, cond);

#ifdef INETD_ENABLED
	if (svc_is_inetd(svc)) {
		char *iface, *name = service;

		if (svc->inetd.cmd && plugin)
			name = plugin->name;

		if (inetd_new(&svc->inetd, name, service, proto, forking, svc)) {
			_e("Failed registering new inetd service %s/%s", service, proto);
			return svc_del(svc);
		}

	inetd_setup:
		inetd_flush(&svc->inetd);

		if (!ifaces) {
			_d("No specific iface listed for %s, allowing ANY", service);
			inetd_allow(&svc->inetd, NULL);
		} else {
			for (iface = strtok(ifaces, ","); iface; iface = strtok(NULL, ",")) {
				if (iface[0] == '!')
					inetd_deny(&svc->inetd, &iface[1]);
				else
					inetd_allow(&svc->inetd, iface);
			}
		}
	}
#endif

	/* New, recently modified or unchanged ... used on reload. */
	svc_check_dirty(svc, mtime);

	return 0;
}

void service_unregister(svc_t *svc)
{
	svc_del(svc);
}

void service_monitor(pid_t lost)
{
	svc_t *svc;
	char pidfile[MAX_ARG_LEN];

	if (fexist(SYNC_SHUTDOWN) || lost <= 1)
		return;

	if (tty_respawn(lost))
		return;

	plugin_run_hook(HOOK_SVC_LOST, (void *)(uintptr_t)lost);

	svc = svc_find_by_pid(lost);
	if (!svc) {
		_d("collected unknown PID %d", lost);
		return;
	}

	if (!prevlevel && svc_clean_bootstrap(svc))
		return;

	_d("collected %s(%d)", svc->cmd, lost);

	/* Try removing PID file (in case service does not clean up after itself) */
	snprintf(pidfile, sizeof(pidfile), "%s%s.pid", _PATH_VARRUN, basename(svc->cmd));
	if (remove(pidfile)) {
		if (errno != ENOENT)
			logit(LOG_CRIT, "Failed removing service %s pidfile %s", basename(svc->cmd), pidfile);
	}

	/* No longer running, update books. */
	svc->pid = 0;
	service_step(svc);

	sm_step(&sm);
}

static void service_retry(svc_t *svc)
{
	int timeout;
	int *restart_counter = (int *)&svc->restart_counter;

	service_timeout_cancel(svc);

	if (svc->state != SVC_HALTED_STATE ||
	    svc->block != SVC_BLOCK_RESTARTING) {
		_d("%s not crashing anymore", svc->desc);
		*restart_counter = 0;
		return;
	}

	if (*restart_counter >= RESPAWN_MAX) {
		_e("%s keeps crashing, not restarting", svc->desc);
		svc_crashing(svc);
		*restart_counter = 0;
		service_step(svc);
		return;
	}

	(*restart_counter)++;

	_d("%s crashed, trying to start it again, attempt %d", svc->desc, *restart_counter);
	svc_unblock(svc);
	service_step(svc);

	/* Wait 2s for the first 5 respawns, then back off to 5s */
	timeout = ((*restart_counter) <= (RESPAWN_MAX / 2)) ? 2000 : 5000;
	service_timeout_after(svc, timeout, service_retry);
}

static void svc_set_state(svc_t *svc, svc_state_t new)
{
	svc_state_t *state = (svc_state_t *)&svc->state;

	*state = new;

	/* if the PID isn't collected within 3s, kill it! */
	if (*state == SVC_STOPPING_STATE) {
		service_timeout_cancel(svc);
		service_timeout_after(svc, 3000, service_kill);
	}
}

void service_step(svc_t *svc)
{
	int err;
	int *restart_counter = (int *)&svc->restart_counter;
	svc_cmd_t enabled;
	svc_state_t old_state;
	cond_state_t cond;

restart:
	old_state = svc->state;
	enabled = service_enabled(svc);

	_d("%20s(%4d): %8s %3sabled/%-7s cond:%-4s", svc->cmd, svc->pid,
	   svc_status(svc), enabled ? "en" : "dis", svc_dirtystr(svc),
	   condstr(cond_get_agg(svc->cond)));

	switch (svc->state) {
	case SVC_HALTED_STATE:
		if (enabled)
			svc_set_state(svc, SVC_READY_STATE);
		break;

	case SVC_DONE_STATE:
#ifdef INETD_ENABLED
		if (svc_is_inetd_conn(svc)) {
			if (svc_is_busy(svc->inetd.svc)) {
				svc_unblock(svc->inetd.svc);
				service_step(svc->inetd.svc);
			}
			service_unregister(svc);
			return;
		}
#endif
		if (svc_is_changed(svc))
			svc_set_state(svc, SVC_HALTED_STATE);
		break;

	case SVC_STOPPING_STATE:
		if (!svc->pid) {
			/* PID was collected normally, no need to kill it */
			service_timeout_cancel(svc);

			switch (svc->type) {
			case SVC_TYPE_SERVICE:
			case SVC_TYPE_INETD:
				svc_set_state(svc, SVC_HALTED_STATE);
				break;

			case SVC_TYPE_INETD_CONN:
			case SVC_TYPE_TASK:
			case SVC_TYPE_RUN:
				svc_set_state(svc, SVC_DONE_STATE);
				break;

			default:
				_e("unknown service type %d", svc->type);
				break;
			}
		}
		break;

	case SVC_READY_STATE:
		if (!enabled) {
			svc_set_state(svc, SVC_HALTED_STATE);
		} else if (cond_get_agg(svc->cond) == COND_ON) {
			/* wait until all processes has been stopped before continuing... */
			if (sm_is_in_teardown(&sm))
				break;

			err = service_start(svc);
			if (err) {
				(*restart_counter)++;

				if (!svc_is_inetd_conn(svc))
					break;
			}

			svc_mark_clean(svc);
			svc_set_state(svc, SVC_RUNNING_STATE);
		}
		break;

	case SVC_RUNNING_STATE:
		if (!enabled) {
			service_stop(svc);
			svc_set_state(svc, SVC_STOPPING_STATE);
			break;
		}

		if (!svc->pid) {
			if (svc_is_daemon(svc)) {
				svc_restarting(svc);
				svc_set_state(svc, SVC_HALTED_STATE);

				/*
				 * Restart directly after the first crash,
				 * then retry after 2 sec
				 */
				_d("delayed restart of %s", svc->desc);
				service_timeout_after(svc, 1, service_retry);
				break;
			}

			/* Collected inetd connection, drive it to stopping */
			if (svc_is_inetd_conn(svc)) {
				svc_set_state(svc, SVC_STOPPING_STATE);
				break;
			}
		}

		cond = cond_get_agg(svc->cond);
		switch (cond) {
		case COND_OFF:
			service_stop(svc);
			svc_set_state(svc, SVC_STOPPING_STATE);
			break;

		case COND_FLUX:
			kill(svc->pid, SIGSTOP);
			svc_set_state(svc, SVC_WAITING_STATE);
			break;

		case COND_ON:
			if (svc_is_changed(svc)) {
				if (svc->sighup) {
					/* wait until all processes has been stopped before continuing... */
					if (sm_is_in_teardown(&sm))
						break;
					service_restart(svc);
				} else {
					service_stop(svc);
					svc_set_state(svc, SVC_STOPPING_STATE);
				}
				svc_mark_clean(svc);
			}
			break;
		}
		break;

	case SVC_WAITING_STATE:
		if (!enabled) {
			kill(svc->pid, SIGCONT);
			service_stop(svc);
			svc_set_state(svc, SVC_STOPPING_STATE);
			break;
		}

		if (!svc->pid) {
			(*restart_counter)++;
			svc_set_state(svc, SVC_READY_STATE);
			break;
		}

		cond = cond_get_agg(svc->cond);
		switch (cond) {
		case COND_ON:
			kill(svc->pid, SIGCONT);
			svc_set_state(svc, SVC_RUNNING_STATE);
			break;

		case COND_OFF:
			kill(svc->pid, SIGCONT);
			service_stop(svc);
			svc_set_state(svc, SVC_STOPPING_STATE);
			break;

		case COND_FLUX:
			break;
		}
		break;
	}

	if (svc->state != old_state) {
		_d("%20s(%4d): -> %8s", svc->cmd, svc->pid, svc_status(svc));
		goto restart;
	}
}

void service_step_all(int types)
{
	svc_foreach_type(types, service_step);
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
