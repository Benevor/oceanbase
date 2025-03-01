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

#include "lib/utility/ob_macro_utils.h"
#include "ob_trans_service.h"
#include "ob_trans_define.h"
#include "lib/profile/ob_perf_event.h"
#include "lib/stat/ob_session_stat.h"
#include "lib/ob_name_id_def.h"
#include "lib/ob_running_mode.h"
#include "ob_trans_ctx.h"
#include "ob_trans_factory.h"
#include "ob_trans_functor.h"
#include "ob_tx_msg.h"
#include "ob_tx_log_adapter.h"
#include "ob_trans_part_ctx.h"
#include "ob_trans_result.h"
#include "observer/ob_server.h"
#include "observer/ob_server_struct.h"
#include "observer/omt/ob_tenant_config_mgr.h"
#include "storage/ob_i_store.h"
#include "wrs/ob_i_weak_read_service.h"           // ObIWeakReadService
#include "sql/session/ob_basic_session_info.h"
#include "wrs/ob_weak_read_util.h"               // ObWeakReadUtil
#include "storage/memtable/ob_memtable_context.h"
#include "common/storage/ob_sequence.h"
#include "storage/tx_table/ob_tx_table_define.h"
#include "storage/tx_storage/ob_ls_service.h"
#include "storage/tx_storage/ob_ls_handle.h"
#include "storage/ls/ob_ls.h"
#include "ob_xa_service.h"
#include "rootserver/ob_tenant_recovery_reportor.h"

/*  interface(s)  */
namespace oceanbase {
namespace transaction {

using namespace memtable;

int ObTransService::create_ls(const share::ObLSID &ls_id,
                              ObLS &ls,
                              ObITxLogParam *param,
                              ObITxLogAdapter *log_adapter)
{
  int ret = OB_SUCCESS;
  ObTableHandleV2 lock_memtable;
  ObTxTable *tx_table = nullptr;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    TRANS_LOG(WARN, "ObTransService not inited", K(ret), K(*this));
  } else if (OB_UNLIKELY(!is_running_)) {
    ret = OB_NOT_RUNNING;
    TRANS_LOG(WARN, "ObTransService is not running", K(ret), K(*this));
  } else if (!ls_id.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    TRANS_LOG(WARN, "invalid argument", K(ret), K(ls_id));
  } else if (OB_ISNULL(tx_table = ls.get_tx_table())) {
    TRANS_LOG(WARN, "get tx table fail", K(ret), K(ls_id));
  } else if (OB_FAIL(tx_ctx_mgr_.create_ls(tenant_id_,
                                           ls_id,
                                           tx_table,
                                           ls.get_lock_table(),
                                           *ls.get_tx_svr(),
                                           param,
                                           log_adapter))) {
    TRANS_LOG(WARN, "create ls failed", K(ret), K(*this));
  } else {
    // do nothing
  }
  if (OB_FAIL(ret)) {
    TRANS_LOG(WARN, "create ls failed", K(ret), K(tenant_id_), K(ls_id));
  } else {
    TRANS_LOG(INFO, "create ls success", K(tenant_id_), K(ls_id));
  }

  return ret;
}

int ObTransService::remove_ls(const share::ObLSID &ls_id, const bool graceful)
{
  int ret = OB_SUCCESS;

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    TRANS_LOG(WARN, "ObTransService not inited", K(ret));
  } else if (OB_UNLIKELY(!is_running_)) {
    ret = OB_NOT_RUNNING;
    TRANS_LOG(WARN, "ObTransService is not running", K(ret));
  } else if (!ls_id.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    TRANS_LOG(WARN, "invalid argument", K(ret), K(ls_id));
  } else if (OB_FAIL(tx_ctx_mgr_.remove_ls(ls_id, graceful))) {
    TRANS_LOG(WARN, "participant remove ls_id error", K(ret), K(ls_id), K(graceful));
  // FIXME. xiaoshi.xjl
  //} else if (OB_FAIL(dup_table_lease_task_map_.del(ls_id))) {
  //  if (OB_ENTRY_NOT_EXIST == ret) {
  //    ret = OB_SUCCESS;
  //    TRANS_LOG(INFO, "remove ls success", K(ls_id), K(graceful));
  //  } else {
  //    TRANS_LOG(WARN, "erase lease task from hashmap error", K(ret), K(ls_id));
  //  }
  } else {
    TRANS_LOG(INFO, "remove ls success", K(ls_id), K(graceful));
  }
  if (OB_FAIL(ret)) {
    TRANS_LOG(WARN, "remove ls failed", K(ret), K(ls_id), K(graceful));
  } else {
    TRANS_LOG(INFO, "remove ls success", K(ls_id), K(graceful));
  }

  return ret;
}

#ifdef TX_PARTS_CONTAIN_
#error "redefine TX_PARTS_CONTAIN_"
#else
#define TX_PARTS_CONTAIN_(parts, id_, ls_id, hit)       \
  do {                                                  \
    hit = false;                                        \
    ARRAY_FOREACH_NORET(parts, idx) {                   \
      if (parts.at(idx).id_ == ls_id) {                 \
        hit = true;                                     \
        break;                                          \
      }                                                 \
    }                                                   \
  } while(0)
#endif

int ObTransService::acquire_tx(const char* buf,
                               const int64_t len,
                               int64_t &pos,
                               ObTxDesc *&tx)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(tx_desc_mgr_.alloc(tx))) {
    TRANS_LOG(WARN, "alloc tx fail", K(ret), KPC(this));
  } else if (OB_FAIL(tx->deserialize(buf, len, pos))) {
    tx_desc_mgr_.revert(*tx);
    tx = NULL;
    TRANS_LOG(WARN, "desrialize txDesc fail", K(ret),
              K(len),K(pos), K(buf), KPC(this));
  } else {
    tx->flags_.SHADOW_ = true;
  }
  if (tx) {
    REC_TRANS_TRACE_EXT(&tx->get_tlog(), deserialize,
                        OB_ID(addr), (void*)&tx,
                        OB_ID(txid), tx->tx_id_);
  }
  TRANS_LOG(TRACE, "acquire tx by deserialize", K(ret), K(*this), KP(buf), KPC(tx));
  return ret;
}

/*
 * do_commit_tx_ - the real work of commit tx
 *
 * steps:
 * 1. decide coordinator
 * 2. try local call optimization, if fail fallback to step 3
 * 3. post commit message to coordinator
 *
 * If any failures occurred:
 * - if no message has been sent, state can be revert to
 *   ACTIVE, and the caller can retry
 * - if any message has been sent, a prepose timer task will
 *   drive the retry in background, the commit return success
 *
 * Return:
 * OB_SUCCESS - either local commit started or
 *              remote commit retry task has been registred
 * OB_XXX     - try local commit failed and can not been
 *              fallback to remote commit via send message
 */
int ObTransService::do_commit_tx_(ObTxDesc &tx,
                                  const int64_t expire_ts,
                                  ObITxCallback &cb,
                                  int64_t &commit_version)
{
  int ret = OB_SUCCESS;
  ObTxPart *coord = NULL;
  tx.set_commit_cb(&cb);
  tx.commit_expire_ts_ = expire_ts;
  if (OB_FAIL(decide_tx_commit_info_(tx, coord))) {
    TRANS_LOG(WARN, "decide tx coordinator fail, tx will abort", K(ret), K(tx));
  } else if (OB_FAIL(tx.commit_task_.init(&tx, this))) {
    TRANS_LOG(WARN, "init timeout task fail", K(ret), K(tx));
  } else if (coord->addr_ == self_ && (
             OB_SUCC(local_ls_commit_tx_(tx.tx_id_,
                                         tx.coord_id_,
                                         tx.commit_parts_,
                                         expire_ts,
                                         tx.trace_info_.get_app_trace_info(),
                                         tx.op_sn_,
                                         commit_version))
             || !commit_need_retry_(ret))) {
    if (OB_FAIL(ret)) {
      TRANS_LOG(WARN, "local ls commit tx fail", K(ret), K_(tx.coord_id), K(tx));
    } else {
      TRANS_LOG(TRACE, "local ls commit tx started", K(tx));
    }
  } else if (OB_FAIL(do_commit_tx_slowpath_(tx, expire_ts))) {
    TRANS_LOG(WARN, "commit tx slowpath fail", K(ret),
              K_(tx.coord_id), K_(tx.commit_parts), K(tx));
  } else {
    TRANS_LOG(TRACE, "remote commit started", K(tx), K_(self));
  }
  // start commit fail
  if (OB_FAIL(ret)) {
    tx.cancel_commit_cb();
  }
  return ret;
}

/*
 * try send commit msg to coordinator, and register retry task
 * if msg send fail, the retry task will retry later
 * if both register task fail and send are failed, the commit failed
 */
int ObTransService::do_commit_tx_slowpath_(ObTxDesc &tx, const int64_t expire_ts) {
  int ret = OB_SUCCESS;
  ObTxCommitMsg commit_msg;
  if (OB_FAIL(register_commit_retry_task_(tx))) {
    TRANS_LOG(WARN, "register retry commit task fail", K(ret), K(tx));
  } else if (OB_FAIL(build_tx_commit_msg_(tx, commit_msg))) {
    TRANS_LOG(WARN, "build tx commit msg fail", K(ret), K(tx));
    // build msg fail won't cause commit fail, later driven by retry timer
    ret = OB_SUCCESS;
  } else if (OB_FAIL(rpc_->post_msg(tx.coord_id_, commit_msg))) {
    TRANS_LOG(WARN, "post tx commit msg fail", K(ret), K(tx), K(commit_msg));
    // send msg fail won't cause commit fail, later driven by retry timer
    ret = OB_SUCCESS;
  }
  TRANS_LOG(TRACE, "do commit tx slowpath", K(ret), K(tx));
  return ret;
}

int ObTransService::register_commit_retry_task_(ObTxDesc &tx, const int64_t max_delay)
{
  int ret = OB_SUCCESS;
  int64_t delay = ObTransCtx::MAX_TRANS_2PC_TIMEOUT_US;
  int64_t now = ObClockGenerator::getClock();
  int64_t expire_after = std::min(tx.expire_ts_ - now, tx.commit_expire_ts_ - now);
  if (expire_after > 0) { // KEEP delay always > 0
    delay = std::min(delay, expire_after);
  }
  delay = std::min(delay, max_delay);
  if (OB_FAIL(tx_desc_mgr_.acquire_tx_ref(tx.tx_id_))) {
    TRANS_LOG(WARN, "acquire tx ref fail", KR(ret), K(tx));
  } else {
    if (OB_FAIL(timer_.register_timeout_task(tx.commit_task_, delay))) {
      TRANS_LOG(WARN, "register tx retry task fail", KR(ret), K(delay), K(tx));
      tx_desc_mgr_.revert(tx);
    }
  }
#ifndef NDEBUG
  TRANS_LOG(INFO, "register commit retry task", K(ret), K(delay), K(tx));
#else
  if (OB_FAIL(ret)) {
    TRANS_LOG(WARN, "register commit retry task fail", K(ret), K(delay), K(tx));
  }
#endif
  return ret;
}

// unregister commit retry task, handle its reference to tx correctly
int ObTransService::unregister_commit_retry_task_(ObTxDesc &tx)
{
  int ret = OB_SUCCESS;

  if (!tx.commit_task_.is_registered()) {
    // task has not been scheduled, it has't ref to txDesc
    TRANS_LOG(INFO, "task canceled", K(tx));
  } else if (OB_SUCC(timer_.unregister_timeout_task(tx.commit_task_))) {
    // task has been scheduled but hasn't ran and won't ran in the future
    // release ref of TxDesc hold by task.
    tx_desc_mgr_.revert(tx);
    TRANS_LOG(TRACE, "timeout task deregistered", K(tx));
  } else if(OB_TIMER_TASK_HAS_NOT_SCHEDULED == ret) {
    // task has been scheduled and then was picked up to run
    // it must will run finally, its ref will handle by itself.
    ret = OB_SUCCESS;
    TRANS_LOG(TRACE, "timeout task not scheduled, deregistered", K(tx));
  } else if (FALSE_IT(tx.commit_task_.set_registered(false))) {
  } else {
    TRANS_LOG(WARN, "deregister timeout task fail", K(tx));
  }

  return ret;
}
/*
 * retry tx commit
 * 1. if tx already terminated, ignore
 * 2. send commit msg to coordinator
 * 3. register retry task again
 */
int ObTransService::handle_tx_commit_timeout(ObTxDesc &tx, const int64_t delay)
{
  int ret = OB_SUCCESS;
  // remember tx_id because tx maybe cleanout and reused
  // in this function's following steps.
  auto tx_id = tx.tx_id_;
  int64_t now = ObClockGenerator::getClock();
  if (OB_FAIL(tx.lock_.lock(5000000))) {
    TRANS_LOG(WARN, "failed to acquire lock in specified time", K(tx));
    // FIXME: how to handle it without lock protection
  } else {
    if (!tx.commit_task_.is_registered()){
      TRANS_LOG(INFO, "task canceled", K(tx));
    } else if (tx.flags_.RELEASED_) {
      TRANS_LOG(INFO, "tx released, cancel commit retry", K(tx));
    } else if (FALSE_IT(tx.commit_task_.set_registered(false))) {
    } else if (tx.state_ != ObTxDesc::State::IN_TERMINATE) {
      ret = OB_ERR_UNEXPECTED;
      TRANS_LOG(WARN, "unexpect tx state", K(ret), K_(tx.state), K(tx));
    } else if (tx.expire_ts_ <= now){
      TRANS_LOG(WARN, "tx has timeout", K_(tx.expire_ts), K(tx));
      handle_tx_commit_result_(tx, OB_TRANS_TIMEOUT);
    } else if (tx.commit_expire_ts_ <= now) {
      TRANS_LOG(WARN, "tx commit timeout", K_(tx.commit_expire_ts), K(tx));
      handle_tx_commit_result_(tx, OB_TRANS_STMT_TIMEOUT);
    } else {
      ObTxCommitMsg commit_msg;
      if (OB_FAIL(build_tx_commit_msg_(tx, commit_msg))) {
        TRANS_LOG(WARN, "build tx commit msg fail", K(ret), K(tx));
      } else if (OB_FAIL(rpc_->post_msg(tx.coord_id_, commit_msg))) {
        TRANS_LOG(WARN, "post commit msg fail", K(ret), K(tx));
      }
      // register again
      if (OB_FAIL(register_commit_retry_task_(tx))) {
        TRANS_LOG(WARN, "reregister task fail", K(ret), K(tx));
      }
    }
    tx.lock_.unlock();
    tx.execute_commit_cb();
  }
  // NOTE:
  // it not safe and meaningless to access tx after commit_cb
  // has been called, the tx may has been reused or release
  // in the commit_cb
  TRANS_LOG(INFO, "handle tx commit timeout", K(ret), K(tx_id));
  return ret;
}

/*
 * handle_tx_commit_result - callback from coordinator
 */
int ObTransService::handle_tx_commit_result(const ObTransID &tx_id,
                                            const int result,
                                            const int64_t commit_version)
{
  int ret = OB_SUCCESS;
  ObTxDesc *tx = NULL;
  if (OB_FAIL(tx_desc_mgr_.get(tx_id, tx))) {
    TRANS_LOG(WARN, "cannot found tx by id", K(ret), K(tx_id), K(result));
  } else {
    bool need_cb = false;
    tx->lock_.lock();
    if (tx->state_ < ObTxDesc::State::IN_TERMINATE) {
      ret = OB_ERR_UNEXPECTED;
      TRANS_LOG(ERROR, "unexpected tx state", K(ret),
                K_(tx->state), K(tx_id), K(result), KPC(tx));
      tx->print_trace_();
    } else if (tx->state_ > ObTxDesc::State::IN_TERMINATE) {
      TRANS_LOG(WARN, "tx has terminated", K_(tx->state),
                K(tx_id), K(result), KPC(tx));
      tx->print_trace_();
    } else {
      need_cb = true;
      ret = handle_tx_commit_result_(*tx, result, commit_version);
    }
    tx->lock_.unlock();
    if (need_cb) { tx->execute_commit_cb(); }
  }
  if (OB_NOT_NULL(tx)) {
    tx_desc_mgr_.revert(*tx);
  }
  return ret;
}

/* handle_tx_commit_result_ - handle commit's result
 *
 * the result may not be final result
 *
 * result was fall into three categories:
 * 1) finished and finalized:
 *    COMMITTED / ABORTED / NOT_FOUND / TIME_OUT
 * 2) special error hint a retry is expected:
 *    eg. NOT_MASTER | SWITCH_TO_FOLLOWER | FROZEN_BLOKING etc.
 * 3) other errors : should be ignored and retry
 */
int ObTransService::handle_tx_commit_result_(ObTxDesc &tx,
                                             const int result,
                                             const int64_t commit_version)
{
  int ret = OB_SUCCESS;
  bool commit_fin = true;
  int commit_out = OB_SUCCESS;
  switch (result) {
  case OB_EAGAIN:
  case OB_BLOCK_FROZEN:
    // for single log stream trans, the leader is freezing
    // and is not able to submit log right now,
    // return this result to drive and try again later.
  case OB_SWITCHING_TO_FOLLOWER_GRACEFULLY:
    // 1. callback from switch_to_follower_gracefully on local
  case OB_NOT_MASTER:
    // 1. callback from switch_to_follower_forcedly on local
    // 2. callback from commit_response (from remote)
    commit_fin = false;
    if (tx.commit_task_.is_registered()) {
      // the task maybe already registred:
      // 1. location cache stale: leader on local actually
      // 2. L--(regier)-->F-->L--(here)-->F
      if (OB_FAIL(unregister_commit_retry_task_(tx))) {
        TRANS_LOG(ERROR, "deregister timeout task fail", K(tx));
      }
    }
    if (OB_SUCC(ret)) {
      int64_t max_delay = INT64_MAX;
      if (OB_SWITCHING_TO_FOLLOWER_GRACEFULLY == result) {
        max_delay = 300 * 1000;
      }

      if (OB_FAIL(register_commit_retry_task_(tx, max_delay))) {
        commit_fin = true;
        tx.state_ = ObTxDesc::State::ROLLED_BACK;
        commit_out = OB_TRANS_ROLLBACKED;
      }
    }
    break;
  case OB_TRANS_COMMITED:
  case OB_SUCCESS:
    tx.state_ = ObTxDesc::State::COMMITTED;
    tx.commit_version_ = commit_version;
    commit_out = OB_SUCCESS;
    break;
  case OB_TRANS_KILLED:
  case OB_TRANS_ROLLBACKED:
    tx.state_ = ObTxDesc::State::ROLLED_BACK;
    commit_out = result;
    break;
  case OB_TRANS_TIMEOUT:
    TX_STAT_TIMEOUT_INC
  case OB_TRANS_STMT_TIMEOUT:
    tx.state_ = ObTxDesc::State::COMMIT_TIMEOUT;
    commit_out = result;
    break;
  case OB_TRANS_UNKNOWN:
    tx.state_ = ObTxDesc::State::COMMIT_UNKNOWN;
    commit_out = result;
    break;
  default:
    commit_fin = false;
    TRANS_LOG(WARN, "recv unrecongized commit result, just ignore", K(result), K(tx));
    break;
  }
  // commit finished, cleanup
  if (commit_fin) {
    if (tx.finish_ts_ <= 0) { // maybe aborted early
      tx.finish_ts_ = ObClockGenerator::getClock();
    }
    tx.commit_out_ = commit_out;
    if (tx.commit_task_.is_registered()) {
      if (OB_FAIL(unregister_commit_retry_task_(tx))) {
        TRANS_LOG(ERROR, "deregister timeout task fail", K(tx));
      }
    }
    tx_post_terminate_(tx);
  }
#ifndef NDEBUG
  TRANS_LOG(INFO, "handle tx commit result", K(ret), K(tx), K(commit_fin), K(result));
#else
  if (OB_FAIL(ret) || (OB_SUCCESS != result) || (ObClockGenerator::getClock() - tx.commit_ts_) > 5 * 1000 * 1000) {
    TRANS_LOG(INFO, "handle tx commit result", K(ret), K(tx), K(commit_fin), K(result));
  }
#endif
  ObTransTraceLog &tlog = tx.get_tlog();
  REC_TRANS_TRACE_EXT(&tlog, handle_tx_commit_result, Y(ret),
                      OB_ID(arg), result,
                      OB_ID(is_finish), commit_fin,
                      OB_ID(result), commit_out,
                      OB_ID(commit_version), commit_version,
                      OB_ID(state), tx.state_);
  return ret;
}

int ObTransService::abort_tx_(ObTxDesc &tx, const int cause, const bool cleanup)
{
  int ret = OB_SUCCESS;
  if (tx.state_ >= ObTxDesc::State::IN_TERMINATE) {
    ret = OB_TRANS_HAS_DECIDED;
    TRANS_LOG(WARN, "try abort tx which has decided",
              K(ret), K(tx), K(cause));
  } else {
    tx.state_ = ObTxDesc::State::IN_TERMINATE;
    tx.abort_cause_ = cause;
    abort_participants_(tx);
    tx.state_ = ObTxDesc::State::ABORTED;
    if (!cleanup) {
      invalid_registered_snapshot_(tx);
    } else {
      tx_post_terminate_(tx);
    }
  }
  TRANS_LOG(INFO, "abort tx", K(ret), K(*this), K(tx), K(cause));
  return ret;
}

void ObTransService::invalid_registered_snapshot_(ObTxDesc &tx)
{
  int ret = OB_SUCCESS;
  ARRAY_FOREACH(tx.savepoints_, i) {
    auto &it = tx.savepoints_[i];
    if (it.is_snapshot()) {
      it.rollback();
    }
  }
}

void ObTransService::registered_snapshot_clear_part_(ObTxDesc &tx)
{
  int ret = OB_SUCCESS;
  ARRAY_FOREACH(tx.savepoints_, i) {
    auto &p = tx.savepoints_[i];
    if (p.is_snapshot() && p.snapshot_->valid_) {
      p.snapshot_->parts_.reset();
    }
  }
}

/*
 * decide tx commit coordinator and participants
 *
 * choice local participant as coordinator preferentially
 */
int ObTransService::decide_tx_commit_info_(ObTxDesc &tx, ObTxPart *&coord)
{
  int ret = OB_SUCCESS;
  ObTxPartList &parts = tx.parts_;
  coord = NULL;
  tx.coord_id_.reset();
  tx.commit_parts_.reset();
  ARRAY_FOREACH(parts, i) {
    if (OB_FAIL(tx.commit_parts_.push_back(parts[i].id_))) {
      TRANS_LOG(WARN, "part id push fail", K(ret), K(tx));
    } else if (!tx.coord_id_.is_valid() && parts[i].addr_ == self_) {
      tx.coord_id_ = parts[i].id_;
      coord = &parts[i];
    } else if (OB_ISNULL(coord)) {
      coord = &parts[i];
    }
  }
  if (OB_SUCC(ret) && !tx.coord_id_.is_valid() && OB_NOT_NULL(coord)) {
    tx.coord_id_ = coord->id_;
  }

  TRANS_LOG(TRACE, "decide tx coord", K(ret), K_(tx.coord_id), K(*this), K(tx));
  return ret;
}

/*
 * get coordinator id for 2pc caller
 * it's need to remember coordinaotr in phase 2 of 2PC
 * it's required to remember coordinaotr in phase 2 of 2PC
 * case 1: xa trans gets its coord before xa prepare
 */
int ObTransService::prepare_tx_coord(ObTxDesc &tx, share::ObLSID &coord_id)
{
  // TODO: for xa
  int ret = OB_SUCCESS;
  tx.lock_.lock();
  ObTxPart *coord = NULL;
  if (OB_FAIL(decide_tx_commit_info_(tx, coord))) {
    TRANS_LOG(WARN, "fail to decide tx coordinator, tx will abort", K(ret), K(tx));
  } else if (NULL == coord) {
    // in this case, the trans may be a read-only trans.
    ret = OB_ERR_READ_ONLY_TRANSACTION;
    tx.state_ = ObTxDesc::State::COMMITTED;
    TRANS_LOG(INFO, "coord is null", K(ret), K(tx));
  } else {
    coord_id = coord->id_;
  }
  TRANS_LOG(INFO, "generate tx coord", K(ret), K(tx), K(coord_id));
  tx.lock_.unlock();
  return ret;
}

/*
 * phase one of 2pc, i.e., prepare phase
 * persist log and trans state to ensure recoverablity
 * case 1: xa prepare
 */
#define OB_TRANS_RDONLY 0
int ObTransService::prepare_tx(ObTxDesc &tx,
                               const int64_t timeout_us,
                               ObITxCallback &cb)
{
  int ret = OB_SUCCESS;
  int64_t now = ObClockGenerator::getClock();
  tx.lock_.lock();
  tx.set_commit_cb(&cb);
  tx.commit_expire_ts_ = now + timeout_us;
  tx.state_ = ObTxDesc::State::SUB_PREPARING;
  ObTxSubPrepareMsg prepare_msg;
  // TODO, retry mechanism
  if (OB_FAIL(tx.commit_task_.init(&tx, this))) {
    TRANS_LOG(WARN, "fail to init timeout task", K(ret), K(tx));
  } else if (OB_FAIL(register_commit_retry_task_(tx))) {
    TRANS_LOG(WARN, "fail to register retry commit task", K(ret), K(tx));
  } else if (OB_FAIL(build_tx_sub_prepare_msg_(tx, prepare_msg))) {
    TRANS_LOG(WARN, "fail to build tx sub-prepare msg", K(ret), K(tx));
  } else if (OB_FAIL(rpc_->post_msg(tx.coord_id_, prepare_msg))) {
    TRANS_LOG(WARN, "fail to post tx sub-prepare msg", K(ret), K(tx), K(prepare_msg));
    // send msg fail won't cause commit fail, later driven by retry timer
    ret = OB_SUCCESS;
  }
  TRANS_LOG(INFO, "prepare tx", K(ret), K(tx), KP(&cb));
  tx.lock_.unlock();
  return ret;
}

int ObTransService::build_tx_sub_prepare_msg_(const ObTxDesc &tx, ObTxSubPrepareMsg &msg)
{
  int ret = OB_SUCCESS;
  msg.cluster_version_ = tx.cluster_version_;
  msg.tenant_id_ = tx.tenant_id_;
  msg.tx_id_ = tx.tx_id_;
  msg.expire_ts_ = tx.commit_expire_ts_;
  msg.receiver_ = tx.coord_id_;
  msg.sender_addr_ = self_;
  msg.sender_ = share::SCHEDULER_LS;
  msg.cluster_id_ = tx.cluster_id_;
  msg.request_id_ = tx.op_sn_;
  msg.xid_ = tx.xid_;
  if (OB_FAIL(msg.parts_.assign(tx.commit_parts_))) {
    TRANS_LOG(WARN, "fail to assign parts", K(ret), K(tx));
  }
  return ret;
}

/*
 * phase two of 2pc
 * alloc trxDesc and register transMgr
 *  if exist ? get and use
 * case 1) coordinator is local, direct function call
 * case 2) send RPC
 * finially, deregister and release trxDesc
 */
int ObTransService::end_two_phase_tx(const ObTransID &tx_id,
                                     const ObXATransID &xid,
                                     const share::ObLSID &coord,
                                     const int64_t timeout_us,
                                     const bool is_rollback,
                                     ObITxCallback &cb)
{
  int ret = OB_SUCCESS;
  int64_t now = ObClockGenerator::getClock();
  // TODO, alloc tx desc from tx mgr
  ObTxDesc *tx = NULL;
  if (OB_FAIL(tx_desc_mgr_.alloc(tx))) {
    TRANS_LOG(WARN, "alloc tx fail", K(ret), KPC(this));
  } else if (OB_FAIL(tx_desc_mgr_.add_with_txid(tx_id, *tx))) {
    TRANS_LOG(WARN, "add tx to txMgr fail", K(ret), K(tx));
  } else {
    tx->commit_expire_ts_ = now + timeout_us;
    tx->coord_id_ = coord;
    tx->xid_ = xid;
    tx->set_commit_cb(&cb);
    if (OB_FAIL(tx->commit_task_.init(tx, this))) {
      TRANS_LOG(WARN, "fail to init timeout task", K(ret), K(*tx));
    } else if (OB_FAIL(register_commit_retry_task_(*tx))) {
      TRANS_LOG(WARN, "fail to register retry commit task", K(ret), K(*tx));
    } else {
      if (is_rollback) {
        // two phase rollback
        ObTxSubRollbackMsg msg;
        tx->state_ = ObTxDesc::State::SUB_ROLLBACKING;
        if (OB_FAIL(build_tx_sub_rollback_msg_(*tx, msg))) {
          TRANS_LOG(WARN, "fail to build tx sub-rollback msg", K(ret), K(*tx));
        } else if (OB_FAIL(rpc_->post_msg(tx->coord_id_, msg))) {
          TRANS_LOG(WARN, "fail to post tx sub-rollback msg", K(ret), K(*tx), K(msg));
          // send msg fail won't cause commit fail, later driven by retry timer
          ret = OB_SUCCESS;
        }
      } else {
        // two phase commit
        ObTxSubCommitMsg msg;
        tx->state_ = ObTxDesc::State::SUB_COMMITTING;
        if (OB_FAIL(build_tx_sub_commit_msg_(*tx, msg))) {
          TRANS_LOG(WARN, "fail to build tx sub-commit msg", K(ret), K(*tx));
        } else if (OB_FAIL(rpc_->post_msg(tx->coord_id_, msg))) {
          TRANS_LOG(WARN, "fail to post tx sub-commit msg", K(ret), K(*tx), K(msg));
          // send msg fail won't cause commit fail, later driven by retry timer
          ret = OB_SUCCESS;
        }
      }
    }
  }
  TRANS_LOG(INFO, "end two phase tx", K(tx_id), K(is_rollback), K(xid), KP(&cb));
  return ret;
}

int ObTransService::build_tx_sub_commit_msg_(const ObTxDesc &tx, ObTxSubCommitMsg &msg)
{
  int ret = OB_SUCCESS;
  msg.tenant_id_ = tenant_id_;
  msg.tx_id_ = tx.tx_id_;
  msg.receiver_ = tx.coord_id_;
  msg.sender_addr_ = self_;
  msg.sender_ = share::SCHEDULER_LS;
  msg.xid_ = tx.xid_;
  msg.cluster_version_ = GET_MIN_CLUSTER_VERSION();
  // invalid
  msg.cluster_id_ = GCONF.cluster_id;
  // TODO, a special request id
  msg.request_id_ = tx.op_sn_;
  return ret;
}

int ObTransService::build_tx_sub_rollback_msg_(const ObTxDesc &tx, ObTxSubRollbackMsg &msg)
{
  int ret = OB_SUCCESS;
  msg.tenant_id_ = tenant_id_;
  msg.tx_id_ = tx.tx_id_;
  msg.receiver_ = tx.coord_id_;
  msg.sender_addr_ = self_;
  msg.sender_ = share::SCHEDULER_LS;
  msg.xid_ = tx.xid_;
  msg.cluster_version_ = GET_MIN_CLUSTER_VERSION();
  // invalid
  msg.cluster_id_ = GCONF.cluster_id;
  // TODO, a special request id
  msg.request_id_ = tx.op_sn_;
  return ret;
}

int ObTransService::interrupt(ObTxDesc &tx, int cause)
{
  int ret = OB_SUCCESS;
  TRANS_LOG(INFO, "start interrupt tx", KPC(this), K(tx.tx_id_), K(cause));
  bool busy_wait = false;
  {
    ObSpinLockGuard guard(tx.lock_);
    if (tx.flags_.BLOCK_) {
      tx.flags_.INTERRUPTED_ = true;
      TRANS_LOG(INFO, "will busy wait tx quit from block state", K(tx));
      busy_wait = true;
    }
  }
  while (busy_wait) {
    if (tx.flags_.BLOCK_) {
      ob_usleep(500);
    } else {
      ObSpinLockGuard guard(tx.lock_);
      tx.flags_.INTERRUPTED_ = false;
      break;
    }
  }
  TRANS_LOG(INFO, "interrupt tx done", KR(ret), KPC(this), K(cause));
  return ret;
}

/*
 * participant keepalive
 * this has two effects:
 * 1) GC participant: if tx terminated, participant will abort it self
 * 2) fast abort transaction: if participant report itself failure,
 *    whole transaction will terminated from top to bottom
 */

int ObTransService::handle_trans_keepalive(const ObTxKeepaliveMsg &msg, ObTransRpcResult &result)
{
  int ret = OB_SUCCESS;
  const ObTransID &tx_id = msg.tx_id_;
  ObTxDesc *tx = NULL;
  if (OB_FAIL(tx_desc_mgr_.get(tx_id, tx)) &&
      OB_ENTRY_NOT_EXIST != ret) {
    TRANS_LOG(WARN, "get tx fail", K(ret), K(tx_id), K(msg));
  } else if (OB_ISNULL(tx)) {
    ret = OB_TRANS_CTX_NOT_EXIST;
  } else if (OB_SUCCESS != msg.status_) {
    TRANS_LOG(WARN, "tx participant in failed, abort tx", KPC(tx), K(msg));
    if (OB_FAIL(abort_tx(*tx, msg.status_))) {
      TRANS_LOG(WARN, "do abort tx fail", K(ret), KPC(tx));
    }
  }
  ObTxKeepaliveRespMsg resp;
  resp.cluster_version_ = GET_MIN_CLUSTER_VERSION();
  resp.tenant_id_ = tenant_id_;
  resp.cluster_id_ = GCONF.cluster_id;
  resp.request_id_ = ObClockGenerator::getClock();
  resp.tx_id_ = tx_id;
  resp.sender_addr_ = self_;
  resp.sender_ = share::SCHEDULER_LS;
  resp.receiver_ = msg.sender_;
  resp.status_ = ret;
  if (OB_FAIL(rpc_->post_msg(resp.receiver_, resp))) {
    TRANS_LOG(WARN, "post tx keepalive resp fail", K(ret), K(resp), KPC(this));
  }
  result.reset();
  result.init(ret, resp.get_timestamp());
  if (OB_NOT_NULL(tx)) {
    tx_desc_mgr_.revert(*tx);
  }
  if (REACH_TIME_INTERVAL(10 * 1000 * 1000)) {
    TRANS_LOG(INFO, "handle trans keepalive", K(ret), K(msg));
  }
  return ret;
}

int ObTransService::handle_trans_keepalive_response(const ObTxKeepaliveRespMsg &msg, obrpc::ObTransRpcResult &result)
{
  int ret = OB_SUCCESS;
  ObPartTransCtx *ctx = NULL;
  const ObTransID &tx_id = msg.tx_id_;
  const share::ObLSID &ls_id = msg.receiver_;
  if (OB_FAIL(get_tx_ctx_(ls_id, tx_id, ctx))) {
    TRANS_LOG(WARN, "get tx ctx fail", K(tx_id), K(ls_id));
  } else {
    (void)ctx->tx_keepalive_response_(msg.status_);
  }
  if (OB_NOT_NULL(ctx)) {
    revert_tx_ctx_(ctx);
  }
  result.reset();
  result.init(ret, msg.get_timestamp());
  return ret;
}

int ObTransService::find_parts_after_sp_(ObTxDesc &tx,
                                         ObTxPartRefList &parts,
                                         const int64_t scn)
{
  int ret = OB_SUCCESS;
  ARRAY_FOREACH(tx.parts_, i) {
    if (tx.parts_.at(i).last_scn_ > scn &&
        !tx.parts_.at(i).is_clean()) {
      if (OB_FAIL(parts.push_back(tx.parts_.at(i)))) {
        TRANS_LOG(WARN, "push back participant failed", K(ret));
      }
    }
  }
  return ret;
}

int ObTransService::get_read_store_ctx(const ObTxReadSnapshot &snapshot,
                                       const bool read_latest,
                                       const int64_t lock_timeout,
                                       ObStoreCtx &store_ctx)
{
  int ret = OB_SUCCESS;
  auto ls_id = store_ctx.ls_id_;
  if (!ls_id.is_valid() || !snapshot.valid_) {
    ret = OB_INVALID_ARGUMENT;
    TRANS_LOG(WARN, "invalid ls_id or invalid snapshot store_ctx", K(ret), K(snapshot), K(store_ctx), K(lbt()));
  } else if (snapshot.is_special()) {
    if (OB_FAIL(validate_snapshot_version_(snapshot.core_.version_,
                                           store_ctx.timeout_,
                                           *store_ctx.ls_))) {
      TRANS_LOG(WARN, "invalid speficied snapshot", K(ret), K(snapshot), K(store_ctx));
    }
  } else if (snapshot.is_ls_snapshot() && snapshot.snapshot_lsid_ != ls_id) {
      // try to access differ logstream with snapshot from another logstream
      // FIXME: return code should hint to indicate caller to make sens
      ret = OB_NOT_SUPPORTED;
      TRANS_LOG(WARN, "use a local snapshot to access other logstream",
                K(ret), K(store_ctx), K(snapshot));
  }

  bool check_readable_ok = false;
  auto snap_tx_id = snapshot.core_.tx_id_;
  ObPartTransCtx *tx_ctx = NULL;
  if (OB_SUCC(ret) && snap_tx_id.is_valid()) {
    // inner tx read, we verify txCtx's status
    bool exist = false;
    TX_PARTS_CONTAIN_(snapshot.parts_, left_, ls_id, exist);
    if (exist || read_latest) {
      if (OB_FAIL(get_tx_ctx_(ls_id, store_ctx.ls_, snap_tx_id, tx_ctx))) {
        if (OB_TRANS_CTX_NOT_EXIST == ret && !exist) {
          ret = OB_SUCCESS;
        } else {
          TRANS_LOG(WARN, "get tx ctx fail",
                    K(ret), K(store_ctx), K(snapshot), K(ls_id), K(exist), K(read_latest));
        }
      } else if (OB_FAIL(tx_ctx->check_status())) {
        TRANS_LOG(WARN, "check status fail", K(ret), K(store_ctx), KPC(tx_ctx));
      } else {
        check_readable_ok = true;
      }
      if (OB_FAIL(ret) && OB_NOT_NULL(tx_ctx)) {
        revert_tx_ctx_(store_ctx.ls_, tx_ctx);
        tx_ctx = NULL;
      }
    }
  }

  // need continue to check replica's readability
  if (OB_SUCC(ret) && !check_readable_ok &&
      OB_FAIL(check_replica_readable_(snapshot.core_.version_,
                                      snapshot.core_.elr_,
                                      snapshot.source_,
                                      ls_id,
                                      store_ctx.timeout_,
                                      *store_ctx.ls_))) {
    TRANS_LOG(WARN, "replica not readable", K(ret), K(snapshot), K(ls_id), K(store_ctx));
  }

  // setup tx_table_guard
  ObTxTableGuard tx_table_guard;
  if (OB_SUCC(ret) &&
      OB_FAIL(get_tx_table_guard_(store_ctx.ls_, ls_id, tx_table_guard))) {
    TRANS_LOG(WARN, "get tx_table_guard fail", K(ret), K(ls_id), K(store_ctx));
  }

  // fail, rollback
  if (OB_FAIL(ret)) {
    if (OB_NOT_NULL(tx_ctx)) {
      revert_tx_ctx_(store_ctx.ls_, tx_ctx);
      tx_ctx = NULL;
    }
  }

  // go well, commit
  if (OB_SUCC(ret)) {
    store_ctx.mvcc_acc_ctx_.init_read(
     tx_ctx,
     (tx_ctx ? tx_ctx->get_memtable_ctx() : NULL),
     tx_table_guard,
     snapshot.core_,
     store_ctx.timeout_,
     lock_timeout,
     snapshot.is_weak_read()
    );
    update_max_read_ts_(tenant_id_, ls_id, snapshot.core_.version_);
  }

  TRANS_LOG(TRACE, "get-read-store-ctx", K(ret), K(store_ctx), K(read_latest), K(snapshot));
  return ret;
}



int ObTransService::get_read_store_ctx(const int64_t snapshot_version,
                                       const int64_t lock_timeout,
                                       ObStoreCtx &store_ctx)
{
  int ret = OB_SUCCESS;
  if (snapshot_version <= 0) {
    ret = OB_INVALID_ARGUMENT;
    TRANS_LOG(WARN, "invalid speficied snapshot", K(ret), K(snapshot_version));
  } else {
    ObTxReadSnapshot snapshot;
    snapshot.valid_ = true;
    snapshot.core_.version_ = snapshot_version;
    snapshot.source_ = ObTxReadSnapshot::SRC::SPECIAL;
    ret = get_read_store_ctx(snapshot, false, lock_timeout, store_ctx);
  }
  TRANS_LOG(INFO, "get-read-store-ctx for specified snapshot", K(ret), K(snapshot_version), K(store_ctx));
  return ret;
}

int ObTransService::get_write_store_ctx(ObTxDesc &tx,
                                        const ObTxReadSnapshot &snapshot,
                                        storage::ObStoreCtx &store_ctx)
{
  int ret = OB_SUCCESS;
  const share::ObLSID &ls_id = store_ctx.ls_id_;
  ObPartTransCtx *tx_ctx = NULL;
  const int64_t data_scn = ObSequence::inc_and_get_max_seq_no();
  ObTxSnapshot snap = snapshot.core_;
  ObTxTableGuard tx_table_guard;
  bool access_started = false;
  if (tx.access_mode_ == ObTxAccessMode::RD_ONLY) {
    ret = OB_ERR_READ_ONLY_TRANSACTION;
    TRANS_LOG(WARN, "tx is readonly", K(ret), K(ls_id), K(tx), KPC(this));
  } else if (!snapshot.valid_) {
    ret = OB_INVALID_ARGUMENT;
    TRANS_LOG(WARN, "snapshot invalid", K(ret), K(snapshot));
  } else if (snapshot.is_none_read() && OB_FAIL(acquire_local_snapshot_(ls_id, snap.version_))) {
    TRANS_LOG(WARN, "acquire ls snapshot for mvcc write fail", K(ret), K(ls_id));
  } else if (snapshot.is_ls_snapshot() && snapshot.snapshot_lsid_ != ls_id) {
    ret = OB_NOT_SUPPORTED;
    TRANS_LOG(WARN, "use ls snapshot access another ls", K(ret), K(snapshot), K(ls_id));
  } else if (OB_FAIL(acquire_tx_ctx(ls_id, tx, tx_ctx, store_ctx.ls_))) {
    TRANS_LOG(WARN, "acquire tx ctx fail", K(ret), K(tx), K(ls_id), KPC(this));
  } else if (OB_FAIL(tx_ctx->start_access(tx, data_scn))) {
    TRANS_LOG(WARN, "tx ctx start access fail", K(ret), K(tx_ctx), K(ls_id), KPC(this));
  } else if (FALSE_IT(access_started = true)) {
  } else if (OB_FAIL(get_tx_table_guard_(store_ctx.ls_, ls_id, tx_table_guard))) {
    TRANS_LOG(WARN, "acquire tx table guard fail", K(ret), K(tx), K(ls_id), KPC(this));
  }
  // fail, rollback
  if (OB_FAIL(ret)) {
    if (OB_NOT_NULL(tx_ctx)) {
      if (access_started) { tx_ctx->end_access(); }
      revert_tx_ctx_(store_ctx.ls_, tx_ctx);
      tx_ctx = NULL;
    }
  }
  // succ, commit
  if (OB_SUCC(ret)) {
    store_ctx.mvcc_acc_ctx_.init_write(
      *tx_ctx,
      *tx_ctx->get_memtable_ctx(),
      tx.tx_id_,
      data_scn,
      tx,
      tx_table_guard,
      snap,
      store_ctx.timeout_,
      tx.lock_timeout_us_
    );
    if (tx.get_active_ts() <= 0) {
      tx.active_ts_ = ObClockGenerator::getClock();
    }
    /* NOTE: some write with adjoint reads:
     * eg. insert row to a table with primary key will _check_
     * rowkey-exist before do insert (this check is a read).
     *
     * so it's required to update `max_read_ts` for these write
     */
    update_max_read_ts_(tenant_id_, ls_id, snap.version_);
  }
  TRANS_LOG(TRACE, "get-write-store-ctx", K(ret),
            K(store_ctx), KPC(this), K(tx), K(snapshot), K(lbt()));
  return ret;
}

/*
 * the get here imply `get if exist` or `create if should`
 * create predication:
 *      the create must ensure current replica is leader
 *      at the time of create finish
 */
int ObTransService::acquire_tx_ctx(const share::ObLSID &ls_id, const ObTxDesc &tx, ObPartTransCtx *&ctx, ObLS *ls)
{
  int ret = OB_SUCCESS;
  bool exist = false;
  TX_PARTS_CONTAIN_(tx.parts_, id_, ls_id, exist);
  if (exist) {
    if (OB_FAIL(get_tx_ctx_(ls_id, ls, tx.tx_id_, ctx))) {
      TRANS_LOG(WARN, "get tx ctx fail", K(ret), K(ls_id), K(tx));
      if (ret == OB_TRANS_CTX_NOT_EXIST) {
        TRANS_LOG(WARN, "participant lost update", K(ls_id), K_(tx.tx_id));
      }
    }
  } else if (OB_FAIL(create_tx_ctx_(ls_id, ls, tx, ctx))) {
    TRANS_LOG(WARN, "create tx ctx fail", K(ret), K(ls_id), K(tx));
  }
  TRANS_LOG(TRACE, "acquire tx ctx", K(ret), K(*this), K(ls_id), K(tx), KP(ctx));
  return ret;
}

// plain create
int ObTransService::get_tx_ctx_(const share::ObLSID &ls_id,
                                ObLS *ls,
                                const ObTransID &tx_id,
                                ObPartTransCtx *&ctx)
{
  int ret = OB_SUCCESS;
  if (OB_NOT_NULL(ls)) {
    ret = ls->get_tx_ctx(tx_id, false, ctx);
  } else {
    ret = tx_ctx_mgr_.get_tx_ctx(ls_id, tx_id, false, ctx);
  }

  TRANS_LOG(TRACE, "get tx ctx", K(ret), K(tx_id), K(ls_id), KP(ctx), KP(ls));
  return ret;
}

int ObTransService::get_tx_ctx_(const share::ObLSID &ls_id,
                                const ObTransID &tx_id,
                                ObPartTransCtx *&ctx)
{ return get_tx_ctx_(ls_id, NULL, tx_id, ctx); }

int ObTransService::revert_tx_ctx_(ObLS* ls, ObPartTransCtx *ctx)
{
  int ret = OB_SUCCESS;
  if (OB_NOT_NULL(ls)) {
    ret = ls->revert_tx_ctx(ctx);
  } else {
    ret = tx_ctx_mgr_.revert_tx_ctx(ctx);
  }

  TRANS_LOG(TRACE, "revert tx ctx", KP(ctx));
  return ret;
}

int ObTransService::revert_tx_ctx_(ObPartTransCtx *ctx)
{ return revert_tx_ctx_(NULL, ctx); }

/*
 * create fresh tranaction ctx
 * 1) allocate
 * 2) initialize
 */
int ObTransService::create_tx_ctx_(const share::ObLSID &ls_id,
                                   ObLS *ls,
                                   const ObTxDesc &tx,
                                   ObPartTransCtx *&ctx)
{
  int ret = OB_SUCCESS;
  bool existed = false;
  int64_t epoch = 0;
  ObTxCreateArg arg(tx.can_elr_,  /* can_elr */
                    false,  /* for_replay */
                    tx.tenant_id_,
                    tx.tx_id_,
                    ls_id,
                    tx.cluster_id_,
                    tx.cluster_version_,
                    tx.sess_id_, /*session_id*/
                    tx.addr_,
                    tx.get_expire_ts(),
                    this);
  ret = OB_NOT_NULL(ls) ?
    ls->create_tx_ctx(arg, existed, ctx) :
    tx_ctx_mgr_.create_tx_ctx(arg, existed, ctx);
  if (OB_FAIL(ret)) {
    TRANS_LOG(WARN, "get tx ctx from mgr fail", K(ret), K(tx.tx_id_), K(ls_id), K(tx), K(arg));
    ctx = NULL;
  }
  TRANS_LOG(TRACE, "create tx ctx", K(ret), K(ls_id), K(tx));
  return ret;
}

int ObTransService::create_tx_ctx_(const share::ObLSID &ls_id,
                                   const ObTxDesc &tx,
                                   ObPartTransCtx *&ctx)
{ return create_tx_ctx_(ls_id, NULL, tx, ctx); }

void ObTransService::fetch_cflict_tx_ids_from_mem_ctx_to_desc_(ObMvccAccessCtx &acc_ctx)// for deadlock
{
  // merge all ctx(in every logstream)'s conflict trans ids to trans_desc
  int ret = OB_SUCCESS;
  common::ObArray<ObTransIDAndAddr> array;
  if (OB_ISNULL(acc_ctx.mem_ctx_)) {
    ret = OB_BAD_NULL_ERROR;
    DETECT_LOG(ERROR, "mem_ctx_ on acc_ctx is null", KR(ret), K(array));
  } else if (OB_FAIL(acc_ctx.mem_ctx_->get_conflict_trans_ids(array))) {
    DETECT_LOG(WARN, "get conflict ids from mem_ctx failed", KR(ret), K(acc_ctx));
  } else if (FALSE_IT(acc_ctx.mem_ctx_->reset_conflict_trans_ids())) {
  } else if (OB_FAIL(acc_ctx.tx_desc_->merge_conflict_txs(array))) {
    DETECT_LOG(WARN, "fail to merge ctx conflict trans array", KR(ret), K(acc_ctx));
  } else {
    DETECT_LOG(TRACE, "fetch conflict ids from mem_ctx to desc", KR(ret), K(array));
  }
}

int ObTransService::revert_store_ctx(storage::ObStoreCtx &store_ctx)
{
  int ret = OB_SUCCESS;
  auto &acc_ctx = store_ctx.mvcc_acc_ctx_;
  auto *tx_ctx = acc_ctx.tx_ctx_;
  if (acc_ctx.is_read()) {
    if (OB_NOT_NULL(tx_ctx)) {
      acc_ctx.tx_ctx_ = NULL;
      revert_tx_ctx_(store_ctx.ls_, tx_ctx);
    }
  } else if (acc_ctx.is_write()) {
    if (OB_ISNULL(tx_ctx)) {
      ret = OB_ERR_UNEXPECTED;
      TRANS_LOG(ERROR, "write access but tx ctx is NULL", K(ret), K(store_ctx));
    } else {
      /*
       * record transaction participant info
       */
      ObTxDesc *tx = acc_ctx.tx_desc_;
      acc_ctx.tx_ctx_ = NULL;
      if (tx->exec_info_reap_ts_ == 0) {
        tx->exec_info_reap_ts_ = ObSequence::get_max_seq_no();
      }
      ObTxPart p;
      p.id_         = tx_ctx->ls_id_;
      p.addr_       = self_;
      p.epoch_      = tx_ctx->epoch_;
      p.first_scn_  = tx_ctx->first_scn_;
      p.last_scn_   = tx_ctx->last_scn_;
      if (OB_FAIL(tx->update_part(p))) {
        TRANS_LOG(WARN, "append part fail", K(ret), K(p), KPC(tx_ctx));
      }
      (void) fetch_cflict_tx_ids_from_mem_ctx_to_desc_(acc_ctx);
      tx_ctx->end_access();
      revert_tx_ctx_(store_ctx.ls_, tx_ctx);
    }
  } else {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(ERROR, "unexpected store ctx type", K(ret), K(store_ctx));
  }
  TRANS_LOG(TRACE, "revert store ctx", K(ret), K(*this), K(lbt()));
  return ret;
}

/*
 * used to validate specified snapshot version
 * precondition: version <= current gts value
 */
int ObTransService::validate_snapshot_version_(const int64_t snapshot,
                                               const int64_t expire_ts,
                                               ObLS &ls)
{
  int ret = OB_SUCCESS;
  int64_t ls_weak_read_ts = ls.get_ls_wrs_handler()->get_ls_weak_read_ts();
  if (snapshot <= tx_version_mgr_.get_max_commit_ts(false) ||
      snapshot <= tx_version_mgr_.get_max_read_ts() ||
      snapshot <= ls_weak_read_ts) {
  } else {
    int64_t gts = 0;
    const MonotonicTs stc_ahead = MonotonicTs::current_time() - MonotonicTs(GCONF._ob_get_gts_ahead_interval);
    MonotonicTs tmp_receive_gts_ts(0);
    do {
      ret = ts_mgr_->get_gts(tenant_id_, stc_ahead, NULL, gts, tmp_receive_gts_ts);
      if (ret == OB_EAGAIN) {
        if (expire_ts <= ObClockGenerator::getClock()) {
          ret = OB_TIMEOUT;
        } else {
          ob_usleep(100);
        }
      } else if (OB_FAIL(ret)) {
        TRANS_LOG(WARN, "get gts fail", KR(ret));
      } else if (gts <= 0) {
        ret = OB_ERR_UNEXPECTED;
        TRANS_LOG(WARN, "get gts fail", K(gts));
      } else if (snapshot > gts) {
        ret = OB_INVALID_QUERY_TIMESTAMP;
        TRANS_LOG(WARN, "validate snapshot version fail", K(snapshot), K(gts));
      } else {
        TRANS_LOG(DEBUG, "snapshot is valid", K(snapshot), K(gts));
      }
    } while (ret == OB_EAGAIN);
  }
  TRANS_LOG(TRACE, "validate snapshot version",
      K(ret), K(snapshot), K(expire_ts), K(ls_weak_read_ts));
  return ret;
}

/*
 * check ls's readable snapshot
 *
 * here introduce a concept named 'replica_readable_version'
 *
 * it was updated by:
 * 1. tx commit:
 *    on Leader: on pre_commit
 *    on Sync Replica: on pre_commit
 * 2. transaction log replay:
 *    on Follower replica
 * 3. and by read on Leader or Sync Replica
 *
 * with this concept, we can verify replica readable as
 * compare with replica_readable_snapshot:
 * 1. v = my_read_snapshot_version
 * 2. if v <= replica_readble_snapshot return OK, otherwise
 * 3. check is_leader or sync_replica of ls
 *    if so, update replica_readable_snapshot = v and return OK, otherwise
 * 4. return OB_REPLICA_NOT_READABLE
 */
int ObTransService::check_replica_readable_(const int64_t snapshot,
                                            const bool elr,
                                            const ObTxReadSnapshot::SRC src,
                                            const share::ObLSID &ls_id,
                                            const int64_t expire_ts,
                                            ObLS &ls)
{
  int ret = OB_SUCCESS;
  bool leader = false;
  int64_t epoch = 0;
  int64_t ls_weak_read_ts = ls.get_ls_wrs_handler()->get_ls_weak_read_ts();
  bool readable = snapshot <= ls_weak_read_ts;
  if (!readable) {
    if (OB_FAIL(ls.get_tx_svr()->get_tx_ls_log_adapter()->get_role(leader, epoch))) {
      TRANS_LOG(WARN, "get replica status fail", K(ls_id));
    } else if (leader || is_sync_replica_(ls_id)) {
      ret = OB_SUCCESS;
    } else if (ObTxReadSnapshot::SRC::SPECIAL == src ||
               ObTxReadSnapshot::SRC::WEAK_READ_SERVICE == src) {
      // to compatible with SQL's retry-logic, trigger re-choose replica
      ret = OB_REPLICA_NOT_READABLE;
    } else {
      if (OB_SUCC(wait_follower_readable_(ls, expire_ts, snapshot))) {
        TRANS_LOG(INFO, "read from follower", K(snapshot),  K(snapshot), K(ls));
      } else {
        ret = OB_NOT_MASTER;
      }
    }
  }
  TRANS_LOG(TRACE, "check replica readable", K(ret), K(snapshot), K(ls_id));
  return ret;
}

int ObTransService::wait_follower_readable_(ObLS &ls,
                                            const int64_t expire_ts,
                                            const int64_t snapshot)
{
  int ret = OB_REPLICA_NOT_READABLE;
  int64_t compare_timeout = 0;
  const uint64_t tenant_id = MTL_ID();
  omt::ObTenantConfigGuard tenant_config(TENANT_CONF(tenant_id));
  if (tenant_config.is_valid()) {
    compare_timeout = tenant_config->_follower_snapshot_read_retry_duration;
  }
  if (compare_timeout > 0) {
    int64_t compare_expired_time = ObClockGenerator::getClock() + compare_timeout;
    int64_t stmt_timeout = expire_ts - ObClockGenerator::getClock();
    int64_t retry_interval = 0;
    do {
      if (OB_UNLIKELY(ObClockGenerator::getClock() >= expire_ts)) {
        ret = OB_TIMEOUT;
      } else if (snapshot <= ls.get_ls_wrs_handler()->get_ls_weak_read_ts()) {
        TRANS_LOG(WARN, "read from follower", K(snapshot), K(ls.get_ls_id()), K(tenant_id));
        ret = OB_SUCCESS;
      } else if (ObClockGenerator::getClock() >= compare_expired_time) {
        break;
      } else if (OB_REPLICA_NOT_READABLE == ret) {
        stmt_timeout = expire_ts - ObClockGenerator::getClock();
        compare_timeout = compare_expired_time - ObClockGenerator::getClock();
        retry_interval = MIN(MIN3(GCONF.weak_read_version_refresh_interval, compare_timeout, stmt_timeout), 100000);
        ob_usleep(static_cast<int>(retry_interval));
      } else {
        // do nothing
      }
    } while (OB_REPLICA_NOT_READABLE == ret);
  }
  return ret;
}

/*
 * collect trans exec result
 */
int ObTransService::collect_tx_exec_result(ObTxDesc &tx,
                                           ObTxExecResult &result)
{
  int ret = OB_SUCCESS;
  ret = get_tx_exec_result(tx, result);
  TRANS_LOG(TRACE, "collect tx exec result", K(ret), K(tx), K(result), K(lbt()));
  return ret;
}

int ObTransService::build_tx_commit_msg_(const ObTxDesc &tx, ObTxCommitMsg &msg)
{
  int ret = OB_SUCCESS;
  msg.cluster_version_ = tx.cluster_version_;
  msg.tenant_id_ = tx.tenant_id_;
  msg.tx_id_ = tx.tx_id_;
  msg.expire_ts_ = tx.commit_expire_ts_;
  msg.receiver_ = tx.coord_id_;
  msg.sender_addr_ = self_;
  msg.sender_ = share::SCHEDULER_LS;
  msg.cluster_id_ = tx.cluster_id_;
  msg.request_id_ = tx.op_sn_;
  if (OB_FAIL(msg.parts_.assign(tx.commit_parts_))) {
    TRANS_LOG(WARN, "assign parts fail", K(ret), K(tx));
  }
  return ret;
}

int ObTransService::abort_participants_(const ObTxDesc &tx_desc)
{
  int ret = OB_SUCCESS;
  const ObTxPartList &parts = tx_desc.parts_;
  // ignore ret
  ARRAY_FOREACH_NORET(parts, idx) {
    const ObTxPart &p = parts.at(idx);
    if (OB_FAIL(post_tx_abort_part_msg_(tx_desc, p))) {
      TRANS_LOG(WARN, "post tx abort part msg", K(ret), K(tx_desc), K(p));
    }
  }
  return ret;
}

int ObTransService::acquire_local_snapshot_(const share::ObLSID &ls_id,
                                            int64_t &snapshot)
{
  int ret = OB_SUCCESS;
  int64_t epoch = 0;
  bool leader = false;
  int64_t snapshot0 = 0;
  ObLSTxCtxMgr *ls_tx_ctx_mgr = NULL;
  if (OB_FAIL(tx_ctx_mgr_.get_ls_tx_ctx_mgr(ls_id, ls_tx_ctx_mgr))) {
    TRANS_LOG(WARN, "get ls_tx_ctx_mgr fail", K(ret), K(ls_id));
  } else if (!ls_tx_ctx_mgr->in_leader_serving_state()) {
    ret = OB_NOT_MASTER;
    // XXX In standby cluster mode, the failure to call acquire_local_snapshot_ is an
    // normal situation, no error log needs to be printed
    // TRANS_LOG(WARN, "check ls tx service leader serving state fail", K(ret), K(ls_id), K(ret));
  } else if (OB_FAIL(ls_tx_ctx_mgr->get_ls_log_adapter()->get_role(leader, epoch))) {
    TRANS_LOG(WARN, "get replica role fail", K(ret), K(ls_id));
  } else if (!leader) {
    ret = OB_NOT_MASTER;
  } else if (0 >= (snapshot0 = tx_version_mgr_.get_max_commit_ts(true))) {
    ret = OB_EAGAIN;
  } else {
    snapshot = snapshot0;
  }
  if (OB_NOT_NULL(ls_tx_ctx_mgr)) {
    tx_ctx_mgr_.revert_ls_tx_ctx_mgr(ls_tx_ctx_mgr);
  }
  TRANS_LOG(TRACE, "acquire local snapshot", K(ret), K(ls_id), K(snapshot));
  return ret;
}

int ObTransService::sync_acquire_global_snapshot_(ObTxDesc &tx,
                                                  const int64_t expire_ts,
                                                  int64_t &snapshot,
                                                  int64_t &uncertain_bound)
{
  int ret = OB_SUCCESS;
  auto op_sn = tx.op_sn_;
  tx.flags_.BLOCK_ = true;
  tx.lock_.unlock();
  ret = acquire_global_snapshot__(expire_ts,
                                  GCONF._ob_get_gts_ahead_interval,
                                  snapshot,
                                  uncertain_bound,
                                  [&]() -> bool { return tx.flags_.INTERRUPTED_; });
  tx.lock_.lock();
  bool interrupted = tx.flags_.INTERRUPTED_;
  tx.flags_.BLOCK_ = false;
  if (OB_SUCC(ret)) {
    if (op_sn != tx.op_sn_) {
      if (interrupted) {
        ret = OB_ERR_INTERRUPTED;
        TRANS_LOG(WARN, "txn has been interrupted", KR(ret), K(tx));
      } else {
        ret = OB_ERR_UNEXPECTED;
        TRANS_LOG(WARN, "txn has been disturbed", KR(ret), K(tx));
      }
    }
  }
  return ret;
}

int ObTransService::acquire_global_snapshot__(const int64_t expire_ts,
                                              const int64_t gts_ahead,
                                              int64_t &snapshot,
                                              int64_t &uncertain_bound,
                                              ObFunction<bool()> interrupt_checker)
{
  int ret = OB_SUCCESS;
  const MonotonicTs now0 = MonotonicTs::current_time();
  const MonotonicTs now = now0 - MonotonicTs(gts_ahead);
  do {
    int64_t n = ObClockGenerator::getClock();
    MonotonicTs rts(0);
    if (n >= expire_ts) {
      ret = OB_TIMEOUT;
    } else if (OB_FAIL(ts_mgr_->get_gts(tenant_id_, now, NULL, snapshot, rts))) {
      if (OB_EAGAIN == ret) {
        if (interrupt_checker()) {
          ret = OB_ERR_INTERRUPTED;
        } else {
          ob_usleep(500);
        }
      } else {
        TRANS_LOG(WARN, "get gts fail", K(now));
      }
    } else if (OB_UNLIKELY(snapshot <= 0)) {
      ret = OB_ERR_UNEXPECTED;
      TRANS_LOG(WARN, "invalid snapshot from gts", K(snapshot), K(now));
    } else {
      uncertain_bound = rts.mts_ + gts_ahead;
    }
  } while (OB_EAGAIN == ret);

  if (OB_FAIL(ret)) {
    TRANS_LOG(WARN, "acquire global snapshot fail", K(ret),
              K(gts_ahead), K(expire_ts), K(now), K(now0),
              K(snapshot), K(uncertain_bound));
  }
  return ret;
}


/********************************************************************
 *
 * RPC and Message Handle
 *
 ********************************************************************/

int ObTransService::batch_post_tx_msg_(ObTxRollbackSPMsg &msg,
                                       const ObIArray<ObTxLSEpochPair> &list)
{
  int ret = OB_SUCCESS;
  int last_ret = OB_SUCCESS;
  const ObTxDesc *tx_ptr = msg.tx_ptr_;
  ARRAY_FOREACH_NORET(list, idx) {
    auto &p = list.at(idx);
    msg.receiver_ = p.left_;
    msg.epoch_ = p.right_;
    if (msg.epoch_ > 0) {
      msg.tx_ptr_ = NULL;
    }
    if (OB_FAIL(rpc_->post_msg(p.left_, msg))) {
      TRANS_LOG(WARN, "post msg falied", K(ret), K(msg), K(p));
      last_ret = ret;
    }
    msg.tx_ptr_ = tx_ptr;
  }
  return last_ret;
}

int ObTransService::post_tx_abort_part_msg_(const ObTxDesc &tx_desc,
                                            const ObTxPart &p)
{
  int ret = OB_SUCCESS;
  ObTxAbortMsg msg;
  msg.cluster_version_ = tx_desc.cluster_version_;
  msg.tenant_id_ = tx_desc.tenant_id_;
  msg.tx_id_ = tx_desc.tx_id_;
  msg.receiver_ = p.id_;
  msg.sender_addr_ = self_;
  msg.sender_ = share::SCHEDULER_LS;
  msg.cluster_id_ = tx_desc.cluster_id_;
  msg.request_id_ = tx_desc.op_sn_;
  msg.reason_ = tx_desc.abort_cause_;
  bool local_opt = false;
  if (p.addr_ == self_) {
    ObTransRpcResult r;
    if (OB_SUCC(handle_trans_abort_request(msg, r))) {
      local_opt = true;
    }
  }
  if (!local_opt) {
    ret = rpc_->post_msg(p.id_, msg);
  }
  return ret;
}



bool ObTransService::is_sync_replica_(const share::ObLSID &ls_id)
{
  UNUSED(ls_id);
  // FIXME:
  /*
   * 1. ls.props.is_for_dup_table = true
   * 2. replica's in lease
   */
  return false;
}

int ObTransService::handle_trans_commit_response(ObTxCommitRespMsg &resp, ObTransRpcResult &result)
{
  int ret = OB_SUCCESS;
  ret = handle_tx_commit_result(resp.tx_id_, resp.ret_, resp.commit_version_);
  result.reset();
  result.init(ret, resp.get_timestamp());
#ifndef NDEBUG
  TRANS_LOG(INFO, "handle trans commit response", K(ret), K(resp));
#else
  if (OB_FAIL(ret)) {
    TRANS_LOG(WARN, "handle trans commit response fail", K(ret), K(resp));
  }
#endif
  return ret;
}

/*
 * handle tx commit request
 * 1. get txCtx and call its commit
 * 2. if txCtx not exist, get txState from txTable
 * 3. if both of txTable and txCtx not exist, replay with TRANS_UNKNOWN
 */
int ObTransService::handle_trans_commit_request(ObTxCommitMsg &msg,
                                                ObTransRpcResult &result)
{
  int ret = OB_SUCCESS;
  int64_t commit_version = -1;
  if (OB_FAIL(local_ls_commit_tx_(msg.tx_id_,
                                  msg.receiver_,
                                  msg.parts_,
                                  msg.expire_ts_,
                                  msg.app_trace_info_,
                                  msg.request_id_,
                                  commit_version))) {
    TRANS_LOG(WARN, "handle tx commit request fail", K(ret), K(msg));
  }
  result.reset();
  result.init(ret, msg.get_timestamp());
  result.private_data_ = commit_version;
#ifndef NDEBUG
  TRANS_LOG(INFO, "handle trans commit request", K(ret), K(msg));
#else
  if (OB_FAIL(ret)) {
    TRANS_LOG(WARN, "handle trans commit request failed", K(ret), K(msg));
  }
#endif
  return ret;
}

int ObTransService::local_ls_commit_tx_(const ObTransID &tx_id,
                                        const share::ObLSID &coord,
                                        const share::ObLSArray &parts,
                                        const int64_t &expire_ts,
                                        const common::ObString &app_trace_info,
                                        const int64_t &request_id,
                                        int64_t &commit_version)
{
  int ret = OB_SUCCESS;
  MonotonicTs commit_time = MonotonicTs::current_time();
  ObPartTransCtx *ctx = NULL;
  if (OB_FAIL(get_tx_ctx_(coord, tx_id, ctx))) {
    TRANS_LOG(WARN, "get coordinator tx context fail", K(ret), K(tx_id), K(coord));
    if (OB_TRANS_CTX_NOT_EXIST == ret) {
      int tx_state;
      if (OB_FAIL(get_tx_state_from_tx_table_(coord, tx_id, tx_state, commit_version))) {
        TRANS_LOG(WARN, "get tx state from tx table fail", K(ret), K(coord), K(tx_id));
        if (OB_TRANS_CTX_NOT_EXIST == ret) {
          ret = OB_TRANS_KILLED; // presume abort
        }
      } else {
        switch (tx_state) {
        case ObTxData::COMMIT:
          ret = OB_TRANS_COMMITED;
          break;
        case ObTxData::ABORT:
          ret = OB_TRANS_KILLED;
          break;
        case ObTxData::RUNNING:
        default:
          ret = OB_ERR_UNEXPECTED;
          TRANS_LOG(WARN, "tx in-progress but ctx miss", K(ret), K(tx_state), K(tx_id), K(coord));
        }
      }
    }
  } else {
    if (OB_FAIL(ctx->commit(parts, commit_time, expire_ts, app_trace_info, request_id))) {
      TRANS_LOG(WARN, "commit fail", K(ret), K(coord), K(tx_id));
    }
  }
  if (OB_NOT_NULL(ctx)) {
    revert_tx_ctx_(ctx);
  }
  return ret;
}

int ObTransService::get_tx_state_from_tx_table_(const share::ObLSID &lsid,
                                                const ObTransID &tx_id,
                                                int &state,
                                                int64_t &commit_version)
{
  int ret = OB_SUCCESS;
  ObTxTableGuard tx_table_guard;
  ObTxTable *tx_table = NULL;
  int64_t _state = 0;
  int64_t read_epoch = ObTxTable::INVALID_READ_EPOCH;
  if (OB_FAIL(get_tx_table_guard_(NULL, lsid, tx_table_guard))) {
    TRANS_LOG(WARN, "get tx table guard failed", KR(ret), K(lsid), KPC(this));
  } else if (!tx_table_guard.is_valid()) {
    TRANS_LOG(WARN, "tx table is null", KR(ret), K(lsid), KPC(this));
  } else if (FALSE_IT(tx_table = tx_table_guard.get_tx_table())) {
  } else if (FALSE_IT(read_epoch = tx_table_guard.epoch())) {
  } else if (OB_FAIL(tx_table->try_get_tx_state(tx_id, read_epoch, _state, commit_version))) {
    TRANS_LOG(WARN, "get tx state failed", KR(ret), K(lsid), K(tx_id), KPC(this));
  } else {
    state = (int)_state;
  }
  return ret;
}

int ObTransService::handle_trans_abort_request(ObTxAbortMsg &abort_req, ObTransRpcResult &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  ObPartTransCtx *ctx = NULL;
  if (OB_FAIL(get_tx_ctx_(abort_req.get_receiver(), abort_req.get_trans_id(), ctx))) {
    // We donot respond with the abort response, because we think the abort is
    // eventually always successful if we have never send the commit request
    TRANS_LOG(WARN, "get transaction context error", KR(ret), K(abort_req.get_trans_id()));
  } else {
    if (OB_FAIL(ctx->abort(abort_req.reason_))) {
      TRANS_LOG(WARN, "trans rollback error", KR(ret), K(abort_req));
    }
  }
  if (OB_NOT_NULL(ctx)) {
    revert_tx_ctx_(ctx);
  }
  TRANS_LOG(INFO, "handle trans abort request", K(ret), K(abort_req));
  return ret;
}

int ObTransService::handle_sp_rollback_request(ObTxRollbackSPMsg &msg,
                                               obrpc::ObTxRpcRollbackSPResult &result)
{
  int ret = OB_SUCCESS;
  int64_t ctx_born_epoch = -1;
  ret = ls_rollback_to_savepoint_(msg.tx_id_,
                                  msg.receiver_,
                                  msg.epoch_,
                                  msg.op_sn_,
                                  msg.savepoint_,
                                  ctx_born_epoch,
                                  msg.tx_ptr_);
  if (OB_NOT_NULL(msg.tx_ptr_)) {
    ob_free((void*)msg.tx_ptr_);
    msg.tx_ptr_ = NULL;
  }
  result.status_ = ret;
  result.addr_ = self_;
  result.born_epoch_ = ctx_born_epoch;
  result.send_timestamp_ = msg.get_timestamp();
#ifndef NDEBUG
  TRANS_LOG(INFO, "handle savepoint rollback request", K(ret), K(msg), K(result));
#else
  if (OB_FAIL(ret)) {
    TRANS_LOG(INFO, "handle savepoint rollback request fail", K(ret), K(msg), K(result));
  }
#endif
  return ret;
}

int ObTransService::check_ls_status_(const share::ObLSID &ls_id, bool &leader)
{
  int ret = OB_SUCCESS;
  ObLSService *ls_svr =  MTL(ObLSService *);
  common::ObRole role = common::ObRole::INVALID_ROLE;
  storage::ObLSHandle handle;
  ObLS *ls = nullptr;
  int64_t UNUSED = 0;

  if (OB_ISNULL(ls_svr)) {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(WARN, "log stream service is NULL", K(ret));
  } else if (OB_FAIL(ls_svr->get_ls(ls_id, handle, ObLSGetMod::TRANS_MOD))) {
    TRANS_LOG(WARN, "get id service log stream failed");
  } else if (OB_ISNULL(ls = handle.get_ls())) {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(WARN, "id service log stream not exist");
  } else if (OB_FAIL(ls->get_log_handler()->get_role(role, UNUSED))) {
    if (OB_NOT_RUNNING == ret) {
      ret = OB_LS_NOT_EXIST;
    } else {
      TRANS_LOG(WARN, "get ls role fail", K(ret));
    }
  } else if (common::ObRole::LEADER == role) {
    leader = true;
  } else {
    leader = false;
  }

  return ret;
}

// need_check_leader : just for unittest case
int ObTransService::handle_tx_batch_req(int msg_type,
                                        const char *buf,
                                        int32_t size,
                                        const bool need_check_leader)
{
  int ret = OB_SUCCESS;
  bool leader = false;
  int64_t UNUSED = 0;
#define CASE__(msg_type__, msg_class__, msg_handler__)                  \
    case msg_type__:                                                    \
  {                                                                     \
    int64_t pos = 0;                                                    \
    ObPartTransCtx *ctx = NULL;                                         \
    msg_class__ msg;                                                         \
    if (OB_FAIL(msg.deserialize(buf, size, pos))) {                     \
      TRANS_LOG(WARN, "deserialize msg failed", K(ret), K(msg_type), K(size)); \
    } else if (!msg.is_valid()) {                                       \
      ret = OB_INVALID_ARGUMENT;                                        \
      TRANS_LOG(ERROR, "msg is invalid", K(ret), K(msg_type), K(msg));  \
    } else if (OB_FAIL(get_tx_ctx_(msg.get_receiver(), msg.get_trans_id(), ctx))) { \
      TRANS_LOG(WARN, "get tx context fail", K(ret),  K(msg));          \
      if (OB_TRANS_CTX_NOT_EXIST == ret ||                              \
          OB_PARTITION_NOT_EXIST == ret) {                              \
        /* need_check_leader : just for unittest case*/                 \
        handle_orphan_2pc_msg_(msg, need_check_leader);                 \
      }                                                                 \
    } else if (OB_FAIL(ctx->get_ls_tx_ctx_mgr()                         \
                 ->get_ls_log_adapter()->get_role(leader, UNUSED))) {   \
      TRANS_LOG(WARN, "check ls leader status error", K(ret), K(msg));  \
    } else if (!leader) {                                               \
      ret = OB_NOT_MASTER;                                              \
      TRANS_LOG(WARN, "ls not master", K(ret), K(msg));                 \
    } else if (ctx->is_exiting()) {                                     \
      ret = OB_TRANS_CTX_NOT_EXIST;                                     \
      TRANS_LOG(INFO, "tx context is exiting",K(ret),K(msg));           \
      handle_orphan_2pc_msg_(msg, false);                               \
    } else if (OB_FAIL(ctx->msg_handler__(msg))) {                      \
        TRANS_LOG(WARN, "handle 2pc request fail", K(ret), K(msg));     \
    }                                                                   \
    if (OB_NOT_NULL(ctx)) {                                             \
      revert_tx_ctx_(ctx);                                              \
    }                                                                   \
    break;                                                              \
  }

  switch (msg_type) {
    CASE__(TX_2PC_PREPARE_REDO_REQ, Ob2pcPrepareRedoReqMsg, handle_tx_2pc_prepare_redo_req)
    CASE__(TX_2PC_PREPARE_REDO_RESP, Ob2pcPrepareRedoRespMsg, handle_tx_2pc_prepare_redo_resp)
    CASE__(TX_2PC_PREPARE_VERSION_REQ, Ob2pcPrepareVersionReqMsg, handle_tx_2pc_prepare_version_req)
    CASE__(TX_2PC_PREPARE_VERSION_RESP, Ob2pcPrepareVersionRespMsg, handle_tx_2pc_prepare_version_resp)
    CASE__(TX_2PC_PREPARE_REQ, Ob2pcPrepareReqMsg, handle_tx_2pc_prepare_req)
    CASE__(TX_2PC_PREPARE_RESP, Ob2pcPrepareRespMsg, handle_tx_2pc_prepare_resp)
    CASE__(TX_2PC_PRE_COMMIT_REQ, Ob2pcPreCommitReqMsg, handle_tx_2pc_pre_commit_req)
    CASE__(TX_2PC_PRE_COMMIT_RESP, Ob2pcPreCommitRespMsg, handle_tx_2pc_pre_commit_resp)
    CASE__(TX_2PC_COMMIT_REQ, Ob2pcCommitReqMsg, handle_tx_2pc_commit_req)
    CASE__(TX_2PC_COMMIT_RESP, Ob2pcCommitRespMsg, handle_tx_2pc_commit_resp)
    CASE__(TX_2PC_ABORT_REQ, Ob2pcAbortReqMsg, handle_tx_2pc_abort_req)
    CASE__(TX_2PC_ABORT_RESP, Ob2pcAbortRespMsg, handle_tx_2pc_abort_resp)
    CASE__(TX_2PC_CLEAR_REQ, Ob2pcClearReqMsg, handle_tx_2pc_clear_req)
    CASE__(TX_2PC_CLEAR_RESP, Ob2pcClearRespMsg, handle_tx_2pc_clear_resp)
    default: {
      ret = OB_NOT_SUPPORTED;
      TRANS_LOG(WARN, "unsupported msg type", K(ret), K(msg_type));
      break;
    }
  }
#undef CASE__
  return ret;
}
int ObTransService::handle_sp_rollback_resp(const share::ObLSID &ls_id,
                                            const int64_t epoch,
                                            const transaction::ObTransID &tx_id,
                                            const int status,
                                            const ObAddr &addr,
                                            const int64_t request_id,
                                            const obrpc::ObTxRpcRollbackSPResult &result)
{
  int ret = OB_SUCCESS;
  ObTxDesc *tx = NULL;
  if (OB_FAIL(tx_desc_mgr_.get(tx_id, tx))) {
    TRANS_LOG(WARN, "get trans_desc fail", K(ret), K(tx_id));
  } else {
    ObSpinLockGuard guard(tx->lock_);
    if (tx->state_ != ObTxDesc::State::ROLLBACK_SAVEPOINT) {
      TRANS_LOG(WARN, "receive stale rollback response message",
                K(addr), K(status), K(request_id), K(result), KPC(tx));
    } else if (tx->op_sn_ > request_id) {
      TRANS_LOG(WARN, "receive old rpc result msg",
                K(ret), K_(tx->op_sn), K(request_id), K(tx_id));
    } else if (status == OB_TRANS_RPC_TIMEOUT) {
      // ignore, waiter will drive retry
    } else if (status == OB_NEED_RETRY) {
      // ignore, waiter will retry periodically
      // FIXME: may cause rollback slow
    } else if (status == OB_NOT_MASTER) {
      // ignore, waiter will drive retry
    } else if (status == OB_LS_NOT_EXIST || status == OB_PARTITION_NOT_EXIST) {
      TRANS_LOG(WARN, "ls not exist on receiver", K(status), K(ls_id), K(tx_id), K(addr));
    } else if (status == OB_TENANT_NOT_EXIST) {
      TRANS_LOG(WARN, "tenant not exist on receiver", K(status), K(ls_id), K(tx_id), K(addr));
    } else if (status == OB_SUCCESS) {
      ObTxLSEpochPair pair(ls_id, epoch);
      if (tx->brpc_mask_set_.is_mask(pair)) {
        TRANS_LOG(DEBUG, "has marked received", K(pair));
      } else {
        if (epoch <= 0) {
          tx->update_clean_part(ls_id, result.born_epoch_, result.addr_);
        }
        (void)tx->brpc_mask_set_.mask(pair);
        //MEM_BARRIER();
        if (tx->brpc_mask_set_.is_all_mask()) {
          tx->rpc_cond_.notify(OB_SUCCESS);
        }
      }
    } else { // other failure
      // notify waiter, cause the savepoint rollback fail
      TRANS_LOG(WARN, "rollback_sp response an error", K(status),
                K(tx_id), K(tx->tx_id_), K(addr),
                K(request_id), K(ls_id), K(result));
      tx->rpc_cond_.notify(status);
    }
  }
  if (OB_NOT_NULL(tx)) {
    tx_desc_mgr_.revert(*tx);
  }
  return ret;
}

int ObTransService::handle_trans_msg_callback(const share::ObLSID &sender_ls_id,
                                              const share::ObLSID &receiver_ls_id,
                                              const ObTransID &tx_id,
                                              const int16_t msg_type,
                                              const int status,
                                              const ObAddr &receiver_addr,
                                              const int64_t request_id,
                                              const int64_t private_data)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_running_)) {
    ret = OB_NOT_RUNNING;
    TRANS_LOG(WARN, "ObTransService is not running", K(ret));
  } else if (!tx_id.is_valid()
             || !ObTxMsgTypeChecker::is_valid_msg_type(msg_type)
             || !receiver_addr.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    TRANS_LOG(WARN, "invalid argument", K(ret), K(tx_id),
              K(msg_type), K(status), K(receiver_addr), K(request_id));
  } else if (common::OB_TENANT_NOT_IN_SERVER == status
             || common::OB_TRANS_RPC_TIMEOUT == status) {
    // upper layer do retry
  } else if (TX_COMMIT == msg_type) {
    switch(status) {
    case OB_NOT_MASTER:
    case OB_SUCCESS: break;
    default:
      auto commit_version = private_data;
      if (OB_FAIL(handle_tx_commit_result(tx_id, status, commit_version))) {
        TRANS_LOG(WARN, "handle tx commit fail", K(ret), K(tx_id));
      }
    }
  } else if (SUBPREPARE == msg_type) {
    switch (status) {
    case OB_NOT_MASTER:
    case OB_SUCCESS: break;
    default:
      if (OB_FAIL(handle_sub_prepare_result(tx_id, status))) {
        TRANS_LOG(WARN, "handle tx commit fail", K(ret), K(tx_id));
      }
    }
  } else if (SUBCOMMIT == msg_type) {
    switch (status) {
    case OB_NOT_MASTER:
    case OB_SUCCESS: break;
    default:
      if (OB_FAIL(handle_sub_commit_result(tx_id, status))) {
        TRANS_LOG(WARN, "handle tx commit fail", K(ret), K(tx_id));
      }
    }
  } else if (SUBROLLBACK == msg_type) {
    switch (status) {
    case OB_NOT_MASTER:
    case OB_SUCCESS: break;
    default:
      if (OB_FAIL(handle_sub_rollback_result(tx_id, status))) {
        TRANS_LOG(WARN, "handle tx commit fail", K(ret), K(tx_id));
      }
    }
  }
#ifndef NDEBUG
  TRANS_LOG(INFO, "handle trans msg callback", K(ret),
            K(tx_id), K(sender_ls_id), K(receiver_ls_id),
            K(msg_type), K(status), K(receiver_addr), K(request_id));
#else
  if (OB_FAIL(ret) || OB_SUCCESS != status) {
    TRANS_LOG(WARN, "handle trans msg callback", K(ret),
              K(tx_id), K(sender_ls_id), K(receiver_ls_id),
              K(msg_type), K(status), K(receiver_addr), K(request_id));
  }
#endif
  return ret;
}

int ObTransService::update_max_read_ts_(const uint64_t tenant_id,
                                        const share::ObLSID &lsid,
                                        const int64_t ts)
{
  int ret = OB_SUCCESS;
  tx_version_mgr_.update_max_read_ts(ts);
  TRANS_LOG(TRACE, "update max read ts", K(ret), K(tenant_id), K(lsid), K(ts));
  return ret;
}

// need_check_leader : just for unittest case
void ObTransService::handle_orphan_2pc_msg_(const ObTxMsg &msg, const bool need_check_leader)
{
  int ret = OB_SUCCESS;
  bool leader = false;

  if (need_check_leader && OB_FAIL(check_ls_status_(msg.get_receiver(), leader))) {
    TRANS_LOG(WARN, "check ls status error", K(ret), K(msg));
  } else if (need_check_leader && !leader) {
    ret = OB_NOT_MASTER;
    TRANS_LOG(WARN, "receiver not master", K(ret), K(msg));
  } else if (OB_FAIL(ObPartTransCtx::handle_tx_orphan_2pc_msg(msg, get_server(), get_trans_rpc()))) {
    TRANS_LOG(WARN, "handle tx orphan 2pc msg failed", K(ret), K(msg));
  } else {
    // do nothing
  }
}

int ObTransService::refresh_location_cache(const share::ObLSID ls)
{
  int ret = OB_SUCCESS;

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    TRANS_LOG(WARN, "ObTransService not inited", K(ret));
  } else if (OB_UNLIKELY(!is_running_)) {
    ret = OB_NOT_RUNNING;
    TRANS_LOG(WARN, "ObTransService is not running", K(ret));
  } else if (!ls.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    TRANS_LOG(WARN, "invalid argument", K(ret), K(ls));
  } else if (OB_FAIL(location_adapter_->nonblock_renew(GCONF.cluster_id, tenant_id_, ls))) {
    TRANS_LOG(WARN, "refresh location cache error", KR(ret), K(ls));
  } else {
    if (EXECUTE_COUNT_PER_SEC(16)) {
      TRANS_LOG(INFO, "refresh location cache success", K(ls), K(lbt()));
    }
  }

  return ret;
}

int ObTransService::gen_trans_id_(ObTransID &trans_id)
{
  int ret = OB_SUCCESS;

  int retry_times = 0;
  if (!MTL_IS_PRIMARY_TENANT()) {
    ret = OB_STANDBY_READ_ONLY;
    TRANS_LOG(WARN, "standby tenant support read only", K(ret));
  } else {
    const int MAX_RETRY_TIMES = 50;
    int64_t tx_id = 0;
    do {
      if (OB_SUCC(gti_source_->get_trans_id(tx_id))) {
      } else if (OB_EAGAIN == ret) {
        if (retry_times++ > MAX_RETRY_TIMES) {
          ret = OB_GTI_NOT_READY;
          TRANS_LOG(WARN, "get trans id not ready", K(ret), K(retry_times), KPC(this));
        } else {
          ob_usleep(1000);
        }
      } else {
        TRANS_LOG(WARN, "get trans id fail", KR(ret));
      }
    } while (OB_EAGAIN == ret);
    if (OB_SUCC(ret)) {
      trans_id = ObTransID(tx_id);
    }
  }
  TRANS_LOG(TRACE, "gen trans id", K(ret), K(trans_id), K(retry_times));
  return ret;
}

bool ObTransService::commit_need_retry_(const int ret)
{
  return OB_NOT_MASTER == ret || OB_BLOCK_FROZEN == ret || OB_TX_NOLOGCB == ret || OB_EAGAIN == ret;
}

int ObTransService::get_min_uncommit_tx_prepare_version(const share::ObLSID& ls_id, int64_t &min_prepare_version)
{
  int ret = OB_SUCCESS;

  if (IS_NOT_INIT) {
    TRANS_LOG(WARN, "ObTransService not inited");
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(!is_running_)) {
    TRANS_LOG(WARN, "ObTransService is not running");
    ret = OB_NOT_RUNNING;
  } else if (!ls_id.is_valid()) {
    TRANS_LOG(WARN, "invalid argument", K(ls_id));
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(tx_ctx_mgr_.get_ls_min_uncommit_tx_prepare_version(ls_id, min_prepare_version))) {
    TRANS_LOG(WARN, "ObPartTransCtxMgr set memstore version error", KR(ret), K(ls_id));
  } else if (min_prepare_version <= 0) {
    TRANS_LOG(ERROR, "invalid min prepare version, unexpected error", K(ls_id), K(min_prepare_version));
    ret = OB_ERR_UNEXPECTED;
  } else {
    TRANS_LOG(DEBUG, "get min uncommit prepare version success", K(ls_id), K(min_prepare_version));
  }
  return ret;
}

int ObTransService::kill_all_tx(const share::ObLSID &ls_id, const KillTransArg &arg,
    bool &is_all_tx_cleaned_up)
{
  int ret = OB_SUCCESS;

  if (IS_NOT_INIT) {
    TRANS_LOG(WARN, "ObTransService not inited");
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(!is_running_)) {
    TRANS_LOG(WARN, "ObTransService is not running");
    ret = OB_NOT_RUNNING;
  } else if (!ls_id.is_valid()) {
    TRANS_LOG(WARN, "invalid argument", K(ls_id));
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(tx_ctx_mgr_.kill_all_tx(ls_id, arg.graceful_, is_all_tx_cleaned_up))) {
    TRANS_LOG(WARN, "kill all tx failed", KR(ret), K(ls_id), K(arg));
  } else {
    TRANS_LOG(INFO, "kill all tx success", K(ls_id), K(arg));
  }

  return ret;
}

int ObTransService::block_ls(const share::ObLSID &ls_id, bool &is_all_tx_cleaned_up)
{
  int ret = OB_SUCCESS;

  if (IS_NOT_INIT) {
    TRANS_LOG(WARN, "ObTransService not inited");
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(!is_running_)) {
    TRANS_LOG(WARN, "ObTransService is not running");
    ret = OB_NOT_RUNNING;
  } else if (!ls_id.is_valid()) {
    TRANS_LOG(WARN, "invalid argument", K(ls_id));
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(tx_ctx_mgr_.block_ls(ls_id, is_all_tx_cleaned_up))) {
    TRANS_LOG(WARN, "block ls error", KR(ret), K(ls_id));
  } else {
    TRANS_LOG(INFO, "block ls_id success", K(ls_id), K(is_all_tx_cleaned_up));
  }
  return ret;
}

int ObTransService::iterate_tx_ctx_mgr_stat(ObTxCtxMgrStatIterator &tx_ctx_mgr_stat_iter)
{
  int ret = OB_SUCCESS;

  if (IS_NOT_INIT) {
    TRANS_LOG(WARN, "ObTransService not inited");
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(!is_running_)) {
    TRANS_LOG(WARN, "ObTransService is not running");
    ret = OB_NOT_RUNNING;
  } else if (OB_FAIL(tx_ctx_mgr_.iterate_tx_ctx_mgr_stat(self_, tx_ctx_mgr_stat_iter))) {
    TRANS_LOG(WARN, "iterate_tx_ctx_mgr_stat error", KR(ret), K_(self));
  } else if (OB_FAIL(tx_ctx_mgr_stat_iter.set_ready())) {
    TRANS_LOG(WARN, "tx_ctx_mgr_stat_iter set ready error", KR(ret));
  } else {
    // do nothing
  }
  return ret;
}

int ObTransService::iterate_tx_lock_stat(const share::ObLSID& ls_id,
    ObTxLockStatIterator &tx_lock_stat_iter)
{
  int ret = OB_SUCCESS;

  if (IS_NOT_INIT) {
    TRANS_LOG(WARN, "ObTransService not inited");
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(!is_running_)) {
    TRANS_LOG(WARN, "ObTransService is not running");
    ret = OB_NOT_RUNNING;
  } else if (OB_FAIL(tx_ctx_mgr_.iterate_ls_tx_lock_stat(ls_id, tx_lock_stat_iter))) {
    TRANS_LOG(WARN, "iterate_tx_lock_stat error", KR(ret));
  } else if (OB_FAIL(tx_lock_stat_iter.set_ready())) {
    TRANS_LOG(WARN, "iterate_tx_lock_stat set ready error", KR(ret));
  } else {
    // do nothing
    TRANS_LOG(INFO, "iterate_tx_lock_stat set ready succ", KR(ret));
  }

  return ret;
}

int ObTransService::iterate_ls_id(ObLSIDIterator &ls_id_iter)
{
  int ret = OB_SUCCESS;

  if (IS_NOT_INIT) {
    TRANS_LOG(WARN, "ObTransService not inited");
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(!is_running_)) {
    TRANS_LOG(WARN, "ObTransService is not running");
    ret = OB_NOT_RUNNING;
  } else if (OB_FAIL(tx_ctx_mgr_.iterate_ls_id(ls_id_iter))) {
    TRANS_LOG(WARN, "iterate ls id error", KR(ret));
  } else if (OB_FAIL(ls_id_iter.set_ready())) {
    TRANS_LOG(WARN, "ls_id_iter set ready error", KR(ret));
  } else {
    // do nothing
  }

  return ret;
}

int ObTransService::iterate_all_observer_tx_stat(ObTxStatIterator &tx_stat_iter)
{
  int ret = OB_SUCCESS;
  const int64_t PRINT_SCHE_COUNT = 128;

  if (IS_NOT_INIT) {
    TRANS_LOG(WARN, "ObTransService not inited");
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(!is_running_)) {
    TRANS_LOG(WARN, "ObTransService is not running");
    ret = OB_NOT_RUNNING;
  } else if (OB_FAIL(tx_ctx_mgr_.iterate_all_observer_tx_stat(tx_stat_iter))) {
      TRANS_LOG(WARN, "iterate tx stat error", KR(ret));
  } else {
    // do nothing
  }
  if (REACH_TIME_INTERVAL(60 * 1000 * 1000)) {
    // TODO 4.0, dump scheduler trans ...
  }

  return ret;
}

int ObTransService::recover_tx(const ObTxInfo &tx_info, ObTxDesc *&tx)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(tx_desc_mgr_.alloc(tx))) {
    TRANS_LOG(WARN, "alloc tx desc fail", K(ret));
  } else if (OB_FAIL(tx->parts_.assign(tx_info.parts_))) {
    tx_desc_mgr_.revert(*tx);
    tx = NULL;
    TRANS_LOG(WARN, "assgin parts fail", K(ret), K(tx));
  } else if (OB_FAIL(tx_desc_mgr_.add_with_txid(tx_info.tx_id_, *tx))) {
    TRANS_LOG(WARN, "add tx to txMgr fail", K(ret), K(tx));
  } else {
    tx->flags_.REPLICA_ = true;
    tx->tenant_id_ = tx_info.tenant_id_;
    tx->cluster_id_ = tx_info.cluster_id_;
    tx->cluster_version_ = tx_info.cluster_version_;
    tx->addr_ = tx_info.addr_;
    tx->tx_id_ = tx_info.tx_id_;
    tx->isolation_ = tx_info.isolation_;
    tx->access_mode_ = tx_info.access_mode_;
    tx->snapshot_version_ = tx_info.snapshot_version_;
    tx->snapshot_uncertain_bound_ = tx_info.snapshot_uncertain_bound_;
    tx->op_sn_ = tx_info.op_sn_;
    tx->alloc_ts_ = tx_info.alloc_ts_;
    tx->active_ts_ = tx_info.active_ts_;
    tx->timeout_us_ = tx_info.timeout_us_;
    tx->expire_ts_ = tx_info.expire_ts_;
    tx->finish_ts_ = tx_info.finish_ts_;
    tx->active_scn_ = tx_info.active_scn_;
    tx->state_ = ObTxDesc::State::ACTIVE;
  }
  return ret;
}

int ObTransService::get_tx_info(ObTxDesc &tx, ObTxInfo &tx_info)
{
  int ret = OB_SUCCESS;
  tx.lock_.lock();
  if (OB_FAIL(tx_info.parts_.assign(tx.parts_))) {
    TRANS_LOG(WARN, "assgin parts fail", K(ret), K(tx));
  } else {
    tx_info.tenant_id_ = tx.tenant_id_;
    tx_info.cluster_id_ = tx.cluster_id_;
    tx_info.cluster_version_ = tx.cluster_version_;
    tx_info.addr_ = tx.addr_;
    tx_info.tx_id_ = tx.tx_id_;
    tx_info.isolation_ = tx.isolation_;
    tx_info.access_mode_ = tx.access_mode_;
    tx_info.snapshot_version_ = tx.snapshot_version_;
    tx_info.snapshot_uncertain_bound_ = tx.snapshot_uncertain_bound_;
    tx_info.op_sn_ = tx.op_sn_;
    tx_info.alloc_ts_ = tx.alloc_ts_;
    tx_info.active_ts_ = tx.active_ts_;
    tx_info.timeout_us_ = tx.timeout_us_;
    tx_info.expire_ts_ = tx.expire_ts_;
    tx_info.finish_ts_ = tx.finish_ts_;
    tx_info.active_scn_ = tx.active_scn_;
  }
  tx.lock_.unlock();
  return ret;
}

int ObTransService::get_tx_stmt_info(ObTxDesc &tx, ObTxStmtInfo &stmt_info)
{
  int ret = OB_SUCCESS;
  tx.lock_.lock();
  if (OB_FAIL(stmt_info.parts_.assign(tx.parts_))) {
    TRANS_LOG(WARN, "assgin parts fail", K(ret), K(tx));
  } else {
    stmt_info.tx_id_ = tx.tx_id_;
    stmt_info.op_sn_ = tx.op_sn_;
    stmt_info.state_ = tx.state_;
  }
  tx.lock_.unlock();
  return ret;
}

int ObTransService::update_tx_with_stmt_info(const ObTxStmtInfo &tx_info, ObTxDesc *&tx)
{
  int ret = OB_SUCCESS;
  tx->lock_.lock();
  tx->op_sn_ = tx_info.op_sn_;
  tx->state_ = tx_info.state_;
  tx->update_parts_(tx_info.parts_);
  tx->lock_.unlock();
  return ret;
}

int ObTransService::get_tx_table_guard_(ObLS *ls,
                                        const share::ObLSID &ls_id,
                                        ObTxTableGuard &guard)
{
  int ret = OB_SUCCESS;
  if (OB_NOT_NULL(ls)) {
    if (OB_FAIL(ls->get_tx_table_guard(guard))) {
      TRANS_LOG(WARN, "get ls tx_table_guard fail", K(ret), K(ls_id), KPC(ls), KPC(this));
    }
  } else {
    ObLSTxCtxMgr *ls_tx_ctx_mgr = NULL;
    if (OB_FAIL(tx_ctx_mgr_.get_ls_tx_ctx_mgr(ls_id, ls_tx_ctx_mgr))) {
      TRANS_LOG(WARN, "get ls tx_ctx_mgr fail", KR(ret), K(ls_id));
    } else if (OB_FAIL(ls_tx_ctx_mgr->get_tx_table_guard(guard))) {
      TRANS_LOG(WARN, "get ls tx_table_guard fail", KR(ret), K(ls_id), KP(ls_tx_ctx_mgr));
    }
    if (OB_NOT_NULL(ls_tx_ctx_mgr)) {
      tx_ctx_mgr_.revert_ls_tx_ctx_mgr(ls_tx_ctx_mgr);
    }
  }
  return ret;
}

int ObTransService::handle_timeout_for_xa(ObTxDesc &tx, const int64_t delay)
{
  int ret = OB_SUCCESS;
  int64_t now = ObClockGenerator::getClock();
  if (OB_FAIL(tx.lock_.lock(5000000))) {
    TRANS_LOG(WARN, "failed to acquire lock in specified time", K(tx));
    // FIXME: how to handle it without lock protection
    // according to handle_tx_commit_timeout
  } else {
    if (!tx.commit_task_.is_registered()){
      TRANS_LOG(INFO, "task canceled", K(tx));
    } else if (tx.flags_.RELEASED_) {
      TRANS_LOG(INFO, "tx released, cancel commit retry", K(tx));
    } else if (FALSE_IT(tx.commit_task_.set_registered(false))) {
    } else {
      if (ObTxDesc::State::SUB_PREPARING == tx.state_) {
        ret = handle_sub_prepare_timeout_(tx, delay);
      } else if (ObTxDesc::State::SUB_COMMITTING == tx.state_) {
        ret = handle_sub_commit_timeout_(tx, delay);
      } else if (ObTxDesc::State::SUB_ROLLBACKING == tx.state_) {
        ret = handle_sub_rollback_timeout_(tx, delay);
      } else {
      }
    }
    tx.lock_.unlock();
    tx.execute_commit_cb();
  }
  TRANS_LOG(INFO, "handle tx commit timeout", K(ret), K(tx));
  return ret;
}

int ObTransService::handle_sub_prepare_timeout_(ObTxDesc &tx, const int64_t delay)
{
  int ret = OB_SUCCESS;
  int64_t now = ObClockGenerator::getClock();
  if (tx.state_ != ObTxDesc::State::SUB_PREPARING) {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(WARN, "unexpect trans state", K(ret), K_(tx.state), K(tx));
  } else if (tx.commit_expire_ts_ <= now) {
    TRANS_LOG(WARN, "sub prepare timeout", K_(tx.commit_expire_ts), K(tx));
    ret = handle_sub_prepare_result_(tx, OB_TRANS_STMT_TIMEOUT);
  } else {
    ObTxSubPrepareMsg sub_prepare_msg;
    if (OB_FAIL(build_tx_sub_prepare_msg_(tx, sub_prepare_msg))) {
      TRANS_LOG(WARN, "build tx commit msg fail", K(ret), K(tx));
    } else if (OB_FAIL(rpc_->post_msg(tx.coord_id_, sub_prepare_msg))) {
      TRANS_LOG(WARN, "post commit msg fail", K(ret), K(tx));
    }
    if (OB_FAIL(register_commit_retry_task_(tx))) {
      TRANS_LOG(WARN, "reregister task fail", K(ret), K(tx));
    }
  }
  TRANS_LOG(INFO, "handle sub prepare timeout", K(ret), K(tx));
  return ret;
}

int ObTransService::handle_sub_rollback_timeout_(ObTxDesc &tx, const int64_t delay)
{
  int ret = OB_SUCCESS;
  int64_t now = ObClockGenerator::getClock();
  if (tx.state_ != ObTxDesc::State::SUB_ROLLBACKING) {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(WARN, "unexpect trans state", K(ret), K_(tx.state), K(tx));
  } else if (tx.commit_expire_ts_ <= now) {
    TRANS_LOG(WARN, "sub prepare timeout", K_(tx.commit_expire_ts), K(tx));
    const bool is_rollback = true;
    ret = handle_sub_end_tx_result_(tx, is_rollback, OB_TRANS_STMT_TIMEOUT);
  } else {
    ObTxSubRollbackMsg sub_rollback_msg;
    if (OB_FAIL(build_tx_sub_rollback_msg_(tx, sub_rollback_msg))) {
      TRANS_LOG(WARN, "build tx commit msg fail", K(ret), K(tx));
    } else if (OB_FAIL(rpc_->post_msg(tx.coord_id_, sub_rollback_msg))) {
      TRANS_LOG(WARN, "post commit msg fail", K(ret), K(tx));
    }
    if (OB_FAIL(register_commit_retry_task_(tx))) {
      TRANS_LOG(WARN, "reregister task fail", K(ret), K(tx));
    }
  }
  TRANS_LOG(INFO, "handle sub rollback timeout", K(ret), K(tx));
  return ret;
}

int ObTransService::handle_sub_commit_timeout_(ObTxDesc &tx, const int64_t delay)
{
  int ret = OB_SUCCESS;
  int64_t now = ObClockGenerator::getClock();
  if (tx.state_ != ObTxDesc::State::SUB_COMMITTING) {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(WARN, "unexpect trans state", K(ret), K_(tx.state), K(tx));
  } else if (tx.commit_expire_ts_ <= now) {
    TRANS_LOG(WARN, "sub prepare timeout", K_(tx.commit_expire_ts), K(tx));
    const bool is_rollback = false;
    ret = handle_sub_end_tx_result_(tx, is_rollback, OB_TRANS_STMT_TIMEOUT);
  } else {
    ObTxSubCommitMsg sub_commit_msg;
    if (OB_FAIL(build_tx_sub_commit_msg_(tx, sub_commit_msg))) {
      TRANS_LOG(WARN, "build tx commit msg fail", K(ret), K(tx));
    } else if (OB_FAIL(rpc_->post_msg(tx.coord_id_, sub_commit_msg))) {
      TRANS_LOG(WARN, "post commit msg fail", K(ret), K(tx));
    }
    if (OB_FAIL(register_commit_retry_task_(tx))) {
      TRANS_LOG(WARN, "reregister task fail", K(ret), K(tx));
    }
  }
  TRANS_LOG(INFO, "handle sub commit timeout", K(ret), K(tx));
  return ret;
}

int ObTransService::handle_sub_prepare_request(const ObTxSubPrepareMsg &msg,
                                               ObTransRpcResult &result)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sub_prepare_local_ls_(msg.tx_id_,
                                    msg.receiver_,
                                    msg.parts_,
                                    msg.expire_ts_,
                                    msg.app_trace_info_,
                                    msg.request_id_,
                                    msg.xid_))) {
    TRANS_LOG(WARN, "handle tx commit request fail", K(ret), K(msg));
  }
  result.reset();
  result.init(ret, msg.get_timestamp());
  TRANS_LOG(INFO, "handle sub prepare request", K(ret), K(msg));
  return ret;
}

int ObTransService::sub_prepare_local_ls_(const ObTransID &tx_id,
                                          const share::ObLSID &coord,
                                          const share::ObLSArray &parts,
                                          const int64_t &expire_ts,
                                          const common::ObString & app_trace_info,
                                          const int64_t &request_id,
                                          const ObXATransID &xid)
{
  int ret = OB_SUCCESS;
  MonotonicTs commit_time = MonotonicTs::current_time();
  ObPartTransCtx *ctx = NULL;
  if (OB_FAIL(get_tx_ctx_(coord, tx_id, ctx))) {
    TRANS_LOG(WARN, "get coordinator context fail", K(ret), K(tx_id), K(coord));
    if (OB_TRANS_CTX_NOT_EXIST == ret) {
      int tx_state;
      int64_t commit_version = 0;
      if (OB_FAIL(get_tx_state_from_tx_table_(coord, tx_id, tx_state, commit_version))) {
        TRANS_LOG(WARN, "get tx state from tx table fail", K(ret), K(coord), K(tx_id));
        if (OB_ENTRY_NOT_EXIST == ret) {
          ret = OB_TRANS_KILLED; // presume abort
        }
      } else {
        switch (tx_state) {
        case ObTxData::COMMIT:
          ret = OB_TRANS_COMMITED;
          break;
        case ObTxData::ABORT:
          ret = OB_TRANS_KILLED;
          break;
        case ObTxData::RUNNING:
        default:
          ret = OB_ERR_UNEXPECTED;
          TRANS_LOG(WARN, "unexpected non-existence status of trans ctx", K(ret),
                    K(tx_state), K(tx_id), K(coord));
        }
      }
    }
  } else if (OB_FAIL(ctx->sub_prepare(parts, commit_time, expire_ts, app_trace_info, request_id,
          xid))) {
    TRANS_LOG(WARN, "commit fail", K(ret), K(coord), K(tx_id));
  }
  if (OB_NOT_NULL(ctx)) {
    revert_tx_ctx_(ctx);
  }
  return ret;
}

int ObTransService::handle_sub_prepare_response(const ObTxSubPrepareRespMsg &msg,
                                                ObTransRpcResult &result)
{
  int ret = OB_SUCCESS;
  ret = handle_sub_prepare_result(msg.tx_id_, msg.ret_);
  result.reset();
  result.init(ret, msg.get_timestamp());
  TRANS_LOG(INFO, "handle sub prepare response", K(ret), K(msg));
  return ret;
}

int ObTransService::handle_sub_prepare_result(const ObTransID &tx_id,
                                              const int result)
{
  int ret = OB_SUCCESS;
  ObTxDesc *tx = NULL;
  if (OB_FAIL(tx_desc_mgr_.get(tx_id, tx))) {
    TRANS_LOG(WARN, "cannot found tx by id", K(ret), K(tx_id), K(result));
  } else {
    tx->lock_.lock();
    // TODO, check state
    if (ObTxDesc::State::IN_TERMINATE > tx->state_) {
      ret = OB_ERR_UNEXPECTED;
      TRANS_LOG(WARN, "unexpected tx state", K(ret),
                K_(tx->state), K(tx_id), K(result), KPC(tx));
    } else if (ObTxDesc::State::SUB_PREPARED == tx->state_) {
      TRANS_LOG(WARN, "tx has been prepared", K_(tx->state),
                K(tx_id), K(result), KPC(tx));
    } else if (ObTxDesc::State::ROLLED_BACK == tx->state_) {
      TRANS_LOG(WARN, "tx has been rollbacked", K_(tx->state),
                K(tx_id), K(result), KPC(tx));
    } else if (ObTxDesc::State::SUB_PREPARING != tx->state_) {
      ret = OB_ERR_UNEXPECTED;
      TRANS_LOG(WARN, "unexpected tx state", K_(tx->state),
                K(tx_id), K(result), KPC(tx));
    } else {
      ret = handle_sub_prepare_result_(*tx, result);
    }
    tx->lock_.unlock();
    tx->execute_commit_cb();
  }
  if (OB_NOT_NULL(tx)) {
    tx_desc_mgr_.revert(*tx);
  }
  return ret;
}

int ObTransService::handle_sub_prepare_result_(ObTxDesc &tx,
                                               const int result)
{
  int ret = OB_SUCCESS;
  bool commit_fin = true;
  int commit_out = OB_SUCCESS;
  switch (result) {
  case OB_BLOCK_FROZEN:
  case OB_NOT_MASTER:
    commit_fin = false;
    if (tx.commit_task_.is_registered()) {
      // the task maybe already registred:
      // 1. location cache stale: leader on local actually
      // 2. L--(regier)-->F-->L--(here)-->F
    } else if (OB_FAIL(register_commit_retry_task_(tx))) {
      commit_fin = true;
      tx.state_ = ObTxDesc::State::ROLLED_BACK;
      commit_out = OB_TRANS_ROLLBACKED;
    }
    break;
  case OB_SUCCESS:
    // success of sub prepare
    tx.state_ = ObTxDesc::State::SUB_PREPARED;
    commit_out = OB_SUCCESS;
    break;
  case OB_TRANS_COMMITED:
    commit_fin = true;
    tx.state_ = ObTxDesc::State::COMMITTED;
    commit_out = OB_ERR_UNEXPECTED;
    TRANS_LOG(WARN, "unexpected tx result", K(tx), K(result));
    break;
  case OB_TRANS_KILLED:
  case OB_TRANS_ROLLBACKED:
  default:
    tx.state_ = ObTxDesc::State::ROLLED_BACK;
    commit_out = result;
    break;
  }
  // commit finished, cleanup
  if (commit_fin) {
    if (tx.finish_ts_ <= 0) { // maybe aborted early
      tx.finish_ts_ = ObClockGenerator::getClock();
    }
    tx.commit_out_ = commit_out;
    if (tx.commit_task_.is_registered()) {
      if (OB_FAIL(unregister_commit_retry_task_(tx))) {
        TRANS_LOG(ERROR, "deregister timeout task fail", K(tx));
      }
    }
    tx_post_terminate_(tx);
  }
  TRANS_LOG(INFO, "handle sub prepare result", K(ret), K(tx), K(commit_fin), K(result));
  return ret;
}

int ObTransService::handle_sub_commit_request(const ObTxSubCommitMsg &msg,
                                              ObTransRpcResult &result)
{
  int ret = OB_SUCCESS;
  const bool is_rollback = false;
  if (OB_FAIL(sub_end_tx_local_ls_(msg.tx_id_,
                                   msg.receiver_,
                                   msg.request_id_,
                                   msg.xid_,
                                   msg.sender_addr_,
                                   is_rollback))) {
    TRANS_LOG(WARN, "fail to handle tx commit request", K(ret), K(msg));
  }
  result.reset();
  result.init(ret, msg.get_timestamp());
  TRANS_LOG(INFO, "handle sub commit request", K(ret), K(msg));
  return ret;
}

int ObTransService::handle_sub_rollback_request(const ObTxSubRollbackMsg &msg,
                                                ObTransRpcResult &result)
{
  int ret = OB_SUCCESS;
  const bool is_rollback = true;
  if (OB_FAIL(sub_end_tx_local_ls_(msg.tx_id_,
                                   msg.receiver_,
                                   msg.request_id_,
                                   msg.xid_,
                                   msg.sender_addr_,
                                   is_rollback))) {
    TRANS_LOG(WARN, "fail to handle tx rollback request", K(ret), K(msg));
  }
  result.reset();
  result.init(ret, msg.get_timestamp());
  TRANS_LOG(INFO, "handle sub rollback request", K(ret), K(msg));
  return ret;
}

int ObTransService::sub_end_tx_local_ls_(const ObTransID &tx_id,
                                         const share::ObLSID &coord,
                                         const int64_t &request_id,
                                         const ObXATransID &xid,
                                         const ObAddr &sender_addr,
                                         const bool is_rollback)
{
  int ret = OB_SUCCESS;
  MonotonicTs commit_time = MonotonicTs::current_time();
  ObPartTransCtx *ctx = NULL;
  if (OB_FAIL(get_tx_ctx_(coord, tx_id, ctx))) {
    TRANS_LOG(WARN, "fail to get coordinator tx context", K(ret), K(tx_id), K(coord));
    if (OB_TRANS_CTX_NOT_EXIST == ret) {
      int tx_state;
      int64_t commit_version = 0;
      if (OB_FAIL(get_tx_state_from_tx_table_(coord, tx_id, tx_state, commit_version))) {
        TRANS_LOG(WARN, "get tx state from tx table fail", K(ret), K(coord), K(tx_id));
        if (OB_ENTRY_NOT_EXIST == ret) {
          ret = OB_TRANS_KILLED; // presume abort
        }
      } else {
        switch (tx_state) {
        case ObTxData::COMMIT:
          ret = OB_TRANS_COMMITED;
          break;
        case ObTxData::ABORT:
          ret = OB_TRANS_KILLED;
          break;
        case ObTxData::RUNNING:
        default:
          ret = OB_ERR_UNEXPECTED;
          TRANS_LOG(WARN, "unexpected non-existence status of trans ctx", K(ret),
                    K(tx_state), K(tx_id), K(coord));
        }
      }
    }
  } else if (OB_FAIL(ctx->sub_end_tx(request_id, xid, sender_addr, is_rollback))) {
    TRANS_LOG(WARN, "fail to end trans", K(ret), K(coord), K(tx_id));
  }
  if (OB_NOT_NULL(ctx)) {
    revert_tx_ctx_(ctx);
  }
  return ret;
}

int ObTransService::handle_sub_commit_response(const ObTxSubCommitRespMsg &msg,
                                               ObTransRpcResult &result)
{
  int ret = OB_SUCCESS;
  ret = handle_sub_commit_result(msg.tx_id_, msg.ret_);
  result.reset();
  result.init(ret, msg.get_timestamp());
  TRANS_LOG(INFO, "handle sub commit response", K(ret), K(msg));
  return ret;
}

int ObTransService::handle_sub_commit_result(const ObTransID &tx_id,
                                             const int result)
{
  int ret = OB_SUCCESS;
  ObTxDesc *tx = NULL;
  if (OB_FAIL(tx_desc_mgr_.get(tx_id, tx))) {
    TRANS_LOG(WARN, "fail to get trans desc by trans id", K(ret), K(tx_id), K(result));
  } else if (NULL == tx) {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(WARN, "unexpected trans desc", K(ret), K(tx_id), K(result));
  } else {
    tx->lock_.lock();
    // TODO, check state
    if (ObTxDesc::State::SUB_COMMITTING != tx->state_) {
      ret = OB_ERR_UNEXPECTED;
      TRANS_LOG(WARN, "unexpected trans state", K_(tx->state),
                K(tx_id), K(result), KPC(tx));
    } else {
      int final_result = result;
      const bool is_rollback = false;
      if (OB_TRANS_COMMITED == result) {
        final_result = OB_SUCCESS;
      }
      ret = handle_sub_end_tx_result_(*tx, is_rollback, final_result);
    }
    tx_desc_mgr_.remove(*tx);
    tx->lock_.unlock();
    tx->execute_commit_cb();
    // revert of add
    tx_desc_mgr_.revert(*tx);
  }
  if (OB_NOT_NULL(tx)) {
    tx_desc_mgr_.revert(*tx);
  }
  return ret;
}

int ObTransService::handle_sub_rollback_response(const ObTxSubRollbackRespMsg &msg,
                                                 ObTransRpcResult &result)
{
  int ret = OB_SUCCESS;
  ret = handle_sub_rollback_result(msg.tx_id_, msg.ret_);
  result.reset();
  result.init(ret, msg.get_timestamp());
  TRANS_LOG(INFO, "handle sub rollback response", K(ret), K(msg));
  return ret;
}

int ObTransService::handle_sub_rollback_result(const ObTransID &tx_id,
                                               const int result)
{
  int ret = OB_SUCCESS;
  ObTxDesc *tx = NULL;
  if (OB_FAIL(tx_desc_mgr_.get(tx_id, tx))) {
    TRANS_LOG(WARN, "fail to get trans desc by trans id", K(ret), K(tx_id), K(result));
  } else if (NULL == tx) {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(WARN, "unexpected trans desc", K(ret), K(tx_id), K(result));
  } else {
    tx->lock_.lock();
    // TODO, check state
    if (ObTxDesc::State::SUB_ROLLBACKING != tx->state_) {
      ret = OB_ERR_UNEXPECTED;
      TRANS_LOG(WARN, "unexpected trans state", K_(tx->state),
                K(tx_id), K(result), KPC(tx));
    } else {
      int final_result = result;
      const bool is_rollback = true;
      if (OB_TRANS_KILLED == result) {
        final_result = OB_SUCCESS;
      }
      ret = handle_sub_end_tx_result_(*tx, is_rollback, final_result);
    }
    tx_desc_mgr_.remove(*tx);
    tx->lock_.unlock();
    tx->execute_commit_cb();
    // revert of add
    tx_desc_mgr_.revert(*tx);
  }
  if (OB_NOT_NULL(tx)) {
    tx_desc_mgr_.revert(*tx);
  }
  return ret;
}

int ObTransService::handle_sub_end_tx_result_(ObTxDesc &tx,
                                              const bool is_rollback,
                                              const int result)
{
  int ret = OB_SUCCESS;
  bool commit_fin = true;
  switch (result) {
    case OB_BLOCK_FROZEN:
    case OB_NOT_MASTER: {
      commit_fin = false;
      if (tx.commit_task_.is_registered()) {
        // the task maybe already registred:
        // 1. location cache stale: leader on local actually
        // 2. L--(regier)-->F-->L--(here)-->F
      } else if (OB_FAIL(register_commit_retry_task_(tx))) {
        commit_fin = true;
        tx.state_ = ObTxDesc::State::ROLLED_BACK;
        tx.commit_out_ = OB_TRANS_ROLLBACKED;
      }
      break;
    }
    case OB_TRANS_STMT_TIMEOUT: {
      commit_fin = true;
      tx.commit_out_ = OB_TRANS_STMT_TIMEOUT;
      // TODO, use other state to denote timeout
      tx.state_ = ObTxDesc::State::ROLLED_BACK;
      TRANS_LOG(WARN, "stmt timeout of sub end trans", K(tx), K(result));
      break;
    }
    case OB_SUCCESS: {
      commit_fin = true;
      tx.commit_out_ = OB_SUCCESS;
      if (is_rollback) {
        tx.state_ = ObTxDesc::State::SUB_ROLLBACKED;
      } else {
        tx.state_ = ObTxDesc::State::SUB_COMMITTED;
      }
      break;
    }
    case OB_TRANS_UNKNOWN: {
      commit_fin = true;
      tx.state_ = ObTxDesc::State::COMMIT_UNKNOWN;
      tx.commit_out_ = result;
      break;
    }
    default: {
      commit_fin = false;
      TRANS_LOG(WARN, "recv unrecongized commit result, just ignore", K(result), K(tx));
      break;
    }
  }
  if (commit_fin) {
    if (tx.commit_task_.is_registered()) {
      if (OB_FAIL(unregister_commit_retry_task_(tx))) {
        TRANS_LOG(ERROR, "deregister timeout task fail", K(tx));
      }
    }
  }
  return ret;
}

int ObTransService::check_scheduler_status(const share::ObLSID &ls_id)
{
  int ret = OB_SUCCESS;

  if (IS_NOT_INIT) {
    TRANS_LOG(WARN, "ObTransService not inited");
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(!is_running_)) {
    TRANS_LOG(WARN, "ObTransService is not running");
    ret = OB_NOT_RUNNING;
  } else if (!ls_id.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    TRANS_LOG(WARN, "invalid argument", KR(ret), K(ls_id));
  } else if (OB_FAIL(tx_ctx_mgr_.check_scheduler_status(ls_id))) {
    TRANS_LOG(WARN, "check_scheduler_status error", KR(ret), K(ls_id));
  } else {
    TRANS_LOG(INFO, "check_scheduler_status success", K(ls_id));
  }

  return ret;
}

/*
 * create_in_txn_implicit_savepoint - create an implicit savepoint when txn is active
 */
int ObTransService::create_in_txn_implicit_savepoint(ObTxDesc &tx, int64_t &savepoint)
{
  int ret = OB_SUCCESS;

  ObTxParam tx_param;
  tx_param.timeout_us_ = tx.timeout_us_;
  tx_param.lock_timeout_us_ = tx.lock_timeout_us_;
  tx_param.access_mode_ = tx.access_mode_;
  tx_param.isolation_ = tx.isolation_;
  tx_param.cluster_id_ = tx.cluster_id_;
  if (tx_param.is_valid()) {
    ret = create_implicit_savepoint(tx, tx_param, savepoint);
  } else {
    ret = OB_ERR_UNEXPECTED;
    TRANS_LOG(WARN, "create in txn implicit savepoint, but txn not in txn", K(ret), K(tx));
  }
  return ret;
}

} // transaction
} // ocenabase
