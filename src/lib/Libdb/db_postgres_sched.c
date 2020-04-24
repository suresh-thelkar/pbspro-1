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
 * @file    db_postgres_sched.c
 *
 * @brief
 *      Implementation of the scheduler data access functions for postgres
 */
#include <pbs_config.h>   /* the master config generated by configure */
#include "pbs_db.h"
#include "db_postgres.h"

/**
 * @brief
 *	Prepare all the scheduler related sqls. Typically called after connect
 *	and before any other sql exeuction
 *
 * @param[in]	conn - Database connection handle
 *
 * @return      Error code
 * @retval	-1 - Failure
 * @retval	 0 - Success
 *
 */
int
pg_db_prepare_sched_sqls(pbs_db_conn_t *conn)
{
	snprintf(conn->conn_sql, MAX_SQL_LENGTH, "insert into "
		"pbs.scheduler( "
		"sched_name, "
		"sched_savetm, "
		"sched_creattm, "
		"attributes "
		") "
		"values ($1, localtimestamp, localtimestamp, hstore($2::text[])) "
		"returning to_char(sched_savetm, 'YYYY-MM-DD HH24:MI:SS.US') as sched_savetm");
	if (pg_prepare_stmt(conn, STMT_INSERT_SCHED, conn->conn_sql, 2) != 0)
		return -1;

	/* rewrite all attributes for a FULL update */
	snprintf(conn->conn_sql, MAX_SQL_LENGTH, "update pbs.scheduler set "
		"sched_savetm = localtimestamp, "
		"attributes = attributes || hstore($2::text[]) "
		"where sched_name = $1 "
		"returning to_char(sched_savetm, 'YYYY-MM-DD HH24:MI:SS.US') as sched_savetm");
	if (pg_prepare_stmt(conn, STMT_UPDATE_SCHED, conn->conn_sql, 2) != 0)
		return -1;

	snprintf(conn->conn_sql, MAX_SQL_LENGTH, "update pbs.scheduler set "
		"sched_savetm = localtimestamp,"
		"attributes = attributes - $2::text[] "
		"where sched_name = $1 "
		"returning to_char(sched_savetm, 'YYYY-MM-DD HH24:MI:SS.US') as sched_savetm");
	if (pg_prepare_stmt(conn, STMT_REMOVE_SCHEDATTRS, conn->conn_sql, 2) != 0)
		return -1;

	snprintf(conn->conn_sql, MAX_SQL_LENGTH, "select "
		"sched_name, "
		"to_char(sched_savetm, 'YYYY-MM-DD HH24:MI:SS.US') as sched_savetm, "
		"to_char(sched_creattm, 'YYYY-MM-DD HH24:MI:SS.US') as sched_creattm, "
		"hstore_to_array(attributes) as attributes "
		"from "
		"pbs.scheduler "
		"where sched_name = $1");
	if (pg_prepare_stmt(conn, STMT_SELECT_SCHED, conn->conn_sql, 1) != 0)
		return -1;

	snprintf(conn->conn_sql, MAX_SQL_LENGTH, "select "
		"sched_name, "
		"to_char(sched_savetm, 'YYYY-MM-DD HH24:MI:SS.US') as sched_savetm, "
		"to_char(sched_creattm, 'YYYY-MM-DD HH24:MI:SS.US') as sched_creattm, "
		"hstore_to_array(attributes) as attributes "
		"from "
		"pbs.scheduler ");
	if (pg_prepare_stmt(conn, STMT_SELECT_SCHED_ALL, conn->conn_sql, 0) != 0)
		return -1;

	snprintf(conn->conn_sql, MAX_SQL_LENGTH, "delete from pbs.scheduler where sched_name = $1");
	if (pg_prepare_stmt(conn, STMT_DELETE_SCHED, conn->conn_sql, 1) != 0)
		return -1;

	return 0;
}

/**
 * @brief
 *	Insert scheduler data into the database
 *
 * @param[in]	conn - Connection handle
 * @param[in]	obj  - Information of scheduler to be inserted
 *
 * @return      Error code
 * @retval	-1 - Failure
 * @retval	 0 - Success
 *
 */
int
pg_db_save_sched(pbs_db_conn_t *conn, pbs_db_obj_info_t *obj, int savetype)
{
	pbs_db_sched_info_t *psch = obj->pbs_db_un.pbs_db_sched;
	char *stmt = NULL;
	int params;
	char *raw_array = NULL;
	static int sched_savetm_fnum;
	static int fnums_inited = 0;

	SET_PARAM_STR(conn, psch->sched_name, 0);

	/* sched does not have a QS area, so ignoring that */
	
	/* are there attributes to save to memory or local cache? */
	if (psch->cache_attr_list.attr_count > 0) {
		dist_cache_save_attrs(psch->sched_name, &psch->cache_attr_list);
	}

	if ((psch->db_attr_list.attr_count > 0) || (savetype & OBJ_SAVE_NEW)) {
		int len = 0;
		/* convert attributes to postgres raw array format */
		if ((len = attrlist_2_dbarray(&raw_array, &psch->db_attr_list)) <= 0)
			return -1;

		SET_PARAM_BIN(conn, raw_array, len, 1);
		stmt = STMT_UPDATE_SCHED;
		params = 2;
	}

	if (savetype & OBJ_SAVE_NEW)
		stmt = STMT_INSERT_SCHED;

	if (stmt != NULL) {
		if (pg_db_cmd(conn, stmt, params) != 0) {
			free(raw_array);
			return -1;
		}
		if (fnums_inited == 0) {
			sched_savetm_fnum = PQfnumber(conn->conn_resultset, "sched_savetm");
			fnums_inited = 1;
		}
		GET_PARAM_STR(conn->conn_resultset, 0, psch->sched_savetm, sched_savetm_fnum);
		PQclear(conn->conn_resultset);
		free(raw_array);
	}

	return 0;
}

/**
 * @brief
 *	Load scheduler data from the row into the scheduler object
 *
 * @param[in]	res - Resultset from a earlier query
 * @param[out]	psch  - Scheduler object to load data into
 * @param[in]	row - The current row to load within the resultset
 *
 * @return      Error code
 * @retval	-1 - On Error
 * @retval	 0 - On Success
 * @retval	>1 - Number of attributes
 */
static int
load_sched(PGresult *res, pbs_db_sched_info_t *psch, int row)
{
	char *raw_array;
	static int sched_name_fnum, sched_savetm_fnum, sched_creattm_fnum, attributes_fnum;
	static int fnums_inited = 0;

	if (fnums_inited == 0) {
		sched_name_fnum = PQfnumber(res, "sched_name");
		sched_savetm_fnum = PQfnumber(res, "sched_savetm");
		sched_creattm_fnum = PQfnumber(res, "sched_creattm");
		attributes_fnum = PQfnumber(res, "attributes");
		fnums_inited = 1;
	}

	GET_PARAM_STR(res, row, psch->sched_name, sched_name_fnum);
	GET_PARAM_STR(res, row, psch->sched_savetm, sched_savetm_fnum);
	GET_PARAM_STR(res, row, psch->sched_creattm, sched_creattm_fnum);
	GET_PARAM_BIN(res, row, raw_array, attributes_fnum);

	/* convert attributes from postgres raw array format */
	return (dbarray_2_attrlist(raw_array, &psch->db_attr_list));

}

/**
 * @brief
 *	Load scheduler data from the database
 *
 * @param[in]	conn - Connection handle
 * @param[out]	obj  - Load scheduler information into this object
 *
 * @return      Error code
 * @retval	-1 - Failure
 * @retval	 0 - Success
 * @retval	 1 -  Success but no rows loaded
 *
 */
int
pg_db_load_sched(pbs_db_conn_t *conn, pbs_db_obj_info_t *obj)
{
	PGresult *res;
	int rc;
	pbs_db_sched_info_t *psch = obj->pbs_db_un.pbs_db_sched;

	SET_PARAM_STR(conn, psch->sched_name, 0);

	if ((rc = pg_db_query(conn, STMT_SELECT_SCHED, 1, &res)) != 0)
		return rc;

	rc = load_sched(res, psch, 0);

	PQclear(res);

	if (rc == 0) {
		/* in case of multi-server, also read NOSAVM attributes from distributed cache */
		/* call in this functions since all call paths lead to this before decode */
		//if (use_dist_cache) {
		//	dist_cache_recov_attrs(psch->sched_name, &psch->sched_savetm, &psch->cache_attr_list);
		//}
	}

	return rc;
}

/**
 * @brief
 *	Find scheduler
 *
 * @param[in]	conn - Connection handle
 * @param[out]	st   - The cursor state variable updated by this query
 * @param[in]	obj  - Information of sched to be found
 * @param[in]	opts - Any other options (like flags, timestamp)
 *
 * @return      Error code
 * @retval	-1 - Failure
 * @retval	 0 - Success
 * @retval	 1 -  Success but no rows found
 *
 */
int
pg_db_find_sched(pbs_db_conn_t *conn, void *st, pbs_db_obj_info_t *obj,
	pbs_db_query_options_t *opts)
{
	PGresult *res;
	pg_query_state_t *state = (pg_query_state_t *) st;
	int rc;
	int params;

	if (!state)
		return -1;
	strncpy(conn->conn_sql, STMT_SELECT_SCHED_ALL, (MAX_SQL_LENGTH-1));
	conn->conn_sql[MAX_SQL_LENGTH-1] = '\0';
	params = 0;

	if ((rc = pg_db_query(conn, conn->conn_sql, params, &res)) != 0)
		return rc;

	state->row = 0;
	state->res = res;
	state->count = PQntuples(res);

	return 0;
}

/**
 * @brief
 *	Deletes attributes of a Scheduler
 *
 * @param[in]	conn - Connection handle
 * @param[in]	obj  - Scheduler information
 * @param[in]	obj_id  - Scheduler id
 * @param[in]	attr_list - List of attributes
 *
 * @return      Error code
 * @retval	 0 - Success
 * @retval	-1 - On Failure
 *
 */
int
pg_db_del_attr_sched(pbs_db_conn_t *conn, void *obj_id, char *sv_time, pbs_db_attr_list_t *attr_list)
{
	char *raw_array = NULL;
	int len = 0;
	static int sched_savetm_fnum;
	static int fnums_inited = 0;

	if ((len = attrlist_2_dbarray_ex(&raw_array, attr_list, 1)) <= 0)
		return -1;

	SET_PARAM_STR(conn, obj_id, 0);
	SET_PARAM_BIN(conn, raw_array, len, 1);

	if (pg_db_cmd(conn, STMT_REMOVE_SCHEDATTRS, 2) != 0) {
		free(raw_array);
		return -1;
	}

	if (fnums_inited == 0) {
		sched_savetm_fnum = PQfnumber(conn->conn_resultset, "sched_savetm");
		fnums_inited = 1;
	}
	GET_PARAM_STR(conn->conn_resultset, 0, sv_time, sched_savetm_fnum);
	PQclear(conn->conn_resultset);
	free(raw_array);

	return 0;
}

/**
 * @brief
 *	Get the next scheduler from the cursor
 *
 * @param[in]	conn - Connection handle
 * @param[out]	st   - The cursor state
 * @param[in]	obj  - Scheduler information is loaded into this object
 *
 * @return      Error code
 * @retval	-1 - Failure
 * @retval	 0 - Success
 *
 */
int
pg_db_next_sched(pbs_db_conn_t *conn, void *st, pbs_db_obj_info_t *obj)
{
	pg_query_state_t *state = (pg_query_state_t *) st;
	obj->pbs_db_un.pbs_db_sched->sched_savetm[0] = '\0';

	return (load_sched(state->res, obj->pbs_db_un.pbs_db_sched, state->row));

}

/**
 * @brief
 *	Delete the scheduler from the database
 *
 * @param[in]	conn - Connection handle
 * @param[in]	obj  - scheduler information
 *
 * @return      Error code
 * @retval	-1 - Failure
 * @retval	 0 - Success
 * @retval	 1 -  Success but no rows deleted
 *
 */
int
pg_db_delete_sched(pbs_db_conn_t *conn, pbs_db_obj_info_t *obj)
{
	pbs_db_sched_info_t *sc = obj->pbs_db_un.pbs_db_sched;
	SET_PARAM_STR(conn, sc->sched_name, 0);
	return (pg_db_cmd(conn, STMT_DELETE_SCHED, 1));
}

