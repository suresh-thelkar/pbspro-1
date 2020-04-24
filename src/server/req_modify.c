/*
 * Copyright (C) 1994-2020 Altair Engineering, Inc.
 * For more information, contact Altair at www.altair.com.
 *
 * This file is part of both the OpenPBS software ("OpenPBS")
 * and the PBS Professional ("PBS Pro") software.
 *
 * Open Source License Information:
 *
 * OpenPBS is free software. You can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * OpenPBS is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Affero General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Commercial License Information:
 *
 * PBS Pro is commercially licensed software that shares a common core with
 * the OpenPBS software.  For a copy of the commercial license terms and
 * conditions, go to: (http://www.pbspro.com/agreement.html) or contact the
 * Altair Legal Department.
 *
 * Altair's dual-license business model allows companies, individuals, and
 * organizations to create proprietary derivative works of OpenPBS and
 * distribute them - whether embedded or bundled with other software -
 * under a commercial license agreement.
 *
 * Use of Altair's trademarks, including but not limited to "PBS™",
 * "OpenPBS®", "PBS Professional®", and "PBS Pro™" and Altair's logos is
 * subject to Altair's trademark licensing policies.
 */

/**
 * @file    req_modify.c
 *
 * @brief
 * 		req_modify.c	-	Functions relating to the Modify Job Batch Requests.
 *
 * Included funtions are:
 *	post_modify_req()
 *	req_modifyjob()
 *	find_name_in_svrattrl()
 *	modify_job_attr()
 */
#include <pbs_config.h>   /* the master config generated by configure */

#include <stdio.h>
#include <sys/types.h>
#include "libpbs.h"
#include <signal.h>
#include "server_limits.h"
#include "list_link.h"
#include "attribute.h"
#include "resource.h"
#include "server.h"
#include "credential.h"
#include "batch_request.h"
#include "job.h"
#include "reservation.h"
#include "queue.h"
#include "work_task.h"
#include "pbs_error.h"
#include "log.h"
#include "pbs_nodes.h"
#include "svrfunc.h"
#include "hook.h"
#include "sched_cmds.h"
#include "pbs_internal.h"
#include "pbs_sched.h"
#include "acct.h"


/* Global Data Items: */

extern attribute_def	    job_attr_def[];
extern char *msg_jobmod;
extern char *msg_manager;
extern char *msg_mombadmodify;
extern char *msg_defproject;
extern char *msg_max_no_minwt;
extern char *msg_min_gt_maxwt;
extern char *msg_nostf_jobarray;
extern int   comp_resc_gt;
extern int   comp_resc_lt;
extern char *resc_in_err;

static resource_def *pseldef = NULL;
extern int scheduler_jobs_stat;
extern int resc_access_perm;
extern char *msg_nostf_resv;

int modify_resv_attr(resc_resv *presv, svrattrl *plist, int perm, int *bad);
extern void resv_revert_alter_times(resc_resv *presv);
extern int gen_future_reply(resc_resv *presv, long fromNow);
extern job  *chk_job_request(char *, struct batch_request *, int *, int *);
extern resc_resv  *chk_rescResv_request(char *, struct batch_request *);




/*
 * post_modify_req - clean up after sending modify request to MOM
 */
static void
post_modify_req(struct work_task *pwt)
{
	struct batch_request *preq;

	if (pwt->wt_aux2 != PROT_TPP)
		svr_disconnect(pwt->wt_event);  /* close connection to MOM */
	preq = pwt->wt_parm1;
	preq->rq_conn = preq->rq_orgconn;  /* restore socket to client */

	if (preq->rq_reply.brp_code) {
		(void)sprintf(log_buffer, msg_mombadmodify, preq->rq_reply.brp_code);
		log_event(PBSEVENT_JOB, PBS_EVENTCLASS_JOB, LOG_INFO,
			preq->rq_ind.rq_modify.rq_objname, log_buffer);
		req_reject(preq->rq_reply.brp_code, 0, preq);
	} else
		reply_ack(preq);
}




/**
 * @brief
 * 		Service the Modify Job Request from client such as qalter.
 *
 * @par	Functionality:
 *		This request automatically modifies one or more of a job's attributes.
 *		An error is returned to the client if the user does not have permission
 *		to perform the modification, the attribute is read-only, the job is
 *		running and the attribute is only modifiable when the job is not
 *		running, the user attempts to modify a subjob of an array.
 *
 *		If any "move job" hooks are in place, they modify the request before
 *		the Server does anything with the request.
 *
 * @param[in] preq - pointer to batch request from client
 */

void
req_modifyjob(struct batch_request *preq)
{
	int		 add_to_am_list = 0; /* if altered during sched cycle */
	int		 bad = 0;
	int		 jt;		/* job type */
	int		 newstate;
	int		 newsubstate;
	resource_def	*outsideselect = NULL;
	job		*pjob;
	svrattrl	*plist;
	resource	*presc;
	resource_def	*prsd;
	int		 rc;
	int		 running = 0;
	int		 sendmom = 0;
	char		hook_msg[HOOK_MSG_SIZE];
	int		mod_project = 0;
	pbs_sched	*psched;

	switch (process_hooks(preq, hook_msg, sizeof(hook_msg),
			pbs_python_set_interrupt)) {
		case 0:	/* explicit reject */
			reply_text(preq, PBSE_HOOKERROR, hook_msg);
			return;
		case 1:   /* explicit accept */
			if (recreate_request(preq) == -1) { /* error */
				/* we have to reject the request, as 'preq' */
				/* may have been partly modified            */
				strcpy(hook_msg,
					"modifyjob event: rejected request");
				log_event(PBSEVENT_ERROR, PBS_EVENTCLASS_HOOK,
					LOG_ERR, "", hook_msg);
				reply_text(preq, PBSE_HOOKERROR, hook_msg);
				return;
			}
			break;
		case 2:	/* no hook script executed - go ahead and accept event*/
			break;
		default:
			log_event(PBSEVENT_DEBUG2, PBS_EVENTCLASS_HOOK,
				LOG_INFO, "", "modifyjob event: accept req by default");
	}

	if (pseldef == NULL)  /* do one time to keep handy */
		pseldef = find_resc_def(svr_resc_def, "select", svr_resc_size);

	pjob = chk_job_request(preq->rq_ind.rq_modify.rq_objname, preq, &jt, NULL);
	if (pjob == NULL)
		return;

	if ((jt == IS_ARRAY_Single) || (jt == IS_ARRAY_Range)) {
		req_reject(PBSE_IVALREQ, 0, preq);
		return;
	}

	psched = find_sched_from_sock(preq->rq_conn);
	/* allow scheduler to modify job */
	if (psched == NULL) {
		/* provisioning job is not allowed to be modified */
		if ((pjob->ji_qs.ji_state == JOB_STATE_RUNNING) &&
			(pjob->ji_qs.ji_substate == JOB_SUBSTATE_PROVISION)) {
			req_reject(PBSE_BADSTATE, 0, preq);
			return;
		}
	}

	/* cannot be in exiting or transit, exiting has already be checked */

	if (pjob->ji_qs.ji_state == JOB_STATE_TRANSIT) {
		req_reject(PBSE_BADSTATE, 0, preq);
		return;
	}

	plist = (svrattrl *)GET_NEXT(preq->rq_ind.rq_modify.rq_attr);
	if (plist == NULL) {	/* nothing to do */
		reply_ack(preq);
		return;
	}

	/*
	 * Special checks must be made:
	 *	if during a scheduling cycle and certain attributes are altered,
	 *	   make a note of the job to prevent it from being run now;
	 *	if job is running, only certain attributes/resources can be
	 *	   altered.
	 */

	if (pjob->ji_qs.ji_state == JOB_STATE_RUNNING) {
		running = 1;
	}
	while (plist) {
		int i;

		i = find_attr(job_attr_def, plist->al_name, JOB_ATR_LAST);

		/*
		 * Is the attribute being altered one which could change
		 * scheduling (ATR_DFLAG_SCGALT set) and if a scheduling
		 * cycle is in progress, then set flag to add the job to list
		 * of jobs which cannot be run in this cycle.
		 * If the scheduler itself sends a modify job request,
		 * no need to delay the job until next cycle.
		 */
		if ((psched == NULL) && (scheduler_jobs_stat) && (job_attr_def[i].at_flags & ATR_DFLAG_SCGALT))
			add_to_am_list = 1;

		/* Is the attribute modifiable in RUN state ? */

		if (i < 0) {
			reply_badattr(PBSE_NOATTR, 1, plist, preq);
			return;
		}
		if ((running == 1) &&
			((job_attr_def[i].at_flags & ATR_DFLAG_ALTRUN) == 0)) {

			reply_badattr(PBSE_MODATRRUN, 1, plist, preq);
			return;
		}
		if (i == (int)JOB_ATR_resource) {

			prsd = find_resc_def(svr_resc_def, plist->al_resc,
				svr_resc_size);

			if (prsd == 0) {
				reply_badattr(PBSE_UNKRESC, 1, plist, preq);
				return;
			}

			/* is the specified resource modifiable while */
			/* the job is running                         */

			if (running) {

				if ((prsd->rs_flags & ATR_DFLAG_ALTRUN) == 0) {
					reply_badattr(PBSE_MODATRRUN, 1, plist, preq);
					return;
				}

				sendmom = 1;
			}

			/* should the resource be only in a select spec */

			if (prsd->rs_flags & ATR_DFLAG_CVTSLT && !outsideselect &&
				plist->al_atopl.value && plist->al_atopl.value[0]) {
				/* if "-lresource" is set and has non-NULL value,
				** remember as potential bad resource
				** if this appears along "select".
				*/
				outsideselect = prsd;
			}
		}
		if (strcmp(plist->al_name, ATTR_project) == 0) {
			mod_project = 1;
		} else if ((strcmp(plist->al_name, ATTR_runcount) == 0) &&
			((plist->al_flags & ATR_VFLAG_HOOK) == 0) &&
			(plist->al_value != NULL) &&
			(plist->al_value[0] != '\0') &&
			((preq->rq_perm & (ATR_DFLAG_MGWR | ATR_DFLAG_OPWR)) == 0) &&
		(atol(plist->al_value) < \
		    pjob->ji_wattr[(int)JOB_ATR_runcount].at_val.at_long)) {
			sprintf(log_buffer,
				"regular user %s@%s cannot decrease '%s' attribute value from %ld to %ld",
				preq->rq_user, preq->rq_host, ATTR_runcount,
				pjob->ji_wattr[(int)JOB_ATR_runcount].at_val.at_long,
				atol(plist->al_value));
			log_event(PBSEVENT_JOB, PBS_EVENTCLASS_JOB, LOG_ERR,
				pjob->ji_qs.ji_jobid, log_buffer);
			req_reject(PBSE_PERM, 0, preq);
			return;
		}
		plist = (svrattrl *)GET_NEXT(plist->al_link);
	}

	if (outsideselect) {
		presc = find_resc_entry(&pjob->ji_wattr[(int)JOB_ATR_resource],
			pseldef);
		if (presc &&
			((presc->rs_value.at_flags & ATR_VFLAG_DEFLT) == 0)) {
			/* select is not a default, so reject qalter */

			resc_in_err = strdup(outsideselect->rs_name);
			req_reject(PBSE_INVALJOBRESC, 0, preq);
			return;
		}

	}

	/* modify the jobs attributes */

	bad = 0;
	plist = (svrattrl *)GET_NEXT(preq->rq_ind.rq_modify.rq_attr);
	rc = modify_job_attr(pjob, plist, preq->rq_perm, &bad);
	if (rc) {
		if (pjob->ji_clterrmsg)
			reply_text(preq, rc, pjob->ji_clterrmsg);
		else
			reply_badattr(rc, bad, plist, preq);
		return;
	}

	/* If certain attributes modified and if in scheduling cycle  */
	/* then add to list of jobs which cannot be run in this cycle */

	if (add_to_am_list)
		am_jobs_add(pjob);	/* see req_runjob() */

	/* check if project attribute was requested to be modified to */
	/* be the default project value */
	if (mod_project && (pjob->ji_wattr[(int)JOB_ATR_project].at_flags & \
							ATR_VFLAG_SET)) {

		if (strcmp(pjob->ji_wattr[(int)JOB_ATR_project].at_val.at_str,
			PBS_DEFAULT_PROJECT) == 0) {
			sprintf(log_buffer, msg_defproject,
				ATTR_project, PBS_DEFAULT_PROJECT);
#ifdef NAS /* localmod 107 */
			log_event(PBSEVENT_DEBUG4, PBS_EVENTCLASS_JOB, LOG_INFO,
				pjob->ji_qs.ji_jobid, log_buffer);
#else
			log_event(PBSEVENT_JOB, PBS_EVENTCLASS_JOB, LOG_INFO,
				pjob->ji_qs.ji_jobid, log_buffer);
#endif /* localmod 107 */
		}
	}

	if (pjob->ji_wattr[(int)JOB_ATR_resource].at_flags & ATR_VFLAG_MODIFY) {
		presc = find_resc_entry(&pjob->ji_wattr[(int)JOB_ATR_resource],
			pseldef);
		if (presc && (presc->rs_value.at_flags & ATR_VFLAG_DEFLT)) {
			/* changing Resource_List and select is a default   */
			/* clear "select" so it is rebuilt inset_resc_deflt */
			pseldef->rs_free(&presc->rs_value);
		}
	}

	/* Reset any defaults resource limit which might have been unset */
	if ((rc = set_resc_deflt((void *)pjob, JOB_OBJECT, NULL)) != 0) {
		req_reject(rc, 0, preq);
		return;
	}

	if (find_sched_from_sock(preq->rq_conn) == NULL)
		log_alter_records_for_attrs(pjob, plist);

	/* if job is not running, may need to change its state */
	if (pjob->ji_qs.ji_state != JOB_STATE_RUNNING) {
		svr_evaljobstate(pjob, &newstate, &newsubstate, 0);
		(void)svr_setjobstate(pjob, newstate, newsubstate);
	}
	
	job_save_db(pjob); /* we must save the updates anyway, if any */
	
	(void)sprintf(log_buffer, msg_manager, msg_jobmod, preq->rq_user, preq->rq_host);
	log_event(PBSEVENT_JOB, PBS_EVENTCLASS_JOB, LOG_INFO, pjob->ji_qs.ji_jobid, log_buffer);

	/* if a resource limit changed for a running job, send to MOM */
	if (sendmom) {
		rc = relay_to_mom(pjob, preq, post_modify_req);
		if (rc)
			req_reject(rc, 0, preq);    /* unable to get to MOM */
		return;
	}

	reply_ack(preq);
}

/**
 * @brief
 * 		Returns the svrattrl entry matching attribute 'name', or NULL if not found.
 *
 * @param[in]	plist	-	head of svrattrl list
 * @param[in]	name	-	matching attribute 'name'
 *
 * @return	svrattrl entry matching attribute 'name'
 * @retval	NULL	: if entry not found
 */
static svrattrl *
find_name_in_svrattrl(svrattrl *plist, char *name)
{

	if (!name)
		return NULL;

	while (plist) {

		if (strcmp(plist->al_name, name) == 0) {
			return plist;
		}

		plist = (svrattrl *)GET_NEXT(plist->al_link);
	}
	return NULL;
}

/**
 * @brief
 * 		modify_job_attr - modify the attributes of a job automatically
 *		Used by req_modifyjob() to alter the job attributes and by
 *		stat_update() [see req_stat.c] to update with latest from MOM
 *
 * @param[in,out]	pjob	-	job structure
 * @param[in,out]	plist	-	Pointer to list of attributes
 * @param[in]	perm	-	Permissions of the caller requesting the operation
 * @param[out]	bad	-	Pointer to the attribute index in case of a failed
 */
int
modify_job_attr(job *pjob, svrattrl *plist, int perm, int *bad)
{
	int	   changed_resc;
	int	   allow_unkn;
	long	   i;
	attribute *newattr;
	attribute *pre_copy;
	attribute *attr_save;
	attribute *pattr;
	resource  *prc;
	int	   rc;
	int	   newstate = -1;
	int	   newsubstate = -1;
	long	   newaccruetype = -1;

	if (pjob->ji_qhdr->qu_qs.qu_type == QTYPE_Execution)
		allow_unkn = -1;
	else
		allow_unkn = (int)JOB_ATR_UNKN;

	pattr = pjob->ji_wattr;

	/* call attr_atomic_set to decode and set a copy of the attributes.
	 * We need 2 copies: 1 for copying to pattr and 1 for calling the action functions
	 * We can't use the same copy for the action functions because copying to pattr
	 * is a shallow copy and array pointers will be cleared during the copy.
	 */

	newattr = calloc(JOB_ATR_LAST, sizeof(attribute));
	if (newattr == NULL)
		return PBSE_SYSTEM;
	rc = attr_atomic_set(plist, pattr, newattr, job_attr_def, JOB_ATR_LAST,
		allow_unkn, perm, bad);
	if (rc) {
		attr_atomic_kill(newattr, job_attr_def, JOB_ATR_LAST);
		return rc;
	}

	pre_copy = calloc(JOB_ATR_LAST, sizeof(attribute));
	if(pre_copy == NULL) {
		attr_atomic_kill(newattr, job_attr_def, JOB_ATR_LAST);
		return PBSE_SYSTEM;
	}
	attr_atomic_copy(pre_copy, newattr, job_attr_def, JOB_ATR_LAST);

	attr_save = calloc(JOB_ATR_LAST, sizeof(attribute));
	if (attr_save == NULL) {
		attr_atomic_kill(newattr, job_attr_def, JOB_ATR_LAST);
		attr_atomic_kill(pre_copy, job_attr_def, JOB_ATR_LAST);
		return PBSE_SYSTEM;
	}

	attr_atomic_copy(attr_save, pattr, job_attr_def, JOB_ATR_LAST);

	/* If resource limits are being changed ... */

	changed_resc = newattr[(int)JOB_ATR_resource].at_flags & ATR_VFLAG_SET;
	if ((rc == 0) && (changed_resc != 0)) {

		/* first, remove ATR_VFLAG_DEFLT from any value which was set */
		/* it can no longer be a "default" as it explicitly changed   */

		prc = (resource *)GET_NEXT(newattr[(int)JOB_ATR_resource].at_val.at_list);
		while (prc) {
			if ((prc->rs_value.at_flags & (ATR_VFLAG_MODIFY|ATR_VFLAG_DEFLT)) == (ATR_VFLAG_MODIFY|ATR_VFLAG_DEFLT))
				prc->rs_value.at_flags &= ~ATR_VFLAG_DEFLT;

			if ((prc->rs_value.at_flags & (ATR_VFLAG_MODIFY|ATR_VFLAG_SET)) == (ATR_VFLAG_MODIFY|ATR_VFLAG_SET)) {
				/* if being changed at all, see if "select" */
				if (prc->rs_defin == pseldef) {
					/* select is modified, recalc chunk sums */
					rc = set_chunk_sum(&prc->rs_value,
						&newattr[(int)JOB_ATR_resource]);
					if (rc)
						break;
				}
			}
			prc = (resource *)GET_NEXT(prc->rs_link);
		}

		/* Manager/Operator can modify job just about any old way     */
		/* So, the following checks are made only if not the Op/Admin */

		if ((perm & (ATR_DFLAG_MGWR | ATR_DFLAG_OPWR)) == 0) {
			if (pjob->ji_qs.ji_state == JOB_STATE_RUNNING) {

				/* regular user cannot raise the limits of a running job */

				if ((comp_resc(&pjob->ji_wattr[(int)JOB_ATR_resource],
					&newattr[(int)JOB_ATR_resource]) == -1) ||
					comp_resc_lt)
					rc = PBSE_PERM;

			}

			/* Also check against queue, system and entity limits */


			if (rc == 0) {
				rc =  chk_resc_limits(&newattr[(int)JOB_ATR_resource],
					pjob->ji_qhdr);
			}
			if (rc == 0) {
				rc = check_entity_resc_limit_max(pjob, pjob->ji_qhdr,
					&newattr[(int)JOB_ATR_resource]);
				if (rc == 0) {
					rc = check_entity_resc_limit_queued(pjob, pjob->ji_qhdr,
						&newattr[(int)JOB_ATR_resource]);
					if (rc == 0)
					{
						rc = check_entity_resc_limit_max(pjob, NULL,
							&newattr[(int)JOB_ATR_resource]);
						if (rc == 0)
							rc = check_entity_resc_limit_queued(pjob, NULL,
								&newattr[(int)JOB_ATR_resource]);
					}
				}
			}
		}
	}

	/* special check on permissions for hold */

	if ((rc == 0) &&
		(newattr[(int)JOB_ATR_hold].at_flags & ATR_VFLAG_MODIFY)) {
		svrattrl *hold_e = find_name_in_svrattrl(plist, ATTR_h);
		/* don't perform permission check if Hold_Types attribute */
		/* was set in a hook script (special privilege) */
		if ((hold_e == NULL) ||
			((hold_e->al_flags & ATR_VFLAG_HOOK) == 0)) {
			i = newattr[(int)JOB_ATR_hold].at_val.at_long ^
				(pattr+(int)JOB_ATR_hold)->at_val.at_long;
			rc = chk_hold_priv(i, perm);
		}
	}


	if ((rc == 0) &&
		((newattr[(int)JOB_ATR_userlst].at_flags & ATR_VFLAG_MODIFY) ||
			(newattr[(int)JOB_ATR_grouplst].at_flags & ATR_VFLAG_MODIFY))) {
		/* Need to reset execution uid and gid */
		rc = set_objexid((void *)pjob, JOB_OBJECT, newattr);
	}

	if (rc) {
		attr_atomic_kill(newattr, job_attr_def, JOB_ATR_LAST);
		attr_atomic_kill(attr_save, job_attr_def, JOB_ATR_LAST);
		attr_atomic_kill(pre_copy, job_attr_def, JOB_ATR_LAST);
		return (rc);
	}

	/* OK, if resources changed, reset entity sums */

	if (changed_resc) {
		account_entity_limit_usages(pjob, NULL,
				&newattr[(int)JOB_ATR_resource], INCR, ETLIM_ACC_ALL_RES);
		account_entity_limit_usages(pjob, pjob->ji_qhdr,
				&newattr[(int)JOB_ATR_resource], INCR, ETLIM_ACC_ALL_RES);
	}

	/* Now copy the new values into the job attribute array for the purposes of running the action functions */

	for (i = 0; i < JOB_ATR_LAST; i++) {
		if (newattr[i].at_flags & ATR_VFLAG_MODIFY) {
			/*
			 * The function update_eligible_time() expects it is the only one setting accrue_type.
			 * If we set it here, it will get confused.  There is no action function for accrue_type,
			 * so pre-setting it for the action function calls isn't required.
			 */
			if (i == JOB_ATR_accrue_type)
				continue;
			job_attr_def[i].at_free(&pattr[i]);
			if ((pre_copy[i].at_type == ATR_TYPE_LIST) ||
				(pre_copy[i].at_type == ATR_TYPE_RESC)) {
				list_move(&pre_copy[i].at_val.at_list,
					  &pattr[i].at_val.at_list);
			} else {
				pattr[i] = pre_copy[i];
			}
			/* ATR_VFLAG_MODCACHE will be included if set */
			pattr[i].at_flags = pre_copy[i].at_flags;
		}
	}

	for (i = 0; i < JOB_ATR_LAST; i++) {
		/* Check newattr instead of pattr for modify.  It is possible that
		 * the attribute already has the modify flag before we added the new attributes to it.
		 * We only want to call the action functions for attributes which are being modified by this function.
		 */
		if (newattr[i].at_flags & ATR_VFLAG_MODIFY) {
			if ((job_attr_def[i].at_flags & ATR_DFLAG_NOSAVM))
				continue;
				
			if (job_attr_def[i].at_action) {
				rc = job_attr_def[i].at_action(&newattr[i],
					pjob, ATR_ACTION_ALTER);
				if (rc) {
					*bad = i;
					break;
				}
			}
		}
	}
	if (rc) {
		attr_atomic_copy(pjob->ji_wattr, attr_save, job_attr_def, JOB_ATR_LAST);
		free(pre_copy);
		attr_atomic_kill(newattr, job_attr_def, JOB_ATR_LAST);
		attr_atomic_kill(attr_save, job_attr_def, JOB_ATR_LAST);
		return (rc);
	}

	/* The action functions may have modified the attributes, need to set them to newattr2 */
	for (i = 0; i < JOB_ATR_LAST; i++) {
		if (newattr[i].at_flags & ATR_VFLAG_MODIFY) {
			job_attr_def[i].at_free(&pattr[i]);
			switch (i) {
				case JOB_ATR_state:
					newstate = state_char2int(newattr[i].at_val.at_char);
					break;
				case JOB_ATR_substate:
					newsubstate = newattr[i].at_val.at_long;
					break;
				case JOB_ATR_accrue_type:
					newaccruetype = newattr[i].at_val.at_long;
					break;
				default:
					if ((newattr[i].at_type == ATR_TYPE_LIST) ||
					    (newattr[i].at_type == ATR_TYPE_RESC)) {
						list_move(&newattr[i].at_val.at_list,
							  &pattr[i].at_val.at_list);
					} else {
						pattr[i] = newattr[i];
					}
			}
			/* ATR_VFLAG_MODCACHE will be included if set */
			pattr[i].at_flags = newattr[i].at_flags;
		}
	}

	if (newstate != -1 && newsubstate != -1) {
		svr_setjobstate(pjob, newstate, newsubstate);
	}

	if (newaccruetype != -1)
		update_eligible_time(newaccruetype, pjob);

	free(newattr);
	free(pre_copy);
	attr_atomic_kill(attr_save, job_attr_def, JOB_ATR_LAST);
	return (0);
}

/**
 * @brief Service the Modify Reservation Request from client such as pbs_ralter.
 *
 *	This request atomically modifies one or more of a reservation's attributes.
 *	An error is returned to the client if the user does not have permission
 *	to perform the modification, the attribute is read-only, the reservation is
 *	running and the attribute is only modifiable when the reservation is not
 *	running or is empty.
 *
 * @param[in] preq - pointer to batch request from client
 */
void
req_modifyReservation(struct batch_request *preq)
{
	char		*rid = NULL;
	svrattrl	*psatl = NULL;
	attribute_def	*pdef = NULL;
	int		rc = 0;
	int		bad = 0;
	char		buf[PBS_MAXUSER + PBS_MAXHOSTNAME + 32] = {0};
	int		sock;
	int		resc_access_perm_save = 0;
	int		send_to_scheduler = 0;
	int		log_len = 0;
	char		*fmt = "%a %b %d %H:%M:%S %Y";
	int		is_standing = 0;
	int		next_occr_start = 0;
	extern char	*msg_stdg_resv_occr_conflict;
	resc_resv	*presv;
	int num_jobs;
	long new_end_time = 0;

	if (preq == NULL)
		return;

	sock = preq->rq_conn;

	presv = chk_rescResv_request(preq->rq_ind.rq_modify.rq_objname, preq);
	/* Note: on failure, chk_rescResv_request invokes req_reject
	 * appropriate reply is sent and batch_request is freed.
	 */
	if (presv == NULL)
		return;

	rid = preq->rq_ind.rq_modify.rq_objname;
	if ((presv = find_resv(rid)) == NULL) {
		/* Not on "all_resvs" list try "new_resvs" list */
		presv = (resc_resv *)GET_NEXT(svr_newresvs);
		while (presv) {
			if (!strcmp(presv->ri_qs.ri_resvID, rid))
				break;
			presv = (resc_resv *)GET_NEXT(presv->ri_allresvs);
		}
	}

	if (presv == NULL) {
		req_reject(PBSE_UNKRESVID, 0, preq);
		return;
	}

	num_jobs = presv->ri_qp->qu_numjobs;
	if (svr_chk_history_conf()) {
		num_jobs -= (presv->ri_qp->qu_njstate[JOB_STATE_MOVED] + presv->ri_qp->qu_njstate[JOB_STATE_FINISHED] +
			presv->ri_qp->qu_njstate[JOB_STATE_EXPIRED]);
	}

	is_standing = presv->ri_wattr[RESV_ATR_resv_standing].at_val.at_long;
	if (is_standing)
		next_occr_start = get_occurrence(presv->ri_wattr[RESV_ATR_resv_rrule].at_val.at_str,
					presv->ri_wattr[RESV_ATR_start].at_val.at_long,
					presv->ri_wattr[RESV_ATR_resv_timezone].at_val.at_str, 2);

	resc_access_perm_save = resc_access_perm;
	psatl = (svrattrl *)GET_NEXT(preq->rq_ind.rq_modify.rq_attr);
	presv->ri_alter_flags = 0;
	presv->ri_alter_state = presv->ri_wattr[RESV_ATR_state].at_val.at_long;

	while (psatl) {
		long temp = 0;
		char *end = NULL;
		int index;

		/* identify the attribute by name */
		index = find_attr(resv_attr_def, psatl->al_name, RESV_ATR_LAST);
		if (index < 0) {
			/* didn`t recognize the name */
			reply_badattr(PBSE_NOATTR, 1, psatl, preq);
			return;
		}
		pdef = &resv_attr_def[index];

		/* Does attribute's definition flags indicate that
		 * we have sufficient permission to write the attribute?
		 */

		resc_access_perm = resc_access_perm_save; /* reset */
		if (psatl->al_flags & ATR_VFLAG_HOOK) {
			resc_access_perm = ATR_DFLAG_USWR |
					   ATR_DFLAG_OPWR |
					   ATR_DFLAG_MGWR |
					   ATR_DFLAG_SvWR |
					   ATR_DFLAG_Creat;
		}
		if ((pdef->at_flags & resc_access_perm) == 0) {
			reply_badattr(PBSE_ATTRRO, 1, psatl, preq);
			return;
		}

		switch (index) {
			case RESV_ATR_start:
				if ((presv->ri_wattr[RESV_ATR_state].at_val.at_long != RESV_RUNNING) || !num_jobs) {
					temp = strtol(psatl->al_value, &end, 10);
					if (temp > time(NULL)) {
						if (!is_standing || (temp < next_occr_start)) {
							send_to_scheduler = RESV_START_TIME_MODIFIED;
							presv->ri_alter_stime = presv->ri_wattr[RESV_ATR_start].at_val.at_long;
							presv->ri_alter_flags |= RESV_START_TIME_MODIFIED;
						} else {
							resv_revert_alter_times(presv);
							snprintf(log_buffer, sizeof(log_buffer), "%s", msg_stdg_resv_occr_conflict);
							log_event(PBSEVENT_RESV, PBS_EVENTCLASS_RESV, LOG_INFO,
								preq->rq_ind.rq_modify.rq_objname, log_buffer);
							req_reject(PBSE_STDG_RESV_OCCR_CONFLICT, 0, preq);
							return;
						}
					} else {
						resv_revert_alter_times(presv);
						req_reject(PBSE_BADTSPEC, 0, preq);
						return;
					}
				} else {
					resv_revert_alter_times(presv);
					if (num_jobs)
						req_reject(PBSE_RESV_NOT_EMPTY, 0, preq);
					else
						req_reject(PBSE_BADTSPEC, 0, preq);
					return;
				}

				break;
			case RESV_ATR_end:
				temp = strtol(psatl->al_value, &end, 10);
				if (!is_standing || temp < next_occr_start) {
					send_to_scheduler = RESV_END_TIME_MODIFIED;
					presv->ri_alter_etime = presv->ri_wattr[RESV_ATR_end].at_val.at_long;
					presv->ri_alter_flags |= RESV_END_TIME_MODIFIED;
				} else {
					resv_revert_alter_times(presv);
					snprintf(log_buffer, sizeof(log_buffer), "%s", msg_stdg_resv_occr_conflict);
					log_event(PBSEVENT_RESV, PBS_EVENTCLASS_RESV, LOG_INFO,
						preq->rq_ind.rq_modify.rq_objname, log_buffer);
					req_reject(PBSE_STDG_RESV_OCCR_CONFLICT, 0, preq);
					return;
				}

				break;
			case RESV_ATR_duration:
				send_to_scheduler = RESV_DURATION_MODIFIED;
				presv->ri_alter_flags |= RESV_DURATION_MODIFIED;
				break;
			default:
				break;
		}

		/* decode attribute */
		rc = pdef->at_decode(&presv->ri_wattr[index],
			psatl->al_name, psatl->al_resc, psatl->al_value);

		if (rc != 0) {
			reply_badattr(rc, 1, psatl, preq);
			return;
		}

		psatl = (svrattrl *)GET_NEXT(psatl->al_link);
	}


	if (presv->ri_wattr[RESV_ATR_state].at_val.at_long == RESV_RUNNING && num_jobs) {
		if ((presv->ri_alter_flags & RESV_DURATION_MODIFIED) && (presv->ri_alter_flags & RESV_END_TIME_MODIFIED)) {
			resv_revert_alter_times(presv);
			req_reject(PBSE_RESV_NOT_EMPTY, 0, preq);
			return;
		}
	}
	resc_access_perm = resc_access_perm_save; /* restore perm */

	new_end_time = presv->ri_wattr[RESV_ATR_start].at_val.at_long + presv->ri_wattr[RESV_ATR_duration].at_val.at_long;

	if ((presv->ri_alter_flags & RESV_DURATION_MODIFIED) && presv->ri_alter_etime == 0) {
		if (!is_standing || new_end_time < next_occr_start) {
			presv->ri_alter_etime = presv->ri_wattr[RESV_ATR_end].at_val.at_long;
		} else {
			log_event(PBSEVENT_RESV, PBS_EVENTCLASS_RESV, LOG_INFO,
				preq->rq_ind.rq_modify.rq_objname, msg_stdg_resv_occr_conflict);
			req_reject(PBSE_STDG_RESV_OCCR_CONFLICT, 0, preq);
			resv_revert_alter_times(presv);
			return;
		}
	}

	if ((presv->ri_alter_flags & RESV_DURATION_MODIFIED) && presv->ri_alter_stime == 0) {
		if (!is_standing || new_end_time < next_occr_start) {
			presv->ri_alter_stime = presv->ri_wattr[RESV_ATR_start].at_val.at_long;
		} else {
			log_event(PBSEVENT_RESV, PBS_EVENTCLASS_RESV, LOG_INFO,
				preq->rq_ind.rq_modify.rq_objname, msg_stdg_resv_occr_conflict);
			req_reject(PBSE_STDG_RESV_OCCR_CONFLICT, 0, preq);
			resv_revert_alter_times(presv);
			return;
		}
	}


	if (send_to_scheduler) {
		resv_setResvState(presv, RESV_BEING_ALTERED, presv->ri_qs.ri_substate);
		/*"start", "end","duration", and "wall"; derive and check */
		if (start_end_dur_wall(presv, RESC_RESV_OBJECT)) {
			req_reject(PBSE_BADTSPEC, 0, preq);
			resv_revert_alter_times(presv);
			return;
		}
		presv->ri_wattr[RESV_ATR_resource].at_flags |= ATR_VFLAG_SET | ATR_VFLAG_MODIFY | ATR_VFLAG_MODCACHE;
	}
	bad = 0;
	psatl = (svrattrl *)GET_NEXT(preq->rq_ind.rq_modify.rq_attr);
	if (psatl)
		rc = modify_resv_attr(presv, psatl, preq->rq_perm, &bad);

	/* If Authorized_Groups is modified, we need to update the queue's acl_users
	 * Authorized_Users cannot be unset, it must always have a value
	 * The queue will have acl_user_enable set to 1 by default
	 * If Authorized_Groups is modified, we need to update the queue's acl_groups and acl_group_enable
	 * Authorized_Groups could be unset, so we need to update the queue accordingly, unsetting both acl_groups and acl_group_enable
	 */
	if (presv->ri_wattr[(int)RESV_ATR_auth_u].at_flags & ATR_VFLAG_MODIFY) {
		svrattrl *pattrl;
		resv_attr_def[(int)RESV_ATR_auth_u].at_encode(&presv->ri_wattr[(int)RESV_ATR_auth_u], NULL, resv_attr_def[(int)RESV_ATR_auth_u].at_name, NULL, ATR_ENCODE_CLIENT, &pattrl);
		set_attr_svr(&presv->ri_qp->qu_attr[(int)QA_ATR_AclUsers], &que_attr_def[(int)QA_ATR_AclUsers], pattrl->al_atopl.value);
		free(pattrl);
	}
	if (presv->ri_wattr[(int)RESV_ATR_auth_g].at_flags & ATR_VFLAG_MODIFY) {
		if (presv->ri_wattr[(int)RESV_ATR_auth_g].at_flags & ATR_VFLAG_SET) {
			svrattrl *pattrl = NULL;
			resv_attr_def[(int)RESV_ATR_auth_g].at_encode(&presv->ri_wattr[(int)RESV_ATR_auth_g], NULL, resv_attr_def[(int)RESV_ATR_auth_g].at_name, NULL, ATR_ENCODE_CLIENT, &pattrl);
			set_attr_svr(&presv->ri_qp->qu_attr[(int)QE_ATR_AclGroup], &que_attr_def[(int)QE_ATR_AclGroup], pattrl->al_atopl.value);
			if (!(presv->ri_qp->qu_attr[(int)QE_ATR_AclGroupEnabled].at_flags & ATR_VFLAG_SET) ||
				(presv->ri_qp->qu_attr[(int)QE_ATR_AclGroupEnabled].at_val.at_long == 0)) {
				presv->ri_qp->qu_attr[(int)QE_ATR_AclGroupEnabled].at_val.at_long = 1;
				presv->ri_qp->qu_attr[(int)QE_ATR_AclGroupEnabled].at_flags |= ATR_VFLAG_SET | ATR_VFLAG_MODIFY | ATR_VFLAG_MODCACHE;
			}
			que_save_db(presv->ri_qp);
			free(pattrl);
		} else {
			resv_attr_def[(int)RESV_ATR_auth_g].at_free(&presv->ri_wattr[(int)RESV_ATR_auth_g]);
			presv->ri_wattr[(int)RESV_ATR_auth_g].at_flags |= ATR_VFLAG_MODIFY;
			que_attr_def[(int)QE_ATR_AclGroup].at_free(&presv->ri_qp->qu_attr[(int)QE_ATR_AclGroup]);
			presv->ri_qp->qu_attr[(int)QE_ATR_AclGroup].at_flags |= ATR_VFLAG_MODIFY;
			que_attr_def[(int)QE_ATR_AclGroupEnabled].at_free(&presv->ri_qp->qu_attr[(int)QE_ATR_AclGroupEnabled]);
			presv->ri_qp->qu_attr[(int)QE_ATR_AclGroupEnabled].at_flags |= ATR_VFLAG_MODIFY;
			que_save_db(presv->ri_qp);
		}
	}

	if (send_to_scheduler)
		notify_scheds_about_resv(SCH_SCHEDULE_RESV_RECONFIRM, presv);

	(void)sprintf(log_buffer, "Attempting to modify reservation");
	if (presv->ri_alter_flags & RESV_START_TIME_MODIFIED) {
		strftime(buf, sizeof(buf), fmt, localtime((time_t *) &presv->ri_wattr[RESV_ATR_start].at_val.at_long));
		log_len = strlen(log_buffer);
		snprintf(log_buffer + log_len, sizeof(log_buffer) - log_len," start=%s", buf);
	}
	if (presv->ri_alter_flags & RESV_END_TIME_MODIFIED) {
		strftime(buf, sizeof(buf), fmt, localtime((time_t *) &presv->ri_wattr[RESV_ATR_end].at_val.at_long));
		log_len = strlen(log_buffer);
		snprintf(log_buffer + log_len, sizeof(log_buffer) - log_len," end=%s", buf);
	}
	log_event(PBSEVENT_RESV, PBS_EVENTCLASS_RESV, LOG_INFO, preq->rq_ind.rq_modify.rq_objname, log_buffer);

	if ((presv->ri_wattr[RESV_ATR_interactive].at_flags &
		ATR_VFLAG_SET) == 0) {
		char buf1[PBS_MAXUSER + PBS_MAXHOSTNAME + 32] = {0};
		/*Not "interactive" so don't wait on scheduler, reply now*/

		sprintf(buf, "%s ALTER REQUESTED",  presv->ri_qs.ri_resvID);
		sprintf(buf1, "requestor=%s@%s", preq->rq_user, preq->rq_host);

		if ((rc = reply_text(preq, PBSE_NONE, buf))) {
			/* reply failed,  close connection; DON'T purge resv */
			close_client(sock);
			return;
		}
	} else {
		/*Don't reply back until scheduler decides*/
		long dt;
		presv->ri_brp = preq;
		dt = presv->ri_wattr[RESV_ATR_interactive].at_val.at_long;
		/*reply with id and state no decision in +dt secs*/
		(void)gen_future_reply(presv, dt);
		(void)snprintf(buf, sizeof(buf), "requestor=%s@%s Interactive=%ld",
			preq->rq_user, preq->rq_host, dt);
	}
}


/**
 * @brief modify the attributes of a reservation atomically.
 *
 * @param[in]  presv - pointer to the reservation structure.
 * @param[in]  plist - list of attributes to modify.
 * @param[in]  perm  - permissions.
 * @param[out] bad   - the index of the attribute which caused an error.
 *
 * @return 0 on success.
 * @return PBS error code.
 */
int
modify_resv_attr(resc_resv *presv, svrattrl *plist, int perm, int *bad)
{
	int	   allow_unkn = 0;
	long	   i = 0;
	attribute  newattr[(int)RESV_ATR_LAST];
	attribute *pattr;
	int	   rc = 0;

	if (presv == NULL || plist == NULL)
		return PBSE_INTERNAL;

	allow_unkn = -1;
	pattr = presv->ri_wattr;

	/* call attr_atomic_set to decode and set a copy of the attributes */

	rc = attr_atomic_set(plist, pattr, newattr, resv_attr_def, RESV_ATR_LAST,
		allow_unkn, perm, bad);

	if (rc == 0) {
		for (i = 0; i < RESV_ATR_LAST; i++) {
			if (newattr[i].at_flags & ATR_VFLAG_MODIFY) {
				if (resv_attr_def[i].at_action) {
					rc = resv_attr_def[i].at_action(&newattr[i],
						presv, ATR_ACTION_ALTER);
					if (rc)
						break;
				}
			}
		}
		if ((rc == 0) &&
			((newattr[(int)RESV_ATR_userlst].at_flags & ATR_VFLAG_MODIFY) ||
			(newattr[(int)RESV_ATR_grouplst].at_flags & ATR_VFLAG_MODIFY))) {
			/* Need to reset execution uid and gid */
			rc = set_objexid((void *)presv, JOB_OBJECT, newattr);
		}

	}
	if (rc) {
		for (i = 0; i < RESV_ATR_LAST; i++)
			resv_attr_def[i].at_free(newattr+i);
		return (rc);
	}

	/* Now copy the new values into the reservation attribute array */

	for (i = 0; i < RESV_ATR_LAST; i++) {
		if (newattr[i].at_flags & ATR_VFLAG_MODIFY) {
			resv_attr_def[i].at_free(pattr+i);
			if ((newattr[i].at_type == ATR_TYPE_LIST) || (newattr[i].at_type == ATR_TYPE_RESC)) {
				list_move(&newattr[i].at_val.at_list, &(pattr+i)->at_val.at_list);
			} else {
				*(pattr+i) = newattr[i];
			}
			/* ATR_VFLAG_MODCACHE will be included if set */
			(pattr+i)->at_flags = newattr[i].at_flags;
		}
	}

	return (0);
}
