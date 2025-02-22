/* Finit state machine
 *
 * Copyright (c) 2016  Jonas Johansson <jonas.johansson@westermo.se>
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

#include <paths.h>
#include <sys/types.h>

#include "finit.h"
#include "cond.h"
#include "conf.h"
#include "helpers.h"
#include "private.h"
#include "schedule.h"
#include "service.h"
#include "sig.h"
#include "tty.h"
#include "sm.h"
#include "utmp-api.h"

sm_t sm;

#ifndef FINIT_NOLOGIN_PATH
#define FINIT_NOLOGIN_PATH _PATH_NOLOGIN /* Stop user logging in. */
#endif

/*
 * Wait for system bootstrap to complete, all SVC_TYPE_RUNTASK must be
 * allowed to complete their work in [S], or timeout, before we switch
 * to the configured runlevel and call finalize(), should not take more
 * than 120 sec.
 */
static void sm_check_bootstrap(void *work)
{
	static int timeout = 120;
	int bootstrap_done;
	int level = cfglevel;
	svc_t *svc = NULL;

	dbg("Step all services ...");
	service_step_all(SVC_TYPE_ANY);

	bootstrap_done = service_completed(&svc);
	if (timeout-- > 0 && !bootstrap_done) {
		dbg("Not all bootstrap run/tasks have completed yet ... %d", timeout);
		schedule_work(work);
		return;
	}

	if (timeout > 0) {
		dbg("All run/task have completed, resuming bootstrap.");
	} else {
		dbg("Timeout, resuming bootstrap.");
		if (svc)
			print(2, "Timeout waiting for %s to run, resuming bootstrap", svc_ident(svc, NULL, 0));
		else
			print(2, "Timeout waiting for unknown run/task, resuming bootstrap");
	}

	dbg("Flushing pending .conf file events ...");
	conf_flush_events();

	/*
	 * Start all tasks/services in the configured runlevel, or jump
	 * into the runlevel selected from the command line.
	 */
	if (cmdlevel) {
		dbg("Runlevel %d requested from command line, starting all services ...", cmdlevel);
		level = cmdlevel;
	} else
		dbg("Change to default runlevel(%d), starting all services ...", cfglevel);

	service_runlevel(level);

	/* Clean up bootstrap-only tasks/services that never started */
	dbg("Clean up all bootstrap-only tasks/services ...");
	svc_prune_bootstrap();

	/* All services/tasks/etc. in configure runlevel have started */
	dbg("Running svc up hooks ...");
	plugin_run_hooks(HOOK_SVC_UP);
}

static char *sm_status(sm_state_t state)
{
	switch (state) {
	case SM_BOOTSTRAP_STATE:
		return "bootstrap";

	case SM_BOOTSTRAP_WAIT_STATE:
		return "bootstrap/wait";

	case SM_RUNNING_STATE:
		return "running";

	case SM_RUNLEVEL_CHANGE_STATE:
		return "runlevel/change";

	case SM_RUNLEVEL_WAIT_STATE:
		return "runlevel/wait";

	case SM_RUNLEVEL_CLEAN_STATE:
		return "runlevel/clean";

	case SM_RELOAD_CHANGE_STATE:
		return "reload/change";

	case SM_RELOAD_WAIT_STATE:
		return "reload/wait";

	case SM_RELOAD_CLEAN_STATE:
		return "reload/clean";
	}

	return "unknown";
}

static char sm_runlevel(int lvl)
{
	if (lvl == INIT_LEVEL)
		return 'S';
	return lvl + '0';
}

/*
 * Disable login in single user mode and shutdown/reboot
 *
 * Re-enable only when going from these runlevels, this way a user can
 * manage FINIT_NOLOGIN_PATH manually within the other runlevels without
 * us pulling the rug from under their feet.
 */
static void nologin(void)
{
	if (runlevel == 1 || IS_RESERVED_RUNLEVEL(runlevel))
		touch(FINIT_NOLOGIN_PATH);

	if (prevlevel == 1 || IS_RESERVED_RUNLEVEL(prevlevel))
		erase(FINIT_NOLOGIN_PATH);
}

void sm_init(sm_t *sm)
{
	static struct wq work = {
		.cb = sm_check_bootstrap,
		.delay = 1000
	};

	sm->state = SM_BOOTSTRAP_STATE;
	sm->newlevel = -1;
	sm->reload = 0;
	sm->in_teardown = 0;

	dbg("Starting bootstrap finalize timer ...");
	schedule_work(&work);
}

void sm_set_runlevel(sm_t *sm, int newlevel)
{
	sm->newlevel = newlevel;

	dbg("Flushing pending .conf file events ...");
	conf_flush_events();
}

void sm_set_reload(sm_t *sm)
{
	sm->reload = 1;

	dbg("Flushing pending .conf file events ...");
	conf_flush_events();
}

int sm_is_in_teardown(sm_t *sm)
{
	return sm->in_teardown;
}

void sm_step(sm_t *sm)
{
	sm_state_t old_state;
	svc_t *svc;

restart:
	old_state = sm->state;

	dbg("state: %s, runlevel: %c, newlevel: %d, teardown: %d, reload: %d",
	    sm_status(sm->state), sm_runlevel(runlevel), sm->newlevel, sm->in_teardown, sm->reload);

	switch (sm->state) {
	case SM_BOOTSTRAP_STATE:
		dbg("Bootstrapping all services in runlevel S from %s", finit_conf);
		service_step_all(SVC_TYPE_RUNTASK | SVC_TYPE_SERVICE | SVC_TYPE_SYSV);

		sm->state = SM_BOOTSTRAP_WAIT_STATE;
		break;

	/*
	 * Handle bootstrap transition to configured runlevel, start TTYs
	 *
	 * This is the final stage of bootstrap.  It changes to the default
	 * (configured) runlevel, calls all external start scripts and final
	 * bootstrap hooks before bringing up TTYs.
	 *
	 * We must ensure that all declared `task [S]` and `run [S]` jobs in
	 * finit.conf, or *.conf in finit.d/, run to completion before we
	 * finalize the bootstrap process by calling this function.
	 */
	case SM_BOOTSTRAP_WAIT_STATE:
		/* We come here from bootstrap, runlevel change and conf reload */
		service_step_all(SVC_TYPE_ANY);

		/* Allow runparts to start */
		cond_set_oneshot("int/bootstrap");

		if (sm->newlevel == -1)
			break;

		/* Hooks that should run at the very end */
		dbg("Calling all system up hooks ...");
		plugin_run_hooks(HOOK_SYSTEM_UP);
		service_step_all(SVC_TYPE_ANY);

		/* Disable progress output at normal runtime */
		enable_progress(0);

		/* System bootrapped, launch TTYs et al */
		bootstrap = 0;
		service_step_all(SVC_TYPE_RESPAWN);
		sm->state = SM_RUNNING_STATE;
		break;

	case SM_RUNNING_STATE:
		/* We come here from bootstrap, runlevel change and conf reload */
		service_step_all(SVC_TYPE_ANY);

		/* runlevel changed? */
		if (sm->newlevel >= 0 && sm->newlevel <= 9) {
			if (runlevel == sm->newlevel) {
				sm->newlevel = -1;
				break;
			}
			sm->state = SM_RUNLEVEL_CHANGE_STATE;
			break;
		}

		/* reload ? */
		if (sm->reload) {
			sm->reload = 0;
			sm->state = SM_RELOAD_CHANGE_STATE;
		}
		break;

	case SM_RUNLEVEL_CHANGE_STATE:
		prevlevel    = runlevel;
		runlevel     = sm->newlevel;
		sm->newlevel = -1;

		/* Restore terse mode and run hooks before shutdown */
		if (runlevel == 0 || runlevel == 6) {
			api_exit();
			log_exit();
			plugin_run_hooks(HOOK_SHUTDOWN);
		}

		dbg("Setting new runlevel --> %c <-- previous %c", sm_runlevel(runlevel), sm_runlevel(prevlevel));
		if (osheading)
			logit(LOG_CONSOLE | LOG_NOTICE, "%s, entering runlevel %c", osheading, sm_runlevel(runlevel));
		else
			logit(LOG_CONSOLE | LOG_NOTICE, "Entering runlevel %c", sm_runlevel(runlevel));
		runlevel_set(prevlevel, runlevel);

		/* Disable login in single-user mode as well as shutdown/reboot */
		nologin();

		if (runlevel != 0 && runlevel != 6) {
			/* Make sure to (re)load all *.conf in /etc/finit.d/ */
			if (conf_any_change())
				conf_reload();
		}

		/* Reset once flag of runtasks */
		service_runtask_clean();

		dbg("Stopping services not allowed in new runlevel ...");
		sm->in_teardown = 1;
		service_step_all(SVC_TYPE_ANY);

		sm->state = SM_RUNLEVEL_WAIT_STATE;
		break;

	case SM_RUNLEVEL_WAIT_STATE:
		/*
		 * Need to wait for any services to stop? If so, exit early
		 * and perform second stage from service_monitor later.
		 */
		svc = svc_stop_completed();
		if (svc) {
			dbg("Waiting to collect %s, cmd %s(%d) ...", svc_ident(svc, NULL, 0), svc->cmd, svc->pid);
			break;
		}

		/* Prev runlevel services stopped, call hooks before starting new runlevel ... */
		dbg("All services have been stopped, calling runlevel change hooks ...");
		plugin_run_hooks(HOOK_RUNLEVEL_CHANGE);  /* Reconfigure HW/VLANs/etc here */

		dbg("Starting services new to this runlevel ...");
		sm->in_teardown = 0;
		service_step_all(SVC_TYPE_ANY);

		sm->state = SM_RUNLEVEL_CLEAN_STATE;
		break;

	case SM_RUNLEVEL_CLEAN_STATE:
		/*
		 * Wait for post:script or cleanup:script to be collected,
		 * which moves the svc to HALTED or DEAD state.  We will
		 * be called by the service_monitor() on collect.
		 */
		svc = svc_clean_completed();
		if (svc) {
			dbg("Waiting to collect post/cleanup script for %s, cmd %s(%d) ...",
			    svc_ident(svc, NULL, 0), svc->cmd, svc->pid);
			break;
		}

		/* Cleanup stale services */
		svc_clean_dynamic(service_unregister);

#ifdef FINIT_RC_LOCAL
		/* Compat SysV init */
		if (prevlevel == INIT_LEVEL && !rescue)
			run_bg(FINIT_RC_LOCAL, NULL);
#endif

		/*
		 * "I've seen things you people wouldn't believe.  Attack ships on fire off
		 *  the shoulder of Orion.  I watched C-beams glitter in the dark near the
		 *  Tannhäuser Gate.  All those .. moments .. will be lost in time, like
		 *  tears ... in ... rain."
		 */
		if (runlevel == 0 || runlevel == 6)
			do_shutdown(halt);

		sm->state = SM_RUNNING_STATE;
		break;

	case SM_RELOAD_CHANGE_STATE:
		/* First reload all *.conf in /etc/finit.d/ */
		conf_reload();

		/*
		 * Then, mark all affected service conditions as in-flux and
		 * let all affected services move to WAITING/HALTED
		 */
		dbg("Stopping services not allowed after reconf ...");
		sm->in_teardown = 1;
		cond_reload();
		service_step_all(SVC_TYPE_ANY);

		sm->state = SM_RELOAD_WAIT_STATE;
		break;

	case SM_RELOAD_WAIT_STATE:
		/*
		 * Need to wait for any services to stop? If so, exit early
		 * and perform second stage from service_monitor later.
		 */
		svc = svc_stop_completed();
		if (svc) {
			dbg("Waiting to collect %s, cmd %s(%d) ...", svc_ident(svc, NULL, 0), svc->cmd, svc->pid);
			break;
		}

		sm->in_teardown = 0;

		dbg("Starting services after reconf ...");
		service_step_all(SVC_TYPE_ANY);

		sm->state = SM_RELOAD_CLEAN_STATE;
		break;

	case SM_RELOAD_CLEAN_STATE:
		/*
		 * Wait for post:script or cleanup:script to be collected,
		 * which moves the svc to HALTED or DEAD state.  We will
		 * be called by the service_monitor() on collect.
		 */
		svc = svc_clean_completed();
		if (svc) {
			dbg("Waiting to collect post/cleanup script for %s, cmd %s(%d) ...",
			    svc_ident(svc, NULL, 0), svc->cmd, svc->pid);
			break;
		}

		/* Cleanup stale services */
		svc_clean_dynamic(service_unregister);

		dbg("Calling reconf hooks ...");
		plugin_run_hooks(HOOK_SVC_RECONF);

		dbg("Update configuration generation of unmodified non-native services ...");
		service_notify_reconf();

		dbg("Reconfiguration done");
		sm->state = SM_RUNNING_STATE;
		break;
	}

	if (sm->state != old_state)
		goto restart;
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
