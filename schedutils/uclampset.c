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
 */

#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "c.h"
#include "nls.h"
#include "closestream.h"
#include "strutils.h"
#include "procutils.h"
#include "sched_attr.h"

#define PROCFS(file)	"/proc/sys/kernel/" #file

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
	" uclampset [options] <util_min> <util_max> [cmd <arg>...]\n"
	" uclampset [options] --pid <util_min> <util_max> <pid>\n"), out);
	fputs(USAGE_SEPARATOR, out);
	fputs(_("Get util clamp for a process:\n"
	" uclampset [options] -p <pid>\n"), out);

	fputs(USAGE_SEPARATOR, out);
	fputs(_("Set util clamp for the sytem:\n"
	" uclampset [options] --system <util_min> <util_max>\n"), out);
	fputs(USAGE_SEPARATOR, out);
	fputs(_("Get util clamp for the system:\n"
	" uclampset [options] -s\n"), out);

	fputs(USAGE_SEPARATOR, out);
	fputs(_("Other options:\n"), out);
	fputs(_(" -a, --all-tasks      operate on all the tasks (threads) for a given pid\n"), out);
	fputs(_(" -p, --pid            operate on existing given pid\n"), out);
	fputs(_(" -s, --system         operate on system\n"), out);
	fputs(_(" -m, --max            show min and max valid clamp values\n"), out);
	fputs(_(" -v, --verbose        display status information\n"), out);

	fputs(USAGE_SEPARATOR, out);
	printf(USAGE_HELP_OPTIONS(22));

	printf(USAGE_MAN_TAIL("uclampset(1)"));
	exit(EXIT_SUCCESS);
}

static void proc_pid_name(pid_t pid, char *name, int len)
{
	char *proc_comm_fmt = "/proc/%d/comm";
	char proc_comm[32];
	FILE *fp;
	int size;

	size = snprintf(proc_comm, 32, proc_comm_fmt, pid);
	if (size >= 32 || size < 0)
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
	char comm[128];

	/* don't display "pid 0" as that is confusing */
	if (!pid)
		pid = getpid();


	proc_pid_name(pid, comm, 128);

	if (sched_getattr(pid, &sa, sizeof(sa), 0) != 0)
		err(EXIT_FAILURE, _("failed to get pid %d's uclamp values"), pid);

	printf(_("%s-%d\n\tutil_min: %d\n\tutil_max: %d\n"),
		  comm, pid, sa.sched_util_min, sa.sched_util_max);
#else
	err(EXIT_FAILURE, _("uclamp is not supported on this system"));
#endif
}

static void show_uclamp_system_info(void)
{
	unsigned int size;
	FILE *fp_min;
	FILE *fp_max;
	char min[16];
	char max[16];

	fp_min = fopen(PROCFS(sched_util_clamp_min), "r");
	if (!fp_min)
		err(EXIT_FAILURE, _("cannot open %s"), PROCFS(sched_util_min));

	fp_max = fopen(PROCFS(sched_util_clamp_max), "r");
	if (!fp_max) {
		fclose(fp_min);
		err(EXIT_FAILURE, _("cannot open %s"), PROCFS(sched_util_max));
	}

	size = fread(min, 1, sizeof(min), fp_min);
	min[size-1] = '\0';
	size = fread(max, 1, sizeof(max), fp_max);
	max[size-1] = '\0';

	if (ferror(fp_min)) {
		fclose(fp_min);
		fclose(fp_max);
		err(EXIT_FAILURE, _("error writing %s"), PROCFS(sched_util_min));
	}

	if (ferror(fp_max)) {
		fclose(fp_min);
		fclose(fp_max);
		err(EXIT_FAILURE, _("error writing %s"), PROCFS(sched_util_max));
	}

	fclose(fp_min);
	fclose(fp_max);

	printf(_("System\n\tutil_min: %s\n\tutil_max: %s\n"), min, max);
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
	} else
		show_uclamp_pid_info(ctl->pid);
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

	sa.sched_util_min = ctl->util_min;
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

	} else if (set_uclamp_one(ctl, ctl->pid) == -1)
		err(EXIT_FAILURE, _("failed to set pid %d's uclamp values"), ctl->pid);
}

static void set_uclamp_system(struct uclampset *ctl)
{
	unsigned int size;
	FILE *fp_min;
	FILE *fp_max;
	char min[16];
	char max[16];

	if (ctl->util_min > ctl->util_max) {
		errno = EINVAL;
		err(EXIT_FAILURE, _("util_min must be <= util_max"));
	}

	fp_min = fopen(PROCFS(sched_util_clamp_min), "w");
	if (!fp_min)
		err(EXIT_FAILURE, _("cannot open %s"), PROCFS(sched_util_min));

	fp_max = fopen(PROCFS(sched_util_clamp_max), "w");
	if (!fp_max) {
		fclose(fp_min);
		err(EXIT_FAILURE, _("cannot open %s"), PROCFS(sched_util_max));
	}

	size = snprintf(min, sizeof(min), "%d", ctl->util_min);
	min[size] = '\n';
	min[size+1] = '\0';
	fwrite(min, 1, sizeof(min), fp_min);

	size = snprintf(max, sizeof(max), "%d", ctl->util_max);
	max[size] = '\n';
	max[size+1] = '\0';
	fwrite(max, 1, sizeof(max), fp_max);

	if (ferror(fp_min)) {
		fclose(fp_min);
		fclose(fp_max);
		err(EXIT_FAILURE, _("error writing %s"), PROCFS(sched_util_min));
	}

	if (ferror(fp_max)) {
		fclose(fp_min);
		fclose(fp_max);
		err(EXIT_FAILURE, _("error writing %s"), PROCFS(sched_util_max));
	}

	fclose(fp_min);
	fclose(fp_max);
}

int main(int argc, char **argv)
{
	struct uclampset _ctl = { .pid = -1 }, *ctl = &_ctl;
	int c;

	static const struct option longopts[] = {
		{ "all-tasks",  no_argument, NULL, 'a' },
		{ "pid",	no_argument, NULL, 'p' },
		{ "system",	no_argument, NULL, 's' },
		{ "help",	no_argument, NULL, 'h' },
		{ "max",        no_argument, NULL, 'm' },
		{ "verbose",	no_argument, NULL, 'v' },
		{ "version",	no_argument, NULL, 'V' },
		{ NULL,		no_argument, NULL, 0 }
	};

	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);
	close_stdout_atexit();

	while((c = getopt_long(argc, argv, "+asphmvV", longopts, NULL)) != -1)
	{
		switch (c) {
		case 'a':
			ctl->all_tasks = 1;
			break;
		case 'm':
			show_min_max();
			return EXIT_SUCCESS;
		case 'p':
			errno = 0;
			ctl->pid = strtos32_or_err(argv[argc - 1], _("invalid PID argument"));
			break;
		case 's':
			ctl->system = 1;
			break;
		case 'v':
			ctl->verbose = 1;
			break;
		case 'V':
			print_version(EXIT_SUCCESS);
		case 'h':
			usage();
		default:
			errtryhelp(EXIT_FAILURE);
		}
	}

	if (((ctl->pid > -1) && argc - optind < 1) ||
	    ((ctl->pid == -1) && argc - optind < 2 && !ctl->system)) {
		warnx(_("bad usage3"));
		errtryhelp(EXIT_FAILURE);
}

	if ((ctl->pid > -1) && (ctl->verbose || argc - optind == 1)) {
		show_uclamp_info(ctl);
		if (argc - optind == 1)
			return EXIT_SUCCESS;
	}

	if ((ctl->system) && (ctl->verbose || argc - optind < 2)) {
		show_uclamp_info(ctl);
		if (argc - optind < 2)
			return EXIT_SUCCESS;
	}

	if (ctl->pid == -1)
		ctl->pid = 0;

	if ((ctl->system && argc - optind == 2) || (argc - optind >= 3)) {
		ctl->util_min = strtos32_or_err(argv[optind], _("invalid util_min argument"));
		ctl->util_max = strtos32_or_err(argv[optind+1], _("invalid util_max argument"));
		printf("Setting util_min: %d util_max: %d\n", ctl->util_min, ctl->util_max);
	} else {
		warnx(_("bad usage"));
		errtryhelp(EXIT_FAILURE);
	}

	if (ctl->system) {
		/*
		 * A bit of a hack to deal with the fact that the order of the
		 * right matters depending on the previous values of the system
		 * knobs.
		 *
		 * For example:
		 *
		 *	uclampset -s 512 512
		 *
		 * followed by
		 *
		 *	uclampset -s 1024 1024
		 *
		 * Would result in
		 *
		 * System
		 *	util_min: 512
		 *	util_max: 1024
		 *
		 * because of the rule that util_min must be <= util_max
		 *
		 * By writing 1024 to util_min first, we temporarily end up
		 * with util_min (1024) > util_max (512), hence it fails.
		 *
		 * The same situatioin could happen in other ways. Rather than
		 * encode the rules, doing the update twice should work.
		 */
		set_uclamp_system(ctl);
		set_uclamp_system(ctl);
	} else {
		set_uclamp_pid(ctl);
	}

	if (ctl->verbose)
		show_uclamp_info(ctl);

	if (!ctl->pid && !ctl->system) {
		argv += optind + 2;
		execvp(argv[0], argv);
		errexec(argv[0]);
	}

	return EXIT_SUCCESS;
}
