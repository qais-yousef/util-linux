/*
 * uclampset.c - change utilization clamping attributes of a task or the system
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as
 * published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright (C) 2020 Qais Yousef
 * Copyright (C) 2020 Arm Ltd
 */

#include <errno.h>
#include <getopt.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "closestream.h"
#include "procutils.h"
#include "sched_attr.h"
#include "strutils.h"

#define PROCFS(file)	"/proc/sys/kernel/" #file

#define PROCFS_UCLAMP_MIN	PROCFS(sched_util_clamp_min)
#define PROCFS_UCLAMP_MAX	PROCFS(sched_util_clamp_max)

#define MAX_OPT		1000
#define COMM_LEN	64
#define NOT_SET		-1U

struct uclampset {
	unsigned int util_min;
	unsigned int util_max;

	pid_t pid;
	unsigned int all_tasks;			/* all threads of the PID */

	unsigned int system;

	unsigned int verbose;
};

static void __attribute__((__noreturn__)) usage(void)
{
	FILE *out = stdout;

	fputs(_("Show or change the utilization clamping attributes of a process or the system.\n"), out);
	fputs(USAGE_SEPARATOR, out);
	fputs(_("Set util clamp for a process:\n"
	" uclampset [options] [-m <util_min>] [-M <util_max>] [cmd <arg>...]\n"
	" uclampset [options] [-m <util_min>] [-M <util_max>] --pid <pid>\n"), out);
	fputs(USAGE_SEPARATOR, out);
	fputs(_("Get util clamp for a process:\n"
	" uclampset [options] -p <pid>\n"), out);

	fputs(USAGE_SEPARATOR, out);
	fputs(_("Set util clamp for the sytem:\n"
	" uclampset [options] --system [-m <util_min>] [-M <util_max>]\n"), out);
	fputs(USAGE_SEPARATOR, out);
	fputs(_("Get util clamp for the system:\n"
	" uclampset [options] -s\n"), out);

	fputs(USAGE_SEPARATOR, out);
	fputs(_("Other options:\n"), out);
	fputs(_(" -m                   util_min value to set\n"), out);
	fputs(_(" -M                   util_max value to set\n"), out);
	fputs(_(" -a, --all-tasks      operate on all the tasks (threads) for a given pid\n"), out);
	fputs(_(" -p, --pid            operate on existing given pid\n"), out);
	fputs(_(" -s, --system         operate on system\n"), out);
	fputs(_(" --max                show min and max valid uclamp values\n"), out);
	fputs(_(" -v, --verbose        display status information\n"), out);

	fputs(USAGE_SEPARATOR, out);
	printf(USAGE_HELP_OPTIONS(22));

	printf(USAGE_MAN_TAIL("uclampset(1)"));
	exit(EXIT_SUCCESS);
}

static void proc_pid_name(pid_t pid, char *name, int len)
{
	char *proc_comm_fmt = "/proc/%d/comm";
	char proc_comm[COMM_LEN];
	FILE *fp;
	int size;

	size = snprintf(proc_comm, COMM_LEN, proc_comm_fmt, pid);
	if (size >= COMM_LEN || size < 0)
		goto error;

	fp = fopen(proc_comm, "r");
	if (!fp)
		goto error;

	size = fread(name, 1, len, fp);
	name[size-1] = '\0';

	fclose(fp);

	if (ferror(fp))
		goto error;

	return;
error:
	strncpy(name, "unknown", len);
}

static void show_uclamp_pid_info(pid_t pid)
{
#ifdef HAVE_SCHED_SETATTR
	struct sched_attr sa;
	char comm[COMM_LEN];

	/* don't display "pid 0" as that is confusing */
	if (!pid)
		pid = getpid();


	proc_pid_name(pid, comm, COMM_LEN);

	if (sched_getattr(pid, &sa, sizeof(sa), 0) != 0)
		err(EXIT_FAILURE, _("failed to get pid %d's uclamp values"), pid);

	printf(_("%s-%d\n\tutil_min: %d\n\tutil_max: %d\n"),
		  comm, pid, sa.sched_util_min, sa.sched_util_max);
#else
	err(EXIT_FAILURE, _("uclamp is not supported on this system"));
#endif
}

static unsigned int read_uclamp_sysfs(char *dir)
{
	unsigned int size;
	char buf[16];
	FILE *fp;

	fp = fopen(dir, "r");
	if (!fp)
		err(EXIT_FAILURE, _("cannot open %s"), dir);

	size = fread(buf, 1, sizeof(buf), fp);
	buf[size-1] = '\0';

	if (ferror(fp)) {
		fclose(fp);
		err(EXIT_FAILURE, _("error writing %s"), dir);
	}

	fclose(fp);

	return strtou32_or_err(buf, _("invalid util clamp value"));
}

static void write_uclamp_sysfs(char *dir, unsigned int val)
{
	unsigned int size;
	char buf[16];
	FILE *fp;

	fp = fopen(dir, "w");
	if (!fp)
		err(EXIT_FAILURE, _("cannot open %s"), dir);

	size = snprintf(buf, sizeof(buf), "%d", val);
	buf[size] = '\n';
	buf[size+1] = '\0';
	fwrite(buf, 1, sizeof(buf), fp);

	if (ferror(fp)) {
		fclose(fp);
		err(EXIT_FAILURE, _("error writing %s"), dir);
	}

	fclose(fp);
}

static void show_uclamp_system_info(void)
{
	unsigned int min, max;

	min = read_uclamp_sysfs(PROCFS_UCLAMP_MIN);
	max = read_uclamp_sysfs(PROCFS_UCLAMP_MAX);

	printf(_("System\n\tutil_min: %u\n\tutil_max: %u\n"), min, max);
}

static void show_uclamp_info(struct uclampset *ctl)
{
	if (ctl->system) {
		show_uclamp_system_info();
	} else if (ctl->all_tasks) {
		pid_t tid;
		struct proc_tasks *ts = proc_open_tasks(ctl->pid);

		if (!ts)
			err(EXIT_FAILURE, _("cannot obtain the list of tasks"));

		while (!proc_next_tid(ts, &tid))
			show_uclamp_pid_info(tid);

		proc_close_tasks(ts);
	} else {
		show_uclamp_pid_info(ctl->pid);
	}
}

static void show_min_max(void)
{
	printf(_("util_min and util_max must be in the range of [0:1024] inclusive\n"));
}

#ifndef HAVE_SCHED_SETATTR
static int set_uclamp_one(struct uclampset *ctl, pid_t pid)
{
	err(EXIT_FAILURE, _("uclamp is not supported on this system"));
}

#else /* !HAVE_SCHED_SETATTR */
static int set_uclamp_one(struct uclampset *ctl, pid_t pid)
{
	struct sched_attr sa;

	if (sched_getattr(pid, &sa, sizeof(sa), 0) != 0)
		err(EXIT_FAILURE, _("failed to get pid %d's uclamp values"), pid);

	if (ctl->util_min != NOT_SET)
		sa.sched_util_min = ctl->util_min;
	if (ctl->util_max != NOT_SET)
		sa.sched_util_max = ctl->util_max;

	sa.sched_flags = SCHED_FLAG_KEEP_POLICY |
			 SCHED_FLAG_KEEP_PARAMS |
			 SCHED_FLAG_UTIL_CLAMP_MIN |
			 SCHED_FLAG_UTIL_CLAMP_MAX;

	return sched_setattr(pid, &sa, 0);
}
#endif /* HAVE_SCHED_SETATTR */

static void set_uclamp_pid(struct uclampset *ctl)
{
	if (ctl->all_tasks) {
		pid_t tid;
		struct proc_tasks *ts = proc_open_tasks(ctl->pid);

		if (!ts)
			err(EXIT_FAILURE, _("cannot obtain the list of tasks"));

		while (!proc_next_tid(ts, &tid))
			if (set_uclamp_one(ctl, tid) == -1)
				err(EXIT_FAILURE, _("failed to set tid %d's uclamp values"), tid);

		proc_close_tasks(ts);

	} else if (set_uclamp_one(ctl, ctl->pid) == -1) {
		err(EXIT_FAILURE, _("failed to set pid %d's uclamp values"), ctl->pid);
	}
}

static void set_uclamp_system(struct uclampset *ctl)
{
	if (ctl->util_min == NOT_SET)
		ctl->util_min = read_uclamp_sysfs(PROCFS_UCLAMP_MIN);

	if (ctl->util_max == NOT_SET)
		ctl->util_max = read_uclamp_sysfs(PROCFS_UCLAMP_MAX);

	if (ctl->util_min > ctl->util_max) {
		errno = EINVAL;
		err(EXIT_FAILURE, _("util_min must be <= util_max"));
	}

	write_uclamp_sysfs(PROCFS_UCLAMP_MIN, ctl->util_min);
	write_uclamp_sysfs(PROCFS_UCLAMP_MAX, ctl->util_max);
}

int main(int argc, char **argv)
{
	struct uclampset _ctl = {
		.pid = -1,
		.util_min = NOT_SET,
		.util_max = NOT_SET
	};
	struct uclampset *ctl = &_ctl;
	bool no_input;
	int c;

	static const struct option longopts[] = {
		{ "all-tasks",  no_argument, NULL, 'a' },
		{ "pid",	no_argument, NULL, 'p' },
		{ "system",	no_argument, NULL, 's' },
		{ "help",	no_argument, NULL, 'h' },
		{ "max",        no_argument, NULL, MAX_OPT },
		{ "verbose",	no_argument, NULL, 'v' },
		{ "version",	no_argument, NULL, 'V' },
		{ NULL,		no_argument, NULL, 0 }
	};

	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);
	close_stdout_atexit();

	while((c = getopt_long(argc, argv, "+asphmMvV", longopts, NULL)) != -1)
	{
		switch (c) {
		case 'a':
			ctl->all_tasks = 1;
			break;
		case MAX_OPT:
			show_min_max();
			return EXIT_SUCCESS;
		case 'p':
			errno = 0;
			ctl->pid = strtos32_or_err(argv[optind], _("invalid PID argument"));
			optind++;
			break;
		case 's':
			ctl->system = 1;
			break;
		case 'v':
			ctl->verbose = 1;
			break;
		case 'm':
			ctl->util_min = strtos32_or_err(argv[optind], _("invalid util_min argument"));
			optind++;
			break;
		case 'M':
			ctl->util_max = strtos32_or_err(argv[optind], _("invalid util_max argument"));
			optind++;
			break;
		case 'V':
			print_version(EXIT_SUCCESS);
			/* fallthrough */
		case 'h':
			usage();
		default:
			errtryhelp(EXIT_FAILURE);
		}
	}

	no_input = ctl->util_min == NOT_SET && ctl->util_max == NOT_SET;

	if (no_input) {
		show_uclamp_info(ctl);
		return EXIT_SUCCESS;
	}

	if (ctl->pid == -1)
		ctl->pid = 0;

	if (ctl->system)
		set_uclamp_system(ctl);
	else
		set_uclamp_pid(ctl);

	if (ctl->verbose)
		show_uclamp_info(ctl);

	if (!ctl->pid && !ctl->system) {
		argv += optind;
		execvp(argv[0], argv);
		errexec(argv[0]);
	}

	return EXIT_SUCCESS;
}
