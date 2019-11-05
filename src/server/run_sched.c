/*
 * Copyright (C) 1994-2018 Altair Engineering, Inc.
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
 * @file    run_sched.c
 *
 * @brief
 * 		run_sched.c	-	Functions related to the scheduler.
 */
#include <pbs_config.h>   /* the master config generated by configure */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include "portability.h"
#include <errno.h>
#include <netinet/in.h>
#include "log.h"
#include "libpbs.h"
#include "net_connect.h"
#include "sched_cmds.h"
#include "dis.h"
#include "list_link.h"
#include "attribute.h"
#include "job.h"
#include "server.h"
#include "pbs_nodes.h"
#include "svrfunc.h"
#include "work_task.h"
#include "pbs_error.h"
#include "batch_request.h"
#include "pbs_sched.h"
#include "queue.h"
#include "pbs_share.h"
#include "pbs_sched.h"


/* Global Data */

extern struct server server;
extern pbs_net_t pbs_scheduler_addr;
extern unsigned int pbs_scheduler_port;
extern char      server_name[];
extern struct connection *svr_conn;
extern int	 svr_do_schedule;
extern int	 svr_do_sched_high;
extern char     *msg_sched_called;
extern char     *msg_sched_nocall;
extern pbs_list_head svr_deferred_req;
extern void  est_start_timed_task(struct work_task *);
extern char	*msg_noloopbackif;
extern char	*msg_daemonname;

int scheduler_sock = -1;	/* socket open to scheduler during a cycle */
int scheduler_sock2 = -1;
int scheduler_jobs_stat = 0;	/* set to 1 once scheduler queried jobs in a cycle*/
extern int svr_unsent_qrun_req;
#define PRIORITY_CONNECTION 1

pbs_net_t		pbs_scheduler_addr;
unsigned int		pbs_scheduler_port;

/**
 * @brief
 * 		am_jobs - array of pointers to jobs which were moved or which had certain
 * 		attributes altered (qalter) while a schedule cycle was in progress.
 *		If a job in the array is run by the scheduler in the cycle, that run
 *		request is rejected as the move/modification may impact the job's
 *		requirements and placement.
 */
static struct   am_jobs {
	int   am_used;		/* number of jobs in the array  */
	int   am_max;		/* number of slots in the array */
	job **am_array;		/* pointer the malloc-ed array  */
} am_jobs = { 0, 0, NULL };


/* Functions private to this file */
static void scheduler_close(int);

#define SCHEDULER_ALARM_TIME 30
/**
 * @brief
 * 		catchalrm	-	put a timeout alarm in case of timeout occurs when contacting the scheduler.
 *
 * @param[in]	sig	-	not used here.
 */
void
catchalrm(int sig)
{
	log_event(PBSEVENT_SCHED, PBS_EVENTCLASS_SERVER, LOG_NOTICE,
		server_name, "timeout attempting to contact scheduler");
}

int
send_int(int sock, int num)
{
        int32_t conv = htonl(num);
        char *data = (char*)&conv;
        int left = sizeof(conv);
        int rc;
        do {
                rc = write(sock, data, left);
                if (rc < 0) {
                    if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
                        continue;
                    }
                    else if (errno != EINTR) {
                        return -1;
                    }
                }
                else {
                    data += rc;
                    left -= rc;
                }
        } while (left > 0);

        return 0;
}

int
send_str(int sock, char *str)
{
        int left = strlen(str);
        int rc;
        do {
                rc = write(sock, str, left);
                if (rc < 0) {
                    if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
                        continue;
                    }
                    else if (errno != EINTR) {
                        return -1;
                    }
                }
                else {
                    str += rc;
                    left -= rc;
                }
        } while (left > 0);

        return 0;
}


/**
 * @brief
 *		Sends 'cmd'  over to network 'sock', and if 'cmd' is SCH_SCHEDULE_AJOB,  *	sends also the 'jobid'.
 *
 * @param[in]	sock	-	communication endpoint
 * @param[in]	cmd	-	the command to send
 * @param[in]	jobid	-	the jobid to send if 'cmd' is SCH_SCHEDULE_AJOB
 *
 * @return	int
 * @retval	0	for success
 * @retval	-1	for failure
 */

int
put_sched_cmd(int sock, int cmd, char *identifier)
{
	int bytes_tosend;

	if (send_int(sock, cmd)) {
		goto err;
	}
	if ((cmd == SCH_SCHEDULE_AJOB) || (cmd == SCH_SVR_IDENTIFIER)) {
		bytes_tosend = strlen(identifier);
		if (bytes_tosend == 0)
			goto err;
		if (send_int(sock, bytes_tosend)) {
			goto err;
		}
		if (send_str(sock, identifier)) {
			goto err;
		}
	}

	return 0;
err:
	sprintf(log_buffer, "put_sched_cmd end errno =%d", errno);
	log_event(PBSEVENT_SCHED, PBS_EVENTCLASS_SERVER, LOG_INFO, server_name,
		log_buffer);
	return -1;
}

/**
 * @brief
 * 		find_assoc_sched_jid - find the corresponding scheduler which is responsible
 * 		for handling this job.
 *
 * @param[in]	jid - job id
 * @param[out]	target_sched - pointer to the corresponding scheduler to which the job belongs to
 *
 * @retval - 1  if success
 * 	   - 0 if fail
 */
int
find_assoc_sched_jid(char *jid, pbs_sched **target_sched)
{
	job *pj;
	int t;

	*target_sched = NULL;

	t = is_job_array(jid);
	if ((t == IS_ARRAY_NO) || (t == IS_ARRAY_ArrayJob))
		pj = find_job(jid);		/* regular or ArrayJob itself */
	else
		pj = find_arrayparent(jid); /* subjob(s) */

	if (pj == NULL)
		return 0;

	return find_assoc_sched_pque(find_queuebyname(pj->ji_qs.ji_queue, 0), target_sched);
}

/**
 * @brief
 * 		find_assoc_sched_pque - find the corresponding scheduler which is responsible
 * 		for handling this job.
 *
 * @param[in]	pq		- pointer to pbs_queue
 * @param[out]  target_sched	- pointer to the corresponding scheduler to which the job belongs to
 *
  * @retval - 1 if success
 * 	    - 0 if fail
 */
int
find_assoc_sched_pque(pbs_queue *pq, pbs_sched **target_sched)
{
	*target_sched = NULL;
	if (pq == NULL)
		return 0;

	if (pq->qu_attr[QA_ATR_partition].at_flags & ATR_VFLAG_SET) {
		*target_sched = recov_sched_from_db(pq->qu_attr[QA_ATR_partition].at_val.at_str, NULL, 0);
		if (*target_sched == NULL) {
			return 0;
		} else
			return 1;
	} else {
		dflt_scheduler = *target_sched = recov_sched_from_db(NULL, "default", 0);
		if (!dflt_scheduler) {
			dflt_scheduler = sched_alloc(PBS_DFLT_SCHED_NAME, 1);
			set_sched_default(dflt_scheduler, 0);
			(void)sched_save_db(dflt_scheduler, SVR_SAVE_NEW);
			*target_sched = dflt_scheduler;
		}
		dflt_scheduler->pbs_scheduler_addr = pbs_scheduler_addr;
		dflt_scheduler->pbs_scheduler_port = pbs_scheduler_port;
		return 1;
	}
	return 0;

}


/**
 * @brief
 * 		find_sched_from_sock - find the corresponding scheduler which is having
 * 		the given socket.
 *
 * @param[in]	sock	- socket descriptor
 *
 * @retval - pointer to the corresponding pbs_sched object if success
 * 		 -  NULL if fail
 */
pbs_sched *
find_sched_from_sock(int sock)
{
	pbs_sched *psched;

	for (psched = (pbs_sched*) GET_NEXT(svr_allscheds); psched; psched = (pbs_sched*) GET_NEXT(psched->sc_link)) {
		if (psched->scheduler_sock == sock || psched->scheduler_sock2 == sock)
			return psched;
	}
	return NULL;
}

/**
 * @brief
 * 		contact_sched - open connection to the scheduler and send it a command
 *		Jobid is passed if and only if the cmd is SCH_SCHEDULE_AJOB
 *
 * @param[in]	cmd	- the command to send
 * @param[in]	jobid	- the jobid to send if 'cmd' is SCH_SCHEDULE_AJOB
 */

int
contact_sched(int cmd, char *jobid, pbs_sched *psched, enum towhich_conn which_conn)
{
	int sock = -1;
	conn_t *conn;
	char my_index[MAX_SVR_INDEX] = {'\0'};

	if ((cmd == SCH_SCHEDULE_AJOB) && (jobid == NULL))
		return -1;	/* need a jobid */

	if (((which_conn == PRIMARY )&& (psched->scheduler_sock == -1) )||
			((which_conn == SECONDARY) && (psched->scheduler_sock2 == -1))) {
		/* Under win32, this function does a timeout wait on the non-blocking socket */
		sock = client_to_svr(psched->pbs_scheduler_addr, psched->pbs_scheduler_port, 1); /* scheduler connection still uses resv-ports */
		if (pbs_errno == PBSE_NOLOOPBACKIF)
			log_err(PBSE_NOLOOPBACKIF, "client_to_svr" , msg_noloopbackif);

		if (sock < 0) {
			log_err(errno, __func__, msg_sched_nocall);
			return (-1);
		}
		conn = add_conn_priority(sock, FromClientDIS, psched->pbs_scheduler_addr,
			psched->pbs_scheduler_port, process_request, PRIORITY_CONNECTION);
		if (!conn) {
			log_err(errno, __func__, "could not find sock in connection table");
			return (-1);
		}
		conn->cn_authen |=
			PBS_NET_CONN_FROM_PRIVIL | PBS_NET_CONN_AUTHENTICATED | PBS_NET_CONN_NOTIMEOUT;

		net_add_close_func(sock, scheduler_close);

		if (set_nodelay(sock) == -1) {
	#ifdef WIN32
			errno = WSAGetLastError();
	#endif
			snprintf(log_buffer, sizeof(log_buffer), "cannot set nodelay on connection %d (errno=%d)\n", sock, errno);
			log_err(-1, __func__, log_buffer);
			return (-1);
		}

		snprintf(my_index, MAX_SVR_INDEX, "%d", get_my_index());
		if (put_sched_cmd(sock, SCH_SVR_IDENTIFIER, my_index) < 0) {
			close_conn(sock);
			return (-1);
		}

		if (which_conn == PRIMARY)
			psched->scheduler_sock = sock;
		else
			psched->scheduler_sock2 = sock;
		return sock;
	}

	if (sock == -1) {
		if ((which_conn == PRIMARY) && (psched->scheduler_sock != -1))
			sock = psched->scheduler_sock;
		if ((which_conn == SECONDARY) && (psched->scheduler_sock2 != -1))
			sock = psched->scheduler_sock2;
	}

	/* send command to Scheduler */

	if (put_sched_cmd(sock, cmd, jobid) < 0) {
		close_conn(sock);
		return (-1);
	}
	psched->sched_cycle_started = 1;

	(void)sprintf(log_buffer, msg_sched_called, cmd);
	log_event(PBSEVENT_SCHED, PBS_EVENTCLASS_SERVER, LOG_INFO,
		server_name, log_buffer);
	return (sock);
}

/**
 * @brief
 * 		schedule_high	-	send high priority commands to the scheduler
 *
 * @return	int
 * @retval  1	: scheduler busy
 * @retval  0	: scheduler notified
 * @retval	-1	: error
 */
int
schedule_high(pbs_sched *psched)
{
	int s;
	extern int sched_trx_chk;

	if (psched == NULL)
		return -1;

	if (psched->sched_cycle_started == 0) {
		memcache_roll_sched_trx();

		sched_trx_chk = SCHED_TRX_CHK;
		if ((psched = recov_sched_from_db(NULL, psched->sc_name, 0)))
			return -1;

		if ((s = contact_sched(psched->svr_do_sched_high, NULL, psched, SECONDARY)) < 0) {
			set_attr_svr(&(psched->sch_attr[(int) SCHED_ATR_sched_state]), &sched_attr_def[(int) SCHED_ATR_sched_state], SC_DOWN);
			sched_save_db(psched, SVR_SAVE_FULL);
			return (-1);
		}
		psched->svr_do_sched_high = SCH_SCHEDULE_NULL;
		return 0;

		set_attr_svr(&(psched->sch_attr[(int) SCHED_ATR_sched_state]), &sched_attr_def[(int) SCHED_ATR_sched_state], SC_SCHEDULING);
	} else {
		return 1;
	}

	return 1;
}

/**
 * @brief
 * 		Contact scheduler and direct it to run a scheduling cycle
 *		If a request is already outstanding, skip this one.
 *
 * @return	int
 * @retval	-1	: error
 * @reval	0	: scheduler notified
 * @retval	+1	: scheduler busy
 *
 * @par Side Effects:
 *     the global variable (first_time) is changed.
 *
 * @par MT-safe: No
 */

int
schedule_jobs(pbs_sched *psched)
{
	int cmd;
	int s;
	static int first_time = 1;
	struct deferred_request *pdefr;
	char  *jid = NULL;
	extern int sched_trx_chk;

	if (psched == NULL)
		return -1;

	if (first_time)
		cmd = SCH_SCHEDULE_FIRST;
	else
		cmd = psched->svr_do_schedule;

	if (psched->sched_cycle_started == 0) {
		/* are there any qrun requests from manager/operator */
		/* which haven't been sent,  they take priority      */
		pdefr = (struct deferred_request *)GET_NEXT(svr_deferred_req);
		while (pdefr) {
			if (pdefr->dr_sent == 0) {
				s = is_job_array(pdefr->dr_id);
				if (s == IS_ARRAY_NO) {
					if (find_job(pdefr->dr_id) != NULL) {
						jid = pdefr->dr_id;
						cmd = SCH_SCHEDULE_AJOB;
						break;
					}
				} else if ((s == IS_ARRAY_Single) ||
					(s == IS_ARRAY_Range)) {
					if (find_arrayparent(pdefr->dr_id) != NULL) {
						jid = pdefr->dr_id;
						cmd = SCH_SCHEDULE_AJOB;
						break;
					}
				}
			}
			pdefr = (struct deferred_request *)GET_NEXT(pdefr->dr_link);
		}

		memcache_roll_sched_trx();

		sched_trx_chk = SCHED_TRX_CHK;
		if ((psched = recov_sched_from_db(NULL, psched->sc_name, 0)) == NULL)
			return -1;

		if ((s = contact_sched(cmd, jid,  psched, SECONDARY)) < 0) {
			set_attr_svr(&(psched->sch_attr[(int) SCHED_ATR_sched_state]), &sched_attr_def[(int) SCHED_ATR_sched_state], SC_DOWN);
			sched_save_db(psched, SVR_SAVE_FULL);
			return (-1);
		}
		else if (pdefr != NULL)
			pdefr->dr_sent = 1;   /* mark entry as sent to sched */
		psched->svr_do_schedule = SCH_SCHEDULE_NULL;

		set_attr_svr(&(psched->sch_attr[(int) SCHED_ATR_sched_state]), &sched_attr_def[(int) SCHED_ATR_sched_state], SC_SCHEDULING);

		first_time = 0;

		/* if there are more qrun requests queued up, reset cmd so */
		/* they are sent when the Scheduler completes this cycle   */
		pdefr = GET_NEXT(svr_deferred_req);
		while (pdefr) {
			if (pdefr->dr_sent == 0) {
				pbs_sched *target_sched;
				if (find_assoc_sched_jid(pdefr->dr_preq->rq_ind.rq_queuejob.rq_jid, &target_sched))
					target_sched->svr_do_schedule = SCH_SCHEDULE_AJOB;
				break;
			}
			pdefr = (struct deferred_request *)GET_NEXT(pdefr->dr_link);
		}
	} else {
		return 1;
	}

	return (0);

}

/**
 * @brief
 * 		scheduler_close - connection to scheduler has closed, clear scheduler_called
 * @par
 * 		Connection to scheduler has closed, mark scheduler sock as
 *		closed with -1 and if any clean up any outstanding deferred scheduler
 *		requests (qrun).
 * @par
 * 		Perform some cleanup as connection to scheduler has closed
 *
 * @param[in]	sock	-	communication endpoint.
 * 							closed (scheduler connection) socket, not used but
 *							required to match general prototype of functions called when
 *							a socket is closed.
 * @return	void
 */

static void
scheduler_close(int sock)
{
	struct deferred_request *pdefr;
	pbs_sched *psched;

	psched = find_sched_from_sock(sock);

	if (psched == NULL)
		return;

	psched->sched_cycle_started = 0;

	set_attr_svr(&(psched->sch_attr[(int) SCHED_ATR_sched_state]), &sched_attr_def[(int) SCHED_ATR_sched_state], SC_IDLE);

	if ((sock != -1) && (sock == psched->scheduler_sock)) {
		psched->scheduler_sock = -1;
		return;	/* nothing to check if scheduler_sock2 */
	}

	psched->scheduler_sock2 = -1;

	/* clear list of jobs which were altered/modified during cycle */
	am_jobs.am_used = 0;
	scheduler_jobs_stat = 0;

	/**
	 *	If a deferred (from qrun) had been sent to the Scheduler and is still
	 *	there, then the Scheduler must have closed the connection without
	 *	dealing with the job.  Tell qrun it failed if the qrun connection
	 *	is still there.
	 *      If any qrun request is pending in the deffered list, set svr_unsent_qrun_req so
	 * 	they are sent when the Scheduler completes this cycle 
	 */
	pdefr = (struct deferred_request *)GET_NEXT(svr_deferred_req);
	while (pdefr) {
		struct deferred_request *next_pdefr = (struct deferred_request *)GET_NEXT(pdefr->dr_link);
		if (pdefr->dr_sent != 0) {
			log_event(PBSEVENT_ERROR, PBS_EVENTCLASS_JOB,
				LOG_NOTICE, pdefr->dr_id,
				"deferred qrun request to scheduler failed");
			if (pdefr->dr_preq != NULL)
				req_reject(PBSE_INTERNAL, 0, pdefr->dr_preq);
			/* unlink and free the deferred request entry */
			delete_link(&pdefr->dr_link);
			free(pdefr);
		}
		else if((pdefr->dr_sent == 0) && (svr_unsent_qrun_req == 0)) {
			svr_unsent_qrun_req = 1;
		}
		pdefr = next_pdefr;
	}

	server.sv_attr[(int)SRV_ATR_State].at_flags |= ATR_VFLAG_MODCACHE;
}

/**
 * @brief
 * 		Add a job to the am_jobs array, called when a job is moved (locally)
 *		or modified (qalter) during a scheduling cycle
 *
 * @param[in]	pjob	-	pointer to job to add to the array.
 */
void
am_jobs_add(job *pjob)
{
	if (am_jobs.am_used == am_jobs.am_max) {
		/* Need to expand the array, increase by 4 slots */
		job **tmp = realloc(am_jobs.am_array, sizeof(job *) * (am_jobs.am_max + 4));
		if (tmp == NULL)
			return;	/* cannot increase array, so be it */
		am_jobs.am_array = tmp;
		am_jobs.am_max  += 4;
	}
	*(am_jobs.am_array + am_jobs.am_used++) = pjob;
}

/**
 * @brief
 * 		Determine if the job in question is in the list of moved/altered
 *		jobs.  Called when a run request for a job comes from the Scheduler.
 *
 * @param[in]	pjob	-	pointer to job in question.
 *
 * @return	int
 * @retval	0	- job not in list
 * @retval	1	- job is in list
 */
int
was_job_alteredmoved(job *pjob)
{
	int i;
	for (i=0; i<am_jobs.am_used; ++i) {
		if (*(am_jobs.am_array+i) == pjob)
			return 1;
	}
	return 0;
}

/**
 * @brief
 * 		set_scheduler_flag - set the flag to call the Scheduler
 *		certain flag values should not be overwritten
 *
 * @param[in]	flag	-	pointer to job in question.
 * @parm[in] psched -   pointer to sched object. Then set the flag only for this object.
 *                                     NULL. Then set the flag for all the scheduler objects.
 */
void
set_scheduler_flag(int flag, pbs_sched *psched)
{
	int single_sched;

	if (psched)
		single_sched = 1;
	else {
		single_sched = 0;
		psched = (pbs_sched*) GET_NEXT(svr_allscheds);
	}

	for (; psched ; psched = (pbs_sched*) GET_NEXT(psched->sc_link)) {
		/* high priority commands:
		 * Note: A) usually SCH_QUIT is sent directly and not via here
		 *       B) if we ever add a 3rd high prio command, we can lose them
		 */
		if (flag == SCH_CONFIGURE || flag == SCH_QUIT) {
			if (psched->svr_do_sched_high == SCH_QUIT)
				return; /* keep only SCH_QUIT */

			psched->svr_do_sched_high = flag;
		}
		else
			psched->svr_do_schedule = flag;
		if (single_sched)
			break;
	}

}

pbs_sched *
recov_sched_from_db(char *partition, char *sched_name, int lock)
{
	int ret = 0;
	int append = 1;
	pbs_db_sched_info_t dbsched;
	pbs_db_obj_info_t obj;
	pbs_sched *ps = NULL;
	char *sname = "new";
	pbs_db_conn_t *conn = (pbs_db_conn_t *) svr_db_conn;

	obj.pbs_db_obj_type = PBS_DB_SCHED;
	obj.pbs_db_un.pbs_db_sched = &dbsched;

	dbsched.partition_name[0] = '\0';
	dbsched.sched_name[0] = '\0';

	if (partition != NULL) {
		snprintf(dbsched.partition_name, sizeof(dbsched.partition_name), "%%%s%%", partition);
		ps = find_scheduler_by_partition(partition);
	} else if (sched_name != NULL) {
		snprintf(dbsched.sched_name, sizeof(dbsched.sched_name), "%s", sched_name);
		ps = find_scheduler(dbsched.sched_name);
	}

	if (ps) {
		if (memcache_good(&ps->trx_status, 0))
			return ps;
		strcpy(dbsched.sched_savetm, ps->sch_svtime);
	} else {
		ps = sched_alloc(sname, append);
		if (ps == NULL) {
			log_err(-1, "sched_recov", "sched_alloc failed");
			return NULL;
		}
		dbsched.sched_savetm[0] = '\0';
	}

	/* recover sched */
	ret = pbs_db_load_obj(conn, &obj, lock);
	if (ret == -1)
		goto db_err;

	if (ret == -2) {
		memcache_update_state(&ps->trx_status, lock);
		return ps;
	}

	if (db_to_svr_sched(ps, &dbsched) != 0)
		goto db_err;

	pbs_db_reset_obj(&obj);
	memcache_update_state(&ps->trx_status, lock);

	return (ps);

db_err:
	sprintf(log_buffer, "Failed to load sched with %s %s", (partition)?"partition":"name", (partition)?partition:sched_name);
	if (ps) {
		delete_link(&ps->sc_link);
		free(ps);
	}
	
	return NULL;
}

/**
 * @brief
 *	Load a database scheduler object from the scheduler object in server
 *
 * @param[out] ps - Address of the scheduler in pbs server
 * @param[in]  pdbsched  - Address of the database scheduler object
 *
 */
int
db_to_svr_sched(struct pbs_sched *ps, pbs_db_sched_info_t *pdbsched)
{
	/* Following code is for the time being only */
	strcpy(ps->sc_name, pdbsched->sched_name);
	strcpy(ps->sch_svtime, pdbsched->sched_savetm);
	/* since we dont need the sched_name and sched_sv_name free here */
	if ((decode_attr_db(ps, &pdbsched->attr_list, sched_attr_def,
		ps->sch_attr,
		(int) SCHED_ATR_LAST, 0)) != 0)
		return -1;

	return 0;
}

void
connect_to_scheduler(pbs_sched *psched)
{
	(void)contact_sched(SCH_SCHEDULE_NULL, NULL,  psched, PRIMARY);
	(void)contact_sched(SCH_SCHEDULE_NULL, NULL, psched, SECONDARY);
}
