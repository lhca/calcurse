/*
 * Calcurse - text-based organizer
 *
 * Copyright (c) 2004-2017 calcurse Development Team <misc@calcurse.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the
 *        following disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the
 *        following disclaimer in the documentation and/or other
 *        materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Send your feedback or comments to : misc@calcurse.org
 * Calcurse home page : http://calcurse.org
 *
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <paths.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <signal.h>

#include "calcurse.h"

#define DMON_SLEEP_TIME  60

#define DMON_LOG(...) do {                                      \
  if (dmon.log) {                                               \
    char _msg[BUFSIZ];                                          \
    int _len;                                                   \
    struct tm _tm;                                              \
    time_t _now = time(NULL);                                   \
    localtime_r(&_now, &_tm);                                   \
    _len = strftime(_msg, BUFSIZ, "%F %T ", &_tm);              \
    snprintf(_msg + _len, BUFSIZ - _len, __VA_ARGS__);          \
    io_fprintln (path_dmon_log, _msg);                          \
  }                                                             \
} while (0)

#define DMON_ABRT(...) do {                                     \
  DMON_LOG (__VA_ARGS__);                                       \
  if (kill (getpid (), SIGINT) < 0)                             \
    {                                                           \
      DMON_LOG (_("Could not stop daemon properly: %s\n"),      \
                strerror (errno));                              \
      exit (EXIT_FAILURE);                                      \
    }                                                           \
} while (0)

static void dmon_sigs_hdlr(int sig)
{
	if (sig == SIGUSR1) {
		DMON_LOG(_("Reload signal %d\n"), sig);
		want_reload = 1;
		return;
	}
	DMON_LOG(_("Stop  signal %d\n"), sig);
	apoint_llist_free();
	recur_apoint_llist_free();
	if (unlink(path_dpid) != 0) {
		DMON_LOG(_("Could not remove daemon lock file: %s\n"),
			 strerror(errno));
		exit(EXIT_FAILURE);
	}
	exit(EXIT_SUCCESS);
}

static unsigned daemonize(void)
{
	int fd;

	/*
	 * Process independency.
	 *
	 * Obtain a new process group and session in order to get detached from the
	 * controlling terminal.
	 */
	if (setsid() == -1) {
		DMON_LOG(_("Could not detach from the controlling terminal: %s\n"),
			 strerror(errno));
		return 0;
	}

	/*
	 * Change working directory to root directory,
	 * to prevent filesystem unmounts.
	 */
	if (chdir("/") == -1) {
		DMON_LOG(_("Could not change working directory: %s\n"),
			 strerror(errno));
		return 0;
	}

	/* Redirect standard file descriptors to /dev/null. */
	if ((fd = open(_PATH_DEVNULL, O_RDWR, 0)) != -1) {
		dup2(fd, STDIN_FILENO);
		dup2(fd, STDOUT_FILENO);
		dup2(fd, STDERR_FILENO);
		if (fd > 2)
			close(fd);
	}

	/* Write access for the owner only. */
	umask(0022);

	if (!sigs_set_hdlr(SIGINT, dmon_sigs_hdlr)
	    || !sigs_set_hdlr(SIGTERM, dmon_sigs_hdlr)
	    || !sigs_set_hdlr(SIGALRM, dmon_sigs_hdlr)
	    || !sigs_set_hdlr(SIGQUIT, dmon_sigs_hdlr)
	    || !sigs_set_hdlr(SIGUSR1, dmon_sigs_hdlr)
	    || !sigs_set_hdlr(SIGCHLD, SIG_IGN))
		return 0;

	return 1;
}

void dmon_start(void)
{
	struct notify_app a;
	int left, nap;

	/*
	 * First need to fork in order to become a child of the init process,
	 * once the parent exits.
	 */
	switch (fork()) {
	case -1:		/* fork error */
		EXIT(_("Could not fork: %s\n"), strerror(errno));
		break;
	case 0:			/* child */
		break;
	default:		/* parent */
		return;
	}

	if (!daemonize())
		DMON_ABRT(_("Cannot daemonize, aborting\n"));
	if (!io_dump_pid(path_dpid))
		DMON_ABRT(_("Could not set lock file\n"));
	if (!io_file_exists(path_apts))
		DMON_ABRT(_("Could not access \"%s\": %s\n"), path_apts,
			  strerror(errno));
	DMON_LOG("\n");
	/* Non-interactive */
	ui_mode = UI_CMDLINE; /* the only possibility */
	nbar.show = 0;
	quiet = 1;

	/*
	 * Data in memory are inherited after fork().
	 * Free those which are not needed.
	 */
	day_free_vector();
	event_llist_free();
	recur_event_llist_free();
	for (int i = 0; i <= 37; i++)
		ui_day_item_cut_free(i);
	todo_free_list();
	keys_free();

	DMON_LOG(_("Start\n"));
	for (;;) {
		DMON_LOG(_("Awake\n"));

		if (want_reload) {
			want_reload = 0;
			DMON_LOG(_("Reloading\n"));
			io_reload_data();
		}

		if (!notify_get_next(&a))
			DMON_ABRT(_("Error finding next appointment\n"));
		if (!a.got_app) {
			notify_free_app();
		} else {
			if (!notify_same_item(a.time)) {
				notify_update_app(a.time, a.state, a.txt);
			}
		}
		if (a.txt)
			mem_free(a.txt);

		left = notify_time_left();
		if (left > 0 && left <= nbar.cntdwn && notify_trigger()) {
			switch(notify_launch_cmd()) {
			case 0:
				DMON_LOG(_("Error while sending notification\n"));
				break;
			case 1:
				DMON_LOG(_("Launch notification: \"%s\"\n"), notify_app_txt());
				break;
			}
		}

		/* Wake up on the minute. */
		nap = time(NULL) % MININSEC;
		nap = nap ? MININSEC - nap : DMON_SLEEP_TIME;
		DMON_LOG(_("Sleep %d seconds\n"), nap);
		psleep(nap);
	}
}

/*
 * Check if calcurse is running in background, and if yes, send a SIGINT
 * signal to stop it.
 */
void dmon_stop(void)
{
	int dpid;

	dpid = io_get_pid(path_dpid);
	if (!dpid)
		return;

	if (kill((pid_t) dpid, SIGINT) < 0 && errno != ESRCH)
		EXIT(_("Could not stop calcurse daemon: %s\n"),
		     strerror(errno));
}
