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

#ifndef OCEANBASE_STORAGE_TABLELOCK_OB_TABLE_LOCK_COMMON_
#define OCEANBASE_STORAGE_TABLELOCK_OB_TABLE_LOCK_COMMON_
#include "common/ob_simple_iterator.h"
#include "lib/list/ob_dlist.h"
#include "lib/utility/ob_print_utils.h"
#include "storage/tx/ob_trans_define.h"
#include "lib/allocator/ob_mod_define.h"

namespace oceanbase
{

namespace common
{
  class ObTabletID;
}
namespace transaction
{
namespace tablelock
{

// Lock compatibility matrix:
//
// +---------------------+-----------+---------------+-------+---------------------+-----------+
// |                     | ROW SHARE | ROW EXCLUSIVE | SHARE | SHARE ROW EXCLUSIVE | EXCLUSIVE |
// +---------------------+-----------+---------------+-------+---------------------+-----------+
// | ROW SHARE           | Y         | Y             | Y     | Y                   | X         |
// | ROW EXCLUSIVE       | Y         | Y             | X     | X                   | X         |
// | SHARE               | Y         | X             | Y     | X                   | X         |
// | SHARE ROW EXCLUSIVE | Y         | X             | X     | X                   | X         |
// | EXCLUSIVE           | X         | X             | X     | X                   | X         |
// +---------------------+-----------+---------------+-------+---------------------+-----------+

typedef unsigned char ObTableLockMode;
static const char TABLE_LOCK_MODE_COUNT = 5;

static const unsigned char NO_LOCK             = 0x0; // binary 0000
static const unsigned char ROW_SHARE           = 0x8; // binary 1000
static const unsigned char ROW_EXCLUSIVE       = 0x4; // binary 0100
static const unsigned char SHARE               = 0x2; // binary 0010
static const unsigned char SHARE_ROW_EXCLUSIVE = 0x6; // binary 0110, SHARE | ROW_EXCLUSIVE
static const unsigned char EXCLUSIVE           = 0x1; // binary 0001
static const unsigned char MAX_LOCK_MODE       = 0xf;

static const unsigned char LOCK_MODE_ARRAY[TABLE_LOCK_MODE_COUNT] = {
      ROW_SHARE, ROW_EXCLUSIVE, SHARE, SHARE_ROW_EXCLUSIVE, EXCLUSIVE
};

// Each item occupies 4 bits, stand for ROW SHARE, ROW EXCLUSIVE, SHARE, EXCLUSIVE.
static const unsigned char compatibility_matrix[] = { 0x0, /* EXCLUSIVE    : 0000 */
                                                      0xa, /* SHARE        : 1010 */
                                                      0xc, /* ROW EXCLUSIVE: 1100 */
                                                      0xe  /* ROW SHARE    : 1110 */ };

static inline
int lock_mode_to_string(const ObTableLockMode lock_mode,
                        char *str,
                        const int64_t str_len)
{
  int ret = OB_SUCCESS;
  if (NO_LOCK == lock_mode) {
    strncpy(str ,"N", str_len);
  } else if (ROW_SHARE == lock_mode) {
    strncpy(str ,"RS", str_len);
  } else if (ROW_EXCLUSIVE == lock_mode) {
    strncpy(str ,"RX", str_len);
  } else if (SHARE == lock_mode) {
    strncpy(str ,"S", str_len);
  } else if (SHARE_ROW_EXCLUSIVE == lock_mode) {
    strncpy(str ,"SRX", str_len);
  } else if (EXCLUSIVE == lock_mode) {
    strncpy(str ,"X", str_len);
  } else {
    ret = OB_INVALID_ARGUMENT;
  }
  return ret;
}

static inline
bool is_lock_mode_valid(const ObTableLockMode lock_mode)
{
  return lock_mode < MAX_LOCK_MODE;
}

static inline
bool request_lock(ObTableLockMode curr_lock,
                  ObTableLockMode new_lock,
                  int64_t &conflict_modes)
{
  if (!is_lock_mode_valid(curr_lock) || !is_lock_mode_valid(new_lock)) {
    return false;
  } else {
    int64_t index = 0;
    int64_t compat = 0xf;
    conflict_modes = 0;

    while (curr_lock > 0 && compat > 0) {
      if (curr_lock & 1) {
        compat &= compatibility_matrix[index];
        // if new lock conflict with this lock mode.
        if (!(new_lock == (compatibility_matrix[index] & new_lock))) {
          conflict_modes |= (1 << index);
        }
      }
      curr_lock >>= 1;
      index += 1;
    }
    return new_lock == (compat & new_lock);
  }
}

enum ObTableLockOpType : char
{
  UNKNOWN_TYPE = 0,
  IN_TRANS_DML_LOCK = 1,  // will be unlock if we do callback
  OUT_TRANS_LOCK, // will be unlock use OUT_TRANS_UNLOCK
  OUT_TRANS_UNLOCK,
  IN_TRANS_LOCK_TABLE_LOCK,
  MAX_VALID_LOCK_OP_TYPE,
};

static inline
int lock_op_type_to_string(const ObTableLockOpType op_type,
                           char *str,
                           const int64_t str_len)
{
  int ret = OB_SUCCESS;
  if (UNKNOWN_TYPE == op_type) {
    strncpy(str ,"UNKNOWN_TYPE", str_len);
  } else if (IN_TRANS_DML_LOCK == op_type) {
    strncpy(str ,"IN_TRANS_DML_LOCK", str_len);
  } else if (OUT_TRANS_LOCK == op_type) {
    strncpy(str ,"OUT_TRANS_LOCK", str_len);
  } else if (OUT_TRANS_UNLOCK == op_type) {
    strncpy(str ,"OUT_TRANS_UNLOCK", str_len);
  } else if (IN_TRANS_LOCK_TABLE_LOCK == op_type) {
    strncpy(str ,"IN_TRANS_LOCK_TABLE_LOCK", str_len);
  } else {
    ret = OB_INVALID_ARGUMENT;
  }
  return ret;
}

enum ObTableLockOpStatus : char
{
  UNKNOWN_STATUS = 0,
  LOCK_OP_DOING = 1,
  LOCK_OP_COMPLETE
};

static inline
int lock_op_status_to_string(const ObTableLockOpStatus op_status,
                             char *str,
                             const int64_t str_len)
{
  int ret = OB_SUCCESS;
  if (UNKNOWN_STATUS == op_status) {
    strncpy(str ,"UNKNOWN", str_len);
  } else if (LOCK_OP_DOING == op_status) {
    strncpy(str ,"DOING", str_len);
  } else if (LOCK_OP_COMPLETE == op_status) {
    strncpy(str ,"COMPLETE", str_len);
  } else {
    ret = OB_INVALID_ARGUMENT;
  }
  return ret;
}

static inline
bool is_op_type_valid(const ObTableLockOpType type)
{
  return (type < MAX_VALID_LOCK_OP_TYPE &&
          type > UNKNOWN_TYPE);
}

static inline
bool is_out_trans_op_type(const ObTableLockOpType type)
{
  return (type == OUT_TRANS_LOCK ||
          type == OUT_TRANS_UNLOCK);
}

static inline
bool is_in_trans_lock_table_op_type(const ObTableLockOpType type)
{
  return (IN_TRANS_LOCK_TABLE_LOCK == type);
}

static inline
bool is_in_trans_dml_lock_op_type(const ObTableLockOpType type)
{
  return (IN_TRANS_DML_LOCK == type);
}

static inline
bool is_op_status_valid(const ObTableLockOpStatus status)
{
  return (status > UNKNOWN_STATUS && status <= LOCK_OP_COMPLETE);
}

enum class ObLockOBJType : char
{
  OBJ_TYPE_INVALID = 0,
  OBJ_TYPE_TABLE = 1, // table
  OBJ_TYPE_TABLET = 2, // tablet
  OBJ_TYPE_MAX
};

static inline
bool is_lock_obj_type_valid(const ObLockOBJType &type)
{
  return (type > ObLockOBJType::OBJ_TYPE_INVALID &&
          type < ObLockOBJType::OBJ_TYPE_MAX);
}

struct ObLockID final
{
public:
  ObLockID()
    : obj_type_(ObLockOBJType::OBJ_TYPE_INVALID),
    obj_id_(common::OB_INVALID_ID),
    hash_value_(0) {}
  uint64_t hash() const
  { return hash_value_; }
  uint64_t inner_hash() const
  {
    uint64_t hash_val = 0;
    hash_val = murmurhash(&obj_type_, sizeof(obj_type_), hash_val);
    hash_val = murmurhash(&obj_id_, sizeof(obj_id_), hash_val);
    return hash_val;
  }
  bool is_valid() const
  {
    return (common::is_valid_id(obj_id_) &&
            is_lock_obj_type_valid(obj_type_));
  }
  int compare(const ObLockID  &other) const
  {
    int cmp_ret = 0;
    if (obj_type_ > other.obj_type_) {
      cmp_ret = 1;
    } else if (obj_type_ < other.obj_type_) {
      cmp_ret = -1;
    } else if (obj_id_ > other.obj_id_) {
      cmp_ret = 1;
    } else if (obj_id_ < other.obj_id_) {
      cmp_ret = -1;
    }
    return cmp_ret;
  }
  bool operator==(const ObLockID &other) const
  {
   return (0 == compare(other));
  }
  bool operator<(const ObLockID &other) const { return -1 == compare(other); }
  bool operator!=(const ObLockID &other) const { return !operator==(other); }
  int set(const ObLockOBJType &type, const uint64_t obj_id);
  void reset()
  {
    obj_type_ = ObLockOBJType::OBJ_TYPE_INVALID;
    obj_id_ = common::OB_INVALID_ID;
    hash_value_ = 0;
  }
  TO_STRING_KV(K_(obj_type), K_(obj_id));
  NEED_SERIALIZE_AND_DESERIALIZE;
public:
  ObLockOBJType obj_type_;
  uint64_t obj_id_;
  uint64_t hash_value_;
};
int get_lock_id(const uint64_t table_id,
                ObLockID &lock_id);

int get_lock_id(const common::ObTabletID &tablet,
                ObLockID &lock_id);
typedef int64_t ObTableLockOwnerID;

struct ObTableLockOp
{
public:
  static constexpr int64_t MAX_SERIALIZE_SIZE = 256;
  OB_UNIS_VERSION(1);
public:
  ObTableLockOp()
    : lock_id_(),
      lock_mode_(NO_LOCK),
      owner_id_(0),
      create_trans_id_(),
      op_type_(UNKNOWN_TYPE),
      lock_op_status_(UNKNOWN_STATUS),
      lock_seq_no_(0),
      commit_version_(0),
      commit_log_ts_(0),
      create_timestamp_(0),
      create_schema_version_(-1)
  {}
  ObTableLockOp(
      const ObLockID &lock_id,
      const ObTableLockMode lock_mode,
      const ObTableLockOwnerID &owner_id,
      const ObTransID &trans_id,
      const ObTableLockOpType op_type,
      const ObTableLockOpStatus lock_op_status,
      const int64_t seq_no,
      const int64_t create_timestamp,
      const int64_t create_schema_version) :
      lock_id_(),
      lock_mode_(NO_LOCK),
      owner_id_(0),
      create_trans_id_(),
      op_type_(UNKNOWN_TYPE),
      lock_op_status_(UNKNOWN_STATUS),
      lock_seq_no_(0),
      commit_version_(0),
      commit_log_ts_(0),
      create_timestamp_(0),
      create_schema_version_(-1)
  {
    set(lock_id,
        lock_mode,
        owner_id,
        trans_id,
        op_type,
        lock_op_status,
        seq_no,
        create_timestamp,
        create_schema_version);
  }
  void set(
      const ObLockID &lock_id,
      const ObTableLockMode lock_mode,
      const ObTableLockOwnerID &owner_id,
      const ObTransID &trans_id,
      const ObTableLockOpType op_type,
      const ObTableLockOpStatus lock_op_status,
      const int64_t seq_no,
      const int64_t create_timestamp,
      const int64_t create_schema_version);
  bool is_valid() const;
  bool is_out_trans_lock_op() const
  { return is_out_trans_op_type(op_type_); }
  bool need_register_callback() const
  { return !is_out_trans_op_type(op_type_); }
  bool need_multi_source_data() const
  { return is_out_trans_op_type(op_type_); }
  bool need_record_lock_op() const
  {
    return (is_out_trans_op_type(op_type_) ||
            is_need_record_lock_mode_() ||
            is_in_trans_lock_table_op_type(op_type_));
  }
  bool is_dml_lock_op() const
  {
    return is_in_trans_dml_lock_op_type(op_type_);
  }
  bool need_wakeup_waiter() const
  {
    return (is_out_trans_op_type(op_type_) ||
            is_in_trans_lock_table_op_type(op_type_));
  }
  bool operator ==(const ObTableLockOp &other) const;
private:
  bool is_need_record_lock_mode_() const
  {
    return (lock_mode_ == SHARE ||
            lock_mode_ == SHARE_ROW_EXCLUSIVE ||
            lock_mode_ == EXCLUSIVE);
  }
public:
  TO_STRING_KV(K_(lock_id), K_(lock_mode), K_(owner_id), K_(create_trans_id),
               K_(op_type), K_(lock_op_status), K_(lock_seq_no),
               K_(commit_version), K_(commit_log_ts), K_(create_timestamp),
               K_(create_schema_version));

  ObLockID lock_id_;
  ObTableLockMode lock_mode_;
  ObTableLockOwnerID owner_id_;
  ObTransID create_trans_id_;
  ObTableLockOpType op_type_;
  ObTableLockOpStatus lock_op_status_;
  int64_t lock_seq_no_;
  int64_t commit_version_;
  int64_t commit_log_ts_;
  // used to check whether a trans modify before a schema_version or timestamp.
  int64_t create_timestamp_;
  int64_t create_schema_version_;
};

typedef common::ObSEArray<ObTableLockOp, 10, TransModulePageAllocator> ObTableLockOpArray;
struct ObTableLockInfo
{
  OB_UNIS_VERSION(1);
public:
  ObTableLockInfo() : table_lock_ops_(), max_durable_log_ts_(OB_INVALID_TIMESTAMP) {}
  void reset();
  TO_STRING_KV(K_(table_lock_ops), K_(max_durable_log_ts));
  ObTableLockOpArray table_lock_ops_;
  int64_t max_durable_log_ts_;
};

static inline
bool is_need_retry_unlock_error(int err)
{
  return (err == OB_OBJ_LOCK_NOT_COMPLETED ||
          err == OB_OBJ_UNLOCK_CONFLICT);
}

struct ObSimpleIteratorModIds
{
  static constexpr const char OB_OBJ_LOCK[] = "obj_lock";
  static constexpr const char OB_OBJ_LOCK_MAP[] = "obj_lock_map";
};

// Is used to store and traverse all lock id
typedef common::ObSimpleIterator<ObLockID, ObSimpleIteratorModIds::OB_OBJ_LOCK_MAP, 16> ObLockIDIterator;
// Is used to store and traverse all lock op
typedef common::ObSimpleIterator<ObTableLockOp, ObSimpleIteratorModIds::OB_OBJ_LOCK, 16> ObLockOpIterator;

// the threshold of timeout interval which will enable the deadlock avoid.
static const int64_t MIN_DEADLOCK_AVOID_TIMEOUT_US = 60 * 1000 * 1000; // 1 min
bool is_deadlock_avoid_enabled(const int64_t expire_time);

} // tablelock
} // transaction
} // oceanbase


#endif /* OCEANBASE_STORAGE_TABLELOCK_OB_TABLE_LOCK_COMMON_ */
