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

#ifndef OCEANBASE_TRANSACTION_OB_LS_TX_SERVICE
#define OCEANBASE_TRANSACTION_OB_LS_TX_SERVICE

#include "lib/ob_errno.h"
#include "lib/lock/ob_spin_lock.h"
#include "share/ob_ls_id.h"
#include "storage/checkpoint/ob_common_checkpoint.h"
#include "storage/ob_i_store.h"
#include "storage/tablelock/ob_table_lock_common.h"
#include "logservice/ob_log_base_type.h"
#include "logservice/rcservice/ob_role_change_handler.h"
#include "storage/tx/ob_keep_alive_ls_handler.h"

namespace oceanbase
{

namespace storage
{
class ObLS;
}

namespace transaction
{
class ObTransID;
class ObTransCtx;
class ObTxDesc;
class ObLSTxCtxMgr;
class ObTxRetainCtxMgr;
class ObTransService;
class ObTxLSLogWriter;
class ObTxStartWorkingLog;
class ObITxLogAdapter;
class ObTxCreateArg;
}

namespace storage
{

class ObLSTxService : public logservice::ObIReplaySubHandler,
                      public logservice::ObIRoleChangeSubHandler,
                      public logservice::ObICheckpointSubHandler
{
public:
  ObLSTxService(ObLS *parent) : parent_(parent), tenant_id_(0), ls_id_(), mgr_(NULL), trans_service_(NULL) {
    reset_();
  }
  ~ObLSTxService() {}
  void destroy() {
    reset_();
  }
  int offline();
  int online();

public:
  int init(const share::ObLSID &ls_id,
           transaction::ObLSTxCtxMgr *mgr,
           transaction::ObTransService *trans_service);
  int create_tx_ctx(transaction::ObTxCreateArg arg,
                    bool &existed,
                    transaction::ObPartTransCtx *&ctx) const;
  int get_tx_ctx(const transaction::ObTransID &tx_id,
                 const bool for_replay,
                 transaction::ObPartTransCtx *&ctx) const;
  int get_tx_scheduler(const transaction::ObTransID &tx_id,
                       ObAddr &scheduler) const;
  int revert_tx_ctx(transaction::ObTransCtx *ctx) const;
  int get_read_store_ctx(const transaction::ObTxReadSnapshot &snapshot,
                         const bool read_latest,
                         const int64_t lock_timeout,
                         ObStoreCtx &store_ctx) const;
  int get_read_store_ctx(const int64_t snapshot_version,
                         const int64_t lock_timeout,
                         ObStoreCtx &store_ctx) const;
  int get_write_store_ctx(transaction::ObTxDesc &tx,
                          const transaction::ObTxReadSnapshot &snapshot,
                          storage::ObStoreCtx &store_ctx) const;
  int revert_store_ctx(storage::ObStoreCtx &store_ctx) const;
  // Freeze process needs to traverse trans ctx to submit redo log
  int traverse_trans_to_submit_redo_log(transaction::ObTransID &fail_tx_id);
  // submit next log when all trx in frozen memtable have submitted log
  int traverse_trans_to_submit_next_log();
  // check schduler status for gc
  int check_scheduler_status(int64_t &min_start_scn, transaction::MinStartScnStatus &status);

  // for ls gc
  // @return OB_SUCCESS, all the tx of this ls cleaned up
  // @return other, there is something wrong or there is some tx not cleaned up.
  int check_all_tx_clean_up() const;
  int block_tx();
  int kill_all_tx(const bool graceful);
  // for ddl check
  // Check all active and not "for_replay" tx_ctx in this ObLSTxCtxMgr
  // whether all the transactions that modify the specified tablet before
  // a schema version are finished.
  // @param [in] schema_version: the schema_version to check
  // @param [out] block_tx_id: a running transaction that modify the tablet before schema version.
  // Return Values That Need Attention:
  // @return OB_EAGAIN: Some TxCtx that has modify the tablet before schema
  // version is running;
  int check_modify_schema_elapsed(const common::ObTabletID &tablet_id,
                                  const int64_t schema_version,
                                  transaction::ObTransID &block_tx_id);
  // Check all active and not "for_replay" tx_ctx in this ObLSTxCtxMgr
  // whether all the transactions that modify the specified tablet before
  // a timestamp are finished.
  // @param [in] timestamp: the timestamp to check
  // @param [out] block_tx_id: a running transaction that modify the tablet before timestamp.
  // Return Values That Need Attention:
  // @return OB_EAGAIN: Some TxCtx that has modify the tablet before timestamp
  // is running;
  int check_modify_time_elapsed(const common::ObTabletID &tablet_id,
                                const int64_t timestamp,
                                transaction::ObTransID &block_tx_id);
  // get the obj lock op iterator from tx of this ls.
  int iterate_tx_obj_lock_op(transaction::tablelock::ObLockOpIterator &iter) const;
public:
  int replay(const void *buffer, const int64_t nbytes, const palf::LSN &lsn, const int64_t ts_ns);

  int replay_start_working_log(const transaction::ObTxStartWorkingLog &log, int64_t log_ts_ns);
  void switch_to_follower_forcedly();
  int switch_to_leader();
  int switch_to_follower_gracefully();
  int resume_leader();

  int64_t get_rec_log_ts();
  int flush(int64_t rec_log_ts);
  int flush_ls_inner_tablet(const ObTabletID &tablet_id);

  int get_common_checkpoint_info(
    ObIArray<checkpoint::ObCommonCheckpointVTInfo> &common_checkpoint_array);

public:
  transaction::ObTransService *get_trans_service() { return trans_service_; }

  transaction::ObTxLSLogWriter *get_tx_ls_log_writer();
  transaction::ObITxLogAdapter *get_tx_ls_log_adapter();

  int register_common_checkpoint(const checkpoint::ObCommonCheckpointType &type,
                                  checkpoint::ObCommonCheckpoint* common_checkpoint);
  int unregister_common_checkpoint(const checkpoint::ObCommonCheckpointType &type,
                                   const checkpoint::ObCommonCheckpoint* common_checkpoint);
  // undertake dump
  int traversal_flush();
  virtual int64_t get_ls_weak_read_ts();
  int check_in_leader_serving_state(bool& bool_ret);

  transaction::ObTxRetainCtxMgr *get_retain_ctx_mgr();
private:
  void reset_();

  storage::ObLS *parent_;
  int64_t tenant_id_;
  share::ObLSID ls_id_;
  transaction::ObLSTxCtxMgr *mgr_;
  transaction::ObTransService *trans_service_;

  // responsible for maintenance checkpoint unit that write TRANS_SERVICE_LOG_BASE_TYPE clog
  checkpoint::ObCommonCheckpoint *common_checkpoints_[checkpoint::ObCommonCheckpointType::MAX_BASE_TYPE];
  common::ObSpinLock lock_;
};

}

} // oceanbase

#endif // OCEANBASE_TRANSACTION_OB_LS_TX_SERVICE
