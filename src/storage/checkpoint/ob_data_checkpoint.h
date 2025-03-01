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

#ifndef OCEANBASE_STORAGE_OB_DATA_CHECKPOINT_H_
#define OCEANBASE_STORAGE_OB_DATA_CHECKPOINT_H_

#include <cstdint>
#include "storage/checkpoint/ob_common_checkpoint.h"
#include "lib/lock/ob_spin_lock.h"
#include "storage/checkpoint/ob_freeze_checkpoint.h"

namespace oceanbase
{
namespace storage
{
class ObLS;
namespace checkpoint
{

class ObCheckpointIterator;

struct ObCheckpointDList
{
  ObCheckpointDList() {}
  ~ObCheckpointDList() {}

  void reset();
  bool is_empty();
  ObFreezeCheckpoint *get_header();
  int unlink(ObFreezeCheckpoint *ob_freeze_checkpoint);
  int insert(ObFreezeCheckpoint *ob_freeze_checkpoint, bool ordered = true);
  void get_iterator(ObCheckpointIterator &iterator);
  int64_t get_min_rec_log_ts_in_list(bool ordered = true);
  ObFreezeCheckpoint *get_first_greater(const int64_t rec_log_ts);
  int get_freezecheckpoint_info(
    ObIArray<checkpoint::ObFreezeCheckpointVTInfo> &freeze_checkpoint_array);

  ObDList<ObFreezeCheckpoint> checkpoint_list_;
};

// lock outside
class ObCheckpointIterator
{
public:
  ObCheckpointIterator() {}
  ~ObCheckpointIterator() {}

  void init(ObCheckpointDList *dlist);
  ObFreezeCheckpoint *get_next();
  bool has_next() const;

private:
  ObFreezeCheckpoint *cur_;
  ObFreezeCheckpoint *next_;
  ObCheckpointDList *dlist_;
};

// responsible for maintenance transaction checkpoint unit
// including data_memtable, tx_data_memtable
class ObDataCheckpoint : public ObCommonCheckpoint
{
  friend class ObFreezeCheckpoint;

public:
  ObDataCheckpoint()
    : is_inited_(false),
      lock_(),
      ls_(nullptr),
      new_create_list_(),
      active_list_(),
      prepare_list_(),
      ls_frozen_list_(),
      ls_frozen_list_lock_(),
      ls_freeze_finished_(true)
  {}
  ~ObDataCheckpoint() {}

  // used for virtual table
  static const uint64_t LS_DATA_CHECKPOINT_TABLET_ID = 40000;
  int init(ObLS *ls);
  int safe_to_destroy(bool &is_safe_destroy);
  int64_t get_rec_log_ts() override;
  // if min_rec_log_ts <= the input rec_log_ts
  // logstream freeze
  int flush(int64_t recycle_log_ts, bool need_freeze = true) override;
  // if min_rec_log_ts <= the input rec_log_ts
  // add ls_freeze task
  // logstream freeze optimization
  int ls_freeze(int64_t rec_log_ts);
  // logstream_freeze schedule and minor merge schedule
  void road_to_flush(int64_t rec_log_ts);
  // ObFreezeCheckpoint register into ObDataCheckpoint
  int add_to_new_create(ObFreezeCheckpoint *ob_freeze_checkpoint);
  // remove from prepare_list when finish minor_merge
  int unlink_from_prepare(ObFreezeCheckpoint *ob_freeze_checkpoint);
  // timer to tranfer freeze_checkpoint that rec_log_ts is stable from new_create_list to
  // active_list
  int check_can_move_to_active_in_newcreate();

  // judge logstream_freeze task if finished
  bool ls_freeze_finished();

  int get_freezecheckpoint_info(
    ObIArray<checkpoint::ObFreezeCheckpointVTInfo> &freeze_checkpoint_array);

  ObTabletID get_tablet_id() const;

  bool is_flushing() const;

  bool has_prepared_flush_checkpoint();

private:
  // traversal prepare_list to flush memtable
  // case1: some memtable flush failed when ls freeze
  // case2: the memtable that tablet freeze
  int traversal_flush_();
  int unlink_(ObFreezeCheckpoint *ob_freeze_checkpoint, ObCheckpointDList &src);
  int insert_(ObFreezeCheckpoint *ob_freeze_checkpoint,
              ObCheckpointDList &dst,
              bool ordered = true);
  int transfer_(ObFreezeCheckpoint *ob_freeze_checkpoint,
                ObCheckpointDList &src,
                ObCheckpointDList &dst,
                ObFreezeCheckpointLocation location);

  int transfer_from_new_create_to_active_(ObFreezeCheckpoint *ob_freeze_checkpoint);
  int transfer_from_new_create_to_prepare_(ObFreezeCheckpoint *ob_freeze_checkpoint);
  int transfer_from_ls_frozen_to_active_(ObFreezeCheckpoint *ob_freeze_checkpoint);
  int transfer_from_ls_frozen_to_prepare_(ObFreezeCheckpoint *ob_freeze_checkpoint);
  int transfer_from_ls_frozen_to_new_created_(ObFreezeCheckpoint *ob_freeze_checkpoint);
  int transfer_from_active_to_prepare_(ObFreezeCheckpoint *ob_freeze_checkpoint);

  void pop_range_to_ls_frozen_(ObFreezeCheckpoint *last, ObCheckpointDList &list);
  void ls_frozen_to_active_(int64_t &last_time);
  void ls_frozen_to_prepare_(int64_t &last_time);
  void print_list_(ObCheckpointDList &list);
  void set_ls_freeze_finished_(bool is_finished);

  static const int64_t LOOP_TRAVERSAL_INTERVAL_US = 1000L * 50;  // 50ms
  bool is_inited_;
  // avoid leaving out ObFreezeCheckpoint that unlinking and not in any list
  common::ObSpinLock lock_;
  ObLS *ls_;
  // new_create_list is unordered_list
  // active_list and prepare_list is ordered_list and order by rec_log_ts
  // improve computational efficiency of checkpoint

  // new created and rec_log_ts_not_stable ObFreezeCheckpoint in new_create_list
  // rec_log_ts_is_stable and not ready_for_flush ObFreezeCheckpoint in active_list
  // ready_for_flush ObFreezeCheckpoint in prepare_list
  ObCheckpointDList new_create_list_;
  ObCheckpointDList active_list_;
  ObCheckpointDList prepare_list_;
  // tmp_list for ls_freeze to improve performance
  // used when new_create_list_ -> active_list and active_list -> frozen_list
  ObCheckpointDList ls_frozen_list_;
  // avoid blocking other list due to traversal ls_frozen_list 
  common::ObSpinLock ls_frozen_list_lock_;
  bool ls_freeze_finished_;
};

static const ObTabletID LS_DATA_CHECKPOINT_TABLET(ObDataCheckpoint::LS_DATA_CHECKPOINT_TABLET_ID);

}  // namespace checkpoint
}  // namespace storage
}  // namespace oceanbase
#endif
