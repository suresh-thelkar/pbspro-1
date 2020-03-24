/*
 * Copyright (C) 1994-2020 Altair Engineering, Inc.
 * For more information, contact Altair at www.altair.com.
 *
 * This file is part of the PBS Professional ("PBS Pro") software.
 *
 * Open Source License Information:
 *
 * PBS Pro is free software. You can redistribute it and/or modify it under the
 * terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * PBS Pro is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.
 * See the GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Commercial License Information:
 *
 * For a copy of the commercial license terms and conditions,
 * go to: (http://www.pbspro.com/UserArea/agreement.html)
 * or contact the Altair Legal Department.
 *
 * Altair’s dual-license business model allows companies, individuals, and
 * organizations to create proprietary derivative works of PBS Pro and
 * distribute them - whether embedded or bundled with other software -
 * under a commercial license agreement.
 *
 * Use of Altair’s trademarks, including but not limited to "PBS™",
 * "PBS Professional®", and "PBS Pro™" and Altair’s logos is subject to Altair's
 * trademark licensing policies.
 *
 */
/**
 * @file	req_stat.c
 *
 * @brief
 * 		req_stat.c - Functions relating to the Status Job, Status Queue, and
 * 		Status Server Batch Requests.
 *
 * Functions included are:
 * 	do_stat_of_a_job()
 * 	stat_a_jobidname()
 * 	req_stat_job()
 * 	req_stat_que()
 * 	status_que()
 * 	req_stat_node()
 * 	status_node()
 * 	req_stat_svr()
 * 	req_stat_sched()
 * 	update_state_ct()
 * 	update_license_ct()
 * 	req_stat_resv()
 * 	status_resv()
 * 	status_resc()
 * 	req_stat_resc()
 *
 */
#include <pbs_config.h>   /* the master config generated by configure */

#define STAT_CNTL 1

#include <stdio.h>
#include <sys/types.h>
#include <stdlib.h>
#include "libpbs.h"
#include <ctype.h>
#include "server_limits.h"
#include "list_link.h"
#include "attribute.h"
#include "server.h"
#include "credential.h"
#include "batch_request.h"
#include "job.h"
#include "reservation.h"
#include "queue.h"
#include "work_task.h"
#include "pbs_entlim.h"
#include "pbs_error.h"
#include "pbs_nodes.h"
#include "svrfunc.h"
#include "net_connect.h"
#include "pbs_license.h"
#include "resource.h"
#include "pbs_sched.h"


/* Global Data Items: */

extern struct server server;
extern pbs_list_head svr_alljobs;
extern pbs_list_head svr_queues;
extern char          server_name[];
extern attribute_def svr_attr_def[];
extern attribute_def que_attr_def[];
extern attribute_def job_attr_def[];
extern time_t	     time_now;
extern char	    *msg_init_norerun;
extern int resc_access_perm;
extern long svr_history_enable;

/* Extern Functions */

extern int status_attrib(svrattrl *, attribute_def *, attribute *,
	int, int, pbs_list_head *, int *);
extern int status_nodeattrib(svrattrl *, attribute_def *, struct pbsnode *,
	int, int, pbs_list_head *, int *);

extern int svr_chk_histjob(job *);


/* Private Data Definitions */

static int bad;

/* The following private support functions are included */

static int status_que(pbs_queue *, struct batch_request *, pbs_list_head *);
static int status_node(struct pbsnode *, struct batch_request *, pbs_list_head *);
static int status_resv(resc_resv *, struct batch_request *, pbs_list_head *);
/**
 * @brief
 * 		Support function for req_stat_job() and stat_a_jobidname().
 * 		Builds status reply for normal job, Array job, and if requested all of the
 * 		subjobs of the array (but not a single or range of subjobs).
 * @par
 * 		Note,  if dohistjobs is not set and the job is history, no status or error
 * 		is returned.  If an error return is needed, the caller must make that check.
 *
 * @param[in,out]	preq	-	pointer to the stat job batch request, reply updated
 * @param[in]	pjob	-	pointer to the job to be statused
 * @param[in]	dohistjobs	-	flag to include job if it is a history job
 * @param[in]	dosubjobs	-	flag to expand a Array job to include all subjobs
 *
 * @return	int
 * @retval	PBSE_NONE (0)	: no error
 * @retval	non-zero	: PBS error code to return to client
 */
static int
do_stat_of_a_job(struct batch_request *preq, job *pjob, int dohistjobs, int dosubjobs)
{
	int       indx;
	svrattrl *pal;
	int       rc;
	struct batch_reply *preply = &preq->rq_reply;

	/* if history job and not asking for them, just return */
	if ((!dohistjobs) &&
			((pjob->ji_qs.ji_state == JOB_STATE_FINISHED) ||
			(pjob->ji_qs.ji_state == JOB_STATE_MOVED))) {
		return (PBSE_NONE);	/* just return nothing */
	}

	if ((pjob->ji_qs.ji_svrflags & JOB_SVFLG_SubJob) == 0) {
		/* this is not a subjob, go ahead and  */
		/* build the status reply for this job */

		pal = (svrattrl *)GET_NEXT(preq->rq_ind.rq_status.rq_attr);

		rc = status_job(pjob, preq, pal, &preply->brp_un.brp_status, &bad);
		if (dosubjobs && (pjob->ji_qs.ji_svrflags & JOB_SVFLG_ArrayJob) &&
			((rc == PBSE_NONE) || (rc != PBSE_PERM)) && pjob->ji_ajtrk != NULL) {

		    for (indx=0; indx<pjob->ji_ajtrk->tkm_ct; ++indx) {
			 rc = status_subjob(pjob, preq, pal, indx, &preply->brp_un.brp_status, &bad);
			    if (rc && (rc != PBSE_PERM))
				break;
		    }
		}
		if (rc && (rc != PBSE_PERM)) {
			return (rc);
		}
	}
	return (PBSE_NONE);
}

/**
 * @brief
 * 		Support function for req_stat_job().
 * 		Builds status reply for a single job id, which may be: a normal job,
 * 		an Array job, a single subjob or a range of subjobs.
 * 		Finds the job structure for the job id and calls either do_stat_of_a_job()
 * 		or status_subjob() to build that actual status reply.
 *
 * @param[in,out]	preq	-	pointer to the stat job batch request, reply updated
 * @param[in]	name	-	job id to be statused
 * @param[in]	dohistjobs	-	flag to include job if it is a history job
 * @param[in]	dosubjobs	-	flag to expand a Array job to include all subjobs
 *
 * @return	int
 * @retval	PBSE_NONE (0)	: no error
 * @retval	non-zero	: PBS error code to return to client
 */
static int
stat_a_jobidname(struct batch_request *preq, char *name, int dohistjobs, int dosubjobs)
{
	int   i, indx, x, y, z;
	char *pc;
	char *range;
	int   rc;
	job  *pjob;
	struct batch_reply *preply = &preq->rq_reply;
	svrattrl	   *pal;

	if ((i = is_job_array(name)) == IS_ARRAY_Single) {
		pjob = find_arrayparent(name);
		if (pjob == NULL) {
			return (PBSE_UNKJOBID);
		} else if ((!dohistjobs) && (rc = svr_chk_histjob(pjob))) {
			return (rc);
		}
		indx = subjob_index_to_offset(pjob, get_index_from_jid(name));
		if (indx != -1) {
			pal = (svrattrl *)GET_NEXT(preq->rq_ind.rq_status.rq_attr);
			rc = status_subjob(pjob, preq, pal, indx, &preply->brp_un.brp_status, &bad);
		} else {
			rc = PBSE_UNKJOBID;
		}
		return (rc);	/* no job still needs to be stat-ed */

	} else if ((i == IS_ARRAY_NO) || (i == IS_ARRAY_ArrayJob)) {
		pjob = find_job(name);
		if (pjob == NULL) {
			return (PBSE_UNKJOBID);
		} else if ((!dohistjobs) && (rc = svr_chk_histjob(pjob))) {
			return (rc);
		}
		return (do_stat_of_a_job(preq, pjob, dohistjobs, dosubjobs));
	} else {
		/* range of sub jobs */
		range = get_index_from_jid(name);
		if (range == NULL) {
			return (PBSE_IVALREQ);
		}
		pjob = find_arrayparent(name);
		if (pjob == NULL) {
			return (PBSE_UNKJOBID);
		} else if ((!dohistjobs) && (rc = svr_chk_histjob(pjob))) {
			return (rc);
		}
		pal = (svrattrl *)GET_NEXT(preq->rq_ind.rq_status.rq_attr);
		while (1) {
			if ((i=parse_subjob_index(range,&pc,&x,&y,&z,&i)) == -1) {
		    		return (PBSE_IVALREQ);
			} else if (i == 1)
				break;
			while (x <= y) {
				indx = numindex_to_offset(pjob, x);
				if (indx < 0) {
					x += z;
					continue;
				}
				rc = status_subjob(pjob, preq, pal, indx, &preply->brp_un.brp_status, &bad);
				if (rc && (rc != PBSE_PERM)) {
					return (rc);
				}
				x += z;
			}
			range = pc;
		}
		/* stat-ed the range, no more to stat for this id */
		return (PBSE_NONE);
	}
}

/**
 * @brief
 * 		Service the Status Job Request
 * @par
 * 		This request processes the request for status of a single job or
 * 		the set of jobs at a destination.  It uses the currently known data
 * 		for resources_used in the case of a running job.  If Mom for that
 * 		job is down, the data is likely stale.
 * @par
 * 		The requested object may be a job id (either a single regular job, an Array
 * 		job, a subjob or a range of subjobs), a comma separated list of the above,
 * 		a queue name or null (or @...) for all jobs in the Server.
 *
 * @param[in,out]	preq	-	pointer to the stat job batch request, reply updated
 *
 * @return	void
 */

void req_stat_job(struct batch_request *preq)
{
	int		    at_least_one_success = 0;
	int		    dosubjobs = 0;
	int		    dohistjobs = 0;
	char		   *name;
	job		   *pjob = NULL;
	pbs_queue	   *pque = NULL;
	struct batch_reply *preply;
	int		    rc   = 0;
	int		    type = 0;
	char		   *pnxtjid = NULL;

	/* check for any extended flag in the batch request. 't' for
	 * the sub jobs. If 'x' is there, then check if the server is
	 * configured for history job info. If not set or set to FALSE,
	 * return with PBSE_JOBHISTNOTSET error. Otherwise select history
	 * jobs.
	 */
	if (preq->rq_extend) {
		if (strchr(preq->rq_extend, (int)'t'))
			dosubjobs = 1;	/* status sub jobs of an Array Job */
		if (strchr(preq->rq_extend, (int)'x')) {
			if (svr_history_enable == 0) {
				req_reject(PBSE_JOBHISTNOTSET, 0, preq);
				return;
			}
			dohistjobs = 1;	/* status history jobs */
		}
	}

	/*
	 * first, validate the name of the requested object, either
	 * a job, a queue, or the whole server.
	 * type = 1 for a job, Array job, subjob or range of subjobs, or
	 *          a comma separated list of  the above.
	 *        2 for jobs in a queue,
	 *        3 for jobs in the server, or
	 */

	name = preq->rq_ind.rq_status.rq_id;

	if ( isdigit((int)*name) ) {
		/* a single job id */
		type = 1;
		rc = PBSE_UNKJOBID;

	} else if (isalpha((int)*name) ) {
		pque = find_queuebyname(name)	/* status jobs in a queue */;
#ifdef NAS /* localmod 075 */
		if (pque == NULL)
			pque = find_resvqueuebyname(name);
#endif /* localmod 075 */
		if (pque)
			type = 2;
		else
			rc = PBSE_UNKQUE;

	} else if ((*name == '\0') || (*name == '@')) {
		type = 3;	/* status all jobs at server */
	} else
		rc = PBSE_IVALREQ;

	if (type == 0) {		/* is invalid - an error */
		req_reject(rc, 0, preq);
		return;
	}
	preply = &preq->rq_reply;
	preply->brp_choice = BATCH_REPLY_CHOICE_Status;
	CLEAR_HEAD(preply->brp_un.brp_status);

	rc = PBSE_NONE;

	if (type == 1) {
		/*
		 * If there is more than one job id, any status for any
		 * one job is returned, then no error is given.
		 * If a single job id is requested and there is an error
		 * the error is returned.
		 */
		pnxtjid = name;
		while ((name = parse_comma_string_r(&pnxtjid)) != NULL) {
			if ((rc = stat_a_jobidname(preq, name, dohistjobs, dosubjobs)) == PBSE_NONE)
				at_least_one_success = 1;
		}
		if (at_least_one_success == 1)
			reply_send(preq);
		else
			req_reject(rc, 0, preq);
		return;

	} else if (type == 2) {
		pjob = (job *)GET_NEXT(pque->qu_jobs);
		while (pjob && (rc == PBSE_NONE)) {
			rc = do_stat_of_a_job(preq, pjob, dohistjobs, dosubjobs);
			pjob = (job *)GET_NEXT(pjob->ji_jobque);
		}
	} else {
		pjob = (job *)GET_NEXT(svr_alljobs);
		while (pjob && (rc == PBSE_NONE)) {
			rc = do_stat_of_a_job(preq, pjob, dohistjobs, dosubjobs);
			pjob = (job *)GET_NEXT(pjob->ji_alljobs);
		}

	}

	if (rc && (rc != PBSE_PERM))
		req_reject(rc, bad, preq);
	else
		reply_send(preq);
}


/**
 * @brief
 * 		req_stat_que - service the Status Queue Request
 *
 *		This request processes the request for status of a single queue or
 *		the set of queues at a destination.
 *
 * @param[in,out]	preq	-	ptr to the decoded request
 */

void
req_stat_que(struct batch_request *preq)
{
	char		   *name;
	pbs_queue	   *pque;
	struct batch_reply *preply;
	int		    rc   = 0;
	int		    type = 0;

	/*
	 * first, validate the name of the requested object, either
	 * a queue, or null for all queues
	 */

	name = preq->rq_ind.rq_status.rq_id;

	if ((*name == '\0') || (*name =='@'))
		type = 1;
	else {
		pque = find_queuebyname(name);
#ifdef NAS /* localmod 075 */
		if (pque == NULL)
			pque = find_resvqueuebyname(name);
#endif /* localmod 075 */
		if (pque == NULL) {
			req_reject(PBSE_UNKQUE, 0, preq);
			return;
		}
	}

	preply = &preq->rq_reply;
	preply->brp_choice = BATCH_REPLY_CHOICE_Status;
	CLEAR_HEAD(preply->brp_un.brp_status);

	if (type == 0) {	/* get status of the one named queue */
		rc = status_que(pque, preq, &preply->brp_un.brp_status);

	} else {	/* get status of queues */

		pque = (pbs_queue *)GET_NEXT(svr_queues);
		while (pque) {
			rc = status_que(pque, preq, &preply->brp_un.brp_status);
			if (rc != 0) {
				if (rc == PBSE_PERM)
					rc = 0;
				else
					break;
			}
			pque = (pbs_queue *)GET_NEXT(pque->qu_link);
		}
	}
	if (rc) {
		(void)reply_free(preply);
		req_reject(rc, bad, preq);
	} else {
		(void)reply_send(preq);
	}
}

/**
 * @brief
 * 		status_que - Build the status reply for a single queue.
 *
 * @param[in,out]	pque	-	ptr to que to status
 * @param[in]		preq	-	ptr to the decoded request
 * @param[in,out]	pstathd	-	head of list to append status to
 *
 * @return	int
 * @retval	0	: success
 * @retval	!0	: PBSE error code
 */

static int
status_que(pbs_queue *pque, struct batch_request *preq, pbs_list_head *pstathd)
{
	struct brp_status *pstat;
	svrattrl	  *pal;

	if ((preq->rq_perm & ATR_DFLAG_RDACC) == 0)
		return (PBSE_PERM);

	/* ok going to do status, update count and state counts from qu_qs */

	if (!svr_chk_history_conf()) {
		pque->qu_attr[(int)QA_ATR_TotalJobs].at_val.at_long = pque->qu_numjobs;
	} else {
		pque->qu_attr[(int)QA_ATR_TotalJobs].at_val.at_long = pque->qu_numjobs -
			(pque->qu_njstate[JOB_STATE_MOVED] + pque->qu_njstate[JOB_STATE_FINISHED] + pque->qu_njstate[JOB_STATE_EXPIRED]);
	}
	pque->qu_attr[(int)QA_ATR_TotalJobs].at_flags |= ATR_VFLAG_SET|ATR_VFLAG_MODCACHE;

	update_state_ct(&pque->qu_attr[(int)QA_ATR_JobsByState],
		pque->qu_njstate,
		pque->qu_jobstbuf);

	/* allocate status sub-structure and fill in header portion */

	pstat = (struct brp_status *)malloc(sizeof(struct brp_status));
	if (pstat == NULL)
		return (PBSE_SYSTEM);
	pstat->brp_objtype = MGR_OBJ_QUEUE;
	(void)strcpy(pstat->brp_objname, pque->qu_qs.qu_name);
	CLEAR_LINK(pstat->brp_stlink);
	CLEAR_HEAD(pstat->brp_attr);
	append_link(pstathd, &pstat->brp_stlink, pstat);

	/* add attributes to the status reply */

	bad = 0;
	pal = (svrattrl *)GET_NEXT(preq->rq_ind.rq_status.rq_attr);
	if (status_attrib(pal, que_attr_def, pque->qu_attr, QA_ATR_LAST,
		preq->rq_perm, &pstat->brp_attr, &bad))
		return (PBSE_NOATTR);

	return (0);
}



/**
 * @brief
 * 		req_stat_node - service the Status Node Request
 *
 *		This request processes the request for status of a single node or
 *		set of nodes at a destination.
 *
 * @param[in]	preq	-	ptr to the decoded request
 */

void
req_stat_node(struct batch_request *preq)
{
	char		    *name;
	struct batch_reply  *preply;
	svrattrl	    *pal;
	struct pbsnode	    *pnode = NULL;
	int		    rc   = 0;
	int		    type = 0;
	int		    i;

	/*
	 * first, check that the server indeed has a list of nodes
	 * and if it does, validate the name of the requested object--
	 * either name is that of a spedific node, or name[0] is null/@
	 * meaning request is for all nodes in the server's jurisdiction
	 */

	if (pbsndlist == 0  ||  svr_totnodes <= 0) {
		req_reject(PBSE_NONODES, 0, preq);
		return;
	}

	resc_access_perm = preq->rq_perm;

	name = preq->rq_ind.rq_status.rq_id;

	if ((*name == '\0') || (*name =='@'))
		type = 1;
	else {
		pnode = find_nodebyname(name);
		if (pnode == NULL) {
			req_reject(PBSE_UNKNODE, 0, preq);
			return;
		}
	}

	preply = &preq->rq_reply;
	preply->brp_choice = BATCH_REPLY_CHOICE_Status;
	CLEAR_HEAD(preply->brp_un.brp_status);

	if (type == 0) {		/* get status of the named node */
		rc = status_node(pnode, preq, &preply->brp_un.brp_status);

	} else {			/* get status of all nodes */

		for (i = 0; i < svr_totnodes; i++) {
			pnode = pbsndlist[i];

			rc = status_node(pnode, preq,
				&preply->brp_un.brp_status);
			if (rc)
				break;
		}
	}

	if (!rc) {
		(void)reply_send(preq);
	} else {
		if (rc != PBSE_UNKNODEATR)
			req_reject(rc, 0, preq);

		else {
			pal = (svrattrl *)GET_NEXT(preq->rq_ind.
				rq_status.rq_attr);
			reply_badattr(rc, bad, pal, preq);
		}
	}
}



/**
 * @brief
 * 		status_node - Build the status reply for a single node.
 *
 * @param[in,out]	pnode	-	ptr to node receiving status query
 * @param[in]	preq	-	ptr to the decoded request
 * @param[in,out]	pstathd	-	head of list to append status to
 *
 * @return	int
 * @retval	0	: success
 * @retval	!0	: PBSE error code
 */

static int
status_node(struct pbsnode *pnode, struct batch_request *preq, pbs_list_head *pstathd)
{
	int		   rc = 0;
	struct brp_status *pstat;
	svrattrl	  *pal;
	unsigned long		   old_nd_state = VNODE_UNAVAILABLE;

	if (pnode->nd_state & INUSE_DELETED)  /*node no longer valid*/
		return  (0);

	if ((preq->rq_perm & ATR_DFLAG_RDACC) == 0)
		return (PBSE_PERM);

	/* sync state attribute with nd_state */

	if (pnode->nd_state != pnode->nd_attr[(int)ND_ATR_state].at_val.at_long) {
		pnode->nd_attr[(int)ND_ATR_state].at_val.at_long = pnode->nd_state;
		pnode->nd_attr[(int)ND_ATR_state].at_flags |= ATR_VFLAG_MODIFY |
			ATR_VFLAG_MODCACHE;
	}

	/*node is provisioning - mask out the DOWN/UNKNOWN flags while prov is on*/
	if (pnode->nd_attr[(int)ND_ATR_state].at_val.at_long &
		(INUSE_PROV | INUSE_WAIT_PROV)) {
		old_nd_state = pnode->nd_attr[(int)ND_ATR_state].at_val.at_long;

		/* don't want to show job-busy, job/resv-excl while provisioning */
		pnode->nd_attr[(int)ND_ATR_state].at_val.at_long &=
			~(INUSE_DOWN | INUSE_UNKNOWN | INUSE_JOB |
			INUSE_JOBEXCL | INUSE_RESVEXCL);
	}

	/*allocate status sub-structure and fill in header portion*/

	pstat = (struct brp_status *)malloc(sizeof(struct brp_status));
	if (pstat == NULL)
		return (PBSE_SYSTEM);

	pstat->brp_objtype = MGR_OBJ_NODE;
	(void)strcpy(pstat->brp_objname, pnode->nd_name);
	CLEAR_LINK(pstat->brp_stlink);
	CLEAR_HEAD(pstat->brp_attr);

	/*add this new brp_status structure to the list hanging off*/
	/*the request's reply substructure                         */

	append_link(pstathd, &pstat->brp_stlink, pstat);

	/*point to the list of node-attributes about which we want status*/
	/*hang that status information from the brp_attr field for this  */
	/*brp_status structure                                           */
	bad = 0;                                        /*global variable*/
	pal = (svrattrl *)GET_NEXT(preq->rq_ind.rq_status.rq_attr);

	rc = status_nodeattrib(pal, node_attr_def, pnode, ND_ATR_LAST,
		preq->rq_perm, &pstat->brp_attr, &bad);

	/*reverting back the state*/

	if (pnode->nd_attr[(int)ND_ATR_state].at_val.at_long & INUSE_PROV)
		pnode->nd_attr[(int)ND_ATR_state].at_val.at_long = old_nd_state ;


	return (rc);
}




/**
 * @brief
 * 		req_stat_svr - service the Status Server Request
 * @par
 *		This request processes the request for status of the Server
 *
 * @param[in]	preq	-	ptr to the decoded request
 */

void
req_stat_svr(struct batch_request *preq)
{
	svrattrl	   *pal;
	struct batch_reply *preply;
	struct brp_status  *pstat;


	/* update count and state counts from sv_numjobs and sv_jobstates */

	server.sv_attr[(int)SRV_ATR_TotalJobs].at_val.at_long = server.sv_qs.sv_numjobs;
	server.sv_attr[(int)SRV_ATR_TotalJobs].at_flags |= ATR_VFLAG_SET|ATR_VFLAG_MODCACHE;
	update_state_ct(&server.sv_attr[(int)SRV_ATR_JobsByState],
		server.sv_jobstates,
		server.sv_jobstbuf);

	update_license_ct(&server.sv_attr[(int)SRV_ATR_license_count],
		server.sv_license_ct_buf);

	/* allocate a reply structure and a status sub-structure */

	preply = &preq->rq_reply;
	preply->brp_choice = BATCH_REPLY_CHOICE_Status;
	CLEAR_HEAD(preply->brp_un.brp_status);

	pstat = (struct brp_status *)malloc(sizeof(struct brp_status));
	if (pstat == NULL) {
		reply_free(preply);
		req_reject(PBSE_SYSTEM, 0, preq);
		return;
	}
	CLEAR_LINK(pstat->brp_stlink);
	(void)strcpy(pstat->brp_objname, server_name);
	pstat->brp_objtype = MGR_OBJ_SERVER;
	CLEAR_HEAD(pstat->brp_attr);
	append_link(&preply->brp_un.brp_status, &pstat->brp_stlink, pstat);

	/* add attributes to the status reply */

	bad = 0;
	pal = (svrattrl *)GET_NEXT(preq->rq_ind.rq_status.rq_attr);
	if (status_attrib(pal, svr_attr_def, server.sv_attr, SRV_ATR_LAST,
		preq->rq_perm, &pstat->brp_attr, &bad))
		reply_badattr(PBSE_NOATTR, bad, pal, preq);
	else
		(void)reply_send(preq);
}

/**
 * @brief
 * 		status_sched - Build the status reply for single scheduler
 *
 * @param[in]	psched	-	ptr to sched receiving status query
 * @param[in]	preq	-	ptr to the decoded request
 * @param[out]	pstathd	-	head of list to append status to
 *
 * @return	int
 * @retval	0	: success
 * @retval	!0	: PBSE error code
 */
static int
status_sched(pbs_sched *psched, struct batch_request *preq, pbs_list_head *pstathd)
{
	int		   rc = 0;
	struct brp_status *pstat;
	svrattrl	  *pal;

	pstat = (struct brp_status *)malloc(sizeof(struct brp_status));
	if (pstat == NULL)
		return (PBSE_SYSTEM);

	pstat->brp_objtype = MGR_OBJ_SCHED;
	(void)strncpy(pstat->brp_objname, psched->sc_name, (PBS_MAXSVRJOBID > PBS_MAXDEST ?
			PBS_MAXSVRJOBID : PBS_MAXDEST) -1);
	pstat->brp_objname[(PBS_MAXSVRJOBID > PBS_MAXDEST ? PBS_MAXSVRJOBID : PBS_MAXDEST) - 1] = '\0';

	CLEAR_LINK(pstat->brp_stlink);
	CLEAR_HEAD(pstat->brp_attr);
	append_link(pstathd, &pstat->brp_stlink, pstat);


	bad = 0;
	pal = (svrattrl *)GET_NEXT(preq->rq_ind.rq_status.rq_attr);
	if (status_attrib(pal, sched_attr_def, psched->sch_attr, SCHED_ATR_LAST,
		preq->rq_perm, &pstat->brp_attr, &bad))
		reply_badattr(PBSE_NOATTR, bad, pal, preq);

	return (rc);
}


/**
 * @brief
 * 		req_stat_sched - service a PBS_BATCH_StatusSched request
 * @par
 *		This function processes a request regarding scheduler status
 *
 * @param[in]	preq	-	ptr to the decoded request
 *
 * @par MT-safe: No
 */

void
req_stat_sched(struct batch_request *preq)
{
	svrattrl	   *pal;
	struct batch_reply *preply;
	int rc = 0;
	pbs_sched *psched;

	/* allocate a reply structure and a status sub-structure */

	preply = &preq->rq_reply;
	preply->brp_choice = BATCH_REPLY_CHOICE_Status;
	CLEAR_HEAD(preply->brp_un.brp_status);

	for (psched = (pbs_sched *) GET_NEXT(svr_allscheds);
			(psched != NULL);
			psched = (pbs_sched *) GET_NEXT(psched->sc_link)
		) {
		rc = status_sched(psched, preq, &preply->brp_un.brp_status);
		if (rc != 0) {
			break;
		}
	}

	if (!rc) {
		(void)reply_send(preq);
	} else {
		if (rc != PBSE_NOATTR)
			req_reject(rc, 0, preq);
		else {
			pal = (svrattrl *)GET_NEXT(preq->rq_ind.
				rq_status.rq_attr);
			reply_badattr(rc, bad, pal, preq);
		}
	}
}


/**
 * @brief
 * 		update-state_ct - update the count of jobs per state (in queue and server
 *		attributes.
 *
 * @param[out]	pattr	-	queue or server attribute
 * @param[in]	ct_array	-	number of jobs per state
 * @param[out]	buf	-	job string buffer
 *
 * @par MT-safe: No
 */

void
update_state_ct(attribute *pattr, int *ct_array, char *buf)
{
	static char *statename[] = { "Transit", "Queued", "Held", "Waiting",
		"Running", "Exiting", "Expired", "Begun",
		"Moved", "Finished" };
	int  index;

	buf[0] = '\0';
	for (index=0; index < (PBS_NUMJOBSTATE); index++) {
		if ((index == JOB_STATE_EXPIRED) ||
			(index == JOB_STATE_MOVED) ||
			(index == JOB_STATE_FINISHED))
			continue;	/* skip over Expired/Moved/Finished */
		sprintf(buf+strlen(buf), "%s:%d ", statename[index],
			*(ct_array + index));
	}
	pattr->at_val.at_str = buf;
	pattr->at_flags |= ATR_VFLAG_SET | ATR_VFLAG_MODCACHE;
}

/**
 * @brief
 * 		print_license_ct - update the # of licenses (counters) in buffer
 *		corresponding to the licensing backend.
 *
 * @param[in]	lb	-	licensing backend
 * @param[out]	buf	-	string buffer
 */

void
print_license_ct(enum licensing_backend lb, char *buf)
{
	switch(lb) {
		case LIC_SERVER:
			sprintf(buf, "Avail_Global:%d Avail_Local:%d Used:%d High_Use:%d",
				licenses.lb_glob_floating, licenses.lb_aval_floating,
				licenses.lb_used_floating, licenses.lb_high_used_floating);
			break;
		case LIC_NODES:
			sprintf(buf, "Avail_Nodes:%d Unused_Nodes:%d",
				sockets_total(), sockets_available());
				break;
		case LIC_SOCKETS:
			sprintf(buf, "Avail_Sockets:%d Unused_Sockets:%d",
				sockets_total(), sockets_available());
			break;
		case LIC_TRIAL:
			sprintf(buf, "Avail_Local:%d Used:%d High_Use:%d",
				licenses.lb_aval_floating, licenses.lb_used_floating,
				licenses.lb_high_used_floating);
			break;
		default:
			sprintf(buf, "Avail_Nodes:%d Unused_Nodes:%d", 0, 0);
	}
}

/**
 * @brief
 * 		update_license_ct - update the # of licenses (counters) in 'license_count'
 *			server attribute.
 *
 * @param[out]	pattr	-	server attribute.
 * @param[out]	buf	-	job string buffer
 */

void
update_license_ct(attribute *pattr, char *buf)
{
	buf[0] = '\0';
	if (licstate_is_up(LIC_SERVER))
		print_license_ct(LIC_SERVER, buf);
	else if (licstate_is_up(LIC_NODES))
		print_license_ct(LIC_NODES, buf);
	else if (licstate_is_up(LIC_SOCKETS))
		print_license_ct(LIC_SOCKETS, buf);
	else if (licenses.lb_trial == 1)
		print_license_ct(LIC_TRIAL, buf);
	else
		print_license_ct(last_valid_attempt, buf);

	pattr->at_val.at_str = buf;
	pattr->at_flags |= ATR_VFLAG_SET | ATR_VFLAG_MODCACHE;
}

/**
 * @brief
 * 		req_stat_resv - service the Status Reservation Request
 * @par
 *		This request processes the request for status of a single
 *		reservation or the set of reservations at a destination.
 *
 * @param[in,out]	preq	-	ptr to the decoded request
 */

void
req_stat_resv(struct batch_request * preq)
{
	char		   *name;
	struct batch_reply *preply;
	resc_resv	   *presv = NULL;
	int		    rc   = 0;
	int		    type = 0;

	/*
	 * first, validate the name sent in the request.
	 * This is either the ID of a specific reservation
	 * or a '\0' or "@..." for all reservations.
	 */

	name = preq->rq_ind.rq_status.rq_id;

	if ((*name == '\0') || (*name =='@'))
		type = 1;
	else {
		presv = find_resv(name);
		if (presv == NULL) {
			req_reject(PBSE_UNKRESVID, 0, preq);
			return;
		}
	}

	preply = &preq->rq_reply;
	preply->brp_choice = BATCH_REPLY_CHOICE_Status;
	CLEAR_HEAD(preply->brp_un.brp_status);

	if (type == 0) {
		/* get status of the specifically named reservation */
		rc = status_resv(presv, preq, &preply->brp_un.brp_status);

	} else {
		/* get status of all the reservations */

		presv = (resc_resv *)GET_NEXT(svr_allresvs);
		while (presv) {
			rc = status_resv(presv, preq, &preply->brp_un.brp_status);
			if (rc == PBSE_PERM)
				rc = 0;
			if (rc)
				break;
			presv = (resc_resv *)GET_NEXT(presv->ri_allresvs);
		}
	}

	if (rc == 0)
		(void)reply_send(preq);
	else
		req_reject(rc, bad, preq);
}

/**
 * @brief
 * 		status_resv - Build the status reply for a single resv.
 *
 * @param[in]	presv	-	get status for this reservation
 * @param[in]	preq	-	ptr to the decoded request
 * @param[in,out]	pstathd	-	append retrieved status to list
 *
 * @return	int
 * @retval	0	: success
 * @retval	!0	: PBSE error
 */

static int
status_resv(resc_resv *presv, struct batch_request *preq, pbs_list_head *pstathd)
{
	struct brp_status *pstat;
	svrattrl	  *pal;

	if ((preq->rq_perm & ATR_DFLAG_RDACC) == 0)
		return (PBSE_PERM);

	/*first do any need update to attributes from
	 *"quick save" area of the resc_resv structure
	 */

	/*now allocate status sub-structure and fill header portion*/

	pstat = (struct brp_status *)malloc(sizeof(struct brp_status));
	if (pstat == NULL)
		return (PBSE_SYSTEM);

	pstat->brp_objtype = MGR_OBJ_RESV;
	(void)strcpy(pstat->brp_objname, presv->ri_qs.ri_resvID);
	CLEAR_LINK(pstat->brp_stlink);
	CLEAR_HEAD(pstat->brp_attr);
	append_link(pstathd, &pstat->brp_stlink, pstat);

	/*finally, add the requested attributes to the status reply*/

	bad = 0;	/*global: record ordinal position where got error*/
	pal = (svrattrl *) GET_NEXT(preq->rq_ind.rq_status.rq_attr);

	if (status_attrib(pal, resv_attr_def, presv->ri_wattr,
		RESV_ATR_LAST, preq->rq_perm, &pstat->brp_attr, &bad) == 0)
		return (0);
	else
		return (PBSE_NOATTR);
}

/**
 * @brief
 * 		status_resc - Build the status reply for a single resource.
 *
 * @param[in]	prd	-	pointer to resource def to status
 * @param[in]	preq	-	pointer to the batch request to service
 * @param[in]	pstathd	-	pointer to head of list to append status to
 * @param[in]	private	-	if a pbs private request, the status returns numeric
 * 							values for type and flags. Otherwise it returns strings
 *
 * @par
 * 		At the current time, the only things returned in the reply are
 *		the resource type and the flags, both as "integers".
 *
 * @return	whether the operation was successful or not
 * @retval	0	: on success
 * @retval	PBSE_SYSTEM	: on error
 */

static int
status_resc(struct resource_def *prd, struct batch_request *preq, pbs_list_head *pstathd, int private)
{
	struct attribute   attr;
	struct brp_status *pstat;

	if (((prd->rs_flags & ATR_DFLAG_USRD) == 0) &&
		(preq->rq_perm & (ATR_DFLAG_MGRD | ATR_DFLAG_OPRD)) == 0)
		return (PBSE_PERM);

	/* allocate status sub-structure and fill in header portion */

	pstat = (struct brp_status *)malloc(sizeof(struct brp_status));
	if (pstat == NULL)
		return (PBSE_SYSTEM);
	pstat->brp_objtype = MGR_OBJ_RSC;
	(void)strcpy(pstat->brp_objname, prd->rs_name);
	CLEAR_LINK(pstat->brp_stlink);
	CLEAR_HEAD(pstat->brp_attr);

	/* add attributes to the status reply */
	if (private) {
		attr.at_val.at_long = prd->rs_type;
		attr.at_flags = ATR_VFLAG_SET;
		if (encode_l(&attr, &pstat->brp_attr, ATTR_RESC_TYPE, NULL, 0, NULL) == -1)
			return PBSE_SYSTEM;

		attr.at_val.at_long = prd->rs_flags;
		attr.at_flags = ATR_VFLAG_SET;
		if (encode_l(&attr, &pstat->brp_attr, ATTR_RESC_FLAG, NULL, 0, NULL) == -1)
			return PBSE_SYSTEM;
	}
	else {
		struct resc_type_map *p_resc_type_map;

		p_resc_type_map = find_resc_type_map_by_typev(prd->rs_type);
		if (p_resc_type_map == NULL) {
			return PBSE_SYSTEM;
		}

		attr.at_val.at_str = p_resc_type_map->rtm_rname;
		attr.at_flags = ATR_VFLAG_SET;
		if (encode_str(&attr, &pstat->brp_attr, ATTR_RESC_TYPE, NULL, 0, NULL) == -1)
			return PBSE_SYSTEM;

		attr.at_val.at_str = find_resc_flag_map(prd->rs_flags);
		attr.at_flags = ATR_VFLAG_SET;
		if (encode_str(&attr, &pstat->brp_attr, ATTR_RESC_FLAG, NULL, 0, NULL) == -1)
			return PBSE_SYSTEM;
	}
	append_link(pstathd, &pstat->brp_stlink, pstat);
	return 0;
}

/**
 * @brief
 * 		req_stat_resc - service the Status Resource Request
 *
 *		This request processes the request for status of (information on)
 *		a set of resources
 *
 * @param[in]	preq	-	ptr to the decoded request
 */

void
req_stat_resc(struct batch_request *preq)
{
	int		     i;
	char		    *name;
	char		    *extend;
	struct resource_def *prd = NULL;
	struct batch_reply  *preply;
	int		     rc   = 0;
	int		     type;
	int		     private = 0;

	if (preq == NULL)
		return;
	/*
	 * first, validate the name of the requested object, either
	 * a resource name, or null for all resources
	 */

	name = preq->rq_ind.rq_status.rq_id;

	if ((*name == '\0') || (*name =='@'))
		type = 1;
	else {
		type = 0;
		prd = find_resc_def(svr_resc_def, name, svr_resc_size);
		if (prd == NULL) {
			req_reject(PBSE_UNKRESC, 0, preq);
			return;
		}
	}

	extend = preq->rq_extend;
	if (extend != NULL) {
		if (strchr(preq->rq_extend, (int)'p'))
			private = 1;
	}

	preply = &preq->rq_reply;
	preply->brp_choice = BATCH_REPLY_CHOICE_Status;
	CLEAR_HEAD(preply->brp_un.brp_status);

	if (type == 0) {	/* get status of the one named resource */
		rc = status_resc(prd, preq, &preply->brp_un.brp_status, private);

	} else {	/* get status of all resources */

		i = svr_resc_size;
		prd = &svr_resc_def[0];
		while (i--) {
			/* skip the unknown resource because it would fail
			 * to pass the string encoding routine
			 */
			if (!private  && (strcmp(prd->rs_name, RESOURCE_UNKNOWN) == 0)) {
				prd = prd->rs_next;
				continue;
			}
			rc = status_resc(prd, preq, &preply->brp_un.brp_status, private);
			if (rc == PBSE_PERM) {
				/* we skip resources that are disallowed to be
				 * stat'ed by this user
				 */
				rc = 0;
			}
			prd = prd->rs_next;
		}
	}
	if (rc) {
		(void)reply_free(preply);
		req_reject(rc, bad, preq);
	} else {
		(void)reply_send(preq);
	}
}

/**
 * @brief
 * 		recv_cycle_end - Receives end of cycle notification from the corresponding Scheduler
 *
 * @param[in]	sock	-	socket to read
 *
 * @return	int
 * @retval	0	: on success
 * @retval	-1	: on error
 */
int
recv_cycle_end(int sock)
{
	pbs_sched *psched;
	int rc;
	for (psched = (pbs_sched*) GET_NEXT(svr_allscheds); psched; psched = (pbs_sched*) GET_NEXT(psched->sc_link)) {
		if (psched->scheduler_sock2 == sock) {
			rc = recv_int(sock, &(psched->sched_cycle_started));
			if (rc == -1) {
				/*log_eventf(PBSEVENT_SYSTEM, PBS_EVENTCLASS_REQUEST, LOG_ERR,
					__func__, "Not able to receive sched cycle end, errno = %d", errno);*/
				psched->scheduler_sock2 = -1;
				psched->sched_cycle_started = 0;
				set_sched_state(psched, SC_DOWN);
			} else
				set_sched_state(psched, SC_IDLE);
			return rc;
		}
	}
	return 0;
}

