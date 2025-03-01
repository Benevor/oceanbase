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

#define USING_LOG_PREFIX STORAGE

#include "storage/tablet/ob_tablet_iterator.h"
#include "share/rc/ob_tenant_base.h"
#include "storage/ls/ob_ls.h"
#include "storage/ls/ob_ls_tablet_service.h"
#include "storage/meta_mem/ob_meta_obj_struct.h"
#include "storage/meta_mem/ob_tablet_handle.h"
#include "storage/meta_mem/ob_tablet_map_key.h"
#include "storage/meta_mem/ob_tenant_meta_mem_mgr.h"
#include "storage/tablet/ob_tablet_multi_source_data.h"

namespace oceanbase
{
namespace storage
{
ObLSTabletIterator::ObLSTabletIterator(const int64_t timeout_us)
  : ls_tablet_service_(nullptr),
    tablet_ids_(),
    idx_(0),
    timeout_us_(timeout_us)
{
}

ObLSTabletIterator::~ObLSTabletIterator()
{
  reset();
}

void ObLSTabletIterator::reset()
{
  ls_tablet_service_ = nullptr;
  tablet_ids_.reset();
  idx_ = 0;
}

bool ObLSTabletIterator::is_valid() const
{
  return nullptr != ls_tablet_service_
      && timeout_us_ >= ObTabletCommon::DIRECT_GET_COMMITTED_TABLET_TIMEOUT_US;
}

int ObLSTabletIterator::get_next_tablet(ObTabletHandle &handle)
{
  int ret = OB_SUCCESS;

  handle.reset();
  if (OB_ISNULL(ls_tablet_service_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ls tablet service is nullptr", K(ret), KP(ls_tablet_service_));
  } else {
    do {
      if (OB_UNLIKELY(tablet_ids_.count() == idx_)) {
        ret = OB_ITER_END;
      } else {
        const common::ObTabletID &tablet_id = tablet_ids_.at(idx_);
        if (OB_FAIL(ls_tablet_service_->get_tablet(tablet_id, handle, timeout_us_))
            && OB_TABLET_NOT_EXIST != ret) {
          LOG_WARN("fail to get tablet", K(ret), K(idx_), K(tablet_id), K_(timeout_us));
        } else {
          handle.set_wash_priority(WashTabletPriority::WTP_LOW);
          ++idx_;
        }
      }
    } while (OB_TABLET_NOT_EXIST == ret);
  }

  return ret;
}

// only for write_checkpoint
int ObLSTabletIterator::get_next_tablet_addr(ObTabletMapKey &key, ObMetaDiskAddr &addr)
{
  int ret = OB_SUCCESS;

  if (OB_ISNULL(ls_tablet_service_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ls tablet service is nullptr", K(ret), KP(ls_tablet_service_));
  } else {
    key.ls_id_ = ls_tablet_service_->ls_->get_ls_id();
    do {
      if (OB_UNLIKELY(tablet_ids_.count() == idx_)) {
        ret = OB_ITER_END;
      } else {
        key.tablet_id_ = tablet_ids_.at(idx_);

        if (OB_FAIL(ls_tablet_service_->get_tablet_addr(key, addr))
            && OB_ENTRY_NOT_EXIST != ret) {
          LOG_WARN("fail to get tablet address", K(ret), K(idx_), K(key));
        } else {
          ++idx_;
        }
      }
    } while (OB_ENTRY_NOT_EXIST == ret);
  }

  return ret;
}

int ObLSTabletIterator::get_next_ddl_kv_mgr(ObDDLKvMgrHandle &ddl_kv_mgr_handle)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(ls_tablet_service_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ls tablet service is nullptr", K(ret), KP(ls_tablet_service_));
  } else {
    ObTenantMetaMemMgr *t3m = MTL(ObTenantMetaMemMgr*);
    do {
      ObTabletMapKey key;
      key.ls_id_ = ls_tablet_service_->ls_->get_ls_id();
      if (OB_UNLIKELY(tablet_ids_.count() == idx_)) {
        ret = OB_ITER_END;
      } else {
        key.tablet_id_ = tablet_ids_.at(idx_);

        if (OB_FAIL(t3m->get_tablet_ddl_kv_mgr(key, ddl_kv_mgr_handle))
            && OB_ENTRY_NOT_EXIST != ret) {
          LOG_WARN("fail to get tablet ddl kv mgr", K(ret), K(idx_), K(key));
        } else {
          ++idx_;
        }
      }
    } while (OB_ENTRY_NOT_EXIST == ret);
  }
  
  return ret;
}


ObHALSTabletIDIterator::ObHALSTabletIDIterator(
    const share::ObLSID &ls_id,
    const bool need_initial_state)
  : ls_id_(ls_id),
    tablet_ids_(),
    idx_(0),
    need_initial_state_(need_initial_state)
{
}

ObHALSTabletIDIterator::~ObHALSTabletIDIterator()
{
  reset();
}

bool ObHALSTabletIDIterator::is_valid() const
{
  return ls_id_.is_valid();
}

void ObHALSTabletIDIterator::reset()
{
  ls_id_.reset();
  tablet_ids_.reset();
  idx_ = 0;
}

int ObHALSTabletIDIterator::get_next_tablet_id(common::ObTabletID &tablet_id)
{
  int ret = OB_SUCCESS;
  ObTenantMetaMemMgr *t3m = MTL(ObTenantMetaMemMgr*);
  ObTabletTxMultiSourceDataUnit tx_data;
  ObTabletMapKey key;
  key.ls_id_ = ls_id_;

  while (OB_SUCC(ret)) {
    if (OB_UNLIKELY(tablet_ids_.count() == idx_)) {
      ret = OB_ITER_END;
    } else {
      tx_data.reset();
      key.tablet_id_ = tablet_ids_.at(idx_);
      if (OB_FAIL(t3m->get_tablet_pointer_tx_data(key, tx_data))) {
        if (OB_ENTRY_NOT_EXIST == ret) {
          ++idx_;
          ret = OB_SUCCESS;
        } else {
          LOG_WARN("failed to get tx data from tablet pointer", K(ret), K(key));
        }
      } else if (ObTabletStatus::MAX == tx_data.tablet_status_ && !need_initial_state_ ) {
        LOG_INFO("tablet is in initial state, should skip", K(ret), K(key));
        ++idx_;
      } else {
        ++idx_;
        tablet_id = key.tablet_id_;
        break;
      }
    }
  }

  return ret;
}
} // namespace storage
} // namespace oceanbase
