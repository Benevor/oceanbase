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

#include "log_group_entry_header.h"       // LogGroupEntryHeader
#include "log_entry.h"                    // LogEntry
#include "log_entry_header.h"             // LogEntryHeader
#include "lib/checksum/ob_crc64.h"        // ob_crc64
#include "lib/checksum/ob_parity_check.h" // parity_check
#include "lib/utility/utility.h"          // !FALSE_IT
#include "lib/oblog/ob_log_module.h"      // LOG*
#include "share/rc/ob_tenant_base.h"      // mtl_malloc
#include "log_define.h"                   // is_valid_log_id...
#include "log_writer_utils.h"             // LogWriteBuf

namespace oceanbase
{
namespace palf
{
using namespace common;
using namespace share;

const int64_t LogGroupEntryHeader::HEADER_SER_SIZE = sizeof(LogGroupEntryHeader);

LogGroupEntryHeader::LogGroupEntryHeader()
{
  reset();
}

LogGroupEntryHeader::~LogGroupEntryHeader()
{
  reset();
}

bool LogGroupEntryHeader::is_valid() const
{
  return LogGroupEntryHeader::MAGIC == magic_
         && LOG_GROUP_ENTRY_HEADER_VERSION == version_
         && INVALID_PROPOSAL_ID != proposal_id_
         && true == committed_end_lsn_.is_valid()
         && true == is_valid_log_ts(max_ts_)
         && true == is_valid_log_id(log_id_);
}

void LogGroupEntryHeader::reset()
{
  magic_ = 0;
  version_ = 0;
  group_size_ = 0;
  proposal_id_ = INVALID_PROPOSAL_ID;
  committed_end_lsn_.reset();
  max_ts_ = 0;
  accumulated_checksum_ = 0;
  log_id_ = 0;
  flag_ = 0;
}

int LogGroupEntryHeader::generate(const bool is_raw_write,
                                  const bool is_padding_log,
                                  const LogWriteBuf &log_write_buf,
                                  int64_t data_len,
                                  int64_t max_timestamp,
                                  int64_t log_id,
                                  const LSN &committed_end_lsn,
                                  const int64_t &log_proposal_id,
                                  int64_t &data_checksum)
{
  int ret = OB_SUCCESS;
  if (false == is_valid_log_ts(max_timestamp)
      || false == is_valid_log_id(log_id)
      || false == committed_end_lsn.is_valid()
      || INVALID_PROPOSAL_ID == log_proposal_id) {
    ret = OB_INVALID_ARGUMENT;
    PALF_LOG(ERROR, "Invalid arguments", K(ret),
        K(max_timestamp), K(log_id), K(committed_end_lsn), K(log_proposal_id));
  } else {
    magic_ = LogGroupEntryHeader::MAGIC;
    version_ = LOG_GROUP_ENTRY_HEADER_VERSION;
    group_size_ = static_cast<int32_t>(data_len);
    max_ts_ = max_timestamp;
    log_id_ = log_id;
    committed_end_lsn_ = committed_end_lsn;
    proposal_id_ = log_proposal_id;
    if (is_padding_log) {
      flag_ |= PADDING_TYPE_MASK;
    }
    if (is_raw_write) {
      flag_ |= RAW_WRITE_MASK;
    }
    if (OB_FAIL(calculate_log_checksum_(is_padding_log, log_write_buf, data_len, data_checksum))) {
      PALF_LOG(ERROR, "calculate_log_checksum_ failed", K(ret));
    }
  }
  PALF_LOG(TRACE, "LogGroupEntryHeader generate", K(ret), K(is_padding_log), K(*this), K(data_checksum),
      "haeder_checksum", get_header_parity_check_res_());
  return ret;
}

int LogGroupEntryHeader::calculate_log_checksum_(const bool is_padding_log,
                                                 const LogWriteBuf &log_write_buf,
                                                 const int64_t data_len,
                                                 int64_t &data_checksum)
{
  int ret = OB_SUCCESS;
  if (!log_write_buf.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    PALF_LOG(ERROR, "Invalid arguments", K(ret), K(log_write_buf), K(data_len), K(is_padding_log));
  } else if (is_padding_log) {
    data_checksum = PADDING_LOG_DATA_CHECKSUM;
    PALF_LOG(INFO, "This is a padding log, set log data checksum to 0", K(data_checksum), K(data_len));
  } else {
    char *tmp_buf = NULL;
    bool need_free_mem = false;
    const int64_t total_buf_len = data_len + LogGroupEntryHeader::HEADER_SER_SIZE;
    // no need memcpy
    if (log_write_buf.check_memory_is_continous()) {
      int64_t unused_buf_len = 0;
      const char *first_buf = NULL;
      if (OB_FAIL(log_write_buf.get_write_buf(0, first_buf, unused_buf_len))) {
        PALF_LOG(ERROR, "get_write_buf failed", K(ret), K(log_write_buf), K(data_len));
      } else {
        tmp_buf = const_cast<char*>(first_buf);
        assert(total_buf_len == log_write_buf.get_total_size());
      }
    } else {
      assert(total_buf_len == log_write_buf.get_total_size());
      if (NULL == (tmp_buf = reinterpret_cast<char *>(mtl_malloc(total_buf_len, "LogChecksum")))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        PALF_LOG(ERROR, "allocate memory failed", K(ret));
      } else {
        need_free_mem = true;
        log_write_buf.memcpy_to_continous_memory(tmp_buf);
      }
    }
    int64_t pos = LogGroupEntryHeader::HEADER_SER_SIZE;  // skip group entry header
    assert(total_buf_len > pos);
    LogEntryHeader log_entry_header;
    int64_t log_entry_data_checksum = 0;
    int64_t tmp_log_checksum = 0;
    while (OB_SUCC(ret) && NULL != tmp_buf && pos < total_buf_len) {
      if (OB_FAIL(log_entry_header.deserialize(tmp_buf, total_buf_len, pos))) {
        PALF_LOG(ERROR, "log_entry_header deserialize failed", K(ret), KP(tmp_buf), K(pos), K(total_buf_len));
      } else if (false == log_entry_header.check_header_integrity()) {
        ret = OB_ERR_UNEXPECTED;
        PALF_LOG(ERROR, "log_entry_header is invalid", K(ret), KP(tmp_buf), K(pos), K(total_buf_len),
            K(log_entry_header));
      } else {
        log_entry_data_checksum = log_entry_header.get_data_checksum();
        tmp_log_checksum = common::ob_crc64(tmp_log_checksum, &log_entry_data_checksum, sizeof(log_entry_data_checksum));
        pos += log_entry_header.get_data_len();
      }
    }

    if (OB_SUCC(ret)) {
      data_checksum = tmp_log_checksum;
    }

    if (NULL != tmp_buf && need_free_mem) {
      mtl_free(tmp_buf);
    }
  }
  PALF_LOG(TRACE, "calculate_log_checksum_ finished", K(ret), K(*this), K(data_checksum));
  return ret;
}

bool LogGroupEntryHeader::get_header_parity_check_res_() const
{
  bool bool_ret = parity_check(reinterpret_cast<const uint16_t &>(magic_));
  bool_ret ^= parity_check(reinterpret_cast<const uint16_t &>(version_));
  bool_ret ^= parity_check(reinterpret_cast<const uint32_t &>(group_size_));
  bool_ret ^= parity_check(reinterpret_cast<const uint64_t &>(proposal_id_));
  bool_ret ^= parity_check(committed_end_lsn_.val_);
  bool_ret ^= parity_check(reinterpret_cast<const uint64_t &>(max_ts_));
  bool_ret ^= parity_check(reinterpret_cast<const uint64_t &>(accumulated_checksum_));
  bool_ret ^= parity_check(reinterpret_cast<const uint64_t &>(log_id_));
  int64_t tmp_flag = (flag_ & ~(0x1));
  bool_ret ^= parity_check(reinterpret_cast<const uint64_t &>(tmp_flag));
  return bool_ret;
}

void LogGroupEntryHeader::update_header_checksum()
{
  update_header_checksum_();
}

void LogGroupEntryHeader::update_header_checksum_()
{
  if (get_header_parity_check_res_()) {
    flag_ |= 0x1;
  } else {
    // group header可能会被复用并更新部分字段(比如raw write场景)
    flag_ = (flag_ & ~(0x1));
  }
  PALF_LOG(TRACE, "update_header_checksum_ finished", K(*this), "flag", (flag_ & 0x1));
}

LogGroupEntryHeader& LogGroupEntryHeader::operator=(const LogGroupEntryHeader &header)
{
  magic_ = header.magic_;
  version_ = header.version_;
  group_size_ = header.group_size_;
  proposal_id_ = header.proposal_id_;
  committed_end_lsn_ = header.committed_end_lsn_;
  max_ts_ = header.max_ts_;
  accumulated_checksum_ = header.accumulated_checksum_;
  log_id_ = header.log_id_;
  flag_ = header.flag_;
  return *this;
}

bool LogGroupEntryHeader::operator==(const LogGroupEntryHeader &header) const
{
  return (magic_ == header.magic_
          && version_ == header.version_
          && group_size_ == header.group_size_
          && proposal_id_ == header.proposal_id_
          && committed_end_lsn_ == header.committed_end_lsn_
          && max_ts_ == header.max_ts_
          && accumulated_checksum_ == header.accumulated_checksum_
          && log_id_ == header.log_id_
          && flag_ == header.flag_);
}

bool LogGroupEntryHeader::check_header_integrity() const
{
  return true == is_valid() && true == check_header_checksum_();
}

bool LogGroupEntryHeader::check_integrity(const char *buf,
																					int64_t buf_len) const
{
  int64_t group_log_checksum = 0;
  return check_integrity(buf, buf_len, group_log_checksum);
}

bool LogGroupEntryHeader::check_integrity(const char *buf,
																					int64_t buf_len,
                                          int64_t &group_log_checksum) const
{
  bool bool_ret = false;
  if (LogGroupEntryHeader::MAGIC != magic_) {
    bool_ret = false;
    PALF_LOG(WARN, "magic is different", K(magic_));
  } else if (false == check_header_checksum_()) {
    PALF_LOG(WARN, "check header checsum failed", K(*this));
  } else if (false == check_log_checksum_(buf, buf_len, group_log_checksum)) {
    PALF_LOG(WARN, "check data checksum failed", K(*buf), K(buf_len),
        K(*this));
  } else {
    bool_ret = true;
  }
  PALF_LOG(TRACE, "check_integrity", K(bool_ret), K(group_log_checksum), KPC(this));
  return bool_ret;
}

int LogGroupEntryHeader::update_log_proposal_id(
    const int64_t &log_proposal_id)
{
  int ret = OB_SUCCESS;

  if (INVALID_PROPOSAL_ID == log_proposal_id) {
    ret = OB_INVALID_ARGUMENT;
    PALF_LOG(ERROR, "Invalid argument", K(ret), K(log_proposal_id));
  } else {
    proposal_id_ = log_proposal_id;
    update_header_checksum_();
  }
  return ret;
}

int LogGroupEntryHeader::update_committed_end_lsn(const LSN &lsn)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(! lsn.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    PALF_LOG(ERROR, "invalid argument", K(ret), K(lsn));
  } else {
    committed_end_lsn_ = lsn;
    update_header_checksum_();
  }
  return ret;
}

void LogGroupEntryHeader::update_write_mode(const bool is_raw_write)
{
  if (true == is_raw_write) {
    flag_ |= RAW_WRITE_MASK;
  }
}

DEFINE_SERIALIZE(LogGroupEntryHeader)
{
  int ret = OB_SUCCESS;
  int64_t new_pos = pos;
  if (OB_UNLIKELY(NULL == buf || buf_len <= 0)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(serialization::encode_i16(buf, buf_len, new_pos, magic_))
             || OB_FAIL(serialization::encode_i16(buf, buf_len, new_pos, version_))
             || OB_FAIL(serialization::encode_i32(buf, buf_len, new_pos, group_size_))
             || OB_FAIL(serialization::encode_i64(buf, buf_len, new_pos, proposal_id_))
             || OB_FAIL(committed_end_lsn_.serialize(buf, buf_len, new_pos))
             || OB_FAIL(serialization::encode_i64(buf, buf_len, new_pos, max_ts_))
             || OB_FAIL(serialization::encode_i64(buf, buf_len, new_pos, accumulated_checksum_))
             || OB_FAIL(serialization::encode_i64(buf, buf_len, new_pos, log_id_))
             || OB_FAIL(serialization::encode_i64(buf, buf_len, new_pos, flag_))) {
    ret = OB_BUF_NOT_ENOUGH;
    PALF_LOG(ERROR, "LogGroupEntryHeader serialize failed", K(ret), K(new_pos));
  } else {
    pos = new_pos;
  }
  PALF_LOG(TRACE, "LogGroupEntryHeader serlize", K(ret), K(pos), K(new_pos), K(*this));
  return ret;
}

DEFINE_DESERIALIZE(LogGroupEntryHeader)
{
  int ret = OB_SUCCESS;
  int64_t new_pos = pos;
  if (OB_UNLIKELY(NULL == buf || data_len <= 0)) {
    ret = OB_INVALID_ARGUMENT;
  } else if ((OB_FAIL(serialization::decode_i16(buf, data_len, new_pos, &magic_)))
              || OB_FAIL(serialization::decode_i16(buf, data_len, new_pos, &version_))
              || OB_FAIL(serialization::decode_i32(buf, data_len, new_pos, &group_size_))
              || OB_FAIL(serialization::decode_i64(buf, data_len, new_pos, reinterpret_cast<int64_t*>(&proposal_id_)))
              || OB_FAIL(committed_end_lsn_.deserialize(buf, data_len, new_pos))
              || OB_FAIL(serialization::decode_i64(buf, data_len, new_pos, &max_ts_))
              || OB_FAIL(serialization::decode_i64(buf, data_len, new_pos, &accumulated_checksum_))
              || OB_FAIL(serialization::decode_i64(buf, data_len, new_pos, &log_id_))
              || OB_FAIL(serialization::decode_i64(buf, data_len, new_pos, &flag_))) {
    ret = OB_BUF_NOT_ENOUGH;
  } else if (false == check_header_integrity()) {
    ret = OB_INVALID_DATA;
  } else {
    pos = new_pos;
  }

  return ret;
}

DEFINE_GET_SERIALIZE_SIZE(LogGroupEntryHeader)
{
  int64_t size = 0;
  size += serialization::encoded_length_i16(magic_);
  size += serialization::encoded_length_i16(version_);
  size += serialization::encoded_length_i32(group_size_);
  size += serialization::encoded_length_i64(proposal_id_);
  size += committed_end_lsn_.get_serialize_size();
  size += serialization::encoded_length_i64(max_ts_);
  size += serialization::encoded_length_i64(accumulated_checksum_);
  size += serialization::encoded_length_i64(log_id_);
  size += serialization::encoded_length_i64(flag_);
  return size;
}

void LogGroupEntryHeader::update_accumulated_checksum(int64_t accumulated_checksum)
{
  accumulated_checksum_ = accumulated_checksum;
}

bool LogGroupEntryHeader::check_header_checksum_() const
{
  const int64_t header_checksum = get_header_parity_check_res_() ? 1 : 0;
  const int64_t saved_header_checksum = flag_ & (0x1);
  return (header_checksum == saved_header_checksum);
}

bool LogGroupEntryHeader::check_log_checksum_(const char *buf,
			                                        const int64_t data_len,
                                              int64_t &group_data_checksum) const
{
  bool bool_ret = false;
  int64_t crc_checksum = 0;
  if (OB_ISNULL(buf) || 0 > data_len) {
    PALF_LOG(ERROR, "Invalid argument!!!", K(buf), K(data_len), K(group_size_));
  } else if (is_padding_log()) {
    bool_ret = true;
    PALF_LOG(INFO, "This is a padding log, no need check log checksum", K(bool_ret), K(data_len));
  } else {
    int64_t pos = 0;
    LogEntry log_entry;
    int ret = OB_SUCCESS;
    int64_t log_entry_data_checksum = 0;
    int64_t tmp_group_checksum = 0;
    bool_ret = true;
    while (OB_SUCC(ret) && bool_ret && pos < data_len) {
      if (OB_FAIL(log_entry.deserialize(buf, data_len, pos))) {
        PALF_LOG(ERROR, "log_entry deserialize failed", K(ret), KP(buf), K(data_len), K(pos));
      } else {
        bool_ret = log_entry.check_integrity();
        log_entry_data_checksum = log_entry.get_header().get_data_checksum();
        tmp_group_checksum = common::ob_crc64(tmp_group_checksum, &log_entry_data_checksum, sizeof(log_entry_data_checksum));
      }
    }
    if (OB_FAIL(ret)) {
      bool_ret = false;
    }
    if (bool_ret) {
      group_data_checksum = tmp_group_checksum;
    }
  }
  return bool_ret;
}

bool LogGroupEntryHeader::is_padding_log() const
{
  return (flag_ & PADDING_TYPE_MASK) > 0;
}

bool LogGroupEntryHeader::is_raw_write() const
{
  return (flag_ & RAW_WRITE_MASK) > 0;
}

int LogGroupEntryHeader::truncate(const char *buf,
                                  const int64_t data_len,
                                  const int64_t cut_ts,
                                  const int64_t pre_accum_checksum)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(NULL == buf || data_len <= 0 || OB_INVALID_TIMESTAMP == cut_ts)) {
    ret = OB_INVALID_ARGUMENT;
    PALF_LOG(ERROR, "Invalid arguments", K(ret), KP(buf), K(data_len), K(cut_ts));
  } else if (is_padding_log()) {
    if (max_ts_ > cut_ts) {
      group_size_ = 0;
      update_header_checksum();
    }
    PALF_LOG(INFO, "This is a padding log", K(data_len), K(cut_ts), K(pre_accum_checksum), KPC(this));
  } else {
    int64_t tmp_max_log_ts = 0;
    int64_t pos = 0;
    int64_t cut_pos = 0;
    LogEntryHeader log_entry_header;
    int64_t log_entry_data_checksum = 0;
    int64_t tmp_log_checksum = 0;
    while (OB_SUCC(ret) && pos < data_len) {
      if (OB_FAIL(log_entry_header.deserialize(buf, data_len, pos))) {
        PALF_LOG(ERROR, "log_entry_header deserialize failed", K(ret), KP(buf), K(data_len));
      } else if (log_entry_header.get_log_ts() > cut_ts) {
        break;
      } else {
        log_entry_data_checksum = log_entry_header.get_data_checksum();
        tmp_log_checksum = common::ob_crc64(tmp_log_checksum, &log_entry_data_checksum, sizeof(log_entry_data_checksum));
        pos += log_entry_header.get_data_len();
        cut_pos = pos;
        if (log_entry_header.get_log_ts() > tmp_max_log_ts) {
          tmp_max_log_ts = log_entry_header.get_log_ts();
        }
      }
    }
    if (OB_SUCC(ret)) {
      group_size_ = cut_pos;
      max_ts_ = tmp_max_log_ts;
      update_accumulated_checksum(common::ob_crc64(pre_accum_checksum, const_cast<int64_t *>(&tmp_log_checksum),
                                                   sizeof(tmp_log_checksum)));
      update_header_checksum();
    }
  }
  PALF_LOG(INFO, "truncate finished", K(ret), K(*this));
  return ret;
}

} // end namespace palf
} // end namespace oceanbase
