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
 * @file    queue_recov_db.c
 *
 * @brief
 *		queue_recov_db.c - This file contains the functions to record a queue
 *		data structure to database and to recover it from database.
 *
 *		The data is recorded in the database
 *
 *	The following public functions are provided:
 *		que_save_db()   - save queue to database
 *		que_recov_db()  - recover (read) queue from database
 *		svr_to_db_que()	- Load a database queue object from a server queue object
 *		db_to_svr_que()	- Load a server queue object from a database queue object
 */

#include <pbs_config.h>   /* the master config generated by configure */

#include <sys/types.h>
#include <sys/param.h>
#include "pbs_ifl.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "server_limits.h"
#include "list_link.h"
#include "attribute.h"
#include "job.h"
#include "reservation.h"
#include "queue.h"
#include "log.h"
#include "pbs_nodes.h"
#include "svrfunc.h"
#include "pbs_db.h"


#ifndef PBS_MOM
extern pbs_db_conn_t	*svr_db_conn;
#endif

#ifdef NAS /* localmod 005 */
/* External Functions Called */
extern int save_attr_db(pbs_db_conn_t *conn, pbs_db_attr_info_t *p_attr_info,
	struct attribute_def *padef, struct attribute *pattr,
	int numattr, int newparent);
extern int recov_attr_db(pbs_db_conn_t *conn,
	void *parent,
	pbs_db_attr_info_t *p_attr_info,
	struct attribute_def *padef,
	struct attribute *pattr,
	int limit,
	int unknown);
#endif /* localmod 005 */

/**
 * @brief
 *		Load a database queue object from a server queue object
 *
 * @param[in]	pque	- Address of the queue in the server
 * @param[out]	pdbque  - Address of the database queue object
 *
 *@return 0      Success
 *@return !=0    Failure
 */
static int
svr_to_db_que(pbs_queue *pque, pbs_db_que_info_t *pdbque, int updatetype)
{
	pdbque->qu_name[sizeof(pdbque->qu_name) - 1] = '\0';
	strncpy(pdbque->qu_name, pque->qu_qs.qu_name, sizeof(pdbque->qu_name));
	pdbque->qu_type = pque->qu_qs.qu_type;

	if (updatetype != PBS_UPDATE_DB_QUICK) {
		if ((encode_attr_db(que_attr_def, pque->qu_attr,
			(int)QA_ATR_LAST, &pdbque->attr_list, 1)) != 0) /* encode all attributes */
			return -1;
	}

	return 0;
}

/**
 * @brief
 *		Load a server queue object from a database queue object
 *
 * @param[out]	pque	- Address of the queue in the server
 * @param[in]	pdbque	- Address of the database queue object
 *
 *@return 0      Success
 *@return !=0    Failure
 */
static int
db_to_svr_que(pbs_queue *pque, pbs_db_que_info_t *pdbque)
{
	pque->qu_qs.qu_name[sizeof(pque->qu_qs.qu_name) - 1] = '\0';
	strncpy(pque->qu_qs.qu_name, pdbque->qu_name, sizeof(pque->qu_qs.qu_name));
	pque->qu_qs.qu_type = pdbque->qu_type;
	pque->qu_qs.qu_ctime = pdbque->qu_ctime;
	pque->qu_qs.qu_mtime = pdbque->qu_mtime;

	if ((decode_attr_db(pque, &pdbque->attr_list, que_attr_def,
		pque->qu_attr, (int) QA_ATR_LAST, 0)) != 0)
		return -1;

	return 0;
}

/**
 * @brief
 *	Save a queue to the database
 *
 * @param[in]	pque  - Pointer to the queue to save
 * @param[in]	mode:
 *		QUE_SAVE_FULL - Save full queue information (update)
 *		QUE_SAVE_NEW  - Save new queue information (insert)
 *
 * @return      Error code
 * @retval	0 - Success
 * @retval	1 - Failure
 *
 */
int
que_save_db(pbs_queue *pque, int mode)
{
	pbs_db_que_info_t	dbque;
	pbs_db_obj_info_t	obj;
	pbs_db_conn_t		*conn = (pbs_db_conn_t *) svr_db_conn;
	int savetype = PBS_UPDATE_DB_FULL;

	if (svr_to_db_que(pque, &dbque, savetype) != 0)
		goto db_err;

	obj.pbs_db_obj_type = PBS_DB_QUEUE;
	obj.pbs_db_un.pbs_db_que = &dbque;

	if (mode == QUE_SAVE_NEW)
		savetype = PBS_INSERT_DB;

	if (pbs_db_save_obj(conn, &obj, savetype) != 0)
		goto db_err;

	pbs_db_reset_obj(&obj);

	return (0);

db_err:
	/* free the attribute list allocated by encode_attrs */
	free(dbque.attr_list.attributes);

	strcpy(log_buffer, "que_save failed ");
	if (conn->conn_db_err != NULL)
		strncat(log_buffer, conn->conn_db_err, LOG_BUF_SIZE - strlen(log_buffer) - 1);
	log_err(-1, __func__, log_buffer);

	panic_stop_db(log_buffer);
	return (-1);
}

/**
 * @brief
 *		Recover a queue from the database
 *
 * @param[in]	qname	- Name of the queue to recover
 *
 * @return	The recovered queue structure
 * @retval	NULL	- Failure
 * @retval	!NULL	- Success - address of recovered queue returned
 *
 */
pbs_queue *
que_recov_db(char *qname, pbs_queue *pq_now, int lock)
{
	pbs_queue		*pq;
	pbs_db_que_info_t	dbque;
	pbs_db_obj_info_t	obj;
	pbs_db_conn_t		*conn = (pbs_db_conn_t *) svr_db_conn;

	obj.pbs_db_obj_type = PBS_DB_QUEUE;
	obj.pbs_db_un.pbs_db_que = &dbque;

	pq = que_alloc(qname);  /* allocate & init queue structure space */
	if (pq == NULL) {
		log_err(-1, "que_recov", "que_alloc failed");
		return NULL;
	}

	dbque.qu_name[sizeof(dbque.qu_name) - 1] = '\0';
	if (pq_now)
		dbque.qu_mtime = pq_now->qu_qs.qu_mtime;
	else
		dbque.qu_mtime = 0;

	strncpy(dbque.qu_name, qname, sizeof(dbque.qu_name));

	/* read in job fixed sub-structure */
	if (pbs_db_load_obj(conn, &obj, 0) != 0)
		goto db_err;
	
	if (db_to_svr_que(pq, &dbque) != 0)
		goto db_err;

	pbs_db_reset_obj(&obj);

	/* all done recovering the queue */
	return (pq);

db_err:
	log_err(-1, "que_recov", "read of queuedb failed");
	if (pq)
		que_free(pq);
	return 0;
}
