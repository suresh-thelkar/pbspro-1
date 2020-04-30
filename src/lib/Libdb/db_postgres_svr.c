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
 * @file    db_postgres_svr.c
 *
 * @brief
 *      Implementation of the svr data access functions for postgres
 */

#include <pbs_config.h>   /* the master config generated by configure */
#include "pbs_db.h"
#include <errno.h>
#include "db_postgres.h"

/**
 * @brief
 *	Prepare all the server related sqls. Typically called after connect
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
pg_db_prepare_svr_sqls(pbs_db_conn_t *conn)
{
	snprintf(conn->conn_sql, MAX_SQL_LENGTH, "insert into pbs.server( "
		"sv_savetm, "
		"sv_creattm, "
		"attributes "
		") "
		"values "
		"(localtimestamp, localtimestamp, hstore($1::text[])) "
		"returning to_char(sv_savetm, 'YYYY-MM-DD HH24:MI:SS.US') as sv_savetm");
	if (pg_prepare_stmt(conn, STMT_INSERT_SVR, conn->conn_sql, 1) != 0)
		return -1;

	/* replace all attributes for a FULL update */
	snprintf(conn->conn_sql, MAX_SQL_LENGTH, "update pbs.server set "
		"sv_savetm = localtimestamp, "
		"attributes = attributes || hstore($1::text[]) "
		"returning to_char(sv_savetm, 'YYYY-MM-DD HH24:MI:SS.US') as sv_savetm");
	if (pg_prepare_stmt(conn, STMT_UPDATE_SVR, conn->conn_sql, 1) != 0)
		return -1;

	snprintf(conn->conn_sql, MAX_SQL_LENGTH, "update pbs.server set "
		"sv_savetm = localtimestamp,"
		"attributes = attributes - $1::text[] "
		"returning to_char(sv_savetm, 'YYYY-MM-DD HH24:MI:SS.US') as sv_savetm");
	if (pg_prepare_stmt(conn, STMT_REMOVE_SVRATTRS, conn->conn_sql, 1) != 0)
		return -1;

	snprintf(conn->conn_sql, MAX_SQL_LENGTH, "select "
		"to_char(sv_savetm, 'YYYY-MM-DD HH24:MI:SS.US') as sv_savetm, "
		"to_char(sv_creattm, 'YYYY-MM-DD HH24:MI:SS.US') as sv_creattm, "
		"hstore_to_array(attributes) as attributes "
		"from "
		"pbs.server ");
	if (pg_prepare_stmt(conn, STMT_SELECT_SVR, conn->conn_sql, 0) != 0)
		return -1;

	snprintf(conn->conn_sql, MAX_SQL_LENGTH, "select "
		"pbs_schema_version "
		"from "
		"pbs.info");
	if (pg_prepare_stmt(conn, STMT_SELECT_DBVER, conn->conn_sql, 0) != 0)
		return -1;

	return 0;
}

/**
 * @brief
 *	Truncate all data from ALL tables from the database
 *
 * @param[in]	conn - The database connection handle
 *
 * @return      Error code
 * @retval	-1 - Failure
 *		 0 - Success
 *
 */
int
pbs_db_truncate_all(pbs_db_conn_t *conn)
{
	snprintf(conn->conn_sql, MAX_SQL_LENGTH, "truncate table 	"
		"pbs.scheduler, "
		"pbs.node, "
		"pbs.queue, "
		"pbs.resv, "
		"pbs.job_scr, "
		"pbs.job, "
		"pbs.server");

	if (pbs_db_execute_str(conn, conn->conn_sql) == -1)
		return -1;

	return 0;
}

/**
 * @brief
 *	Insert server data into the database
 *
 * @param[in]	conn - Connection handle
 * @param[in]	obj  - Information of server to be inserted
 *
 * @return      Error code
 * @retval	-1 - Failure
 * @retval	 0 - Success
 *
 */
int
pg_db_save_svr(pbs_db_conn_t *conn, pbs_db_obj_info_t *obj, int savetype)
{
	pbs_db_svr_info_t *ps = obj->pbs_db_un.pbs_db_svr;
	char *stmt = NULL;
	int params;
	char *raw_array = NULL;
	int len = 0;
	static int sv_savetm_fnum;
	static int fnums_inited = 0;

	/* Svr does not have a QS area, so ignoring that */

	/* are there attributes to save to memory or local cache? */
	if (ps->cache_attr_list.attr_count > 0) {
		dist_cache_save_attrs("server", &ps->cache_attr_list);
	}

	if ((ps->db_attr_list.attr_count > 0) || (savetype & OBJ_SAVE_NEW)) {
		/* convert attributes to postgres raw array format */
		if ((len = attrlist_2_dbarray(&raw_array, &ps->db_attr_list)) <= 0)
			return -1;

		SET_PARAM_BIN(conn, raw_array, len, 0);
		params = 1;
		stmt = STMT_UPDATE_SVR;
	}

	if (savetype & OBJ_SAVE_NEW)
		stmt = STMT_INSERT_SVR;

	if (stmt != NULL) {
		if (pg_db_cmd(conn, stmt, params) != 0) {
			free(raw_array);
			return -1;
		}

		if (fnums_inited == 0) {
			sv_savetm_fnum = PQfnumber(conn->conn_resultset, "sv_savetm");
			fnums_inited = 1;
		}
		GET_PARAM_STR(conn->conn_resultset, 0, ps->sv_savetm, sv_savetm_fnum);
		PQclear(conn->conn_resultset);
		free(raw_array);
	}

	return 0;
}

/**
 * @brief
 *	Load server data from the database
 *
 * @param[in]	conn - Connection handle
 * @param[in]	obj  - Load server information into this object
 *
 * @return      Error code
 * @retval	-1 - Failure
 * @retval	 0 - Success
 * @retval	 1 -  Success but no rows loaded
 *
 */
int
pg_db_load_svr(pbs_db_conn_t *conn, pbs_db_obj_info_t *obj)
{
	PGresult *res;
	int rc;
	char *raw_array;
	pbs_db_svr_info_t *ps = obj->pbs_db_un.pbs_db_svr;
	static int sv_savetm_fnum, sv_creattm_fnum, attributes_fnum;
	static int fnums_inited = 0;

	if ((rc = pg_db_query(conn, STMT_SELECT_SVR, 0, &res)) != 0)
		return rc;

	if (fnums_inited == 0) {
		sv_savetm_fnum = PQfnumber(res, "sv_savetm");
		sv_creattm_fnum = PQfnumber(res, "sv_creattm");
		attributes_fnum = PQfnumber(res, "attributes");
		fnums_inited = 1;
	}

	GET_PARAM_STR(res, 0, ps->sv_savetm, sv_savetm_fnum);
	GET_PARAM_STR(res, 0, ps->sv_creattm, sv_creattm_fnum);
	GET_PARAM_BIN(res, 0, raw_array, attributes_fnum);

	/* convert attributes from postgres raw array format */
	rc = dbarray_2_attrlist(raw_array, &ps->db_attr_list);

	PQclear(res);

	if (rc == 0) {
		/* in case of multi-server, also read NOSAVM attributes from distributed cache */
		/* call in this functions since all call paths lead to this before decode */
		//if (use_dist_cache) {
		//	dist_cache_recov_attrs("server", &ps->sv_savetm, &ps->cache_attr_list);
		//}
	}

	return rc;
}

/**
 * @brief
 *	Retrieve the Datastore schema version (maj, min)
 *
 * @param[out]   db_maj_ver - return the major schema version
 * @param[out]   db_min_ver - return the minor schema version
 *
 * @return     Error code
 * @retval     -1 - Failure
 * @retval     0  - Success
 *
 */
int
pbs_db_get_schema_version(pbs_db_conn_t *conn, int *db_maj_ver, int *db_min_ver)
{
	PGresult *res;
	int rc;
	char ver_str[MAX_SCHEMA_VERSION_LEN + 1];
	char *token;

	if ((rc = pg_db_query(conn, STMT_SELECT_DBVER, 0, &res)) != 0)
		return rc;

	ver_str[0] = '\0';
	GET_PARAM_STR(res, 0, ver_str, PQfnumber(res, "pbs_schema_version"));

	PQclear(res);

	if (ver_str[0] == '\0')
		return -1;

	token = strtok(ver_str, ".");
	if (!token)
		return -1;
	*db_maj_ver = atol(token);

	token = strtok(NULL, ".");
	if (!token)
		return -1;
	*db_min_ver = atol(token);

	return 0;
}

/**
 * @brief
 *	Deletes attributes of a server
 *
 * @param[in]	conn - Connection handle
 * @param[in]	obj  - server information
 * @param[in]	obj_id  - server id
 * @param[in]	attr_list - List of attributes
 *
 * @return      Error code
 * @retval	 0 - Success
 * @retval	-1 - On Failure
 *
 */
int
pg_db_del_attr_svr(pbs_db_conn_t *conn, void *obj_id, char *sv_time, pbs_db_attr_list_t *attr_list)
{
	char *raw_array = NULL;
	int len = 0;
	static int sv_savetm_fnum;
	static int fnums_inited = 0;

	if ((len = attrlist_2_dbarray_ex(&raw_array, attr_list, 1)) <= 0)
		return -1;

	SET_PARAM_BIN(conn, raw_array, len, 0);

	if (pg_db_cmd(conn, STMT_REMOVE_SVRATTRS, 1) != 0) {
		PQclear(conn->conn_resultset);
		return -1;
	}

	if (fnums_inited == 0) {
		sv_savetm_fnum = PQfnumber(conn->conn_resultset, "sv_savetm");
		fnums_inited = 1;
	}
	GET_PARAM_STR(conn->conn_resultset, 0, sv_time, sv_savetm_fnum);
	PQclear(conn->conn_resultset);
	free(raw_array);

	return 0;
}
