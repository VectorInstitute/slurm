/*****************************************************************************\
 *  cgroup_v1.c - Cgroup v1 plugin
 *****************************************************************************
 *  Copyright (C) 2021 SchedMD LLC
 *  Written by Felip Moll <felip.moll@schedmd.com>
 *
 *  This file is part of Slurm, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  Slurm is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  Slurm is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Slurm; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#define _GNU_SOURCE

#include "cgroup_v1.h"

/*
 * These variables are required by the generic plugin interface.  If they
 * are not found in the plugin, the plugin loader will ignore it.
 *
 * plugin_name - a string giving a human-readable description of the
 * plugin.  There is no maximum length, but the symbol must refer to
 * a valid string.
 *
 * plugin_type - a string suggesting the type of the plugin or its
 * applicability to a particular form of data or method of data handling.
 * If the low-level plugin API is used, the contents of this string are
 * unimportant and may be anything.  Slurm uses the higher-level plugin
 * interface which requires this string to be of the form
 *
 *	<application>/<method>
 *
 * where <application> is a description of the intended application of
 * the plugin (e.g., "select" for Slurm node selection) and <method>
 * is a description of how this plugin satisfies that application.  Slurm will
 * only load select plugins if the plugin_type string has a
 * prefix of "select/".
 *
 * plugin_version - an unsigned 32-bit integer containing the Slurm version
 * (major.minor.micro combined into a single number).
 */
const char plugin_name[] = "Cgroup v1 plugin";
const char plugin_type[] = "cgroup/v1";
const uint32_t plugin_version = SLURM_VERSION_NUMBER;

static char g_user_cgpath[CG_CTL_CNT][PATH_MAX];
static char g_job_cgpath[CG_CTL_CNT][PATH_MAX];
static char g_step_cgpath[CG_CTL_CNT][PATH_MAX];
static uint16_t g_step_active_cnt[CG_CTL_CNT];

static xcgroup_ns_t g_cg_ns[CG_CTL_CNT];

static xcgroup_t g_root_cg[CG_CTL_CNT];
static xcgroup_t g_user_cg[CG_CTL_CNT];
static xcgroup_t g_job_cg[CG_CTL_CNT];
static xcgroup_t g_step_cg[CG_CTL_CNT];
static xcgroup_t g_sys_cg[CG_CTL_CNT];

const char *g_cg_name[CG_CTL_CNT] = {
	"freezer",
	"cpuset",
	"memory",
	"devices",
	"cpuacct"
};

/* Cgroup v1 control items for the oom monitor */
#define STOP_OOM 0x987987987

typedef struct {
	int cfd;	/* control file fd. */
	int efd;	/* event file fd. */
	int event_fd;	/* eventfd fd. */
} oom_event_args_t;

static bool oom_thread_created = false;
static uint64_t oom_kill_count = 0;
static int oom_pipe[2] = { -1, -1 };
static pthread_t oom_thread;
static pthread_mutex_t oom_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Accounting artifacts */
List g_task_acct_list[CG_CTL_CNT];
static uint32_t g_max_task_id = 0;
/*
 * There are potentially multiple tasks on a node, so we want to
 * track every task cgroup and which taskid it belongs to.
 */
typedef struct {
	xcgroup_t task_cg;
	uint32_t taskid;
} task_cg_info_t;

static int _cgroup_init(cgroup_ctl_type_t sub)
{
	if (sub >= CG_CTL_CNT)
		return SLURM_ERROR;

	if (xcgroup_ns_create(&g_cg_ns[sub], "", g_cg_name[sub])
	    != SLURM_SUCCESS) {
		error("unable to create %s cgroup namespace", g_cg_name[sub]);
		return SLURM_ERROR;
	}

	if (common_cgroup_create(&g_cg_ns[sub], &g_root_cg[sub], "", 0, 0)
	    != SLURM_SUCCESS) {
		error("unable to create root %s xcgroup", g_cg_name[sub]);
		common_cgroup_ns_destroy(&g_cg_ns[sub]);
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}

static int _cpuset_create(stepd_step_rec_t *job)
{
	int rc;
	char *slurm_cgpath, *sys_cgpath = NULL;
	xcgroup_t slurm_cg;
	char *value;
	size_t cpus_size;

	/* create slurm root cg in this cg namespace */
	slurm_cgpath = xcgroup_create_slurm_cg(&g_cg_ns[CG_CPUS]);
	if (slurm_cgpath == NULL)
		return SLURM_ERROR;

	/* check that this cgroup has cpus allowed or initialize them */
	if (xcgroup_load(&g_cg_ns[CG_CPUS], &slurm_cg, slurm_cgpath) !=
	    SLURM_SUCCESS){
		error("unable to load slurm cpuset xcgroup");
		xfree(slurm_cgpath);
		return SLURM_ERROR;
	}

	rc = common_cgroup_get_param(&slurm_cg, "cpuset.cpus", &value,
				     &cpus_size);

	if ((rc != SLURM_SUCCESS) || (cpus_size == 1)) {
		/* initialize the cpusets as it was non-existent */
		if (xcgroup_cpuset_init(&slurm_cg) != SLURM_SUCCESS) {
			xfree(slurm_cgpath);
			common_cgroup_destroy(&slurm_cg);
			return SLURM_ERROR;
		}
	}
	if (job == NULL) {
		/* This is a request to create a cpuset for slurmd daemon */
		xstrfmtcat(sys_cgpath, "%s/system", slurm_cgpath);

		/* create system cgroup in the cpuset ns */
		if ((rc = common_cgroup_create(&g_cg_ns[CG_CPUS],
					       &g_sys_cg[CG_CPUS],
					       sys_cgpath, getuid(), getgid()))
		    != SLURM_SUCCESS) {
			goto end;
		}
		if ((rc = common_cgroup_instantiate(&g_sys_cg[CG_CPUS]))
		    != SLURM_SUCCESS)
			goto end;

		/* set notify on release flag */
		common_cgroup_set_param(&g_sys_cg[CG_CPUS], "notify_on_release",
					"0");

		if ((rc = xcgroup_cpuset_init(&g_sys_cg[CG_CPUS]))
		    != SLURM_SUCCESS)
			goto end;

		debug("system cgroup: system cpuset cgroup initialized");
	} else {
		rc = xcgroup_create_hierarchy(__func__,
					      job,
					      &g_cg_ns[CG_CPUS],
					      &g_job_cg[CG_CPUS],
					      &g_step_cg[CG_CPUS],
					      &g_user_cg[CG_CPUS],
					      g_job_cgpath[CG_CPUS],
					      g_step_cgpath[CG_CPUS],
					      g_user_cgpath[CG_CPUS]);
	}

end:
	xfree(sys_cgpath);
	xfree(slurm_cgpath);
	common_cgroup_destroy(&slurm_cg);

	return rc;
}

static int _remove_cg_subsystem(xcgroup_t *root_cg, xcgroup_t *step_cg,
				xcgroup_t *job_cg, xcgroup_t *user_cg,
				const char *log_str)
{
	int rc = SLURM_SUCCESS;

	/*
	 * Always try to move slurmstepd process to the root cgroup, otherwise
	 * the rmdir(2) triggered by the calls below will always fail if the pid
	 * of stepd is in the cgroup. We don't know what other plugins will do
	 * and whether they will attach the stepd pid to the cg.
	 */
	rc = common_cgroup_move_process(root_cg, getpid());
	if (rc != SLURM_SUCCESS) {
		error("Unable to move pid %d to root cgroup", getpid());
		goto end;
	}
	xcgroup_wait_pid_moved(step_cg, log_str);

	/*
	 * Lock the root cgroup so we don't race with other steps that are being
	 * started.
	 */
	if (xcgroup_lock(root_cg) != SLURM_SUCCESS) {
		error("xcgroup_lock error (%s)", log_str);
		return SLURM_ERROR;
	}

	/* Delete step cgroup. */
	if ((rc = common_cgroup_delete(step_cg)) != SLURM_SUCCESS) {
		debug2("unable to remove step cg (%s): %m", log_str);
		goto end;
	}

	/*
	 * At this point we'll do a best effort for the job and user cgroup,
	 * since other jobs or steps may still be alive and not let us complete
	 * the cleanup. The last job/step in the hierarchy will be the one which
	 * will finally remove these two directories
	 */
	/* Delete job cgroup. */
	if ((rc = common_cgroup_delete(job_cg)) != SLURM_SUCCESS) {
		debug2("not removing job cg (%s): %m", log_str);
		rc = SLURM_SUCCESS;
		goto end;
	}
	/* Delete user cgroup. */
	if ((rc = common_cgroup_delete(user_cg)) != SLURM_SUCCESS) {
		debug2("not removing user cg (%s): %m", log_str);
		rc = SLURM_SUCCESS;
		goto end;
	}

	/*
	 * Invalidate the cgroup structs.
	 */
	common_cgroup_destroy(user_cg);
	common_cgroup_destroy(job_cg);
	common_cgroup_destroy(step_cg);

end:
	xcgroup_unlock(root_cg);
	return rc;
}

static int _rmdir_task(void *x, void *arg)
{
	task_cg_info_t *t = (task_cg_info_t *) x;

	if (common_cgroup_delete(&t->task_cg) != SLURM_SUCCESS)
		debug2("taskid: %d, failed to delete %s %m", t->taskid,
		       t->task_cg.path);

	return SLURM_SUCCESS;
}

extern int init(void)
{
	int i;

	for (i = 0; i < CG_CTL_CNT; i++) {
		g_user_cgpath[i][0] = '\0';
		g_job_cgpath[i][0] = '\0';
		g_step_cgpath[i][0] = '\0';
		g_step_active_cnt[i] = 0;
	}

	debug("%s loaded", plugin_name);
	return SLURM_SUCCESS;
}

extern int fini(void)
{
	debug("unloading %s", plugin_name);
	return SLURM_SUCCESS;
}

extern int cgroup_p_initialize(cgroup_ctl_type_t sub)
{
	int rc = SLURM_SUCCESS;

	if ((rc = _cgroup_init(sub)))
		return rc;

	switch (sub) {
	case CG_TRACK:
	case CG_CPUS:
		break;
	case CG_MEMORY:
		common_cgroup_set_param(&g_root_cg[sub], "memory.use_hierarchy",
					"1");
		break;
	case CG_DEVICES:
	case CG_CPUACCT:
		break;
	default:
		error("cgroup subsystem %u not supported", sub);
		rc = SLURM_ERROR;
		break;
	}

	return rc;
}

extern int cgroup_p_system_create(cgroup_ctl_type_t sub)
{
	char *slurm_cgpath, *sys_cgpath = NULL;
	int rc = SLURM_SUCCESS;

	switch (sub) {
	case CG_CPUS:
		rc = _cpuset_create(NULL);
		break;
	case CG_MEMORY:
		/* create slurm root cg in this cg namespace */
		slurm_cgpath = xcgroup_create_slurm_cg(&g_cg_ns[sub]);
		if (!slurm_cgpath)
			return SLURM_ERROR;

		xstrfmtcat(sys_cgpath, "%s/system", slurm_cgpath);
		xfree(slurm_cgpath);

		if ((rc = common_cgroup_create(&g_cg_ns[sub], &g_sys_cg[sub],
					       sys_cgpath, getuid(), getgid()))
		    != SLURM_SUCCESS)
			goto end;

		if ((rc = common_cgroup_instantiate(&g_sys_cg[sub]))
		    != SLURM_SUCCESS)
			goto end;

		/* set notify on release flag */
		common_cgroup_set_param(&g_sys_cg[sub], "notify_on_release",
					"0");

		if ((rc = common_cgroup_set_param(&g_sys_cg[sub],
					    "memory.use_hierarchy", "1"))
		    != SLURM_SUCCESS) {
			error("system cgroup: unable to ask for hierarchical accounting of system memcg '%s'",
			      g_sys_cg[sub].path);
			goto end;
		}

		if ((rc = common_cgroup_set_uint64_param(&g_sys_cg[sub],
							 "memory.oom_control",
							 1))
		    != SLURM_SUCCESS) {
			error("Resource spec: unable to disable OOM Killer in "
			      "system memory cgroup: %s", g_sys_cg[sub].path);
			goto end;
		}
		break;
	default:
		error("cgroup subsystem %u not supported", sub);
		return SLURM_ERROR;
		break;
	}

end:
	xfree(sys_cgpath);
	return rc;
}

extern int cgroup_p_system_addto(cgroup_ctl_type_t sub, pid_t *pids, int npids)
{
	switch (sub) {
	case CG_TRACK:
		break;
	case CG_CPUS:
		return common_cgroup_add_pids(&g_sys_cg[sub], pids, npids);
	case CG_MEMORY:
		return common_cgroup_add_pids(&g_sys_cg[sub], pids, npids);
	case CG_DEVICES:
		break;
	case CG_CPUACCT:
		error("This operation is not supported for %s", g_cg_name[sub]);
		return SLURM_ERROR;
	default:
		error("cgroup subsystem %u not supported", sub);
		return SLURM_ERROR;
	}

	return common_cgroup_add_pids(&g_step_cg[sub], pids, npids);
}

extern int cgroup_p_system_destroy(cgroup_ctl_type_t sub)
{
	int rc = SLURM_SUCCESS;

	/* Another plugin may have already destroyed this subsystem. */
	if (!g_sys_cg[sub].path)
		return SLURM_SUCCESS;

	/* Custom actions for every cgroup subsystem */
	switch (sub) {
	case CG_TRACK:
		break;
	case CG_CPUS:
		break;
	case CG_MEMORY:
		common_cgroup_set_uint64_param(&g_sys_cg[sub],
					       "memory.force_empty", 1);
		break;
	case CG_DEVICES:
		break;
	case CG_CPUACCT:
		break;
	default:
		error("cgroup subsystem %u not supported", sub);
		return SLURM_ERROR;
		break;
	}

	rc = common_cgroup_move_process(&g_root_cg[sub], getpid());
	if (rc != SLURM_SUCCESS) {
		error("Unable to move pid %d to root cgroup", getpid());
		goto end;
	}
	xcgroup_wait_pid_moved(&g_sys_cg[sub], g_cg_name[sub]);

	if ((rc = common_cgroup_delete(&g_sys_cg[sub])) != SLURM_SUCCESS) {
		debug2("not removing system cg (%s), there may be attached stepds: %m",
		       g_cg_name[sub]);
		goto end;
	}
	common_cgroup_destroy(&g_sys_cg[sub]);
end:
	if (rc == SLURM_SUCCESS) {
		common_cgroup_destroy(&g_root_cg[sub]);
		common_cgroup_ns_destroy(&g_cg_ns[sub]);
	}
	return rc;
}

extern int cgroup_p_step_create(cgroup_ctl_type_t sub, stepd_step_rec_t *job)
{
	int rc = SLURM_SUCCESS;

	/* Don't let other plugins destroy our structs. */
	g_step_active_cnt[sub]++;

	switch (sub) {
	case CG_TRACK:
		/* create a new cgroup for that container */
		if ((rc = xcgroup_create_hierarchy(__func__,
						   job,
						   &g_cg_ns[sub],
						   &g_job_cg[sub],
						   &g_step_cg[sub],
						   &g_user_cg[sub],
						   g_job_cgpath[sub],
						   g_step_cgpath[sub],
						   g_user_cgpath[sub]))
		    != SLURM_SUCCESS)
			goto step_c_err;

		/* stick slurmstepd pid to the newly created job container
		 * (Note: we do not put it in the step container because this
		 * container could be used to suspend/resume tasks using freezer
		 * properties so we need to let the slurmstepd outside of
		 * this one)
		 */
		if (common_cgroup_add_pids(&g_job_cg[sub], &job->jmgr_pid, 1) !=
		    SLURM_SUCCESS) {
			cgroup_p_step_destroy(sub);
			goto step_c_err;
		}

		/* we use slurmstepd pid as the identifier of the container */
		job->cont_id = (uint64_t)job->jmgr_pid;
		break;
	case CG_CPUS:
		if ((rc = _cpuset_create(job))!= SLURM_SUCCESS)
			goto step_c_err;
		break;
	case CG_MEMORY:
		if ((rc = xcgroup_create_hierarchy(__func__,
						   job,
						   &g_cg_ns[sub],
						   &g_job_cg[sub],
						   &g_step_cg[sub],
						   &g_user_cg[sub],
						   g_job_cgpath[sub],
						   g_step_cgpath[sub],
						   g_user_cgpath[sub]))
		    != SLURM_SUCCESS) {
			goto step_c_err;
		}
		if ((rc = common_cgroup_set_param(&g_user_cg[sub],
						  "memory.use_hierarchy",
						  "1")) != SLURM_SUCCESS) {
			error("unable to set hierarchical accounting for %s",
			      g_user_cgpath[sub]);
			cgroup_p_step_destroy(sub);
			break;
		}
		if ((rc = common_cgroup_set_param(&g_job_cg[sub],
						  "memory.use_hierarchy",
						  "1")) != SLURM_SUCCESS) {
			error("unable to set hierarchical accounting for %s",
			      g_job_cgpath[sub]);
			cgroup_p_step_destroy(sub);
			break;
		}
		if ((rc = common_cgroup_set_param(&g_step_cg[sub],
						  "memory.use_hierarchy",
						  "1") != SLURM_SUCCESS)) {
			error("unable to set hierarchical accounting for %s",
			      g_step_cg[sub].path);
			cgroup_p_step_destroy(sub);
			break;
		}
		break;
	case CG_DEVICES:
		/* create a new cgroup for that container */
		if ((rc = xcgroup_create_hierarchy(__func__,
						   job,
						   &g_cg_ns[sub],
						   &g_job_cg[sub],
						   &g_step_cg[sub],
						   &g_user_cg[sub],
						   g_job_cgpath[sub],
						   g_step_cgpath[sub],
						   g_user_cgpath[sub]))
		    != SLURM_SUCCESS)
			goto step_c_err;
		break;
	case CG_CPUACCT:
		error("This operation is not supported for %s", g_cg_name[sub]);
		rc = SLURM_ERROR;
		goto step_c_err;
	default:
		error("cgroup subsystem %u not supported", sub);
		rc = SLURM_ERROR;
		goto step_c_err;
	}

	return rc;

step_c_err:
	/* step cgroup is not created */
	g_step_active_cnt[sub]--;
	return rc;
}

extern int cgroup_p_step_addto(cgroup_ctl_type_t sub, pid_t *pids, int npids)
{
	if (*g_step_cgpath[sub] == '\0')
		return SLURM_ERROR;

	switch (sub) {
	case CG_TRACK:
	case CG_CPUS:
	case CG_MEMORY:
	case CG_DEVICES:
		break;
	case CG_CPUACCT:
		error("This operation is not supported for %s", g_cg_name[sub]);
		return SLURM_ERROR;
	default:
		error("cgroup subsystem %u not supported", sub);
		return SLURM_ERROR;
	}

	return common_cgroup_add_pids(&g_step_cg[sub], pids, npids);
}

extern int cgroup_p_step_get_pids(pid_t **pids, int *npids)
{
	if (*g_step_cgpath[CG_TRACK] == '\0')
		return SLURM_ERROR;

	return common_cgroup_get_pids(&g_step_cg[CG_TRACK], pids, npids);
}

extern int cgroup_p_step_suspend(void)
{
	if (*g_step_cgpath[CG_TRACK] == '\0')
		return SLURM_ERROR;

	return common_cgroup_set_param(&g_step_cg[CG_TRACK], "freezer.state",
				       "FROZEN");
}

extern int cgroup_p_step_resume(void)
{
	if (*g_step_cgpath[CG_TRACK] == '\0')
		return SLURM_ERROR;

	return common_cgroup_set_param(&g_step_cg[CG_TRACK], "freezer.state",
				       "THAWED");
}

extern int cgroup_p_step_destroy(cgroup_ctl_type_t sub)
{
	int rc = SLURM_SUCCESS;

	/* Only destroy the step if we're the only ones using it. */
	if (g_step_active_cnt[sub] == 0) {
		debug("called without a previous init. This shouldn't happen!");
		return SLURM_SUCCESS;
	}
	/* Only destroy the step if we're the only ones using it. */
	if (g_step_active_cnt[sub] > 1) {
		g_step_active_cnt[sub]--;
		debug2("Not destroying %s step dir, resource busy by %d other plugin",
		       g_cg_name[sub], g_step_active_cnt[sub]);
		return SLURM_SUCCESS;
	}

	/* Custom actions for every cgroup subsystem */
	switch (sub) {
	case CG_TRACK:
		break;
	case CG_CPUS:
		break;
	case CG_MEMORY:
		/*
		 * Despite rmdir() offlines memcg, the memcg may still stay
		 * there due to charged file caches. Some out-of-use page caches
		 * may keep charged until memory pressure happens. Avoid this
		 * writting to 'force_empty'. Note that when
		 * memory.kmem.limit_in_bytes is set the charges due to kernel
		 * pages will still be seen.
		 *
		 * Since this adds a large delay (~2 sec) only do this if
		 * running jobacct_gather/cgroup.
		 */
		if (!xstrcmp(slurm_conf.job_acct_gather_type,
			     "jobacct_gather/cgroup") &&
		    g_step_cg[CG_MEMORY].path)
			common_cgroup_set_param(&g_step_cg[CG_MEMORY],
						"memory.force_empty", "1");
		break;
	case CG_DEVICES:
		break;
	case CG_CPUACCT:
		break;
	default:
		error("cgroup subsystem %u not supported", sub);
		return SLURM_ERROR;
		break;
	}

	rc = _remove_cg_subsystem(&g_root_cg[sub],
				  &g_step_cg[sub],
				  &g_job_cg[sub],
				  &g_user_cg[sub],
				  g_cg_name[sub]);

	if (rc == SLURM_SUCCESS) {
		g_step_active_cnt[sub] = 0;
		g_step_cgpath[sub][0] = '\0';
	}

	return rc;
}

/*
 * Is the specified pid in our cgroup g_cg_ns[CG_TRACK]?
 * In the future we may want to replace this with a get pids and a search.
 */
extern bool cgroup_p_has_pid(pid_t pid)
{
	bool rc;
	xcgroup_t cg;

	rc = xcgroup_ns_find_by_pid(&g_cg_ns[CG_TRACK], &cg, pid);
	if (rc != SLURM_SUCCESS)
		return false;

	rc = true;
	if (xstrcmp(cg.path, g_step_cg[CG_TRACK].path))
		rc = false;

	common_cgroup_destroy(&cg);
	return rc;
}

extern cgroup_limits_t *cgroup_p_root_constrain_get(cgroup_ctl_type_t sub)
{
	int rc = SLURM_SUCCESS;
	cgroup_limits_t *limits = xmalloc(sizeof(*limits));

	switch (sub) {
	case CG_TRACK:
		break;
	case CG_CPUS:
		rc = common_cgroup_get_param(&g_root_cg[CG_CPUS], "cpuset.cpus",
					     &limits->allow_cores,
					     &limits->cores_size);

		rc += common_cgroup_get_param(&g_root_cg[CG_CPUS],
					      "cpuset.mems",
					      &limits->allow_mems,
					      &limits->mems_size);

		if (limits->cores_size > 0)
			limits->allow_cores[(limits->cores_size)-1] = '\0';

		if (limits->mems_size > 0)
			limits->allow_mems[(limits->mems_size)-1] = '\0';

		if (rc != SLURM_SUCCESS)
			goto fail;
		break;
	case CG_MEMORY:
	case CG_DEVICES:
		break;
	default:
		error("cgroup subsystem %u not supported", sub);
		rc = SLURM_ERROR;
		break;
	}

	return limits;
fail:
	cgroup_free_limits(limits);
	return NULL;
}

extern int cgroup_p_root_constrain_set(cgroup_ctl_type_t sub,
				       cgroup_limits_t *limits)
{
	int rc = SLURM_SUCCESS;

	if (!limits)
		return SLURM_ERROR;

	switch (sub) {
	case CG_TRACK:
		break;
	case CG_CPUS:
		break;
	case CG_MEMORY:
		rc = common_cgroup_set_uint64_param(&g_root_cg[CG_MEMORY],
						    "memory.swappiness",
						    limits->swappiness);
		break;
	case CG_DEVICES:
		break;
	default:
		error("cgroup subsystem %u not supported", sub);
		rc = SLURM_ERROR;
		break;
	}

	return rc;
}

extern cgroup_limits_t *cgroup_p_system_constrain_get(cgroup_ctl_type_t sub)
{
	cgroup_limits_t *limits = NULL;

	switch (sub) {
	case CG_TRACK:
	case CG_CPUS:
	case CG_MEMORY:
	case CG_DEVICES:
		break;
	default:
		error("cgroup subsystem %u not supported", sub);
		break;
	}

	return limits;
}

extern int cgroup_p_system_constrain_set(cgroup_ctl_type_t sub,
					 cgroup_limits_t *limits)
{
	int rc = SLURM_SUCCESS;

	if (!limits)
		return SLURM_ERROR;

	switch (sub) {
	case CG_TRACK:
		break;
	case CG_CPUS:
		rc = common_cgroup_set_param(&g_sys_cg[CG_CPUS], "cpuset.cpus",
					     limits->allow_cores);
		//rc += common_cgroup_set_param(&g_sys_cg[CG_CPUS], "cpuset.mems",
		//				limits->allow_mems);
		break;
	case CG_MEMORY:
		common_cgroup_set_uint64_param(&g_sys_cg[CG_MEMORY],
					       "memory.limit_in_bytes",
					       limits->limit_in_bytes);
		break;
	case CG_DEVICES:
		break;
	default:
		error("cgroup subsystem %u not supported", sub);
		rc = SLURM_ERROR;
		break;
	}

	return rc;
}

extern int cgroup_p_user_constrain_set(cgroup_ctl_type_t sub,
				       stepd_step_rec_t *job,
				       cgroup_limits_t *limits)
{
	int rc = SLURM_SUCCESS;

	if (!limits)
		return SLURM_ERROR;

	switch (sub) {
	case CG_TRACK:
		break;
	case CG_CPUS:
		rc = common_cgroup_set_param(&g_user_cg[CG_CPUS], "cpuset.cpus",
					     limits->allow_cores);
		rc += common_cgroup_set_param(&g_user_cg[CG_CPUS],
					      "cpuset.mems",
					      limits->allow_mems);
		break;
	case CG_MEMORY:
		break;
	case CG_DEVICES:
		break;
	default:
		error("cgroup subsystem %u not supported", sub);
		rc = SLURM_ERROR;
		break;
	}

	return rc;
}

extern int cgroup_p_job_constrain_set(cgroup_ctl_type_t sub,
				      stepd_step_rec_t *job,
				      cgroup_limits_t *limits)
{
	int rc = SLURM_SUCCESS;

	if (!limits)
		return SLURM_ERROR;

	switch (sub) {
	case CG_TRACK:
		break;
	case CG_CPUS:
		rc = common_cgroup_set_param(&g_job_cg[CG_CPUS], "cpuset.cpus",
					     limits->allow_cores);
		rc += common_cgroup_set_param(&g_job_cg[CG_CPUS], "cpuset.mems",
					      limits->allow_mems);
		break;
	case CG_MEMORY:
		rc = common_cgroup_set_uint64_param(&g_job_cg[CG_MEMORY],
						    "memory.limit_in_bytes",
						    limits->limit_in_bytes);
		rc += common_cgroup_set_uint64_param(
			&g_job_cg[CG_MEMORY],
			"memory.soft_limit_in_bytes",
			limits->soft_limit_in_bytes);

		if (limits->kmem_limit_in_bytes != NO_VAL64)
			rc += common_cgroup_set_uint64_param(
				&g_job_cg[CG_MEMORY],
				"memory.kmem.limit_in_bytes",
				limits->kmem_limit_in_bytes);

		if (limits->memsw_limit_in_bytes != NO_VAL64)
			rc += common_cgroup_set_uint64_param(
				&g_job_cg[CG_MEMORY],
				"memory.memsw.limit_in_bytes",
				limits->memsw_limit_in_bytes);
		break;
	case CG_DEVICES:
		if (limits->allow_device)
			rc = common_cgroup_set_param(&g_job_cg[CG_DEVICES],
						     "devices.allow",
						     limits->device_major);
		else
			rc = common_cgroup_set_param(&g_job_cg[CG_DEVICES],
						     "devices.deny",
						     limits->device_major);
		break;
	default:
		error("cgroup subsystem %u not supported", sub);
		rc = SLURM_ERROR;
		break;
	}

	return rc;
}

extern int cgroup_p_step_constrain_set(cgroup_ctl_type_t sub,
				       stepd_step_rec_t *job,
				       cgroup_limits_t *limits)
{
	int rc = SLURM_SUCCESS;
#ifdef HAVE_NATIVE_CRAY
	char expected_usage[32];
#endif

	if (!limits)
		return SLURM_ERROR;

	switch (sub) {
	case CG_TRACK:
		break;
	case CG_CPUS:
		rc = common_cgroup_set_param(&g_step_cg[CG_CPUS], "cpuset.cpus",
					     limits->allow_cores);
		rc += common_cgroup_set_param(&g_step_cg[CG_CPUS], "cpuset.mems",
					      limits->allow_mems);
#ifdef HAVE_NATIVE_CRAY
		/*
		 * on Cray systems, set the expected usage in bytes.
		 * This is used by the Cray OOM killer
		 */
		snprintf(expected_usage, sizeof(expected_usage), "%"PRIu64,
			 (uint64_t)job->step_mem * 1024 * 1024);

		rc += common_cgroup_set_param(&g_step_cg[CG_CPUS],
					      "cpuset.expected_usage_in_bytes",
					      expected_usage);
#endif
		break;
	case CG_MEMORY:
		rc = common_cgroup_set_uint64_param(&g_step_cg[CG_MEMORY],
						    "memory.limit_in_bytes",
						    limits->limit_in_bytes);
		rc += common_cgroup_set_uint64_param(
			&g_step_cg[CG_MEMORY],
			"memory.soft_limit_in_bytes",
			limits->soft_limit_in_bytes);

		if (limits->kmem_limit_in_bytes != NO_VAL64)
			rc += common_cgroup_set_uint64_param(
				&g_step_cg[CG_MEMORY],
				"memory.kmem.limit_in_bytes",
				limits->kmem_limit_in_bytes);

		if (limits->memsw_limit_in_bytes != NO_VAL64)
			rc += common_cgroup_set_uint64_param(
				&g_step_cg[CG_MEMORY],
				"memory.memsw.limit_in_bytes",
				limits->memsw_limit_in_bytes);
		break;
	case CG_DEVICES:
		if (limits->allow_device)
			rc = common_cgroup_set_param(&g_step_cg[CG_DEVICES],
						     "devices.allow",
						     limits->device_major);
		else
			rc = common_cgroup_set_param(&g_step_cg[CG_DEVICES],
						     "devices.deny",
						     limits->device_major);
		break;
	default:
		error("cgroup subsystem %u not supported", sub);
		rc = SLURM_ERROR;
		break;
	}

	return rc;
}

/*
 * Code based on linux tools/cgroup/cgroup_event_listener.c with adapted
 * modifications for Slurm logic and needs.
 */
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__)
extern int cgroup_p_step_start_oom_mgr(void)
{
	debug("OOM not available on FreeBSD, NetBSD, or macOS");
	return SLURM_SUCCESS;
}

extern cgroup_oom_t *cgroup_p_step_stop_oom_mgr(stepd_step_rec_t *job)
{
	debug("OOM not available on FreeBSD, NetBSD, or macOS");
	return NULL;
}
#else
static int _read_fd(int fd, uint64_t *buf)
{
	int rc = SLURM_ERROR;
	size_t len = sizeof(uint64_t);
	uint64_t *buf_ptr = buf;
	ssize_t nread;

	while (len > 0 && (nread = read(fd, buf_ptr, len)) != 0) {
		if (nread == -1) {
			if (errno == EINTR)
				continue;
			error("read(): %m");
			break;
		}
		len -= nread;
		buf_ptr += nread;
	}

	if (len == 0)
		rc = SLURM_SUCCESS;

	return rc;
}

static void *_oom_event_monitor(void *x)
{
	oom_event_args_t *args = (oom_event_args_t *) x;
	int ret = -1;
	uint64_t res;
	struct pollfd fds[2];

	debug("started.");

	/*
	 * POLLPRI should only meaningful for event_fd, since according to the
	 * poll() man page it may indicate "cgroup.events" file modified.
	 *
	 * POLLRDHUP should only be meaningful for oom_pipe[0], since it refers
	 * to stream socket peer closed connection.
	 *
	 * POLLHUP is ignored in events member, and should be set by the Kernel
	 * in revents even if not defined in events.
	 *
	 */
	fds[0].fd = args->event_fd;
	fds[0].events = POLLIN | POLLPRI;

	fds[1].fd = oom_pipe[0];
	fds[1].events = POLLIN | POLLRDHUP;

	/*
	 * Poll event_fd for oom_kill events plus oom_pipe[0] for stop msg.
	 * Specifying a negative value in timeout means an infinite timeout.
	 */
	while (1) {
		ret = poll(fds, 2, -1);

		if (ret == -1) {
			/* Error. */
			if (errno == EINTR)
				continue;

			error("poll(): %m");
			break;
		} else if (ret == 0) {
			/* Should not happen since infinite timeout. */
			error("poll() timeout.");
			break;
		} else if (ret > 0) {
			if (fds[0].revents & (POLLIN | POLLPRI)) {
				/* event_fd readable. */
				res = 0;
				ret = _read_fd(args->event_fd, &res);
				if (ret == SLURM_SUCCESS) {
					slurm_mutex_lock(&oom_mutex);
					debug3("res: %"PRIu64"", res);
					oom_kill_count += res;
					debug2("oom-kill event count: %"PRIu64"",
					       oom_kill_count);
					slurm_mutex_unlock(&oom_mutex);
				} else
					error("cannot read oom-kill counts.");
			} else if (fds[0].revents & (POLLRDHUP | POLLERR |
						     POLLHUP | POLLNVAL)) {
				error("problem with event_fd");
				break;
			}

			if (fds[1].revents & POLLIN) {
				/* oom_pipe[0] readable. */
				res = 0;
				ret = _read_fd(oom_pipe[0], &res);
				if (ret == SLURM_SUCCESS && res == STOP_OOM) {
					/* Read stop msg. */
					debug2("stop msg read.");
					break;
				}
			} else if (fds[1].revents &
				   (POLLRDHUP | POLLERR | POLLHUP | POLLNVAL)) {
				error("problem with oom_pipe[0]");
				break;
			}
		}
	}

	slurm_mutex_lock(&oom_mutex);
	if (!oom_kill_count)
		debug("No oom events detected.");
	slurm_mutex_unlock(&oom_mutex);

	if ((args->event_fd != -1) && (close(args->event_fd) == -1))
		error("close(event_fd): %m");
	if ((args->efd != -1) && (close(args->efd) == -1))
		error("close(efd): %m");
	if ((args->cfd != -1) && (close(args->cfd) == -1))
		error("close(cfd): %m");
	if ((oom_pipe[0] >= 0) && (close(oom_pipe[0]) == -1))
		error("close(oom_pipe[0]): %m");
	xfree(args);

	debug("stopping.");

	pthread_exit((void *) 0);
}

extern int cgroup_p_step_start_oom_mgr(void)
{
	char *control_file = NULL, *event_file = NULL, *line = NULL;
	int rc = SLURM_SUCCESS, event_fd = -1, cfd = -1, efd = -1;
	oom_event_args_t *event_args;

	xstrfmtcat(control_file, "%s/%s", g_step_cg[CG_MEMORY].path,
		   "memory.oom_control");

	if ((cfd = open(control_file, O_RDONLY | O_CLOEXEC)) == -1) {
		error("Cannot open %s: %m", control_file);
		rc = SLURM_ERROR;
		goto fini;
	}

	xstrfmtcat(event_file, "%s/%s", g_step_cg[CG_MEMORY].path,
		   "cgroup.event_control");

	if ((efd = open(event_file, O_WRONLY | O_CLOEXEC)) == -1) {
		error("Cannot open %s: %m", event_file);
		rc = SLURM_ERROR;
		goto fini;
	}

	if ((event_fd = eventfd(0, EFD_CLOEXEC)) == -1) {
		error("eventfd: %m");
		rc = SLURM_ERROR;
		goto fini;
	}

	xstrfmtcat(line, "%d %d", event_fd, cfd);

	oom_kill_count = 0;

	if (write(efd, line, strlen(line) + 1) == -1) {
		error("Cannot write to %s", event_file);
		rc = SLURM_ERROR;
		goto fini;
	}

	if (pipe2(oom_pipe, O_CLOEXEC) == -1) {
		error("pipe(): %m");
		rc = SLURM_ERROR;
		goto fini;
	}

	/*
	 * Monitoring thread should be responsible for closing the fd's and
	 * freeing the oom_event_args_t struct and members.
	 */
	event_args = xmalloc(sizeof(oom_event_args_t));
	event_args->cfd = cfd;
	event_args->efd = efd;
	event_args->event_fd = event_fd;

	slurm_mutex_init(&oom_mutex);
	slurm_thread_create(&oom_thread, _oom_event_monitor, event_args);
	oom_thread_created = true;

fini:
	xfree(line);
	if (!oom_thread_created) {
		if ((event_fd != -1) && (close(event_fd) == -1))
			error("close: %m");
		if ((efd != -1) && (close(efd) == -1))
			error("close: %m");
		if ((cfd != -1) && (close(cfd) == -1))
			error("close: %m");
		if ((oom_pipe[0] != -1) && (close(oom_pipe[0]) == -1))
			error("close oom_pipe[0]: %m");
		if ((oom_pipe[1] != -1) && (close(oom_pipe[1]) == -1))
			error("close oom_pipe[1]: %m");
	}
	xfree(event_file);
	xfree(control_file);

	if (rc != SLURM_SUCCESS)
		error("Unable to register OOM notifications for %s",
		      g_step_cg[CG_MEMORY].path);
	return rc;
}

static uint64_t _failcnt(xcgroup_t *cg, char *param)
{
	uint64_t value = 0;

	if (xcgroup_get_uint64_param(cg, param, &value) != SLURM_SUCCESS) {
		debug2("unable to read '%s' from '%s'", param, cg->path);
		value = 0;
	}

	return value;
}

extern cgroup_oom_t *cgroup_p_step_stop_oom_mgr(stepd_step_rec_t *job)
{
	cgroup_oom_t *results = NULL;
	uint64_t stop_msg;
	ssize_t ret;

	if (!oom_thread_created) {
		debug("OOM events were not monitored for %ps", &job->step_id);
		goto fail_oom_results;
	}

	if (xcgroup_lock(&g_step_cg[CG_MEMORY]) != SLURM_SUCCESS) {
		error("xcgroup_lock error: %m");
		goto fail_oom_results;
	}

	results = xmalloc(sizeof(*results));

	results->step_memsw_failcnt = _failcnt(&g_step_cg[CG_MEMORY],
					       "memory.memsw.failcnt");
	results->step_mem_failcnt = _failcnt(&g_step_cg[CG_MEMORY],
					     "memory.failcnt");
	results->job_memsw_failcnt = _failcnt(&g_job_cg[CG_MEMORY],
					      "memory.memsw.failcnt");
	results->job_mem_failcnt = _failcnt(&g_job_cg[CG_MEMORY],
					    "memory.failcnt");

	xcgroup_unlock(&g_step_cg[CG_MEMORY]);

	/*
	 * oom_thread created, but could have finished before we attempt
	 * to send the stop msg. If it finished, oom_thread should had
	 * closed the read endpoint of oom_pipe.
	 */
	stop_msg = STOP_OOM;
	while (1) {
		ret = write(oom_pipe[1], &stop_msg, sizeof(stop_msg));
		if (ret == -1) {
			if (errno == EINTR)
				continue;
			debug("oom stop msg write() failed: %m");
		} else if (ret == 0)
			debug("oom stop msg nothing written: %m");
		else if (ret == sizeof(stop_msg))
			debug2("oom stop msg write success.");
		else
			debug("oom stop msg not fully written.");
		break;
	}

	debug2("attempt to join oom_thread.");
	if (oom_thread && pthread_join(oom_thread, NULL) != 0)
		error("pthread_join(): %m");

	slurm_mutex_lock(&oom_mutex);
	results->oom_kill_cnt = oom_kill_count;
	slurm_mutex_unlock(&oom_mutex);

fail_oom_results:
	if ((oom_pipe[1] != -1) && (close(oom_pipe[1]) == -1)) {
		error("close() failed on oom_pipe[1] fd, %ps: %m",
		      &job->step_id);
	}
	slurm_mutex_destroy(&oom_mutex);

	return results;
}
#endif

/***************************************
 ***** CGROUP ACCOUNTING FUNCTIONS *****
 **************************************/
static int _find_task_cg_info(void *x, void *key)
{
	task_cg_info_t *task_cg = (task_cg_info_t*)x;
	uint32_t taskid = *(uint32_t*)key;

	if (task_cg->taskid == taskid)
		return 1;

	return 0;
}

static void _free_task_cg_info(void *object)
{
	task_cg_info_t *task_cg = (task_cg_info_t *)object;

	if (task_cg) {
		common_cgroup_destroy(&task_cg->task_cg);
		xfree(task_cg);
	}
}

static int _handle_task_cgroup(cgroup_ctl_type_t sub, pid_t pid,
			       stepd_step_rec_t *job, uint32_t taskid)
{
	int rc = SLURM_SUCCESS;
	bool need_to_add = false;
	task_cg_info_t *task_cg_info;
	uid_t uid = job->uid;
	gid_t gid = job->gid;
	char *task_cgroup_path = NULL;

	/* build task cgroup relative path */
	xstrfmtcat(task_cgroup_path, "%s/task_%u", g_step_cgpath[sub], taskid);
	if (!task_cgroup_path) {
		error("unable to build task_%u cg relative path for %s: %m",
		      taskid, g_step_cgpath[sub]);
		return SLURM_ERROR;
	}

	if (!(task_cg_info = list_find_first(g_task_acct_list[sub],
					     _find_task_cg_info,
					     &taskid))) {
		task_cg_info = xmalloc(sizeof(*task_cg_info));
		task_cg_info->taskid = taskid;
		need_to_add = true;
	}

	/*
	 * Create task cgroup in the cg ns
	 */
	if (common_cgroup_create(&g_cg_ns[sub], &task_cg_info->task_cg,
				 task_cgroup_path, uid, gid) != SLURM_SUCCESS) {
		error("unable to create task %u cgroup", taskid);
		xfree(task_cg_info);
		xfree(task_cgroup_path);
		return SLURM_ERROR;
	}

	if (common_cgroup_instantiate(&task_cg_info->task_cg) != SLURM_SUCCESS)
	{
		_free_task_cg_info(task_cg_info);
		error("unable to instantiate task %u cgroup", taskid);
		xfree(task_cgroup_path);
		return SLURM_ERROR;
	}

	/* set notify on release flag */
	common_cgroup_set_param(&task_cg_info->task_cg, "notify_on_release",
				"0");

	/* Attach the pid to the corresponding step_x/task_y cgroup */
	rc = common_cgroup_move_process(&task_cg_info->task_cg, pid);
	if (rc != SLURM_SUCCESS)
		error("Unable to move pid %d to %s cg", pid, task_cgroup_path);

	/* Add the cgroup to the list now that it is initialized. */
	if (need_to_add)
		list_append(g_task_acct_list[sub], task_cg_info);

	xfree(task_cgroup_path);
	return rc;
}

extern int cgroup_p_accounting_init(void)
{
	int i, rc = SLURM_SUCCESS;

	if (g_step_cgpath[CG_MEMORY][0] == '\0')
		rc = cgroup_p_initialize(CG_MEMORY);

	if (rc != SLURM_SUCCESS) {
		error("Cannot initialize cgroup memory accounting");
		return rc;
	}

	g_step_active_cnt[CG_MEMORY]++;

	if (g_step_cgpath[CG_CPUACCT][0] == '\0')
		rc = cgroup_p_initialize(CG_CPUACCT);

	if (rc != SLURM_SUCCESS) {
		error("Cannot initialize cgroup cpuacct accounting");
		return rc;
	}

	g_step_active_cnt[CG_CPUACCT]++;

	/* Create the list of tasks which will be accounted for*/
	for (i = 0; i < CG_CTL_CNT; i++) {
		FREE_NULL_LIST(g_task_acct_list[i]);
		g_task_acct_list[i] = list_create(_free_task_cg_info);
	}

	return rc;
}

extern int cgroup_p_accounting_fini(void)
{
	int rc;

	/* Empty the lists of accounted tasks, do a best effort in rmdir */
	(void) list_for_each(g_task_acct_list[CG_MEMORY], _rmdir_task, NULL);
	list_flush(g_task_acct_list[CG_MEMORY]);

	(void) list_for_each(g_task_acct_list[CG_CPUACCT], _rmdir_task, NULL);
	list_flush(g_task_acct_list[CG_CPUACCT]);

	/* Remove job/uid/step directories */
	rc = cgroup_p_step_destroy(CG_MEMORY);
	rc += cgroup_p_step_destroy(CG_CPUACCT);

	return rc;
}

extern int cgroup_p_task_addto_accounting(pid_t pid, stepd_step_rec_t *job,
					  uint32_t task_id)
{
	int rc = SLURM_SUCCESS;

	if (task_id > g_max_task_id)
		g_max_task_id = task_id;

	debug("%ps taskid %u max_task_id %u", &job->step_id, task_id,
	      g_max_task_id);

	if (xcgroup_create_hierarchy(__func__,
				     job,
				     &g_cg_ns[CG_CPUACCT],
				     &g_job_cg[CG_CPUACCT],
				     &g_step_cg[CG_CPUACCT],
				     &g_user_cg[CG_CPUACCT],
				     g_job_cgpath[CG_CPUACCT],
				     g_step_cgpath[CG_CPUACCT],
				     g_user_cgpath[CG_CPUACCT])
	    != SLURM_SUCCESS) {
		return SLURM_ERROR;
	}

	if (xcgroup_create_hierarchy(__func__,
				     job,
				     &g_cg_ns[CG_MEMORY],
				     &g_job_cg[CG_MEMORY],
				     &g_step_cg[CG_MEMORY],
				     &g_user_cg[CG_MEMORY],
				     g_job_cgpath[CG_MEMORY],
				     g_step_cgpath[CG_MEMORY],
				     g_user_cgpath[CG_MEMORY])
	    != SLURM_SUCCESS) {
		cgroup_p_step_destroy(CG_CPUACCT);
		return SLURM_ERROR;
	}

	_handle_task_cgroup(CG_CPUACCT, pid, job, task_id);
	_handle_task_cgroup(CG_MEMORY, pid, job, task_id);

	return rc;
}

extern cgroup_acct_t *cgroup_p_task_get_acct_data(uint32_t taskid)
{
	char *cpu_time = NULL, *memory_stat = NULL, *ptr;
	size_t cpu_time_sz = 0, memory_stat_sz = 0;
	cgroup_acct_t *stats = NULL;
	xcgroup_t *task_cpuacct_cg = NULL;
	xcgroup_t *task_memory_cg = NULL;

	/* Find which task cgroup to use */
	task_memory_cg = list_find_first(g_task_acct_list[CG_MEMORY],
					 _find_task_cg_info,
					 &taskid);
	task_cpuacct_cg = list_find_first(g_task_acct_list[CG_CPUACCT],
					  _find_task_cg_info,
					  &taskid);

	/*
	 * We should always find the task cgroup; if we don't for some reason,
	 * just print an error and return.
	 */
	if (!task_cpuacct_cg) {
		error("Could not find task_cpuacct_cg, this should never happen");
		return NULL;
	}

	if (!task_memory_cg) {
		error("Could not find task_memory_cg, this should never happen");
		return NULL;
	}

	common_cgroup_get_param(task_cpuacct_cg, "cpuacct.stat", &cpu_time,
				&cpu_time_sz);
	common_cgroup_get_param(task_memory_cg, "memory.stat", &memory_stat,
				&memory_stat_sz);

	/*
	 * Initialize values, a NO_VAL64 will indicate to the caller that
	 * something happened here.
	 */
	stats = xmalloc(sizeof(*stats));
	stats->usec = NO_VAL64;
	stats->ssec = NO_VAL64;
	stats->total_rss = NO_VAL64;
	stats->total_pgmajfault = NO_VAL64;

	if (cpu_time != NULL)
		sscanf(cpu_time, "%*s %lu %*s %lu", &stats->usec, &stats->ssec);

	if ((ptr = xstrstr(memory_stat, "total_rss")))
		sscanf(ptr, "total_rss %lu", &stats->total_rss);
	if ((ptr = xstrstr(memory_stat, "total_pgmajfault")))
		sscanf(ptr, "total_pgmajfault %lu", &stats->total_pgmajfault);

	xfree(cpu_time);
	xfree(memory_stat);

	return stats;
}
