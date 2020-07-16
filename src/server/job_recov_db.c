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

#include <pbs_config.h>   /* the master config generated by configure */

#include <stdio.h>
#include <sys/types.h>

#ifndef WIN32
#include <sys/param.h>
#include <execinfo.h>
#endif

#include "pbs_ifl.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include <unistd.h>
#include "server_limits.h"
#include "list_link.h"
#include "attribute.h"

#ifdef WIN32
#include <sys/stat.h>
#include <io.h>
#include <windows.h>
#include "win.h"
#endif

#include "job.h"
#include "reservation.h"
#include "queue.h"
#include "log.h"
#include "pbs_nodes.h"
#include "svrfunc.h"
#include <memory.h>
#include "libutil.h"
#include "pbs_db.h"


#define MAX_SAVE_TRIES 3

#ifndef PBS_MOM
extern pbs_db_conn_t	*svr_db_conn;
#ifndef WIN32
#define BACKTRACE_BUF_SIZE 50
void print_backtrace(char *);
#endif
#endif

/* global data items */
extern time_t time_now;

#ifndef PBS_MOM

/**
 * @brief
 *		convert job structure to DB format
 *
 * @see
 * 		job_save_db
 *
 * @param[in]	pjob - Address of the job in the server
 * @param[out]	dbjob - Address of the database job object
 *
 * @retval	-1  Failure
 * @retval	>=0 What to save: 0=nothing, OBJ_SAVE_NEW or OBJ_SAVE_QS
 */
static int
job_to_db(job *pjob, pbs_db_job_info_t *dbjob)
{
	int savetype = 0;
	int save_all_attrs = 0;

	strcpy(dbjob->ji_jobid, pjob->ji_qs.ji_jobid);

	if (pjob->ji_qs.ji_state == JOB_STATE_FINISHED)
		save_all_attrs = 1;

	if ((encode_attr_db(job_attr_def, pjob->ji_wattr, JOB_ATR_LAST, &dbjob->db_attr_list, save_all_attrs)) != 0)
		return -1;

	if (pjob->newobj) /* object was never saved/loaded before */
		savetype |= (OBJ_SAVE_NEW | OBJ_SAVE_QS);

	if (compare_obj_hash(&pjob->ji_qs, sizeof(pjob->ji_qs), pjob->qs_hash) == 1) {
		savetype |= OBJ_SAVE_QS;

		dbjob->ji_state     = pjob->ji_qs.ji_state;
		dbjob->ji_substate  = pjob->ji_qs.ji_substate;
		dbjob->ji_svrflags  = pjob->ji_qs.ji_svrflags;
		dbjob->ji_numattr   = pjob->ji_qs.ji_numattr;
		dbjob->ji_ordering  = pjob->ji_qs.ji_ordering;
		dbjob->ji_priority  = pjob->ji_qs.ji_priority;
		dbjob->ji_stime     = pjob->ji_qs.ji_stime;
		dbjob->ji_endtBdry  = pjob->ji_qs.ji_endtBdry;
		strcpy(dbjob->ji_queue, pjob->ji_qs.ji_queue);
		strcpy(dbjob->ji_destin, pjob->ji_qs.ji_destin);
		dbjob->ji_un_type   = pjob->ji_qs.ji_un_type;
		if (pjob->ji_qs.ji_un_type == JOB_UNION_TYPE_NEW) {
			dbjob->ji_fromsock  = pjob->ji_qs.ji_un.ji_newt.ji_fromsock;
			dbjob->ji_fromaddr  = pjob->ji_qs.ji_un.ji_newt.ji_fromaddr;
		} else if (pjob->ji_qs.ji_un_type == JOB_UNION_TYPE_EXEC) {
			dbjob->ji_momaddr   = pjob->ji_qs.ji_un.ji_exect.ji_momaddr;
			dbjob->ji_momport   = pjob->ji_qs.ji_un.ji_exect.ji_momport;
			dbjob->ji_exitstat  = pjob->ji_qs.ji_un.ji_exect.ji_exitstat;
		} else if (pjob->ji_qs.ji_un_type == JOB_UNION_TYPE_ROUTE) {
			dbjob->ji_quetime   = pjob->ji_qs.ji_un.ji_routet.ji_quetime;
			dbjob->ji_rteretry  = pjob->ji_qs.ji_un.ji_routet.ji_rteretry;
		} else if (pjob->ji_qs.ji_un_type == JOB_UNION_TYPE_MOM) {
			dbjob->ji_exitstat  = pjob->ji_qs.ji_un.ji_momt.ji_exitstat;
		}
		/* extended portion */
		strcpy(dbjob->ji_4jid, pjob->ji_extended.ji_ext.ji_4jid);
		strcpy(dbjob->ji_4ash, pjob->ji_extended.ji_ext.ji_4ash);
		dbjob->ji_credtype  = pjob->ji_extended.ji_ext.ji_credtype;
		dbjob->ji_qrank = pjob->ji_wattr[(int)JOB_ATR_qrank].at_val.at_long;
	}

	return savetype;
}

/**
 * @brief
 *		convert from database to job structure
 *
 * @see
 * 		job_recov_db
 *
 * @param[out]	pjob - Address of the job in the server
 * @param[in]	dbjob - Address of the database job object
 *
 * @retval   !=0  Failure
 * @retval   0    Success
 */
static int
db_to_job(job *pjob,  pbs_db_job_info_t *dbjob)
{
	/* Variables assigned constant values are not stored in the DB */
	pjob->ji_qs.ji_jsversion = JSVERSION;
	strcpy(pjob->ji_qs.ji_jobid, dbjob->ji_jobid);
	pjob->ji_qs.ji_state = dbjob->ji_state;
	pjob->ji_qs.ji_substate = dbjob->ji_substate;
	pjob->ji_qs.ji_svrflags = dbjob->ji_svrflags;
	pjob->ji_qs.ji_numattr = dbjob->ji_numattr ;
	pjob->ji_qs.ji_ordering = dbjob->ji_ordering;
	pjob->ji_qs.ji_priority = dbjob->ji_priority;
	pjob->ji_qs.ji_stime = dbjob->ji_stime;
	pjob->ji_qs.ji_endtBdry = dbjob->ji_endtBdry;
	strcpy(pjob->ji_qs.ji_queue, dbjob->ji_queue);
	strcpy(pjob->ji_qs.ji_destin, dbjob->ji_destin);
	pjob->ji_qs.ji_fileprefix[0] = 0;
	pjob->ji_qs.ji_un_type = dbjob->ji_un_type;
	if (pjob->ji_qs.ji_un_type == JOB_UNION_TYPE_NEW) {
		pjob->ji_qs.ji_un.ji_newt.ji_fromsock = dbjob->ji_fromsock;
		pjob->ji_qs.ji_un.ji_newt.ji_fromaddr = dbjob->ji_fromaddr;
		pjob->ji_qs.ji_un.ji_newt.ji_scriptsz = 0;
	} else if (pjob->ji_qs.ji_un_type == JOB_UNION_TYPE_EXEC) {
		pjob->ji_qs.ji_un.ji_exect.ji_momaddr = dbjob->ji_momaddr;
		pjob->ji_qs.ji_un.ji_exect.ji_momport = dbjob->ji_momport;
		pjob->ji_qs.ji_un.ji_exect.ji_exitstat = dbjob->ji_exitstat;
	} else if (pjob->ji_qs.ji_un_type == JOB_UNION_TYPE_ROUTE) {
		pjob->ji_qs.ji_un.ji_routet.ji_quetime = dbjob->ji_quetime;
		pjob->ji_qs.ji_un.ji_routet.ji_rteretry = dbjob->ji_rteretry;
	} else if (pjob->ji_qs.ji_un_type == JOB_UNION_TYPE_MOM) {
		pjob->ji_qs.ji_un.ji_momt.ji_svraddr = 0;
		pjob->ji_qs.ji_un.ji_momt.ji_exitstat = dbjob->ji_exitstat;
		pjob->ji_qs.ji_un.ji_momt.ji_exuid = 0;
		pjob->ji_qs.ji_un.ji_momt.ji_exgid = 0;
	}

	/* extended portion */
#if defined(__sgi)
	pjob->ji_extended.ji_ext.ji_jid = 0;
	pjob->ji_extended.ji_ext.ji_ash = 0;
#else
	strcpy(pjob->ji_extended.ji_ext.ji_4jid, dbjob->ji_4jid);
	strcpy(pjob->ji_extended.ji_ext.ji_4ash, dbjob->ji_4ash);
#endif
	pjob->ji_extended.ji_ext.ji_credtype = dbjob->ji_credtype;

	if ((decode_attr_db(pjob, &dbjob->db_attr_list, job_attr_idx, job_attr_def, pjob->ji_wattr, JOB_ATR_LAST, JOB_ATR_UNKN)) != 0)
		return -1;

	compare_obj_hash(&pjob->ji_qs, sizeof(pjob->ji_qs), pjob->qs_hash);

	pjob->newobj = 0;

	return 0;
}

/**
 * @brief
 *		Save job to database
 *
 * @param[in]	pjob - The job to save
 *
 * @return      Error code
 * @retval	 0 - Success
 * @retval	-1 - Failure
 * @retval	 1 - Jobid clash, retry with new jobid
 *
 */
int
job_save_db(job *pjob)
{
	pbs_db_job_info_t dbjob = {{0}};
	pbs_db_obj_info_t obj;
	pbs_db_conn_t *conn = svr_db_conn;
	int savetype;
	int rc = -1;
	int old_mtime, old_flags;

	old_mtime = pjob->ji_wattr[JOB_ATR_mtime].at_val.at_long;
	old_flags = pjob->ji_wattr[JOB_ATR_mtime].at_flags;

	if ((savetype = job_to_db(pjob, &dbjob)) == -1)
		goto done;

	obj.pbs_db_obj_type = PBS_DB_JOB;
	obj.pbs_db_un.pbs_db_job = &dbjob;

	/* update mtime before save, so the same value gets to the DB as well */
	pjob->ji_wattr[JOB_ATR_mtime].at_val.at_long = time_now;
	pjob->ji_wattr[JOB_ATR_mtime].at_flags |= ATR_SET_MOD_MCACHE;
	if ((rc = pbs_db_save_obj(conn, &obj, savetype)) == 0)
		pjob->newobj = 0;

done:
	free_db_attr_list(&dbjob.db_attr_list);

	if (rc != 0) {
		/* revert mtime, flags update */
		pjob->ji_wattr[JOB_ATR_mtime].at_val.at_long = old_mtime;
		pjob->ji_wattr[JOB_ATR_mtime].at_flags = old_flags;

		log_errf(PBSE_INTERNAL, __func__, "Failed to save job %s %s", pjob->ji_qs.ji_jobid, (conn->conn_db_err)? conn->conn_db_err : "");
		if (conn->conn_db_err) {
			if (savetype == OBJ_SAVE_NEW && strstr(conn->conn_db_err, "duplicate key value"))
				rc = 1;
		}

		if (rc == -1)
			panic_stop_db(log_buffer);
	}

	return (rc);
}

/**
 * @brief
 *	Utility function called inside job_recov_db
 *
 * @param[in]	dbjob - Pointer to the database structure of a job
 * @param[in]   pjob  - Pointer to job structure to populate
 *
 * @retval	 NULL - Failure
 * @retval	!NULL - Success, pointer to job structure recovered
 *
 */
job *
job_recov_db_spl(pbs_db_job_info_t *dbjob, job *pjob)
{
	job *pj = NULL;

	if (!pjob) {
		pj = job_alloc();
		pjob = pj;
	}

	if (pjob) {
		if (db_to_job(pjob, dbjob) == 0)
			return (pjob);
	}

	/* error case */
	if (pj)
		job_free(pj); /* free if we allocated here */

	log_errf(PBSE_INTERNAL, __func__,  "Failed to decode job %s", dbjob->ji_jobid);

	return (NULL);
}

/**
 * @brief
 *	Recover job from database
 *
 * @param[in]	jid - Job id of job to recover
 * @param[in]	pjob - job pointer, if any, to be updated
 *
 * @return      The recovered job
 * @retval	 NULL - Failure
 * @retval	!NULL - Success, pointer to job structure recovered
 *
 */
job *
job_recov_db(char *jid, job *pjob)
{
	pbs_db_job_info_t dbjob = {{0}};
	pbs_db_obj_info_t obj;
	int rc = -1;
	pbs_db_conn_t *conn = svr_db_conn;

	strcpy(dbjob.ji_jobid, jid);

	rc = pbs_db_load_obj(conn, &obj);
	if (rc == -2)
		return pjob; /* no change in job, return the same job */

	if (rc == 0)
		pjob = job_recov_db_spl(&dbjob, pjob);
	else
		log_errf(PBSE_INTERNAL, __func__, "Failed to load job %s %s", jid, (conn->conn_db_err)? conn->conn_db_err : "");

	free_db_attr_list(&dbjob.db_attr_list);

	return (pjob);
}

/**
 * @brief
 *		convert resv structure to DB format
 *
 * @see
 * 		resv_save_db
 *
 * @param[in]	presv - Address of the resv in the server
 * @param[out]  dbresv - Address of the database resv object
 *
 * @retval   -1  Failure
 * @retval   >=0 What to save: 0=nothing, OBJ_SAVE_NEW or OBJ_SAVE_QS
 */
static int
resv_to_db(resc_resv *presv,  pbs_db_resv_info_t *dbresv)
{
	int savetype = 0;

	strcpy(dbresv->ri_resvid, presv->ri_qs.ri_resvID);

	if ((encode_attr_db(resv_attr_def, presv->ri_wattr, (int)RESV_ATR_LAST, &(dbresv->db_attr_list), 0)) != 0)
		return -1;

	if (presv->newobj) /* object was never saved or loaded before */
		savetype |= (OBJ_SAVE_NEW | OBJ_SAVE_QS);

	if (compare_obj_hash(&presv->ri_qs, sizeof(presv->ri_qs), presv->qs_hash) == 1) {
		savetype |= OBJ_SAVE_QS;

		strcpy(dbresv->ri_queue, presv->ri_qs.ri_queue);
		dbresv->ri_duration = presv->ri_qs.ri_duration;
		dbresv->ri_etime = presv->ri_qs.ri_etime;
		dbresv->ri_un_type = presv->ri_qs.ri_un_type;
		if (dbresv->ri_un_type == RESV_UNION_TYPE_NEW) {
			dbresv->ri_fromaddr = presv->ri_qs.ri_un.ri_newt.ri_fromaddr;
			dbresv->ri_fromsock = presv->ri_qs.ri_un.ri_newt.ri_fromsock;
		}
		dbresv->ri_numattr = presv->ri_qs.ri_numattr;
		dbresv->ri_resvTag = presv->ri_qs.ri_resvTag;
		dbresv->ri_state = presv->ri_qs.ri_state;
		dbresv->ri_stime = presv->ri_qs.ri_stime;
		dbresv->ri_substate = presv->ri_qs.ri_substate;
		dbresv->ri_svrflags = presv->ri_qs.ri_svrflags;
		dbresv->ri_tactive = presv->ri_qs.ri_tactive;
	}

	return savetype;
}

/**
 * @brief
 *		convert from database to resv structure
 *
 * @param[out]	presv - Address of the resv in the server
 * @param[in]	dbresv - Address of the database resv object
 *
 * @retval   !=0  Failure
 * @retval   0    Success
 */
static int
db_to_resv(resc_resv *presv, pbs_db_resv_info_t *dbresv)
{
	strcpy(presv->ri_qs.ri_resvID, dbresv->ri_resvid);
	strcpy(presv->ri_qs.ri_queue, dbresv->ri_queue);
	presv->ri_qs.ri_duration = dbresv->ri_duration;
	presv->ri_qs.ri_etime = dbresv->ri_etime;
	presv->ri_qs.ri_un_type = dbresv->ri_un_type;
	if (dbresv->ri_un_type == RESV_UNION_TYPE_NEW) {
		presv->ri_qs.ri_un.ri_newt.ri_fromaddr = dbresv->ri_fromaddr;
		presv->ri_qs.ri_un.ri_newt.ri_fromsock = dbresv->ri_fromsock;
	}
	presv->ri_qs.ri_numattr = dbresv->ri_numattr;
	presv->ri_qs.ri_resvTag = dbresv->ri_resvTag;
	presv->ri_qs.ri_state = dbresv->ri_state;
	presv->ri_qs.ri_stime = dbresv->ri_stime;
	presv->ri_qs.ri_substate = dbresv->ri_substate;
	presv->ri_qs.ri_svrflags = dbresv->ri_svrflags;
	presv->ri_qs.ri_tactive = dbresv->ri_tactive;

	if ((decode_attr_db(presv, &dbresv->db_attr_list, resv_attr_idx, resv_attr_def, presv->ri_wattr, RESV_ATR_LAST, RESV_ATR_UNKN)) != 0)
		return -1;

	compare_obj_hash(&presv->ri_qs, sizeof(presv->ri_qs), presv->qs_hash);

	presv->newobj = 0;

	return 0;

}

/**
 * @brief
 *	Save resv to database
 *
 * @param[in]	presv - The resv to save
 * @param[in]   updatetype:
 *		SAVERESV_QUICK - Quick update without attributes
 *		SAVERESV_FULL  - Full update with attributes
 *		SAVERESV_NEW   - New resv, insert into database
 *
 * @return      Error code
 * @retval	 0 - Success
 * @retval	-1 - Failure
 * @retval	 1 - resvid clash, retry with new resvid
 *
 */
int
resv_save_db(resc_resv *presv)
{
	pbs_db_resv_info_t dbresv = {{0}};
	pbs_db_obj_info_t obj;
	pbs_db_conn_t *conn = svr_db_conn;
	int savetype;
	int rc = -1;
	int old_mtime, old_flags;

	old_mtime = presv->ri_wattr[RESV_ATR_mtime].at_val.at_long;
	old_flags = presv->ri_wattr[RESV_ATR_mtime].at_flags;

	if ((savetype = resv_to_db(presv, &dbresv)) == -1)
		goto done;

	obj.pbs_db_obj_type = PBS_DB_RESV;
	obj.pbs_db_un.pbs_db_resv = &dbresv;

	/* update mtime before save, so the same value gets to the DB as well */
	presv->ri_wattr[RESV_ATR_mtime].at_val.at_long = time_now;
	presv->ri_wattr[RESV_ATR_mtime].at_flags |= ATR_SET_MOD_MCACHE;
	if ((rc = pbs_db_save_obj(conn, &obj, savetype)) == 0)
		presv->newobj = 0;

done:
	free_db_attr_list(&dbresv.db_attr_list);

	if (rc != 0) {
		presv->ri_wattr[RESV_ATR_mtime].at_val.at_long = old_mtime;
		presv->ri_wattr[RESV_ATR_mtime].at_flags = old_flags;

		log_errf(PBSE_INTERNAL, __func__, "Failed to save resv %s %s", presv->ri_qs.ri_resvID, (conn->conn_db_err)? conn->conn_db_err : "");
		if(conn->conn_db_err) {
			if (savetype == OBJ_SAVE_NEW && strstr(conn->conn_db_err, "duplicate key value"))
				rc = 1;
		}

		if (rc == -1)
			panic_stop_db(log_buffer);
	}

	return (rc);
}

/**
 * @brief
 *	Recover resv from database
 *
 * @param[in]	resvid - Resv id to recover
 * @param[in]	presv - Resv pointer, if any, to be updated
 *
 * @return      The recovered reservation
 * @retval	 NULL - Failure
 * @retval	!NULL - Success, pointer to resv structure recovered
 *
 */
resc_resv *
resv_recov_db(char *resvid, resc_resv *presv)
{
	resc_resv *pr = NULL;
	pbs_db_resv_info_t dbresv = {{0}};
	pbs_db_obj_info_t obj;
	pbs_db_conn_t *conn = svr_db_conn;
	int rc = -1;

	if (!presv) {
		if ((pr = resv_alloc(resvid)) == NULL) {
			log_err(-1, __func__, "resv_alloc failed");
			return NULL;
		}
		presv = pr;
	}

	strcpy(dbresv.ri_resvid, resvid);
	obj.pbs_db_obj_type = PBS_DB_RESV;
	obj.pbs_db_un.pbs_db_resv = &dbresv;

	rc = pbs_db_load_obj(conn, &obj);
	if (rc == -2)
		return presv; /* no change in resv */

	if (rc == 0) {
		rc = db_to_resv(presv, &dbresv);
	}

	free_db_attr_list(&dbresv.db_attr_list);

	if (rc != 0) {
		presv = NULL; /* so we return NULL */

		log_errf(PBSE_INTERNAL, __func__, "Failed to load resv %s %s", resvid, (conn->conn_db_err)? conn->conn_db_err : "");
		if (pr)
			resv_free(pr); /* free if we allocated here */
	}

	return presv;
}


#endif /* ifndef PBS_MOM */
