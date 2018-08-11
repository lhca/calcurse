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

#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "calcurse.h"

#define NOTIFY_FIELD_LENGTH	25

static struct notify_app notify_app;
static pthread_attr_t detached_thread_attr;

/*
 * Return the number of seconds before next appointment.
 */
int notify_time_left(void)
{
	int left = notify_app.time - time(NULL);

	return left > 0 ? left : 0;
}

unsigned notify_trigger(void)
{
	int flagged = notify_app.state & APOINT_NOTIFY;

	if (!notify_app.got_app)
		return 0;
	if (nbar.notify_all == NOTIFY_ALL)
		return 1;
	if (nbar.notify_all == NOTIFY_UNFLAGGED_ONLY)
		flagged = !flagged;
	return flagged;
}

/*
 * Only this and notify_free_app() are used to update the notify_app structure.
 * Note: the mutex associated with this structure must be locked by the caller!
 */
void notify_update_app(long start, char state, char *msg)
{
	notify_free_app();
	notify_app.got_app = 1;
	notify_app.update = DAYINSEC;
	notify_app.time = start;
	notify_app.state = state;
	notify_app.txt = mem_strdup(msg);
}

/* Return 1 if we need to display the notify-bar, else 0. */
int notify_bar(void)
{
	int display_bar = 0;

	pthread_mutex_lock(&nbar.mutex);
	display_bar = (nbar.show) ? 1 : 0;
	pthread_mutex_unlock(&nbar.mutex);

	return display_bar;
}

/* Initialize the nbar variable used to store notification options. */
void notify_init_vars(void)
{
	const char *time_format = "%T";
	const char *date_format = "%a %F";
	const char *cmd = "printf '\\a'";

	pthread_mutex_init(&nbar.mutex, NULL);
	nbar.show = 1;
	nbar.cntdwn = 300;
	strncpy(nbar.datefmt, date_format, BUFSIZ);
	nbar.datefmt[BUFSIZ - 1] = '\0';
	strncpy(nbar.timefmt, time_format, BUFSIZ);
	nbar.timefmt[BUFSIZ - 1] = '\0';
	strncpy(nbar.cmd, cmd, BUFSIZ);
	nbar.cmd[BUFSIZ - 1] = '\0';

	if ((nbar.shell = getenv("SHELL")) == NULL)
		nbar.shell = "/bin/sh";

	nbar.notify_all = 0;

	pthread_attr_init(&detached_thread_attr);
	pthread_attr_setdetachstate(&detached_thread_attr,
				    PTHREAD_CREATE_DETACHED);
}

/* Extract the appointment file name from the complete file path. */
static int extract_aptsfile(char **file)
{
	*file = strrchr(path_apts, '/');
	if (!*file)
		*file = path_apts;
	else
		(*file)++;
	return strlen(*file);
}

/* Create the notification bar window. */
void notify_init_bar(void)
{
	pthread_mutex_init(&notify_app.mutex, NULL);
	notify_free_app();
	win[NOT].p =
	    newwin(win[NOT].h, win[NOT].w, win[NOT].y, win[NOT].x);
}

/*
 * Reset the notify_app structure and free memory associated with it.
 * Note: the mutex associated with this structure must be locked by the caller!
 */
void notify_free_app(void)
{
	notify_app.time = 0;
	notify_app.update = DAYINSEC;
	notify_app.got_app = 0;
	notify_app.state = APOINT_NULL;
	if (notify_app.txt)
		mem_free(notify_app.txt);
	notify_app.txt = NULL;
}

/* Stop the notify-bar main thread. */
void notify_stop_main_thread(void)
{
	/* Is the thread running? */
	if (pthread_equal(notify_t_main, pthread_self()))
		return;

	pthread_cancel(notify_t_main);
	pthread_join(notify_t_main, NULL);
	notify_t_main = pthread_self();
}

/*
 * The calcurse window geometry has changed so we need to reset the
 * notification window.
 */
void notify_reinit_bar(void)
{
	delwin(win[NOT].p);
	win[NOT].p =
	    newwin(win[NOT].h, win[NOT].w, win[NOT].y, win[NOT].x);
}

/* Launch user defined command as a notification. */
unsigned notify_launch_cmd(void)
{
	int pid;

	if (notify_app.state & APOINT_NOTIFIED)
		return 2;

	notify_app.state |= APOINT_NOTIFIED;

	pid = fork();

	if (pid < 0) {
		ERROR_MSG(_("error while launching command: could not fork"));
		return 0;
	} else if (pid == 0) {
		/* Child: launch user defined command */
		if (execlp(nbar.shell, nbar.shell, "-c", nbar.cmd, NULL) <
		    0) {
			ERROR_MSG(_("error while launching command"));
			_exit(1);
		}
		_exit(0);
	}

	return 1;
}

static void notify_main_thread_cleanup(void *arg)
{
	pthread_mutex_unlock(&notify_app.mutex);
}

/* Update the notication bar content */
/* ARGSUSED0 */
static void *notify_main_thread(void *arg)
{
	const unsigned thread_sleep = 1;
	struct tm tm;
	time_t ntimer, last_check;
	int time_left, rem, reminder, bar_hours, bar_mins;
	int t, d, f, file_pos, date_pos, app_pos, txt_max_len;
	const int space = 3;
	char bar_time[NOTIFY_FIELD_LENGTH];
	char bar_date[NOTIFY_FIELD_LENGTH];
	char bar_mesg[LINEBUF];
	char *bar_file;

	date_pos = space;
	file_pos = app_pos = 0;
	last_check = 0;
	reminder = bar_hours = bar_mins = 0;
	f = extract_aptsfile(&bar_file);

	pthread_cleanup_push(notify_main_thread_cleanup, NULL);
	for (;;) {
		/* Prepare the clock. */
		ntimer = time(NULL);
		localtime_r(&ntimer, &tm);
		t = strftime(bar_time, NOTIFY_FIELD_LENGTH, nbar.timefmt, &tm);
		d = strftime(bar_date, NOTIFY_FIELD_LENGTH, nbar.datefmt, &tm);

		time_left = 0;
		pthread_mutex_lock(&notify_app.mutex);
		if (notify_app.got_app) {
			/*
			 * Prepare the next appointment once every minute.
			 * Note that the appointment may have changed.
			 */
			time_left = notify_app.time - ntimer;
			if (time_left > 0 && time_left <= notify_app.update) {
				file_pos = date_pos + t + d + 7 + space;
				app_pos = file_pos + f + 2 + space;
				/* Round up to nearest minute. */
				rem = time_left % MININSEC;
				bar_mins = time_left / MININSEC + (rem ? 1 : 0);
				bar_hours = bar_mins / HOURINMIN;
				bar_mins = bar_mins % HOURINMIN;
				strncpy(bar_mesg, notify_app.txt, LINEBUF);
				bar_mesg[LINEBUF - 1] = '\0';
				txt_max_len = MAX(col - (app_pos + 13 + space), 3);
				utf8_chop(bar_mesg, txt_max_len);
				reminder = time_left <= nbar.cntdwn && notify_trigger();
				if (reminder && nbar.cmd[0] != '#')
					notify_launch_cmd();
				/*
				 * Next update: round down to nearest minute.
				 * This takes care of start up as well as a changed appointment.
				 */
				notify_app.update = time_left - MININSEC + (rem ? MININSEC - rem : 0);
			} else if (time_left <= 0)
				notify_check_next_app(0);
		} else { /* Check for next appointment once every minute. */
			if (ntimer > last_check + MININSEC) {
				notify_check_next_app(0);
				last_check = ntimer;
			}
		}
		pthread_mutex_unlock(&notify_app.mutex);

		/* Display everything. */
		WINS_NBAR_LOCK;
		custom_apply_attr(win[NOT].p, ATTR_HIGHEST);
		wattron(win[NOT].p, A_REVERSE);
		mvwhline(win[NOT].p, 0, 0, ACS_HLINE, col);
		mvwprintw(win[NOT].p, 0, date_pos, "[ %s | %s ]", bar_date, bar_time);
		if (time_left > 0) {
			mvwprintw(win[NOT].p, 0, file_pos, "(%s)", bar_file);
			if (reminder)
				wattron(win[NOT].p, A_BLINK);
			mvwprintw(win[NOT].p, 0, app_pos, "> %02d:%02d :: %s <",
				  bar_hours, bar_mins, bar_mesg);
			if (reminder)
				wattroff(win[NOT].p, A_BLINK);
		}
		wattroff(win[NOT].p, A_REVERSE);
		custom_remove_attr(win[NOT].p, ATTR_HIGHEST);
		WINS_NBAR_UNLOCK;

		wins_wrefresh(win[NOT].p);
		psleep(thread_sleep);
	}

	pthread_cleanup_pop(0);
	pthread_exit(NULL);
}

/* Fill the given structure with information about next appointment. */
unsigned notify_get_next(struct notify_app *a)
{
	time_t current_time;

	if (!a)
		return 0;

	current_time = time(NULL);

	a->time = current_time + DAYINSEC;
	a->got_app = 0;
	a->state = 0;
	a->txt = NULL;
	recur_apoint_check_next(a, current_time, get_today());
	apoint_check_next(a, current_time);

	return 1;
}

/* Return the description of next appointment to be notified. */
char *notify_app_txt(void)
{
	if (notify_app.got_app)
		return notify_app.txt;
	else
		return NULL;
}

/* Look for the next appointment within the next 24 hours. */
/* ARGSUSED0 */
static void *notify_thread_app(void *arg)
{
	struct notify_app tmp_app;
	int force = (arg ? 1 : 0);

	if (!notify_get_next(&tmp_app))
		pthread_exit(NULL);
	if (!tmp_app.got_app) {
		pthread_mutex_lock(&notify_app.mutex);
		notify_free_app();
		pthread_mutex_unlock(&notify_app.mutex);
	} else {
		if (force || !notify_same_item(tmp_app.time)) {
			pthread_mutex_lock(&notify_app.mutex);
			notify_update_app(tmp_app.time, tmp_app.state,
					  tmp_app.txt);
			pthread_mutex_unlock(&notify_app.mutex);
		}
	}

	if (tmp_app.txt)
		mem_free(tmp_app.txt);

	pthread_exit(NULL);
}

/* Launch the thread notify_thread_app to look for next appointment. */
void notify_check_next_app(int force)
{
	if (!notify_bar())
		return;

	pthread_t notify_t_app;
	void *arg = (force ? (void *)1 : NULL);

	pthread_create(&notify_t_app, &detached_thread_attr,
		       notify_thread_app, arg);
	return;
}

/* Check if the newly created appointment is to be notified. */
void notify_check_added(char *mesg, long start, char state)
{
	time_t current_time;
	int update_notify = 0;
	long gap;

	current_time = time(NULL);
	pthread_mutex_lock(&notify_app.mutex);
	if (!notify_app.got_app) {
		gap = start - current_time;
		if (gap >= 0 && gap <= DAYINSEC)
			update_notify = 1;
	} else if (start < notify_app.time && start >= current_time) {
		update_notify = 1;
	} else if (start == notify_app.time && state != notify_app.state) {
		update_notify = 1;
	}

	if (update_notify) {
		notify_update_app(start, state, mesg);
	}
	pthread_mutex_unlock(&notify_app.mutex);
}

/* Check if the newly repeated appointment is to be notified. */
void notify_check_repeated(struct recur_apoint *i)
{
	time_t current_time, real_app_time;
	int update_notify = 0;

	current_time = time(NULL);
	pthread_mutex_lock(&notify_app.mutex);
	if (recur_item_find_occurrence
	    (i->start, i->dur, &i->exc, i->rpt->type, i->rpt->freq,
	     i->rpt->until, get_today(), &real_app_time)) {
		if (!notify_app.got_app) {
			if (real_app_time - current_time <= DAYINSEC)
				update_notify = 1;
		} else if (real_app_time < notify_app.time
			   && real_app_time >= current_time) {
			update_notify = 1;
		} else if (real_app_time == notify_app.time
			   && i->state != notify_app.state) {
			update_notify = 1;
		}
	}
	if (update_notify) {
		notify_update_app(real_app_time, i->state, i->mesg);
	}
	pthread_mutex_unlock(&notify_app.mutex);
}

int notify_same_item(long time)
{
	int same = 0;

	pthread_mutex_lock(&(notify_app.mutex));
	if (notify_app.got_app && notify_app.time == time)
		same = 1;
	pthread_mutex_unlock(&(notify_app.mutex));

	return same;
}

int notify_same_recur_item(struct recur_apoint *i)
{
	int same = 0;
	time_t item_start = 0;

	recur_item_find_occurrence(i->start, i->dur, &i->exc, i->rpt->type,
				   i->rpt->freq, i->rpt->until,
				   get_today(), &item_start);
	pthread_mutex_lock(&notify_app.mutex);
	if (notify_app.got_app && item_start == notify_app.time)
		same = 1;
	pthread_mutex_unlock(&(notify_app.mutex));

	return same;
}

/* Launch the notify-bar main thread. */
void notify_start_main_thread(void)
{
        /* Avoid starting the notification bar thread twice. */
	notify_stop_main_thread();

	/*
	 * The shared notify_app structure may be stale (if the main
	 * thread was stopped and relaunched).
	 */
	notify_check_next_app(1);
	pthread_create(&notify_t_main, NULL, notify_main_thread, NULL);
}

/*
 * Print an option in the configuration menu.
 * Specific treatment is needed depending on if the option is of type boolean
 * (either YES or NO), or an option holding a string value.
 */
static void
print_option(WINDOW * win, unsigned x, unsigned y, char *name,
	     char *valstr, unsigned valbool, char *desc)
{
	const int MAXCOL = col - 3;
	int x_opt, len;

	x_opt = x + strlen(name);
	mvwprintw(win, y, x, "%s", name);
	erase_window_part(win, x_opt, y, MAXCOL, y);
	if ((len = strlen(valstr)) != 0) {
		unsigned maxlen = MAX(MAXCOL - x_opt - 2, 3);
		char buf[maxlen * UTF8_MAXLEN];

		strncpy(buf, valstr, maxlen * UTF8_MAXLEN);
		buf[maxlen * UTF8_MAXLEN - 1] = '\0';
		utf8_chop(buf, maxlen);

		custom_apply_attr(win, ATTR_HIGHEST);
		mvwaddstr(win, y, x_opt, buf);
		custom_remove_attr(win, ATTR_HIGHEST);
	} else {
		print_bool_option_incolor(win, valbool, y, x_opt);
	}
	mvwaddstr(win, y + 1, x, desc);
}

/* Print options related to the notify-bar. */
static void print_config_option(int i, WINDOW *win, int y, int hilt, void *cb_data)
{
	enum { SHOW, DATE, CLOCK, WARN, CMD, NOTIFYALL, DMON, DMON_LOG,
		    NB_OPT };

	struct opt_s {
		char *name;
		char *desc;
		char valstr[BUFSIZ];
		unsigned valnum;
	} opt[NB_OPT];

	opt[SHOW].name = "appearance.notifybar = ";
	opt[SHOW].desc =
	    _("(if set to YES, notify-bar will be displayed)");

	opt[DATE].name = "format.notifydate = ";
	opt[DATE].desc =
	    _("(Format of the date to be displayed inside notify-bar)");

	opt[CLOCK].name = "format.notifytime = ";
	opt[CLOCK].desc =
	    _("(Format of the time to be displayed inside notify-bar)");

	opt[WARN].name = "notification.warning = ";
	opt[WARN].desc = _("(Warn user if an appointment is within next "
			   "'notify-bar_warning' seconds)");

	opt[CMD].name = "notification.command = ";
	opt[CMD].desc =
	    _("(Command used to notify user of an upcoming appointment)");

	opt[NOTIFYALL].name = "notification.notifyall = ";
	opt[NOTIFYALL].desc =
	    _("(Notify all appointments instead of flagged ones only)");

	opt[DMON].name = "daemon.enable = ";
	opt[DMON].desc =
	    _("(Run in background to get notifications after exiting)");

	opt[DMON_LOG].name = "daemon.log = ";
	opt[DMON_LOG].desc =
	    _("(Log activity when running in background)");

	pthread_mutex_lock(&nbar.mutex);

	/* String value options */
	strncpy(opt[DATE].valstr, nbar.datefmt, BUFSIZ);
	strncpy(opt[CLOCK].valstr, nbar.timefmt, BUFSIZ);
	snprintf(opt[WARN].valstr, BUFSIZ, "%d", nbar.cntdwn);
	strncpy(opt[CMD].valstr, nbar.cmd, BUFSIZ);

	/* Boolean options */
	opt[SHOW].valnum = nbar.show;
	pthread_mutex_unlock(&nbar.mutex);

	opt[DMON].valnum = dmon.enable;
	opt[DMON_LOG].valnum = dmon.log;

	opt[SHOW].valstr[0] = opt[DMON].valstr[0] =
		opt[DMON_LOG].valstr[0] = '\0';

	opt[NOTIFYALL].valnum = nbar.notify_all;
	if (opt[NOTIFYALL].valnum == NOTIFY_FLAGGED_ONLY)
		strcpy(opt[NOTIFYALL].valstr, "flagged-only");
	else if (opt[NOTIFYALL].valnum == NOTIFY_UNFLAGGED_ONLY)
		strcpy(opt[NOTIFYALL].valstr, "unflagged-only");
	else if (opt[NOTIFYALL].valnum == NOTIFY_ALL)
		strcpy(opt[NOTIFYALL].valstr, "all");

	if (hilt)
		custom_apply_attr(win, ATTR_HIGHEST);

	print_option(win, 1, y, opt[i].name, opt[i].valstr,
		     opt[i].valnum, opt[i].desc);

	if (hilt)
		custom_remove_attr(win, ATTR_HIGHEST);
}

static enum listbox_row_type config_option_row_type(int i, void *cb_data)
{
	return LISTBOX_ROW_TEXT;
}

static int config_option_height(int i, void *cb_data)
{
	return 3;
}

static void config_option_edit(int i)
{
	char *buf;
	const char *date_str =
	    _("Enter the date format (see 'man 3 strftime' for possible formats) ");
	const char *time_str =
	    _("Enter the time format (see 'man 3 strftime' for possible formats) ");
	const char *count_str =
	    _("Enter the number of seconds (0 not to be warned before an appointment)");
	const char *cmd_str = _("Enter the notification command ");

	buf = mem_malloc(BUFSIZ);
	buf[0] = '\0';

	switch (i) {
	case 0:
		pthread_mutex_lock(&nbar.mutex);
		nbar.show = !nbar.show;
		pthread_mutex_unlock(&nbar.mutex);
		if (notify_bar())
			notify_start_main_thread();
		else
			notify_stop_main_thread();
		resize = 1;
		break;
	case 1:
		status_mesg(date_str, "");
		pthread_mutex_lock(&nbar.mutex);
		strncpy(buf, nbar.datefmt, BUFSIZ);
		buf[BUFSIZ - 1] = '\0';
		pthread_mutex_unlock(&nbar.mutex);
		if (updatestring(win[STA].p, &buf, 0, 1) == 0) {
			pthread_mutex_lock(&nbar.mutex);
			strncpy(nbar.datefmt, buf, BUFSIZ);
			nbar.datefmt[BUFSIZ - 1] = '\0';
			pthread_mutex_unlock(&nbar.mutex);
		}
		break;
	case 2:
		status_mesg(time_str, "");
		pthread_mutex_lock(&nbar.mutex);
		strncpy(buf, nbar.timefmt, BUFSIZ);
		buf[BUFSIZ - 1] = '\0';
		pthread_mutex_unlock(&nbar.mutex);
		if (updatestring(win[STA].p, &buf, 0, 1) == 0) {
			pthread_mutex_lock(&nbar.mutex);
			strncpy(nbar.timefmt, buf, BUFSIZ);
			nbar.timefmt[BUFSIZ - 1] = '\0';
			pthread_mutex_unlock(&nbar.mutex);
		}
		break;
	case 3:
		status_mesg(count_str, "");
		pthread_mutex_lock(&nbar.mutex);
		snprintf(buf, BUFSIZ, "%d", nbar.cntdwn);
		pthread_mutex_unlock(&nbar.mutex);
		if (updatestring(win[STA].p, &buf, 0, 1) == 0 &&
		    is_all_digit(buf) && atoi(buf) >= 0
		    && atoi(buf) <= DAYINSEC) {
			pthread_mutex_lock(&nbar.mutex);
			nbar.cntdwn = atoi(buf);
			pthread_mutex_unlock(&nbar.mutex);
		}
		break;
	case 4:
		status_mesg(cmd_str, "");
		pthread_mutex_lock(&nbar.mutex);
		strncpy(buf, nbar.cmd, BUFSIZ);
		buf[BUFSIZ - 1] = '\0';
		pthread_mutex_unlock(&nbar.mutex);
		if (updatestring(win[STA].p, &buf, 0, 1) == 0) {
			pthread_mutex_lock(&nbar.mutex);
			strncpy(nbar.cmd, buf, BUFSIZ);
			nbar.cmd[BUFSIZ - 1] = '\0';
			pthread_mutex_unlock(&nbar.mutex);
		}
		break;
	case 5:
		pthread_mutex_lock(&nbar.mutex);
		nbar.notify_all = (nbar.notify_all + 1) % 3;
		pthread_mutex_unlock(&nbar.mutex);
		notify_check_next_app(1);
		break;
	case 6:
		dmon.enable = !dmon.enable;
		break;
	case 7:
		dmon.log = !dmon.log;
		break;
	}

	mem_free(buf);
}

/* Notify-bar configuration. */
void notify_config_bar(void)
{
	static int bindings[] = {
		KEY_GENERIC_QUIT, KEY_MOVE_UP, KEY_MOVE_DOWN, KEY_EDIT_ITEM
	};
	struct listbox lb;
	int key;

	clear();
	listbox_init(&lb, 0, 0, notify_bar() ? row - 3 : row - 2, col,
		     _("notification options"), config_option_row_type,
		     config_option_height, print_config_option);
	listbox_load_items(&lb, 8);
	listbox_draw_deco(&lb, 0);
	listbox_display(&lb, NOHILT);
	wins_set_bindings(bindings, ARRAY_SIZE(bindings));
	wins_status_bar();
	wnoutrefresh(win[STA].p);
	wmove(win[STA].p, 0, 0);
	wins_doupdate();

	while ((key = keys_get(win[KEY].p, NULL, NULL)) != KEY_GENERIC_QUIT) {
		switch (key) {
		case KEY_MOVE_DOWN:
			listbox_sel_move(&lb, 1);
			break;
		case KEY_MOVE_UP:
			listbox_sel_move(&lb, -1);
			break;
		case KEY_EDIT_ITEM:
			config_option_edit(listbox_get_sel(&lb));
			break;
		}

		if (resize) {
			resize = 0;
			wins_get_config();
			wins_reset_noupdate();
			listbox_resize(&lb, 0, 0, notify_bar() ? row - 3 : row - 2, col);
			listbox_draw_deco(&lb, 0);
			delwin(win[STA].p);
			win[STA].p =
			    newwin(win[STA].h, win[STA].w, win[STA].y,
				   win[STA].x);
			keypad(win[STA].p, TRUE);
			if (notify_bar()) {
				notify_reinit_bar();
			}
			clearok(curscr, TRUE);
		}

		listbox_display(&lb, NOHILT);
		wins_status_bar();
		wnoutrefresh(win[STA].p);
		wmove(win[STA].p, 0, 0);
		wins_doupdate();
	}

	listbox_delete(&lb);
}
