/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#define USING_LOG_PREFIX RS

#include "lib/string/ob_sql_string.h"
#include "share/ob_rpc_struct.h"
#include "share/ob_upgrade_utils.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/inner_table/ob_inner_table_schema_constants.h"
#include "observer/ob_server_struct.h"
#include "rootserver/ob_root_service.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "share/ob_rpc_struct.h"

namespace oceanbase
{
using namespace common;
using namespace common::sqlclient;
using namespace share;
using namespace share::schema;
using namespace rootserver;
using namespace sql;

namespace share
{
const uint64_t ObUpgradeChecker::UPGRADE_PATH[DATA_VERSION_NUM] = {
  CALC_VERSION(4UL, 1UL, 0UL, 0UL)   // 4.1.0.0
};

bool ObUpgradeChecker::check_data_version_exist(
     const uint64_t version)
{
  bool bret = false;
  OB_ASSERT(DATA_VERSION_NUM == ARRAYSIZEOF(UPGRADE_PATH));
  for (int64_t i = 0; !bret && i < ARRAYSIZEOF(UPGRADE_PATH); i++) {
    bret = (version == UPGRADE_PATH[i]);
  }
  return bret;
}

#define FORMAT_STR(str) ObHexEscapeSqlStr(str.empty() ? ObString("") : str)

/*
 * Upgrade script will insert failed record to __all_rootservice_job before upgrade job runs.
 * This function is used to check if specific upgrade job runs successfully. If we can't find
 * any records of specific upgrade job, we pretend that such upgrade job have run successfully.
 */
int ObUpgradeUtils::check_upgrade_job_passed(ObRsJobType job_type)
{
  int ret = OB_SUCCESS;
  bool exist = false;
  bool success = false;
  if (JOB_TYPE_INVALID >= job_type
      || job_type >= JOB_TYPE_MAX) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid job_type", K(ret), K(job_type));
  } else if (OB_FAIL(check_rs_job_exist(job_type, exist))) {
    LOG_WARN("fail to check rs job exist", K(ret), K(job_type));
  } else if (!exist) {
    // rs job not exist, see as passed
  } else if (OB_FAIL(check_rs_job_success(job_type, success))) {
    LOG_WARN("fail to check rs job success", K(ret), K(job_type));
  } else if (!success) {
    ret = OB_RUN_JOB_NOT_SUCCESS;
    LOG_WARN("run job not success yet", K(ret));
  } else {
    LOG_INFO("run job success", K(ret), K(job_type));
  }
  return ret;
}

/*
 * Can only run upgrade job when failed record of specific upgrade job exists.
 * 1. Upgrade script will insert failed record to __all_rootservice_job before upgrade job runs.
 *    If we can't find any records of specific upgrade job, it's no need to run such upgrade job.
 * 2. If specific upgrade job run successfully once, it's no need to run such upgrade job again.
 */
int ObUpgradeUtils::can_run_upgrade_job(ObRsJobType job_type, bool &can)
{
  int ret = OB_SUCCESS;
  bool exist = false;
  bool success = false;
  can = false;
  if (JOB_TYPE_INVALID >= job_type
      || job_type >= JOB_TYPE_MAX) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid job_type", K(ret), K(job_type));
  } else if (OB_FAIL(check_rs_job_exist(job_type, exist))) {
    LOG_WARN("fail to check rs job exist", K(ret), K(job_type));
  } else if (!exist) {
    LOG_WARN("can't run job while rs_job table is empty", K(ret));
  } else if (OB_FAIL(check_rs_job_success(job_type, success))) {
    LOG_WARN("fail to check rs job success", K(ret), K(job_type));
  } else if (success) {
    LOG_WARN("can't run job while rs_job table is empty", K(ret));
  } else {
    // task exists && not success yet
    can = true;
  }
  return ret;
}

int ObUpgradeUtils::check_rs_job_exist(ObRsJobType job_type, bool &exist)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    int32_t count = OB_INVALID_COUNT;
    exist = false;
    if (JOB_TYPE_INVALID >= job_type
        || job_type >= JOB_TYPE_MAX) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid job_type", K(ret), K(job_type));
    } else if (OB_ISNULL(GCTX.sql_proxy_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("sql_proxy is null", K(ret));
    } else if (sql.assign_fmt("SELECT floor(count(*)) as count FROM %s WHERE job_type = '%s'",
                              OB_ALL_ROOTSERVICE_JOB_TNAME, ObRsJobTableOperator::get_job_type_str(job_type))) {
      LOG_WARN("fail to assign sql", K(ret));
    } else if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
      LOG_WARN("fail to execute sql", K(ret), K(sql));
    } else if (NULL == (result = res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("fail to get sql result", K(ret));
    } else if (OB_FAIL((result->next()))) {
      LOG_WARN("fail to get result", K(ret));
    } else {
      EXTRACT_INT_FIELD_MYSQL(*result, "count", count, int32_t);
      if (OB_FAIL(ret)) {
      } else if (count < 0) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("invalid count", K(ret), K(count));
      } else {
        exist = (count > 0);
      }
    }
  }
  return ret;
}

int ObUpgradeUtils::check_rs_job_success(ObRsJobType job_type, bool &success)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    int32_t count = OB_INVALID_COUNT;
    success = false;
    if (JOB_TYPE_INVALID >= job_type
        || job_type >= JOB_TYPE_MAX) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid job_type", K(ret), K(job_type));
    } else if (OB_ISNULL(GCTX.sql_proxy_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("sql_proxy is null", K(ret));
    } else if (sql.assign_fmt("SELECT floor(count(*)) as count FROM %s "
                              "WHERE job_type = '%s' and job_status = 'SUCCESS'",
                              OB_ALL_ROOTSERVICE_JOB_TNAME, ObRsJobTableOperator::get_job_type_str(job_type))) {
      LOG_WARN("fail to assign sql", K(ret));
    } else if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
      LOG_WARN("fail to execute sql", K(ret), K(sql));
    } else if (NULL == (result = res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("fail to get sql result", K(ret));
    } else if (OB_FAIL((result->next()))) {
      LOG_WARN("fail to get result", K(ret));
    } else {
      EXTRACT_INT_FIELD_MYSQL(*result, "count", count, int32_t);
      if (OB_FAIL(ret)) {
      } else if (count < 0) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("invalid count", K(ret), K(count));
      } else {
        success = (count > 0);
      }
    }
  }
  return ret;
}

int ObUpgradeUtils::check_schema_sync(bool &is_sync)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = NULL;
    int32_t count = OB_INVALID_COUNT;
    is_sync = false;
    if (OB_ISNULL(GCTX.sql_proxy_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("sql_proxy is null", K(ret));
    } else if (sql.assign_fmt("SELECT floor(count(*)) as count FROM %s AS a "
                              "JOIN %s AS b ON a.tenant_id = b.tenant_id "
                              "WHERE a.refreshed_schema_version != b.refreshed_schema_version",
                              OB_ALL_VIRTUAL_SERVER_SCHEMA_INFO_TNAME,
                              OB_ALL_VIRTUAL_SERVER_SCHEMA_INFO_TNAME)) {
      LOG_WARN("fail to assign sql", K(ret));
    } else if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
      LOG_WARN("fail to execute sql", K(ret), K(sql));
    } else if (NULL == (result = res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("fail to get sql result", K(ret));
    } else if (OB_FAIL((result->next()))) {
      LOG_WARN("fail to get result", K(ret));
    } else {
      EXTRACT_INT_FIELD_MYSQL(*result, "count", count, int32_t);
      if (OB_FAIL(ret)) {
      } else if (count < 0) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("invalid count", K(ret), K(count));
      } else {
        is_sync = (count == 0);
      }
    }
  }
  return ret;
}

/* =========== upgrade sys variable =========== */
//  C++ implement for exec_sys_vars_upgrade_dml() in python upgrade script.
int ObUpgradeUtils::upgrade_sys_variable(
    obrpc::ObCommonRpcProxy &rpc_proxy,
    common::ObISQLClient &sql_client,
    const uint64_t tenant_id)
{
  int ret = OB_SUCCESS;
  ObArray<int64_t> update_list; // sys_var_store_idx, sys var to modify
  ObArray<int64_t> add_list;    // sys_var_store_idx, sys var to add
  if (OB_INVALID_TENANT_ID == tenant_id
      || OB_INVALID_ID == tenant_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid tenant_id", KR(ret), K(tenant_id));
  } else if (OB_FAIL(calc_diff_sys_var_(sql_client, tenant_id, update_list, add_list))) {
    LOG_WARN("fail to calc diff sys var", KR(ret), K(tenant_id));
  } else if (OB_FAIL(update_sys_var_(rpc_proxy, tenant_id, true, update_list))) {
    LOG_WARN("fail to update sys var", KR(ret), K(tenant_id));
  } else if (OB_FAIL(update_sys_var_(rpc_proxy, tenant_id, false, add_list))) {
    LOG_WARN("fail to add sys var", KR(ret), K(tenant_id));
  }
  return ret;
}

int ObUpgradeUtils::calc_diff_sys_var_(
    common::ObISQLClient &sql_client,
    const uint64_t tenant_id,
    common::ObArray<int64_t> &update_list,
    common::ObArray<int64_t> &add_list)
{
  int ret = OB_SUCCESS;
  if (OB_INVALID_TENANT_ID == tenant_id
      || OB_INVALID_ID == tenant_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid tenant_id", KR(ret), K(tenant_id));
  } else {
    ObArray<Name> fetch_names;
    ObSqlString sql;
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      if (OB_FAIL(sql.append_fmt(
                  "select name, data_type, value, info, flags, min_val, max_val from %s "
                  "where tenant_id = %lu and (tenant_id, zone, name, schema_version) in ( "
                  "select tenant_id, zone, name, max(schema_version) from %s "
                  "where tenant_id = %lu group by tenant_id, zone, name)",
                  OB_ALL_SYS_VARIABLE_HISTORY_TNAME,
                  ObSchemaUtils::get_extract_tenant_id(tenant_id, tenant_id),
                  OB_ALL_SYS_VARIABLE_HISTORY_TNAME,
                  ObSchemaUtils::get_extract_tenant_id(tenant_id, tenant_id)))) {
        LOG_WARN("fail to append fmt", KR(ret), K(tenant_id), K(sql));
      } else if (OB_FAIL(sql_client.read(res, tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", KR(ret), K(tenant_id), K(sql));
      } else if (NULL == (result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("result is not expected to be NULL", KR(ret), K(tenant_id), K(sql));
      } else {
        while (OB_SUCC(ret) && OB_SUCC(result->next())) {
          ObString name;
          ObString info;
          ObString max_val;
          ObString min_val;
          int64_t data_type = OB_INVALID_ID;
          int64_t flags = OB_INVALID_ID;
          EXTRACT_VARCHAR_FIELD_MYSQL(*result, "name", name);
          EXTRACT_VARCHAR_FIELD_MYSQL(*result, "info", info);
          EXTRACT_VARCHAR_FIELD_MYSQL(*result, "max_val", max_val);
          EXTRACT_VARCHAR_FIELD_MYSQL(*result, "min_val", min_val);
          EXTRACT_INT_FIELD_MYSQL(*result, "data_type", data_type, int64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "flags", flags, int64_t);

          ObSysVarClassType sys_var_id = SYS_VAR_INVALID;
          int64_t var_store_idx = OB_INVALID_INDEX;
          if (FAILEDx(fetch_names.push_back(name))) {
            LOG_WARN("fail to push back name", KR(ret), K(tenant_id), K(name));
          } else if (SYS_VAR_INVALID == (sys_var_id =
                     ObSysVarFactory::find_sys_var_id_by_name(name))) {
            // maybe has unused sys variable in table, just ignore
            LOG_INFO("sys variable exist in table, but not hard code", KR(ret), K(tenant_id), K(name));
          } else if (OB_FAIL(ObSysVarFactory::calc_sys_var_store_idx(sys_var_id, var_store_idx))) {
            LOG_WARN("fail to calc sys var store idx", KR(ret), K(sys_var_id), K(name));
          } else if (false == ObSysVarFactory::is_valid_sys_var_store_idx(var_store_idx)) {
            ret = OB_SCHEMA_ERROR;
            LOG_WARN("calc sys var store idx success but store_idx is invalid", KR(ret), K(var_store_idx));
          } else {
            const ObString &hard_code_info = ObSysVariables::get_info(var_store_idx);
            const ObObjType &hard_code_type = ObSysVariables::get_type(var_store_idx);
            const ObString &hard_code_min_val = ObSysVariables::get_min(var_store_idx);
            const ObString &hard_code_max_val  = ObSysVariables::get_max(var_store_idx);
            const int64_t hard_code_flag = ObSysVariables::get_flags(var_store_idx);
            if (hard_code_flag != flags
                || static_cast<int64_t>(hard_code_type) != data_type
                || 0 != hard_code_info.compare(info)
                || 0 != hard_code_min_val.compare(min_val)
                || 0 != hard_code_max_val.compare(max_val)) {
              // sys var to modify
              LOG_INFO("[UPGRADE] sys var diff, need modify", K(tenant_id), K(name),
                       K(data_type), K(flags), K(min_val), K(max_val), K(info),
                       K(hard_code_type), K(hard_code_flag), K(hard_code_min_val),
                       K(hard_code_max_val), K(hard_code_info));
              if (OB_FAIL(update_list.push_back(var_store_idx))) {
                LOG_WARN("fail to push_back var_store_idx", KR(ret), K(tenant_id), K(name), K(var_store_idx));
              }
            }
          }
        }
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        } else {
          ret = OB_SUCC(ret) ? OB_ERR_UNEXPECTED : ret;
          LOG_WARN("fail to iter result", KR(ret), K(tenant_id));
        }
      }
    }
    // sys var to add
    if (OB_SUCC(ret)) {
      int64_t sys_var_cnt = ObSysVariables::get_amount();
      for (int64_t i = 0; OB_SUCC(ret) && i < sys_var_cnt; i++) {
        const ObString &name = ObSysVariables::get_name(i);
        bool found = false;
        FOREACH_CNT_X(fetch_name, fetch_names, OB_SUCC(ret)) {
          if (OB_ISNULL(fetch_name)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("name is null", KR(ret), K(tenant_id));
          } else if (0 == name.compare(fetch_name->str())) {
            found = true;
            break;
          }
        }
        if (OB_FAIL(ret) || found) {
        } else if (OB_FAIL(add_list.push_back(i))) {
          LOG_WARN("fail to push back var_store_idx", KR(ret), K(tenant_id), K(name));
        } else {
          LOG_INFO("[UPGRADE] sys var miss, need add", K(tenant_id), K(name), K(i));
        }
      }
    }
  }
  return ret;
}

// modify & add sys var according by hard code schema
int ObUpgradeUtils::update_sys_var_(
    obrpc::ObCommonRpcProxy &rpc_proxy,
    const uint64_t tenant_id,
    const bool is_update,
    common::ObArray<int64_t> &update_list)
{
  int ret = OB_SUCCESS;
  if (OB_INVALID_TENANT_ID == tenant_id
      || OB_INVALID_ID == tenant_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid tenant_id", KR(ret), K(tenant_id));
  } else {
    const int64_t timeout = GCONF.internal_sql_execute_timeout;
    for (int64_t i = 0; OB_SUCC(ret) && i < update_list.count(); i++) {
      int64_t start_ts = ObTimeUtility::current_time();
      int64_t var_store_idx = update_list.at(i);
      const ObString &name = ObSysVariables::get_name(var_store_idx);
      const ObObjType &type = ObSysVariables::get_type(var_store_idx);
      const ObString &value = ObSysVariables::get_value(var_store_idx);
      const ObString &min = ObSysVariables::get_min(var_store_idx);
      const ObString &max = ObSysVariables::get_max(var_store_idx);
      const ObString &info = ObSysVariables::get_info(var_store_idx);
      const int64_t flag = ObSysVariables::get_flags(var_store_idx);
      const ObString zone("");
      ObSysParam sys_param;
      obrpc::ObAddSysVarArg arg;
      arg.exec_tenant_id_ = tenant_id;
      arg.if_not_exist_ = true; // not used
      arg.sysvar_.set_tenant_id(tenant_id);
      arg.update_sys_var_ = is_update;
      if (OB_FAIL(sys_param.init(tenant_id, zone, name.ptr(), type,
          value.ptr(), min.ptr(), max.ptr(), info.ptr(), flag))) {
        LOG_WARN("sys_param init failed", KR(ret), K(tenant_id), K(name),
                 K(type), K(value), K(min), K(max), K(info), K(flag));
      } else if (!sys_param.is_valid()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("sys param is invalid", KR(ret), K(tenant_id), K(sys_param));
      } else if (OB_FAIL(ObSchemaUtils::convert_sys_param_to_sysvar_schema(sys_param, arg.sysvar_))) {
        LOG_WARN("convert sys param to sysvar schema failed", KR(ret));
      } else if (OB_FAIL(rpc_proxy.timeout(timeout).add_system_variable(arg))) {
        LOG_WARN("add system variable failed", KR(ret), K(timeout), K(arg));
      }
      LOG_INFO("[UPGRADE] finish upgrade system variable",
               KR(ret), K(tenant_id), K(name), "cost", ObTimeUtility::current_time() - start_ts);
    }
  }
  return ret;
}

/* =========== upgrade sys variable end =========== */

/* =========== upgrade sys stat =========== */
int ObUpgradeUtils::upgrade_sys_stat(
    common::ObISQLClient &sql_client,
    const uint64_t tenant_id)
{
  int ret = OB_SUCCESS;
  ObSysStat sys_stat;
  if (OB_INVALID_TENANT_ID == tenant_id
      || OB_INVALID_ID == tenant_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid tenant_id", KR(ret), K(tenant_id));
  } else if (OB_FAIL(sys_stat.set_initial_values(tenant_id))) {
    LOG_WARN("fail to init sys stat", KR(ret), K(tenant_id));
  } else if (OB_FAIL(filter_sys_stat(sql_client, tenant_id, sys_stat))) {
    LOG_WARN("fail to filter sys stat", KR(ret), K(tenant_id), K(sys_stat));
  } else if (OB_FAIL(ObDDLOperator::replace_sys_stat(tenant_id, sys_stat, sql_client))) {
    LOG_WARN("fail to add sys stat", KR(ret), K(tenant_id), K(sys_stat));
  } else {
    LOG_INFO("[UPGRADE] upgrade sys stat", KR(ret), K(tenant_id), K(sys_stat));
  }
  return ret;
}

int ObUpgradeUtils::filter_sys_stat(
    common::ObISQLClient &sql_client,
    const uint64_t tenant_id,
    ObSysStat &sys_stat)
{
  int ret = OB_SUCCESS;
  if (OB_INVALID_TENANT_ID == tenant_id
      || OB_INVALID_ID == tenant_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid tenant_id", KR(ret), K(tenant_id));
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      ObSqlString sql;
      if (OB_FAIL(sql.append_fmt("select distinct(name) name from %s", OB_ALL_SYS_STAT_TNAME))) {
        LOG_WARN("fail to append sql", KR(ret), K(tenant_id), K(sql));
      } else if (OB_FAIL(sql_client.read(res, tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", KR(ret), K(tenant_id), K(sql));
      } else if (NULL == (result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("result is not expected to be NULL", KR(ret), K(tenant_id), K(sql));
      } else {
        while (OB_SUCC(ret) && OB_SUCC(result->next())) {
          ObString name;
          EXTRACT_VARCHAR_FIELD_MYSQL(*result, "name", name);
          DLIST_FOREACH_REMOVESAFE_X(node, sys_stat.item_list_, OB_SUCC(ret)) {
            if (OB_NOT_NULL(node)) {
              if (OB_ISNULL(node->name_)) {
                ret = OB_ERR_UNEXPECTED;
                LOG_WARN("name is null", KR(ret), K(tenant_id));
              } else if (0 == name.compare(node->name_)) {
                // filter sys stat which exist in __all_sys_stat
                ObSysStat::Item *item = sys_stat.item_list_.remove(node);
                if (OB_ISNULL(item) || 0 != name.compare(item->name_)) {
                  ret = OB_ERR_UNEXPECTED;
                  LOG_WARN("fail to remove node", KR(ret), K(tenant_id), KPC(node));
                } else {
                  break;
                }
              }
            } else {
              ret = OB_ERR_UNEXPECTED;
              LOG_WARN("node is null", KR(ret), K(tenant_id));
            }
          }
        }
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        } else {
          ret = OB_SUCC(ret) ? OB_ERR_UNEXPECTED : ret;
          LOG_WARN("fail to iter result", KR(ret), K(tenant_id));
        }
      }
    }
  }
  return ret;
}
/* =========== upgrade sys stat end=========== */

/* =========== upgrade processor ============= */
ObUpgradeProcesserSet::ObUpgradeProcesserSet()
  : inited_(false), allocator_("UpgProcSet"),
    processor_list_(OB_MALLOC_NORMAL_BLOCK_SIZE,
                    ModulePageAllocator(allocator_))
{
}

ObUpgradeProcesserSet::~ObUpgradeProcesserSet()
{
  for (int64_t i = 0; i < processor_list_.count(); i++) {
    if (OB_NOT_NULL(processor_list_.at(i))) {
      processor_list_.at(i)->~ObBaseUpgradeProcessor();
    }
  }
}

int ObUpgradeProcesserSet::init(
    ObBaseUpgradeProcessor::UpgradeMode mode,
    common::ObMySQLProxy &sql_proxy,
    obrpc::ObSrvRpcProxy &rpc_proxy,
    obrpc::ObCommonRpcProxy &common_proxy,
    share::schema::ObMultiVersionSchemaService &schema_service,
    share::ObCheckStopProvider &check_server_provider)
{
  int ret = OB_SUCCESS;
  if (inited_) {
    ret = OB_INIT_TWICE;
    LOG_WARN("init twice", KR(ret));
  } else {
#define INIT_PROCESSOR_BY_VERSION(MAJOR, MINOR, MAJOR_PATCH, MINOR_PATCH) \
    if (OB_SUCC(ret)) { \
      void *buf = NULL; \
      ObBaseUpgradeProcessor *processor = NULL; \
      int64_t version = static_cast<int64_t>(cal_version((MAJOR), (MINOR), (MAJOR_PATCH), (MINOR_PATCH))); \
      if (OB_ISNULL(buf = allocator_.alloc(sizeof(ObUpgradeFor##MAJOR##MINOR##MAJOR_PATCH##MINOR_PATCH##Processor)))) { \
        ret = OB_ALLOCATE_MEMORY_FAILED; \
        LOG_WARN("fail to alloc upgrade processor", KR(ret)); \
      } else if (OB_ISNULL(processor = new(buf)ObUpgradeFor##MAJOR##MINOR##MAJOR_PATCH##MINOR_PATCH##Processor)) { \
        ret = OB_NOT_INIT; \
        LOG_WARN("fail to new upgrade processor", KR(ret)); \
      } else if (OB_FAIL(processor->init(version, mode, sql_proxy, rpc_proxy, common_proxy, \
                                         schema_service, check_server_provider))) { \
        LOG_WARN("fail to init processor", KR(ret), K(version)); \
      } else if (OB_FAIL(processor_list_.push_back(processor))) { \
        LOG_WARN("fail to push back processor", KR(ret), K(version)); \
      } \
      if (OB_FAIL(ret)) { \
        if (OB_NOT_NULL(processor)) { \
          processor->~ObBaseUpgradeProcessor(); \
          allocator_.free(buf); \
          processor = NULL; \
          buf = NULL; \
        } else if (OB_NOT_NULL(buf)) { \
          allocator_.free(buf); \
          buf = NULL; \
        } \
      } \
    }
    // order by data version asc
    INIT_PROCESSOR_BY_VERSION(4, 1, 0, 0);
#undef INIT_PROCESSOR_BY_VERSION
    inited_ = true;
  }
  return ret;
}

int ObUpgradeProcesserSet::check_inner_stat() const
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init yet", KR(ret));
  } else if (processor_list_.count() <= 0) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("processer_list cnt is less than 0", KR(ret));
  }
  return ret;
}

int ObUpgradeProcesserSet::get_processor_by_idx(
    const int64_t idx,
    ObBaseUpgradeProcessor *&processor) const
{
  int ret = OB_SUCCESS;
  int64_t cnt = processor_list_.count();
  processor = NULL;
  if (OB_FAIL(check_inner_stat())) {
    LOG_WARN("check inner stat failed", KR(ret));
  } else if (idx >= cnt || idx < 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid idx", KR(ret), K(idx), K(cnt));
  } else {
    processor = processor_list_.at(idx);
    if (OB_ISNULL(processor)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("processor is null", KR(ret), K(idx));
    } else {
      processor->set_tenant_id(OB_INVALID_ID); // reset
    }
  }
  return ret;
}

int ObUpgradeProcesserSet::get_processor_by_version(
    const int64_t version,
    ObBaseUpgradeProcessor *&processor) const
{
  int ret = OB_SUCCESS;
  int64_t idx = OB_INVALID_INDEX;
  if (OB_FAIL(get_processor_idx_by_version(version, idx))) {
    LOG_WARN("fail to get processor idx by version", KR(ret), K(version));
  } else if (OB_FAIL(get_processor_by_idx(idx, processor))) {
    LOG_WARN("fail to get processor by idx", KR(ret), K(version));
  }
  return ret;
}

// run upgrade processor by (start_version, end_version]
int ObUpgradeProcesserSet::get_processor_idx_by_range(
    const int64_t start_version,
    const int64_t end_version,
    int64_t &start_idx,
    int64_t &end_idx)
{
  int ret = OB_SUCCESS;
  start_idx = OB_INVALID_INDEX;
  end_idx = OB_INVALID_INDEX;
  if (OB_FAIL(check_inner_stat())) {
    LOG_WARN("check inner stat failed", KR(ret));
  } else if (start_version <= 0 || end_version <= 0 || start_version > end_version) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid version", KR(ret), K(start_version), K(end_version));
  } else if (OB_FAIL(get_processor_idx_by_version(start_version, start_idx))) {
    LOG_WARN("fail to get processor idx by version", KR(ret), K(start_version));
  } else if (OB_FAIL(get_processor_idx_by_version(end_version, end_idx))) {
    LOG_WARN("fail to get processor idx by version", KR(ret), K(end_version));
  }
  return ret;
}

int ObUpgradeProcesserSet::get_processor_idx_by_version(
    const int64_t version,
    int64_t &idx) const
{
  int ret = OB_SUCCESS;
  idx = OB_INVALID_INDEX;
  if (OB_FAIL(check_inner_stat())) {
    LOG_WARN("check inner stat failed", KR(ret));
  } else if (version <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid version", KR(ret), K(version));
  } else if (processor_list_.count() <= 0) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("processor_list cnt shoud greator than 0", KR(ret));
  } else {
    int64_t start = 0;
    int64_t end = processor_list_.count() - 1;
    while (OB_SUCC(ret) && start <= end) {
      int64_t mid = (start + end) / 2;
      if (OB_ISNULL(processor_list_.at(mid))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("processor is null", KR(ret), K(mid));
      } else if (processor_list_.at(mid)->get_version() == version) {
        idx = mid;
        break;
      } else if (processor_list_.at(mid)->get_version() > version) {
        end = mid - 1;
      } else {
        start = mid + 1;
      }
    }
    if (OB_SUCC(ret) && OB_INVALID_INDEX == idx) {
      ret = OB_ENTRY_NOT_EXIST;
      LOG_WARN("fail to find processor by version", KR(ret), K(version));
    }
  }
  return ret;
}

ObBaseUpgradeProcessor::ObBaseUpgradeProcessor()
  : inited_(false), data_version_(OB_INVALID_VERSION),
    tenant_id_(common::OB_INVALID_ID), mode_(UPGRADE_MODE_INVALID),
    sql_proxy_(NULL), rpc_proxy_(NULL), common_proxy_(NULL), schema_service_(NULL),
    check_stop_provider_(NULL)
{
}

// Standby cluster runs sys tenant's upgrade process only.
int ObBaseUpgradeProcessor::check_inner_stat() const
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init yet", KR(ret));
  } else if (data_version_ <= 0
             || tenant_id_ == OB_INVALID_ID
             || UPGRADE_MODE_INVALID == mode_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid processor status",
             KR(ret), K_(data_version), K_(tenant_id), K_(mode));
  } else if (GCTX.is_standby_cluster() && OB_SYS_TENANT_ID != tenant_id_) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("run upgrade job for non-sys tenant in standby cluster is not supported",
             KR(ret), K_(tenant_id));
  } else if (OB_ISNULL(check_stop_provider_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("check_stop_provider is null", KR(ret));
  } else if (OB_FAIL(check_stop_provider_->check_stop())) {
    LOG_WARN("check stop", KR(ret));
  }
  return ret;
}

int ObBaseUpgradeProcessor::init(
    int64_t data_version,
    UpgradeMode mode,
    common::ObMySQLProxy &sql_proxy,
    obrpc::ObSrvRpcProxy &rpc_proxy,
    obrpc::ObCommonRpcProxy &common_proxy,
    share::schema::ObMultiVersionSchemaService &schema_service,
    share::ObCheckStopProvider &check_server_provider)
{
  int ret = OB_SUCCESS;
  if (inited_) {
    ret = OB_INIT_TWICE;
    LOG_WARN("init twice", KR(ret));
  } else {
    mode_ = mode;
    data_version_ = data_version;
    sql_proxy_ = &sql_proxy;
    rpc_proxy_ = &rpc_proxy;
    common_proxy_ = &common_proxy;
    schema_service_ = &schema_service;
    check_stop_provider_ = &check_server_provider;
    inited_ = true;
  }
  return ret;
}

#undef FORMAT_STR

/* =========== special upgrade processor start ============= */

/* =========== special upgrade processor end   ============= */
} // end share
} // end oceanbase
