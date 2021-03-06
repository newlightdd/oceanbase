////===================================================================
 //
 // ob_memtable.cpp updateserver / Oceanbase
 //
 // Copyright (C) 2010 Taobao.com, Inc.
 //
 // Created on 2010-09-09 by Yubai (yubai.lk@taobao.com) 
 //
 // -------------------------------------------------------------------
 //
 // Description
 //
 //
 // -------------------------------------------------------------------
 // 
 // Change Log
 //
////====================================================================

#include "common/ob_trace_log.h"
#include "ob_memtable.h"
#include "ob_ups_utils.h"
#include "ob_update_server_main.h"

namespace oceanbase
{
  namespace updateserver
  {
    using namespace common;
    using namespace hash;

    MemTable::MemTable() : inited_(false), mem_tank_(), table_engine_(mem_tank_), table_bf_(),
                           version_(0), ref_cnt_(0),
                           checksum_before_mutate_(0), checksum_after_mutate_(0),
                           checksum_(0),
                           uncommited_checksum_(0),
                           last_trans_id_(0),
                           trans_mgr_(),
                           row_counter_(0),
                           tevalue_cb_(),
                           trans_cb_()
    {
      MIN_OBJ.set_min_value();
      MAX_OBJ.set_max_value();
    }

    MemTable::~MemTable()
    {
      if (inited_)
      {
        destroy();
      }
    }

    int MemTable::init(const int64_t hash_size)
    {
      int ret = OB_SUCCESS;
      if (inited_)
      {
        TBSYS_LOG(WARN, "have already inited");
        ret = OB_ERROR;
      }
      else if (OB_SUCCESS != (ret = table_engine_.init(hash_size)))
      {
        TBSYS_LOG(WARN, "init table engine fail ret=%d", ret);
      }
      else if (OB_SUCCESS != (ret = table_bf_.init(BLOOM_FILTER_NHASH, BLOOM_FILTER_NBYTE)))
      {
        table_engine_.destroy();
        TBSYS_LOG(WARN, "init table bloomfilter fail ret=%d", ret);
      }
      else if (OB_SUCCESS != (ret = trans_mgr_.init(MAX_TRANS_NUM)))
      {
        table_engine_.destroy();
        table_bf_.destroy();
        TBSYS_LOG(WARN, "init trans mgr fail ret=%d", ret);
      }
      else
      {
        inited_ = true;
      }
      return ret;
    }

    int MemTable::destroy()
    {
      int ret = OB_SUCCESS;
      if (!inited_)
      {
        TBSYS_LOG(WARN, "have not inited");
      }
      else
      {
        row_counter_ = 0;
        trans_mgr_.destroy();
        checksum_after_mutate_ = 0;
        checksum_before_mutate_ = 0;
        checksum_ = 0;
        uncommited_checksum_ = 0;
        table_bf_.destroy();
        table_engine_.destroy();
        mem_tank_.clear();
        inited_ = false;
      }
      return ret;
    }

    int MemTable::clear()
    {
      int ret = OB_SUCCESS;
      if (!inited_)
      {
        TBSYS_LOG(WARN, "have not inited");
      }
      else
      {
        row_counter_ = 0;
        ret = table_engine_.clear();
      }
      return ret;
    }

    int MemTable::rollback(void *data)
    {
      int ret = OB_SUCCESS;
      RollbackInfo *rollback_info = (RollbackInfo*)data;
      if (NULL == rollback_info
          || NULL == rollback_info->dest)
      {
        ret = OB_INVALID_ARGUMENT;
      }
      else
      {
        TBSYS_LOG(INFO, "rollback %s te_value=%p from [%s] to [%s]",
                  rollback_info->key.log_str(),
                  rollback_info->dest,
                  rollback_info->dest->log_str(),
                  rollback_info->src.log_str());
        *(rollback_info->dest) = rollback_info->src;
      }
      return ret;
    }

    int MemTable::commit(void *data)
    {
      int ret = OB_SUCCESS;
      CommitInfo *commit_info = (CommitInfo*)data;
      if (NULL == commit_info)
      {
        ret = OB_INVALID_ARGUMENT;
      }
      else
      {
        atomic_add((uint64_t*)&row_counter_, commit_info->row_counter);
      }
      return ret;
    }

    int MemTable::copy_cells_(TEValueUCInfo &uci,
                              ObUpsCompactCellWriter &ccw)
    {
      int ret = OB_SUCCESS;
      ObCellInfoNode *node = (ObCellInfoNode*)mem_tank_.node_alloc(static_cast<int32_t>(sizeof(ObCellInfoNode) + ccw.size()));
      if (NULL == node)
      {
        ret = OB_MEM_OVERFLOW;
      }
      else
      {
        node->modify_time = INT64_MAX;
        node->next = NULL;
        memcpy(node->buf, ccw.get_buf(), ccw.size());
        if (NULL == uci.uc_list_tail)
        {
          uci.uc_list_head = node;
        }
        else
        {
          uci.uc_list_tail->next = node;
        }
        uci.uc_list_tail = node;
      }
      return ret;
    }

    int MemTable::copy_cells_(TransNode &tn,
                              TEValue &value,
                              ObUpsCompactCellWriter &ccw)
    {
      int ret = OB_SUCCESS;
      ObCellInfoNode *node = (ObCellInfoNode*)mem_tank_.node_alloc(static_cast<int32_t>(sizeof(ObCellInfoNode) + ccw.size()));
      if (NULL == node)
      {
        ret = OB_MEM_OVERFLOW;
      }
      else
      {
        memcpy(node->buf, ccw.get_buf(), ccw.size());
        node->next = NULL;
        node->modify_time = tn.get_trans_id();
        if (NULL == value.list_tail)
        {
          value.list_head = node;
        }
        else
        {
          value.list_tail->next = node;
        }
        value.list_tail = node;
      }
      return ret;
    }

    int MemTable::build_mtime_cell_(const int64_t mtime,
                                    const uint64_t table_id,
                                    ObUpsCompactCellWriter &ccw)
    {
      ObObj obj;
      obj.set_modifytime(mtime);
      uint64_t column_id = OB_UPS_MODIFY_TIME_COLUMN_ID(table_id);
      return ccw.append(column_id, obj);
    }

    int MemTable::update_value_(RWSessionCtx &session,
                                const uint64_t table_id,
                                const ObCellInfo &cell_info,
                                TEValueUCInfo &uci,
                                ObUpsCompactCellWriter &ccw,
                                ObBatchChecksum &bc)
    {
      int ret = OB_SUCCESS;
      if (!is_nop_(cell_info.value_))
      {
        if (ccw.is_row_deleted())
        {
          ret = build_mtime_cell_(session.get_trans_id(), table_id, ccw);
          uci.uc_cell_info_cnt ++;
          ccw.set_row_deleted(false);
        }
        if (OB_SUCCESS == ret)
        {
          if (is_delete_row_(cell_info.value_))
          {
            ret = ccw.row_delete();
            ccw.set_row_deleted(true);
          }
          else
          {
            ret = ccw.append(cell_info.column_id_, cell_info.value_);
            uci.uc_cell_info_size = static_cast<int16_t>(uci.uc_cell_info_size + get_varchar_length_kb_(cell_info.value_));
          }
          uci.uc_cell_info_cnt++;
          bc.fill(&(cell_info.column_id_), sizeof(cell_info.column_id_));
          cell_info.value_.checksum(bc);
          TBSYS_LOG(DEBUG, "[CHECKSUM][OBJ] %ld %s", bc.calc(), print_obj(cell_info.value_));
        }
      }
      return ret;
    }

    int MemTable::update_value_(const TransNode &tn,
                                const uint64_t table_id,
                                ObCellInfo &cell_info,
                                TEValue &value,
                                ObUpsCompactCellWriter &ccw,
                                ObBatchChecksum &bc)
    {
      int ret = OB_SUCCESS;
      if (!is_nop_(cell_info.value_))
      {
        if (ccw.is_row_deleted())
        {
          ret = build_mtime_cell_(tn.get_trans_id(), table_id, ccw);
          value.cell_info_cnt ++;
          ccw.set_row_deleted(false);
        }
        if (OB_SUCCESS == ret)
        {
          if (is_delete_row_(cell_info.value_))
          {
            ret = ccw.row_delete();
            ccw.set_row_deleted(true);
          }
          else
          {
            ret = ccw.append(cell_info.column_id_, cell_info.value_);
            value.cell_info_size = static_cast<int16_t>(value.cell_info_size + get_varchar_length_kb_(cell_info.value_));
          }
          value.cell_info_cnt++;
          bc.fill(&(cell_info.column_id_), sizeof(cell_info.column_id_));
          cell_info.value_.checksum(bc);
        }
      }
      return ret;
    }

    int MemTable::get_cur_value_(RWSessionCtx &session,
                                ILockInfo &lock_info,
                                const TEKey &cur_key,
                                TEValue *&cur_value,
                                TEValueUCInfo *&cur_uci,
                                bool is_row_changed,
                                int64_t &total_row_counter,
                                int64_t &new_row_counter,
                                ObBatchChecksum &bc)
    {
      int ret = OB_SUCCESS;
      if (is_row_changed
          || NULL == cur_value)
      {
        while (true)
        {
          if (NULL != (cur_value = table_engine_.get(cur_key)))
          {
            if (OB_SUCCESS != (ret = lock_info.on_write_begin(cur_key, *cur_value)))
            {
              TBSYS_LOG(WARN, "lock info on write begin fail, ret=%d %s", ret, cur_key.log_str());
            }
            else
            {
              if (NULL == cur_value->cur_uc_info
                  && cur_value->is_empty())
              {
                new_row_counter++;
              }
            }
            break;
          }
          else if (NULL == (cur_value = (TEValue*)mem_tank_.tevalue_alloc(sizeof(TEValue))))
          {
            ret = OB_MEM_OVERFLOW;
            break;
          }
          else
          {
            cur_value->reset();
            TEKey tmp_row_key = cur_key;
            if (OB_SUCCESS != (ret = lock_info.on_write_begin(cur_key, *cur_value)))
            {
              TBSYS_LOG(WARN, "lock info on write begin fail, ret=%d %s", ret, cur_key.log_str());
              break;
            }
            else if (OB_SUCCESS != (ret = mem_tank_.write_string(cur_key.row_key, &(tmp_row_key.row_key))))
            {
              TBSYS_LOG(WARN, "copy rowkey fail, ret=%d %s", ret, cur_key.log_str());
              break;
            }
            else if (OB_SUCCESS != (ret = table_bf_.insert(tmp_row_key.table_id, tmp_row_key.row_key)))
            {
              TBSYS_LOG(WARN, "insert cur_key to bloomfilter fail ret=%d %s", ret, cur_key.log_str());
              break;
            }
            else if (OB_SUCCESS != (ret = table_engine_.set(tmp_row_key, cur_value)))
            {
              if (OB_ENTRY_EXIST == ret)
              {
                TBSYS_LOG(INFO, "put to table_engine entry exist %s", cur_key.log_str());
                continue;
              }
              else
              {
                TBSYS_LOG(WARN, "put to table_engine fail ret=%d %s", ret, cur_key.log_str());
                break;
              }
            }
            else
            {
              new_row_counter++;
              break;
            }
          }
        }
        if (OB_SUCCESS == ret)
        {
          total_row_counter++;
          cur_key.checksum(bc);
          TBSYS_LOG(DEBUG, "[CHECKSUM][RK] %ld %s", bc.calc(), cur_key.log_str());
        }
      }
      if (OB_SUCCESS == ret
          && NULL != cur_value)
      {
        if (NULL == (cur_uci = cur_value->cur_uc_info)
            || session.get_session_descriptor() != cur_uci->session_descriptor)
        {
          if (NULL == (cur_uci = session.alloc_tevalue_uci()))
          {
            TBSYS_LOG(WARN, "alloc tevalue callback info from session ctx fail");
            ret = OB_MEM_OVERFLOW;
          }
          else if (OB_SUCCESS != (ret = session.add_callback_info(session, &tevalue_cb_, cur_uci)))
          {
            TBSYS_LOG(WARN, "add tevalue callback info fail, ret=%d", ret);
          }
          else
          {
            cur_uci->value = cur_value;
            cur_uci->session_descriptor = session.get_session_descriptor();
            cur_value->cur_uc_info = cur_uci;
            TBSYS_LOG(DEBUG, "alloc uc_info value=%p uc_info=%p", cur_uci->value, cur_value->cur_uc_info);
          }
        }
        else
        {
          TBSYS_LOG(DEBUG, "fetch uc_info value=%p uc_info=%p", cur_uci->value, cur_value->cur_uc_info);
        }
      }
      return ret;
    }

    int MemTable::get_cur_value_(TransNode &tn,
                                TEKey &cur_key,
                                const TEKey &prev_key,
                                TEValue *prev_value,
                                TEValue *&cur_value,
                                bool is_row_changed,
                                int64_t &total_row_counter,
                                int64_t &new_row_counter,
                                ObBatchChecksum &bc)
    {
      int ret = OB_SUCCESS;
      if (!is_row_changed
          || cur_key == prev_key)
      {
        cur_key = prev_key;
        cur_value = prev_value;
      }
      else
      {
        bool is_new_row = false;
        total_row_counter++;
        if (NULL == (cur_value = table_engine_.get(cur_key)))
        {
          new_row_counter++;
          if (NULL == (cur_value = (TEValue*)mem_tank_.tevalue_alloc(sizeof(TEValue))))
          {
            ret = OB_MEM_OVERFLOW;
          }
          else
          {
            cur_value->reset();
            ObRowkey tmp_row_key;
            if (OB_SUCCESS == (ret = mem_tank_.write_string(cur_key.row_key, &tmp_row_key)))
            {
              cur_key.row_key = tmp_row_key;
              if (OB_SUCCESS != (ret = table_bf_.insert(cur_key.table_id, cur_key.row_key)))
              {
                TBSYS_LOG(WARN, "insert cur_key to bloomfilter fail ret=%d %s", ret, cur_key.log_str());
              }
              else if (OB_SUCCESS != (ret = table_engine_.set(cur_key, cur_value)))
              {
                TBSYS_LOG(WARN, "put to table_engine fail ret=%d %s", ret, cur_key.log_str());
              }
              else
              {
              }
            }
          }
        }
        if (NULL != cur_value
            && NULL == cur_value->list_head)
        {
          is_new_row = true;
        }
        if (OB_SUCCESS == ret)
        {
          RollbackInfo *rollback_info = (RollbackInfo*)tn.stack_alloc(sizeof(RollbackInfo));
          if (NULL == rollback_info)
          {
            ret = OB_MEM_OVERFLOW;
          }
          else
          {
            rollback_info->key = cur_key;
            rollback_info->dest = cur_value;
            rollback_info->src = *cur_value;
            if (OB_SUCCESS != (ret = tn.add_rollback_data(this, rollback_info)))
            {
              TBSYS_LOG(WARN, "add rollback data fail ret=%d", ret);
            }
          }
        }
        cur_key.checksum(bc);
      }
      return ret;
    }

    int MemTable::ob_sem_handler_(RWSessionCtx &session,
                                  ILockInfo &lock_info,
                                  const ObCellInfo &cell_info,
                                  const TEKey &cur_key,
                                  TEValue *&cur_value,
                                  const bool is_row_changed,
                                  const bool is_row_finished,
                                  ObUpsCompactCellWriter &ccw,
                                  int64_t &total_row_counter,
                                  int64_t &new_row_counter,
                                  ObBatchChecksum &bc)
    {
      int ret = OB_SUCCESS;
      TEValueUCInfo *cur_uci = NULL;
      if (OB_SUCCESS == (ret = get_cur_value_(session, lock_info,
                                              cur_key, cur_value, cur_uci, is_row_changed,
                                              total_row_counter, new_row_counter, bc)))
      {
        if (NULL == cur_value
            || NULL == cur_uci)
        {
          ret = OB_MEM_OVERFLOW;
        }
        else
        {
          ret = update_value_(session, cur_key.table_id, cell_info, *cur_uci, ccw, bc);
          if (OB_SUCCESS == ret)
          {
            if ((is_row_changed || is_row_finished)
                && (MAX_ROW_SIZE / 2) < cur_value->cell_info_size)
            {
              // 大于门限值的一半时 就强制重新计算min flying trans id
              session.flush_min_flying_trans_id();
            }
            if ((is_row_changed || is_row_finished)
                && (get_max_row_cell_num() < cur_value->cell_info_cnt
                    || MAX_ROW_SIZE < cur_value->cell_info_size))
            {
              merge_(session, cur_key, *cur_value);
            }
            if (is_row_finished)
            {
              ccw.row_finish();
              ret = copy_cells_(*cur_uci, ccw);
              ccw.reset();
            }
            if (OB_SUCCESS == ret
                && !session.get_is_replaying()
                && is_row_too_long_(session, cur_key, *cur_value))
            {
              TBSYS_LOG(WARN, "row size overflow [%s] [%s]", cur_key.log_str(), cur_value->log_str());
              ret = OB_SIZE_OVERFLOW;
            }
            if (is_row_finished)
            {
              lock_info.on_precommit_end();
            }
          }
          else
          {
            TBSYS_LOG(WARN, "update value fail ret=%d %s", ret, print_cellinfo(&cell_info));
          }
        }
      }
      return ret;
    }

    int MemTable::ob_sem_handler_(TransNode &tn,
                                  ObCellInfo &cell_info,
                                  TEKey &cur_key,
                                  TEValue *&cur_value,
                                  const TEKey &prev_key,
                                  TEValue *prev_value,
                                  bool is_row_changed,
                                  bool is_row_finished,
                                  ObUpsCompactCellWriter &ccw,
                                  int64_t &total_row_counter,
                                  int64_t &new_row_counter,
                                  ObBatchChecksum &bc)
    {
      int ret = OB_SUCCESS;
      if (OB_SUCCESS == (ret = get_cur_value_(tn, cur_key, prev_key, prev_value, cur_value, is_row_changed,
                                              total_row_counter, new_row_counter, bc)))
      {
        if (NULL == cur_value)
        {
          ret = OB_MEM_OVERFLOW;
        }
        else
        {
          ret = update_value_(tn, cur_key.table_id, cell_info, *cur_value, ccw, bc);
          if (OB_SUCCESS == ret
              && is_row_finished)
          {
            ccw.row_finish();
            ret = copy_cells_(tn, *cur_value, ccw);
            ccw.reset();
            if (get_max_row_cell_num() < cur_value->cell_info_cnt
                || MAX_ROW_SIZE < cur_value->cell_info_size)
            {
              merge_(tn, cur_key, *cur_value);
            }
          }
          if (MAX_ROW_SIZE < cur_value->cell_info_size)
          {
            TBSYS_LOG(WARN, "row size overflow [%s] [%s]", cur_key.log_str(), cur_value->log_str());
            ret = OB_SIZE_OVERFLOW;
          }
        }
      }
      return ret;
    }

    bool MemTable::is_row_too_long_(const RWSessionCtx &session, const TEKey &te_key, const TEValue &te_value)
    {
      bool bret = true;
      if (MAX_ROW_SIZE > (te_value.cell_info_size + te_value.cur_uc_info->uc_cell_info_size))
      {
        bret = false;
      }
      else
      {
        TBSYS_LOG(INFO, "maybe row is too long calculate the compacted size of whole row %s %s", te_key.log_str(), te_value.log_str());
        ObRowCompaction *rc_iter = GET_TSI_MULT(ObRowCompaction, TSI_UPS_ROW_COMPACTION_1);
        if (NULL == rc_iter)
        {
          TBSYS_LOG(WARN, "get tsi ObRowCompaction fail");
        }
        else
        {
          MemTableGetIter get_iter;
          get_iter.set_(te_key, &te_value, NULL, true, &session);
          rc_iter->set_iterator(&get_iter);
          int64_t size = 0;
          int tmp_ret = OB_SUCCESS;
          while (OB_SUCCESS == (tmp_ret = rc_iter->next_cell()))
          {
            ObCellInfo *ci = NULL;
            if (OB_SUCCESS != (tmp_ret = rc_iter->get_cell(&ci))
                || NULL == ci)
            {
              tmp_ret = (OB_SUCCESS == tmp_ret) ? OB_ERROR : tmp_ret;
              break;
            }
            size += get_varchar_length_kb_(ci->value_);
          }
          if (OB_ITER_END != tmp_ret)
          {
            TBSYS_LOG(WARN, "iter ret=%d unexpected %s %s", tmp_ret, te_key.log_str(), te_value.log_str());
          }
          else
          {
            if (MAX_ROW_SIZE > size)
            {
              TBSYS_LOG(INFO, "compacted size=%ld not too long", size);
              bret = false;
            }
            else
            {
              TBSYS_LOG(WARN, "compacted size=%ld too long", size);
            }
          }
        }
      }
      return bret;
    }

    int MemTable::merge_(RWSessionCtx &session,
                        const TEKey &te_key,
                        TEValue &te_value)
    {
      int ret = OB_SUCCESS;
      int64_t timeu = tbsys::CTimeUtil::getTime();

      TEValue new_value;
      new_value.reset();
      new_value.index_stat = te_value.index_stat;
      new_value.cur_uc_info = te_value.cur_uc_info;
      new_value.row_lock = te_value.row_lock;

      MemTableGetIter get_iter;
      BaseSessionCtx merge_session(session.get_type(), session.get_host());
      merge_session.set_trans_id(session.get_min_flying_trans_id());
      get_iter.set_(te_key, &te_value, NULL, false, &merge_session);
      ObRowCompaction *rc_iter = GET_TSI_MULT(ObRowCompaction, TSI_UPS_ROW_COMPACTION_1);
      FixedSizeBuffer<OB_MAX_PACKET_LENGTH> *tbuf = GET_TSI_MULT(FixedSizeBuffer<OB_MAX_PACKET_LENGTH>, TSI_UPS_FIXED_SIZE_BUFFER_2);
      if (NULL == rc_iter)
      {
        TBSYS_LOG(WARN, "get tsi ObRowCompaction fail");
        ret = OB_ERROR;
      }
      else if(NULL == tbuf)
      {
        TBSYS_LOG(WARN, "get tsi FixedSizeBuffer fail");
        ret = OB_ERROR;
      }
      else
      {
        rc_iter->set_iterator(&get_iter);
      }
      ObUpsCompactCellWriter ccw;
      if (OB_SUCCESS == ret)
      {
        ccw.init(tbuf->get_buffer(), tbuf->get_size(), &mem_tank_);
      }
      int64_t mtime = 0;
      while (OB_SUCCESS == ret
            && OB_SUCCESS == (ret = rc_iter->next_cell()))
      {
        ObCellInfo *ci = NULL;
        ret = rc_iter->get_cell(&ci);
        if (OB_SUCCESS != ret)
        {
          break;
        }
        if (NULL == ci)
        {
          ret = OB_ERROR;
          break;
        }
        if (is_row_not_exist_(ci->value_))
        {
          ret = OB_EAGAIN;
          break;
        }
        if (0 == mtime
            && ObModifyTimeType == ci->value_.get_type())
        {
          ci->value_.get_modifytime(mtime);
        }

        if (is_delete_row_(ci->value_))
        {
          ret = ccw.row_delete();
        }
        else
        {
          ret = ccw.append(ci->column_id_, ci->value_);
        }
        if (OB_SUCCESS != ret)
        {
          break;
        }
        TBSYS_LOG(DEBUG, "merge new obj [%s]", print_obj(ci->value_));
        new_value.cell_info_cnt++;
        new_value.cell_info_size = static_cast<int16_t>(new_value.cell_info_size + get_varchar_length_kb_(ci->value_));
      }

      if (OB_ITER_END == ret)
      {
        ret = OB_SUCCESS;
        if (0 < ccw.size())
        {
          ccw.row_finish();
          ObCellInfoNode *node = (ObCellInfoNode*)mem_tank_.node_alloc(static_cast<int32_t>(sizeof(ObCellInfoNode) + ccw.size()));
          if (NULL == node)
          {
            ret = OB_MEM_OVERFLOW;
          }
          else
          {
            memcpy(node->buf, ccw.get_buf(), ccw.size());
            node->next = NULL;
            node->modify_time = mtime;
            new_value.list_head = node;
            new_value.list_tail = node;
          }
        }
      }

      if (OB_SUCCESS == ret
          && NULL != new_value.list_head
          && NULL != new_value.list_tail)
      {
        ObCellInfoNode *node = const_cast<ObCellInfoNode*>(get_iter.get_cur_node_iter_());
        if (NULL != node)
        {
          new_value.list_tail->next = node;
          if (NULL != te_value.cur_uc_info
              && node == te_value.cur_uc_info->uc_list_head)
          {
            // 已经迭代到未提交数据 因此list_tail就是新的list_head
            new_value.list_tail = new_value.list_head;
          }
          else
          {
            new_value.list_tail = te_value.list_tail;
          }
          while (NULL != node
                && node != new_value.list_tail)
          {
            std::pair<int64_t, int64_t> sc = node->get_size_and_cnt();
            new_value.cell_info_cnt = static_cast<int16_t>(new_value.cell_info_cnt + sc.first);
            new_value.cell_info_size = static_cast<int16_t>(new_value.cell_info_size + sc.second);
            node = node->next;
          }
        }
        timeu = tbsys::CTimeUtil::getTime() - timeu;
        TBSYS_LOG(DEBUG, "merge te_value succ, key-value: [%s] [%s] ==> [%s] value=%p timeu=%ld",
                  te_key.log_str(), te_value.log_str(), new_value.log_str(), &te_value, timeu);
        TBSYS_LOG(DEBUG, "merge te_value succ, list: [%s] ==> [%s] value=%p timeu=%ld",
                  te_value.log_list(), new_value.log_list(), &te_value, timeu);
        if (NULL != new_value.list_head)
        {
          // change te_value to new_value
          te_value = new_value;
          OB_STAT_INC(UPDATESERVER, UPS_STAT_MERGE_COUNT, 1);
          OB_STAT_INC(UPDATESERVER, UPS_STAT_MERGE_TIMEU, timeu);
        }
      }
      return ret;
    }

    int MemTable::merge_(const TransNode &tn,
                        const TEKey &te_key,
                        TEValue &te_value)
    {
      int ret = OB_SUCCESS;
      int64_t timeu = tbsys::CTimeUtil::getTime();

      TEValue new_value;
      new_value.reset();

      MemTableGetIter get_iter;
      TransNodeWrapper4Merge trans_node_wrapper(tn);
      get_iter.set_(te_key, &te_value, NULL, &trans_node_wrapper);
      ObRowCompaction *rc_iter = GET_TSI_MULT(ObRowCompaction, TSI_UPS_ROW_COMPACTION_1);
      FixedSizeBuffer<OB_MAX_PACKET_LENGTH> *tbuf = GET_TSI_MULT(FixedSizeBuffer<OB_MAX_PACKET_LENGTH>, TSI_UPS_FIXED_SIZE_BUFFER_2);
      if (NULL == rc_iter)
      {
        TBSYS_LOG(WARN, "get tsi ObRowCompaction fail");
        ret = OB_ERROR;
      }
      else if(NULL == tbuf)
      {
        TBSYS_LOG(WARN, "get tsi FixedSizeBuffer fail");
        ret = OB_ERROR;
      }
      else
      {
        rc_iter->set_iterator(&get_iter);
      }
      ObUpsCompactCellWriter ccw;
      if (OB_SUCCESS == ret)
      {
        ccw.init(tbuf->get_buffer(), tbuf->get_size(), &mem_tank_);
      }
      int64_t mtime = 0;
      while (OB_SUCCESS == ret
            && OB_SUCCESS == (ret = rc_iter->next_cell()))
      {
        ObCellInfo *ci = NULL;
        ret = rc_iter->get_cell(&ci);
        if (OB_SUCCESS != ret)
        {
          break;
        }
        if (NULL == ci)
        {
          ret = OB_ERROR;
          break;
        }
        if (is_row_not_exist_(ci->value_))
        {
          ret = OB_EAGAIN;
          break;
        }
        if (0 == mtime
            && ObModifyTimeType == ci->value_.get_type())
        {
          ci->value_.get_modifytime(mtime);
        }

        if (is_delete_row_(ci->value_))
        {
          ret = ccw.row_delete();
        }
        else
        {
          ret = ccw.append(ci->column_id_, ci->value_);
        }
        if (OB_SUCCESS != ret)
        {
          break;
        }
        TBSYS_LOG(DEBUG, "merge new obj [%s]", print_obj(ci->value_));
        new_value.cell_info_cnt++;
        new_value.cell_info_size = static_cast<int16_t>(new_value.cell_info_size + get_varchar_length_kb_(ci->value_));
      }

      if (OB_ITER_END == ret)
      {
        ret = OB_SUCCESS;
        if (0 < ccw.size())
        {
          ccw.row_finish();
          ObCellInfoNode *node = (ObCellInfoNode*)mem_tank_.node_alloc(static_cast<int32_t>(sizeof(ObCellInfoNode) + ccw.size()));
          if (NULL == node)
          {
            ret = OB_MEM_OVERFLOW;
          }
          else
          {
            memcpy(node->buf, ccw.get_buf(), ccw.size());
            node->next = NULL;
            node->modify_time = mtime;
            new_value.list_head = node;
            new_value.list_tail = node;
          }
        }
      }

      if (OB_SUCCESS == ret)
      {
        ObCellInfoNode *node = const_cast<ObCellInfoNode*>(get_iter.get_cur_node_iter_());
        if (NULL != node)
        {
          if (NULL == new_value.list_tail)
          {
            new_value.list_head = node;
          }
          else
          {
            new_value.list_tail->next = node;
          }
          new_value.list_tail = te_value.list_tail;
          while (NULL != node)
          {
            std::pair<int64_t, int64_t> sc = node->get_size_and_cnt();
            new_value.cell_info_cnt = static_cast<int16_t>(new_value.cell_info_cnt + sc.first);
            new_value.cell_info_size = static_cast<int16_t>(new_value.cell_info_size + sc.second);
            node = node->next;
          }
        }
        timeu = tbsys::CTimeUtil::getTime() - timeu;
        TBSYS_LOG(DEBUG, "merge te_value succ [%s] [%s] ==> [%s] timeu=%ld",
                  te_key.log_str(), te_value.log_str(), new_value.log_str(), timeu);
        if (NULL != new_value.list_head)
        {
          // change te_value to new_value
          te_value = new_value;
        }
      }
      return ret;
    }

    int MemTable::set(const MemTableTransDescriptor td, ObUpsMutator &mutator, const bool check_checksum,
                      ObUpsTableMgr *ups_table_mgr, ObScanner *scanner)
    {
      int ret = OB_SUCCESS;
      TransNode *tn = NULL;
      FixedSizeBuffer<OB_MAX_PACKET_LENGTH> *tbuf = GET_TSI_MULT(FixedSizeBuffer<OB_MAX_PACKET_LENGTH>, TSI_UPS_FIXED_SIZE_BUFFER_1);
      ObBatchChecksum bc;
      bc.set_base(0);
      if (!inited_)
      {
        TBSYS_LOG(WARN, "have not inited");
        ret = OB_ERROR;
      }
      else if (NULL == tbuf)
      {
        TBSYS_LOG(WARN, "get tsi FixedSizeBuffer fail");
        ret = OB_MEM_OVERFLOW;
      }
      else if (NULL == (tn = trans_mgr_.get_trans_node(td)))
      {
        TBSYS_LOG(WARN, "get trans node fail td=%lu", td);
        ret = OB_ERROR;
      }
      else if (NULL != scanner
              && NULL == ups_table_mgr)
      {
        TBSYS_LOG(WARN, "to return scanner ups_table_mgr must not null");
        ret = OB_INVALID_ARGUMENT;
      }
      else if (check_checksum
              && checksum_before_mutate_ != mutator.get_memtable_checksum_before_mutate())
      {
        handle_checksum_error(mutator);
        ret = OB_ERROR;
      }
      else
      {
        int64_t total_row_counter = 0;
        int64_t new_row_counter = 0;
        int64_t cell_counter = 0;
        TEKey cur_key;
        TEValue *cur_value = NULL;
        TEKey prev_key;
        TEValue *prev_value = NULL;
        CellInfoProcessor ci_proc;
        ObMutatorCellInfo *mutator_cell_info = NULL;
        ObCellInfo *cell_info = NULL;
        ObUpsCompactCellWriter ccw;
        ccw.init(tbuf->get_buffer(), tbuf->get_size(), &mem_tank_);
        while (OB_SUCCESS == ret
              && OB_SUCCESS == (ret = mutator.get_mutator().next_cell()))
        {
          bool is_row_changed = false;
          bool is_row_finished = false;
          if (OB_SUCCESS == (ret = mutator.get_mutator().get_cell(&mutator_cell_info, &is_row_changed, &is_row_finished))
              && NULL != mutator_cell_info)
          {
            cell_info = &(mutator_cell_info->cell_info);
            if (!ci_proc.analyse_syntax(*mutator_cell_info))
            {
              ret = OB_ERROR;
            }
            else if (ci_proc.need_skip())
            {
              continue;
            }
            else
            {
              TBSYS_LOG(DEBUG, "trans set cell_info %s irc=%s irf=%s trans_id=%ld",
                        print_cellinfo(cell_info), STR_BOOL(is_row_changed), STR_BOOL(is_row_finished), tn->get_trans_id());
              cur_key.table_id = cell_info->table_id_;
              cur_key.row_key = cell_info->row_key_;
              if (!ci_proc.is_db_sem())
              {
                ret = ob_sem_handler_(*tn, *cell_info,
                                      cur_key, cur_value, prev_key, prev_value,
                                      is_row_changed, is_row_finished, ccw,
                                      total_row_counter, new_row_counter, bc);
              }
              else
              {
                ret = OB_NOT_SUPPORTED;
                TBSYS_LOG(WARN, "can not handle db sem now %s", print_cellinfo(cell_info));
              }
              if (OB_SUCCESS == ret)
              {
                if (ci_proc.need_return())
                {
                  if (NULL == scanner)
                  {
                    TBSYS_LOG(WARN, "need return apply result but scanner null pointer");
                  }
                  else
                  {
                    ret = OB_NOT_SUPPORTED;
                    TBSYS_LOG(ERROR, "not support return result");
                  }
                }
                prev_key = cur_key;
                prev_value = cur_value;
              }
              cell_counter++;
            }
            // 处理后的mutator全转化为update语义
            mutator_cell_info->op_type.set_ext(common::ObActionFlag::OP_UPDATE);
          }
          else
          {
            TBSYS_LOG(WARN, "mutator get cell fail ret=%d", ret);
          }
        }
        ret = (OB_ITER_END == ret) ? OB_SUCCESS : ret;
        if (OB_SUCCESS == ret)
        {
          CommitInfo *commit_info = (CommitInfo*)tn->stack_alloc(sizeof(CommitInfo));
          if (NULL == commit_info)
          {
            ret = OB_MEM_OVERFLOW;
          }
          else
          {
            commit_info->row_counter = new_row_counter;
            ret = tn->add_commit_data(this, commit_info);
          }
        }
        mutator.reset_iter();
        int64_t trans_id = tn->get_trans_id();
        bc.fill(&trans_id, sizeof(trans_id));
        FILL_TRACE_LOG("total_row_num=%ld new_row_num=%ld cell_num=%ld trans_id=%ld ret=%d",
                      total_row_counter, new_row_counter, cell_counter, trans_id, ret);
      }
      int64_t cur_mutator_checksum = bc.calc();
      checksum_after_mutate_ = ob_crc64(checksum_after_mutate_, &cur_mutator_checksum, sizeof(cur_mutator_checksum));
      if (OB_ERROR == ret && mem_tank_.mem_over_limit())
      {
        ret = OB_MEM_OVERFLOW;
      }
      if (OB_SUCCESS == ret)
      {
        if (check_checksum
            && checksum_after_mutate_ != mutator.get_memtable_checksum_after_mutate())
        {
          handle_checksum_error(mutator);
          ret = OB_ERROR;
        }
        else
        {
          mutator.set_memtable_checksum_before_mutate(checksum_before_mutate_);
          mutator.set_memtable_checksum_after_mutate(checksum_after_mutate_);
          mutator.set_mutate_timestamp(tn->get_trans_id());
        }
      }
      return ret;
    }

    int MemTable::check_checksum(const uint64_t checksum2check,
                                 const uint64_t checksum_before_mutate,
                                 const uint64_t checksum_after_mutate)
    {
      int err = OB_SUCCESS;
      if (checksum_ != checksum_before_mutate
          || checksum2check != checksum_after_mutate)
      {
        err = OB_CHECKSUM_ERROR;
        TBSYS_LOG(ERROR, "checksum_error(mutator[%lu->%lu], memtable[%lu->%lu], last_trans_id=%ld",
                  checksum_before_mutate, checksum_after_mutate,
                  checksum_, checksum2check, last_trans_id_);
      }
      return err;
    }

    int MemTable::set(RWSessionCtx &session_ctx,
                      ObIterator &iter)
    {
      int ret = OB_SUCCESS;
      ILockInfo *lock_info = session_ctx.get_lock_info();
      FixedSizeBuffer<OB_MAX_PACKET_LENGTH> *tbuf = GET_TSI_MULT(FixedSizeBuffer<OB_MAX_PACKET_LENGTH>, TSI_UPS_FIXED_SIZE_BUFFER_1);
      ObBatchChecksum bc;
      bc.set_base(session_ctx.get_uc_info().uc_checksum);
      if (!inited_)
      {
        TBSYS_LOG(WARN, "have not inited");
        ret = OB_ERROR;
      }
      else if (NULL == lock_info)
      {
        TBSYS_LOG(ERROR, "lock info null pointer");
        ret = OB_ERR_UNEXPECTED;
      }
      else if (NULL == tbuf)
      {
        TBSYS_LOG(WARN, "get tsi FixedSizeBuffer fail");
        ret = OB_MEM_OVERFLOW;
      }
      else
      {
        int64_t total_row_counter = 0;
        int64_t new_row_counter = 0;
        int64_t cell_counter = 0;
        TEKey cur_key;
        TEValue *cur_value = NULL;
        CellInfoProcessor ci_proc;
        ObCellInfo *cell_info = NULL;
        ObUpsCompactCellWriter ccw;
        ccw.init(tbuf->get_buffer(), tbuf->get_size(), &mem_tank_);
        while (OB_SUCCESS == ret
              && OB_SUCCESS == (ret = iter.next_cell()))
        {
          bool is_row_changed = false;
          bool is_row_finished = false;
          if (OB_SUCCESS != (ret = iter.get_cell(&cell_info, &is_row_changed))
              || OB_SUCCESS != (ret = iter.is_row_finished(&is_row_finished))
              || NULL == cell_info)
          {
            ret = (OB_SUCCESS == ret) ? OB_ERROR : ret;
            TBSYS_LOG(WARN, "iterator get cell fail ret=%d", ret);
          }
          else
          {
            TBSYS_LOG(DEBUG, "trans set cell_info %s irc=%s irf=%s",
                      print_cellinfo(cell_info), STR_BOOL(is_row_changed), STR_BOOL(is_row_finished));
            cur_key.table_id = cell_info->table_id_;
            cur_key.row_key = cell_info->row_key_;
            if (0 == cell_counter)
            {
              FILL_TRACE_BUF(session_ctx.get_tlog_buffer(), "first row %s", cur_key.log_str());
            }
            if (!ci_proc.is_db_sem())
            {
              ret = ob_sem_handler_(session_ctx, *lock_info, *cell_info,
                                    cur_key, cur_value,
                                    is_row_changed, is_row_finished, ccw,
                                    total_row_counter, new_row_counter, bc);
            }
            else
            {
              ret = OB_NOT_SUPPORTED;
              TBSYS_LOG(WARN, "can not handle db sem now %s", print_cellinfo(cell_info));
            }
            cell_counter++;
            if (OB_SUCCESS == ret)
            {
              // 处理后的mutator全转化为update语义
              if (session_ctx.get_is_replaying())
              {}
              else if (OB_SUCCESS != (ret = session_ctx.get_ups_mutator().get_mutator().update(cell_info->table_id_,
                                                                                              cell_info->row_key_,
                                                                                              cell_info->column_id_,
                                                                                              cell_info->value_)))
              {
                TBSYS_LOG(WARN, "add cell info to mutator fail, ret=%d", ret);
              }
              else if (is_row_finished
                      && OB_SUCCESS != (ret = session_ctx.get_ups_mutator().get_mutator().add_row_barrier()))
              {
                TBSYS_LOG(WARN, "add rowkey barrier to mutator fail, ret=%d", ret);
              }
            }
          }
        }
        ret = (OB_ITER_END == ret) ? OB_SUCCESS : ret;
        FILL_TRACE_BUF(session_ctx.get_tlog_buffer(), "total_row_num=%ld new_row_num=%ld cell_num=%ld ret=%d",
                      total_row_counter, new_row_counter, cell_counter, ret);
        session_ctx.get_uc_info().uc_row_counter += new_row_counter;
        session_ctx.get_uc_info().uc_checksum = bc.calc();
        if (OB_SUCCESS == ret)
        {
          session_ctx.get_ups_result().set_affected_rows(total_row_counter);
        }
      }
      return ret;
    }

    int MemTable::set(RWSessionCtx &session_ctx,
                      ILockInfo &lock_info,
                      ObMutator &mutator)
    {
      int ret = OB_SUCCESS;
      FixedSizeBuffer<OB_MAX_PACKET_LENGTH> *tbuf = GET_TSI_MULT(FixedSizeBuffer<OB_MAX_PACKET_LENGTH>, TSI_UPS_FIXED_SIZE_BUFFER_1);
      ObBatchChecksum bc;
      bc.set_base(session_ctx.get_uc_info().uc_checksum);
      if (!inited_)
      {
        TBSYS_LOG(WARN, "have not inited");
        ret = OB_ERROR;
      }
      else if (NULL == tbuf)
      {
        TBSYS_LOG(WARN, "get tsi FixedSizeBuffer fail");
        ret = OB_MEM_OVERFLOW;
      }
      else
      {
        int64_t total_row_counter = 0;
        int64_t new_row_counter = 0;
        int64_t cell_counter = 0;
        TEKey cur_key;
        TEValue *cur_value = NULL;
        CellInfoProcessor ci_proc;
        ObMutatorCellInfo *mutator_cell_info = NULL;
        ObCellInfo *cell_info = NULL;
        ObUpsCompactCellWriter ccw;
        ccw.init(tbuf->get_buffer(), tbuf->get_size(), &mem_tank_);
        while (OB_SUCCESS == ret
              && OB_SUCCESS == (ret = mutator.next_cell()))
        {
          bool is_row_changed = false;
          bool is_row_finished = false;
          if (OB_SUCCESS != (ret = mutator.get_cell(&mutator_cell_info, &is_row_changed, &is_row_finished))
              || NULL == mutator_cell_info)
          {
            ret = (OB_SUCCESS == ret) ? OB_ERROR : ret;
            TBSYS_LOG(WARN, "mutator get cell fail ret=%d", ret);
          }
          else
          {
            cell_info = &(mutator_cell_info->cell_info);
            if (!ci_proc.analyse_syntax(*mutator_cell_info))
            {
              ret = OB_ERROR;
            }
            else if (ci_proc.need_skip())
            {
              continue;
            }
            else
            {
              TBSYS_LOG(DEBUG, "trans set value=%p cell_info %s irc=%s irf=%s",
                        cur_value, print_cellinfo(cell_info), STR_BOOL(is_row_changed), STR_BOOL(is_row_finished));
              cur_key.table_id = cell_info->table_id_;
              cur_key.row_key = cell_info->row_key_;
              if (0 == cell_counter)
              {
                FILL_TRACE_BUF(session_ctx.get_tlog_buffer(), "first row %s", cur_key.log_str());
              }
              if (!ci_proc.is_db_sem())
              {
                ret = ob_sem_handler_(session_ctx, lock_info, *cell_info,
                                      cur_key, cur_value,
                                      is_row_changed, is_row_finished, ccw,
                                      total_row_counter, new_row_counter, bc);
              }
              else
              {
                ret = OB_NOT_SUPPORTED;
                TBSYS_LOG(WARN, "can not handle db sem now %s", print_cellinfo(cell_info));
              }
              cell_counter++;
            }
            if (OB_SUCCESS == ret)
            {
              // 处理后的mutator全转化为update语义
              mutator_cell_info->op_type.set_ext(common::ObActionFlag::OP_UPDATE);
              if (session_ctx.get_is_replaying())
              {}
              else if (OB_SUCCESS != (ret = session_ctx.get_ups_mutator().get_mutator().add_cell(*mutator_cell_info)))
              {
                TBSYS_LOG(WARN, "add cell info to mutator fail, ret=%d", ret);
              }
              else if (is_row_finished
                      && OB_SUCCESS != (ret = session_ctx.get_ups_mutator().get_mutator().add_row_barrier()))
              {
                TBSYS_LOG(WARN, "add rowkey barrier to mutator fail, ret=%d", ret);
              }
            }
          }
        }
        ret = (OB_ITER_END == ret) ? OB_SUCCESS : ret;
        mutator.reset_iter();
        FILL_TRACE_BUF(session_ctx.get_tlog_buffer(), "total_row_num=%ld new_row_num=%ld cell_num=%ld ret=%d",
                      total_row_counter, new_row_counter, cell_counter, ret);
        session_ctx.get_uc_info().uc_row_counter += new_row_counter;
        session_ctx.get_uc_info().uc_checksum = bc.calc();
      }
      return ret;
    }

    void MemTable::handle_checksum_error(ObUpsMutator &mutator)
    {
      TBSYS_LOG(ERROR, "checksum wrong table_checksum_before=%ld mutator_checksum_before=%ld "
                "table_checksum_after=%ld mutator_checksum_after=%ld",
                checksum_before_mutate_, mutator.get_memtable_checksum_before_mutate(),
                checksum_after_mutate_, mutator.get_memtable_checksum_after_mutate());
      ObUpdateServerMain *ups_main = ObUpdateServerMain::get_instance();
      if (NULL != ups_main)
      {
        ups_main->get_update_server().get_role_mgr().set_state(ObUpsRoleMgr::STOP);
      }
      ObMutatorCellInfo *mutator_cell_info = NULL;
      while (OB_SUCCESS == mutator.next_cell())
      {
        mutator.get_cell(&mutator_cell_info);
        if (NULL != mutator_cell_info)
        {
          TBSYS_LOG(INFO, "%s %s", print_obj(mutator_cell_info->op_type), print_cellinfo(&(mutator_cell_info->cell_info)));
        }
      }
    }

    int MemTable::ensure_cur_row(const TEKey &key, TEValue*& value)
    {
      int ret = OB_SUCCESS;
      TEValue* new_value = NULL;
      value = NULL;
      while (OB_SUCCESS == ret && NULL == value)
      {
        if (NULL != (value = table_engine_.get(key)))
        {}
        else if (NULL == (new_value = (TEValue*)mem_tank_.tevalue_alloc(sizeof(TEValue))))
        {
          ret = OB_MEM_OVERFLOW;
        }
        else
        {
          new_value->reset();
          TEKey tmp_row_key = key;
          if (OB_SUCCESS != (ret = mem_tank_.write_string(key.row_key, &(tmp_row_key.row_key))))
          {
            TBSYS_LOG(WARN, "copy rowkey fail, ret=%d %s", ret, key.log_str());
          }
          else if (OB_SUCCESS != (ret = table_bf_.insert(tmp_row_key.table_id, tmp_row_key.row_key)))
          {
            TBSYS_LOG(WARN, "insert cur_key to bloomfilter fail ret=%d %s", ret, key.log_str());
          }
          else if (OB_SUCCESS != (ret = table_engine_.set(tmp_row_key, new_value))
            && OB_ENTRY_EXIST != ret)
          {
            TBSYS_LOG(WARN, "put to table_engine fail ret=%d %s", ret, key.log_str());
          }
          else if (OB_ENTRY_EXIST == ret)
          {
            ret = OB_SUCCESS;
            TBSYS_LOG(INFO, "put to table_engine entry exist %s", key.log_str());
          }
          else
          {
            value = new_value;
          }
        }
      }
      return ret;
    }

    int MemTable::rdlock_row(ILockInfo* lock_info, const TEKey &key, TEValue*& value, const sql::ObLockFlag lock_flag)
    {
      int ret = OB_SUCCESS;
      if (NULL == lock_info)
      {
        ret = OB_ERR_UNEXPECTED;
        TBSYS_LOG(ERROR, "lock_info == NULL");
      }
      else if ((0 == (lock_flag & sql::LF_WRITE))
               && (NO_LOCK == lock_info->get_isolation_level()
                   || READ_COMMITED == lock_info->get_isolation_level()))
      {
        value = NULL;
      }
      else if (OB_SUCCESS != (ret = ensure_cur_row(key, value)))
      {
        TBSYS_LOG(WARN, "ensure cur row fail, ret=%d, %s", ret, key.log_str());
      }
      else if (NULL == value)
      {
        ret = OB_ERR_UNEXPECTED;
        TBSYS_LOG(ERROR, "ensure_cur_row() return NULL value");
      }
      else if (0 != (lock_flag & sql::LF_WRITE))
      {
        if (OB_SUCCESS != (ret = lock_info->on_write_begin(key, *value)))
        {
          TBSYS_LOG(WARN, "lock info on write begin fail, ret=%d %s", ret, key.log_str());
          ret = OB_ERR_SHARED_LOCK_CONFLICT;
        }
      }
      else
      {
        if (OB_SUCCESS != (ret = lock_info->on_read_begin(key, *value)))
        {
          TBSYS_LOG(WARN, "lock info on read begin fail, ret=%d %s", ret, key.log_str());
        }
      }
      return ret;
    }

    int MemTable::get(const BaseSessionCtx &session_ctx,
                      const uint64_t table_id, const ObRowkey &row_key,
                      MemTableIterator &iterator,
                      ColumnFilter *column_filter/* = NULL*/,
                      const sql::ObLockFlag lock_flag /*=0*/)
    {
      int ret = OB_SUCCESS;
      TEKey key(table_id, row_key);
      TEValue *value = NULL;
      ILockInfo* lock_info = NULL;
      if (!inited_)
      {
        TBSYS_LOG(WARN, "have not inited");
        ret = OB_ERROR;
      }
      else if (ST_READ_WRITE == session_ctx.get_type())
      {
        if (NULL == (lock_info = ((RWSessionCtx&)session_ctx).get_lock_info()))
        {
          ret = OB_ERR_UNEXPECTED;
          TBSYS_LOG(ERROR, "session_ctx is RW_SESSION but lock_info==NULL");
        }
        else if (OB_SUCCESS != (ret = rdlock_row(lock_info, key, value, lock_flag)))
        {
          TBSYS_LOG(WARN, "rdlock_row()=>%d", ret);
        }
      }
      if (OB_SUCCESS == ret && NULL == value)
      {
        if (using_memtable_bloomfilter()
            && !table_bf_.contain(table_id, row_key))
        {} // 保持value=NULL, iterator迭代时可以处理
        else
        {
          value = table_engine_.get(key);
        }
      }
      if (OB_SUCCESS == ret)
      {
        iterator.get_get_iter_().set_(key, value, column_filter, true, &session_ctx);
      }
      return ret;
    }

    int MemTable::get(const MemTableTransDescriptor td,
                      const uint64_t table_id, const ObRowkey &row_key,
                      MemTableIterator &iterator,
                      ColumnFilter *column_filter/* = NULL*/)
    {
      int ret = OB_SUCCESS;
      TransNode *tn = NULL;
      TEKey key;
      key.table_id = table_id;
      key.row_key = row_key;
      /*
      key.rowkey_prefix = 0;
      memcpy(&(key.rowkey_prefix), key.row_key.ptr(),
            std::min(key.row_key.length(), (int32_t)sizeof(key.rowkey_prefix)));
            */
      TEValue *value = NULL;
      if (!inited_)
      {
        TBSYS_LOG(WARN, "have not inited");
        ret = OB_ERROR;
      }
      else if (NULL == (tn = trans_mgr_.get_trans_node(td)))
      {
        TBSYS_LOG(WARN, "get trans node fail td=%lu", td);
        ret = OB_ERROR;
      }
      else if (using_memtable_bloomfilter()
              && !table_bf_.contain(table_id, row_key))
      {
        iterator.get_get_iter_().set_(key, NULL, column_filter, tn);
      }
      else
      {
        value = table_engine_.get(key);
        iterator.get_get_iter_().set_(key, value, column_filter, tn);
      }
      return ret;
    }

    int MemTable::scan(const BaseSessionCtx &session_ctx,
                      const ObNewRange &range,
                      const bool reverse,
                      MemTableIterator &iter,
                      ColumnFilter *column_filter/* = NULL*/)
    {
      int ret = OB_SUCCESS;
      
      TEKey start_key;
      TEKey end_key;
      int start_exclude = get_start_exclude(range);
      int end_exclude = get_end_exclude(range);
      int min_key = get_min_key(range);
      int max_key = get_max_key(range);

      start_key.table_id = get_table_id(range);
      start_key.row_key = get_start_key(range);;
      end_key.table_id = get_table_id(range);
      end_key.row_key = get_end_key(range);

      if (0 != min_key)
      {
        start_exclude = 0;
        min_key = 0;
        start_key.row_key.set_min_row();
      }

      if (0 != max_key)
      {
        end_exclude = 0;
        max_key = 0;
        end_key.row_key.set_max_row();
      }

      if (!inited_)
      {
        TBSYS_LOG(WARN, "have not inited");
        ret = OB_ERROR;
      }
      else if (OB_SUCCESS != table_engine_.scan(start_key, min_key, start_exclude,
                                                end_key, max_key, end_exclude,
                                                reverse, iter.get_scan_iter_().get_te_iter_()))
      {
        ret = OB_ERROR;
      }
      else
      {
        iter.get_scan_iter_().set_(get_table_id(range), column_filter, true, &session_ctx);
      }
      return ret;
    }

    int MemTable::scan(const MemTableTransDescriptor td,
                      const ObNewRange &range,
                      const bool reverse,
                      MemTableIterator &iter,
                      ColumnFilter *column_filter/* = NULL*/)
    {
      int ret = OB_SUCCESS;
      
      TransNode *tn = NULL;
      TEKey start_key;
      TEKey end_key;
      int start_exclude = get_start_exclude(range);
      int end_exclude = get_end_exclude(range);
      int min_key = get_min_key(range);
      int max_key = get_max_key(range);

      start_key.table_id = get_table_id(range);
      start_key.row_key = get_start_key(range);;
      end_key.table_id = get_table_id(range);
      end_key.row_key = get_end_key(range);

      if (0 != min_key)
      {
        start_exclude = 0;
        min_key = 0;
        start_key.row_key.assign(&MIN_OBJ, 1);
      }

      if (0 != max_key)
      {
        end_exclude = 0;
        max_key = 0;
        end_key.row_key.assign(&MAX_OBJ, 1);
      }

      if (!inited_)
      {
        TBSYS_LOG(WARN, "have not inited");
        ret = OB_ERROR;
      }
      else if (NULL == (tn = trans_mgr_.get_trans_node(td)))
      {
        TBSYS_LOG(WARN, "get trans node fail td=%lu", td);
        ret = OB_ERROR;
      }
      else if (OB_SUCCESS != table_engine_.scan(start_key, min_key, start_exclude,
                                                end_key, max_key, end_exclude,
                                                reverse, iter.get_scan_iter_().get_te_iter_()))
      {
        ret = OB_ERROR;
      }
      else
      {
        iter.get_scan_iter_().set_(get_table_id(range), column_filter, tn);
      }
      return ret;
    }

    int MemTable::start_transaction(const TETransType trans_type, MemTableTransDescriptor &td, const int64_t trans_id)
    {
      int ret = OB_SUCCESS;
      if (!inited_)
      {
        TBSYS_LOG(WARN, "have not inited");
        ret = OB_ERROR;
      }
      else if (OB_SUCCESS != (ret = trans_mgr_.start_transaction(trans_type, td, trans_id)))
      {
        TBSYS_LOG(WARN, "trans mgr start transaction fail ret=%d", ret);
      }
      else
      {
        // do nothing
      }
      return ret;
    }

    int MemTable::end_transaction(const MemTableTransDescriptor td, bool rollback)
    {
      int ret = OB_SUCCESS;
      if (!inited_)
      {
        TBSYS_LOG(WARN, "have not inited");
        ret = OB_ERROR;
      }
      else if (OB_SUCCESS != (ret = trans_mgr_.end_transaction(td, rollback)))
      {
        TBSYS_LOG(WARN, "trans mgr end transaction fail ret=%d", ret);
      }
      else
      {
        // do nothing
      }
      return ret;
    }

    int MemTable::start_mutation(const MemTableTransDescriptor td)
    {
      int ret = OB_SUCCESS;
      TransNode *tn = NULL;
      if (!inited_)
      {
        TBSYS_LOG(WARN, "have not inited");
        ret = OB_ERROR;
      }
      else if (NULL == (tn = trans_mgr_.get_trans_node(td)))
      {
        TBSYS_LOG(WARN, "get trans node fail td=%lu", td);
        ret = OB_ERROR;
      }
      else if (OB_SUCCESS != (ret = tn->start_mutation()))
      {
        TBSYS_LOG(WARN, "start mutation fail ret=%d", ret);
      }
      else
      {
        checksum_after_mutate_ = checksum_before_mutate_;
      }
      return ret;
    }

    int MemTable::end_mutation(const MemTableTransDescriptor td, bool rollback)
    {
      int ret = OB_SUCCESS;
      TransNode *tn = NULL;
      if (!inited_)
      {
        TBSYS_LOG(WARN, "have not inited");
        ret = OB_ERROR;
      }
      else if (NULL == (tn = trans_mgr_.get_trans_node(td)))
      {
        TBSYS_LOG(WARN, "get trans node fail td=%lu", td);
        ret = OB_ERROR;
      }
      else if (OB_SUCCESS != (ret = tn->end_mutation(rollback)))
      {
        TBSYS_LOG(WARN, "end mutation fail ret=%d rollback=%d", ret, rollback);
      }
      else
      {
        if (!rollback)
        {
          checksum_before_mutate_ = checksum_after_mutate_;
        }
        else
        {
          checksum_after_mutate_ = checksum_before_mutate_;
        }
      }
      return ret;
    }

    int MemTable::get_bloomfilter(TableBloomFilter &table_bf) const
    {
      return table_bf.deep_copy(table_bf_);
    }

    int MemTable::scan_all(TableEngineIterator &iter)
    {
      int ret = OB_SUCCESS;
      TEKey empty_key;
      int min_key = 1;
      int max_key = 1;
      int start_exclude = 0;
      int end_exclude = 0;
      bool reverse = false;
      if (!inited_)
      {
        TBSYS_LOG(WARN, "have not inited");
        ret = OB_ERROR;
      }
      else if (OB_SUCCESS != table_engine_.scan(empty_key, min_key, start_exclude,
                                                empty_key, max_key, end_exclude,
                                                reverse, iter))
      {
        TBSYS_LOG(WARN, "table engine scan fail ret=%d", ret);
      }
      else
      {
        TBSYS_LOG(INFO, "scan all start succ");
      }
      return ret;
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////

    MemTableGetIter::MemTableGetIter() : te_key_(),
                                         te_value_(NULL),
                                         column_filter_(NULL),
                                         return_rowkey_column_(true),
                                         session_ctx_(NULL),
                                         is_iter_end_(true),
                                         node_iter_(NULL),
                                         cell_iter_(),
                                         iter_counter_(0),
                                         ci_(),
                                         nop_v_node_(),
                                         rne_v_node_(),
                                         mctime_v_node_()
    {
      ObUpsCompactCellWriter ccw;

      ccw.init(nop_v_node_.buf, sizeof(nop_v_buf_));
      ccw.row_nop();
      ccw.row_finish();
      nop_v_node_.next = NULL;

      ccw.init(rne_v_node_.buf, sizeof(rne_v_buf_));
      ccw.row_not_exist();
      ccw.row_finish();
      rne_v_node_.next = NULL;

      mctime_v_node_.next = NULL;
    }

    MemTableGetIter::~MemTableGetIter()
    {
    }

    void MemTableGetIter::reset()
    {
      te_value_ = NULL;
      column_filter_ = NULL;
      //return_rowkey_column_ = true;
      //trans_node_ = NULL;
      session_ctx_ = NULL;
      is_iter_end_ = true;
      node_iter_ = NULL;
      cell_iter_.reset();
      iter_counter_ = 0;
      //ci_.reset();
      nop_v_node_.next = NULL;
      rne_v_node_.next = NULL;
      mctime_v_node_.next = NULL;
    }

    void MemTableGetIter::set_(const TEKey &te_key,
                              const TEValue *te_value,
                              const ColumnFilter *column_filter,
                              const bool return_rowkey_column,
                              const BaseSessionCtx *session_ctx)
    {
      te_key_ = te_key;
      te_value_ = te_value;
      column_filter_ = column_filter;
      return_rowkey_column_ = return_rowkey_column;
      session_ctx_ = session_ctx;
      is_iter_end_ = false;
      node_iter_ = NULL;
      cell_iter_.reset();
      iter_counter_ = 0;
      ci_.table_id_ = te_key_.table_id;
      ci_.row_key_ = te_key_.row_key;
      ci_.column_id_ = OB_INVALID_ID;
      ci_.value_.reset();

      ObCellInfoNode *list_head = get_list_head_();
      if (NULL == list_head)
      {
        cell_iter_.set_rne();
      }
      else
      {
        if (!read_uncommited_data_()
            && NULL != session_ctx
            && list_head->modify_time > session_ctx->get_trans_id())
        {
          cell_iter_.set_rne();
        }
        else
        {
          node_iter_ = list_head;

          if (NULL != column_filter_
              && !column_filter_->is_all_column())
          {
            cell_iter_.set_nop();
          }

          if (return_rowkey_column_)
          {
            cell_iter_.set_rowkey_column(te_key_.table_id, te_key_.row_key);
          }

          uint64_t ctime_column_id = OB_UPS_CREATE_TIME_COLUMN_ID(te_key_.table_id);
          cell_iter_.set_ctime_column_id(ctime_column_id);

          uint64_t mtime_column_id = OB_UPS_MODIFY_TIME_COLUMN_ID(te_key_.table_id);
          cell_iter_.set_mtime(mtime_column_id, node_iter_->modify_time);

          cell_iter_.set_cell_info_node(node_iter_);
        }
      }
      cell_iter_.set_head();
    }

    void MemTableGetIter::set_(const TEKey &te_key,
                              const TEValue *te_value,
                              const ColumnFilter *column_filter,
                              const ITransNode *trans_node)
    {
      te_key_ = te_key;
      te_value_ = te_value;
      column_filter_ = column_filter;
      UNUSED(trans_node);//trans_node_ = trans_node;
      is_iter_end_ = false;
      node_iter_ = NULL;
      cell_iter_.reset();
      iter_counter_ = 0;
      ci_.table_id_ = te_key_.table_id;
      ci_.row_key_ = te_key_.row_key;
      ci_.column_id_ = OB_INVALID_ID;
      ci_.value_.reset();

      if (NULL == te_value_
          || NULL == te_value_->list_head)
      {
        cell_iter_.set_rne();
      }
      else
      {
        ObObj mtime_obj;
        mtime_obj.set_modifytime(te_value_->list_head->modify_time);
        if (trans_end_(mtime_obj))
        {
          cell_iter_.set_rne();
        }
        else
        {
          node_iter_ = te_value_->list_head;

          if (NULL != column_filter_
              && !column_filter_->is_all_column())
          {
            cell_iter_.set_nop();
          }

          cell_iter_.set_rowkey_column(te_key_.table_id, te_key_.row_key);

          uint64_t ctime_column_id = OB_UPS_CREATE_TIME_COLUMN_ID(te_key_.table_id);
          cell_iter_.set_ctime_column_id(ctime_column_id);

          uint64_t mtime_column_id = OB_UPS_MODIFY_TIME_COLUMN_ID(te_key_.table_id);
          cell_iter_.set_mtime(mtime_column_id, node_iter_->modify_time);

          cell_iter_.set_cell_info_node(node_iter_);

          __builtin_prefetch(node_iter_);
        }
      }
      cell_iter_.set_head();
    }

    const ObCellInfoNode *MemTableGetIter::get_cur_node_iter_() const
    {
      return node_iter_;
    }

    bool MemTableGetIter::trans_end_(const ObObj &value)
    {
      bool bret = false;
      if (ObModifyTimeType == value.get_type()
          && !read_uncommited_data_())
      {
        int64_t v = 0;
        value.get_modifytime(v);
        if (NULL != session_ctx_
            && v > session_ctx_->get_trans_id())
        {
          TBSYS_LOG(DEBUG, "trans_end v=%ld trans_id=%ld", v, session_ctx_->get_trans_id());
          bret = true;
        }
      }
      return bret;
    }

    int MemTableGetIter::next_cell()
    {
      int ret = OB_SUCCESS;
      if (is_iter_end_)
      {
        ret = OB_ITER_END;
      }

      while (OB_SUCCESS == ret)
      {
        // 一个node下可能会有多个obj 先迭代node
        ret = cell_iter_.next_cell();
        if (OB_ITER_END == ret)
        {
          if (NULL == node_iter_
              || NULL == (node_iter_ = node_iter_->next))
          {
            ret = OB_ITER_END;
            break;
          }
          else
          {
            __builtin_prefetch(node_iter_);

            cell_iter_.reset();

            if (NULL != column_filter_
                && !column_filter_->is_all_column())
            {
              cell_iter_.set_nop();
            }

            if (return_rowkey_column_)
            {
              cell_iter_.set_rowkey_column(te_key_.table_id, te_key_.row_key);
            }

            uint64_t ctime_column_id = OB_UPS_CREATE_TIME_COLUMN_ID(te_key_.table_id);
            cell_iter_.set_ctime_column_id(ctime_column_id);

            uint64_t mtime_column_id = OB_UPS_MODIFY_TIME_COLUMN_ID(te_key_.table_id);
            cell_iter_.set_mtime(mtime_column_id, node_iter_->modify_time);

            cell_iter_.set_cell_info_node(node_iter_);

            cell_iter_.set_head();
            ret = OB_SUCCESS;
            continue;
          }
        }
        if (OB_SUCCESS != ret)
        {
          break;
        }

        ret = cell_iter_.get_cell(ci_.column_id_, ci_.value_);
        if (OB_SUCCESS != ret)
        {
          break;
        }
        TBSYS_LOG(DEBUG, "NEXT_CELL: tid=%ld value=%p node=%p %s",
                  NULL == session_ctx_ ? 0 : session_ctx_->get_trans_id(), te_value_, node_iter_, print_cellinfo(&ci_));
        if (trans_end_(ci_.value_))
        {
          ret = OB_ITER_END;
          break;
        }

        if (NULL != column_filter_
            && !column_filter_->column_exist(ci_.column_id_))
        {
          continue;
        }
        else
        {
          if (ObModifyTimeType == ci_.value_.get_type())
          {
            int64_t v = 0;
            ci_.value_.get_modifytime(v);
            if (INT64_MAX == v
                && read_uncommited_data_())
            {
              ci_.value_.set_modifytime(session_ctx_->get_trans_id());
            }
          }
          else if (ObCreateTimeType == ci_.value_.get_type())
          {
            int64_t v = 0;
            ci_.value_.get_createtime(v);
            if (INT64_MAX == v
                && read_uncommited_data_())
            {
              ci_.value_.set_createtime(session_ctx_->get_trans_id());
            }
          }
          iter_counter_++;
          break;
        }
      }

      if (OB_SUCCESS == ret)
      {
        is_iter_end_ = false;
      }
      else
      {
        is_iter_end_ = true;
      }
      return ret;
    }

    int MemTableGetIter::get_cell(ObCellInfo **cell_info)
    {
      return get_cell(cell_info, NULL);
    }

    int MemTableGetIter::get_cell(ObCellInfo **cell_info, bool *is_row_changed)
    {
      int ret = OB_SUCCESS;
      if (is_iter_end_
          || 0 >= iter_counter_)
      {
        ret = OB_ITER_END;
      }
      else if (NULL == cell_info)
      {
        ret = OB_INVALID_ARGUMENT;
      }
      else
      {
        *cell_info = &ci_;
        if (NULL != is_row_changed)
        {
          *is_row_changed = (1 == iter_counter_);
        }
      }
      return ret;
    }

    bool MemTableGetIter::read_uncommited_data_()
    {
      bool bret = false;
      if (NULL != session_ctx_
          && NULL != te_value_
          && te_value_->row_lock.is_exclusive_locked_by(session_ctx_->get_session_descriptor()))
      {
        bret = true;
      }
      return bret;
    }

    ObCellInfoNode *MemTableGetIter::get_list_head_()
    {
      ObCellInfoNode *ret = NULL;
      if (NULL != te_value_)
      {
        ret = te_value_->list_head;
        if (NULL != te_value_->cur_uc_info
            && read_uncommited_data_())
        {
          if (NULL == ret)
          {
            ret = te_value_->cur_uc_info->uc_list_head;
          }
          else if (NULL != te_value_->list_tail)
          {
            // 将未提交数据接到链表尾部 方便后续遍历链表 但不修改list_tail 因此对事务提交没有影响
            te_value_->list_tail->next = te_value_->cur_uc_info->uc_list_head;
          }
        }
      }
      return ret;
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////

    MemTableScanIter::MemTableScanIter() : te_iter_(),
                                           table_id_(OB_INVALID_ID),
                                           column_filter_(NULL),
                                           return_rowkey_column_(true),
                                           session_ctx_(NULL),
                                           is_iter_end_(true),
                                           get_iter_()
    {
    }

    MemTableScanIter::~MemTableScanIter()
    {
    }

    void MemTableScanIter::reset()
    {
      te_iter_.reset();
      table_id_ = OB_INVALID_ID;
      column_filter_ = NULL;
      return_rowkey_column_ = true;
      session_ctx_ = NULL;
      is_iter_end_ = true;
      get_iter_.reset();
    }

    void MemTableScanIter::set_(const uint64_t table_id,
                                ColumnFilter *column_filter,
                                const bool return_rowkey_column,
                                const BaseSessionCtx *session_ctx)
    {
      table_id_ = table_id;
      column_filter_ = column_filter;
      return_rowkey_column_ = return_rowkey_column;
      session_ctx_ = session_ctx;
      is_iter_end_ = false;
    }

    void MemTableScanIter::set_(const uint64_t table_id,
                                ColumnFilter *column_filter,
                                const TransNode *trans_node)
    {
      table_id_ = table_id;
      column_filter_ = column_filter;
      UNUSED(trans_node);//trans_node_ = trans_node;
      is_iter_end_ = false;
    }

    TableEngineIterator &MemTableScanIter::get_te_iter_()
    {
      return te_iter_;
    }

    bool MemTableScanIter::is_row_not_exist_(MemTableGetIter &get_iter)
    {
      bool bret = false;
      ObCellInfo *ci = NULL;
      if (OB_SUCCESS == get_iter.get_cell(&ci)
          && NULL != ci
          && ObExtendType == ci->value_.get_type()
          && ObActionFlag::OP_ROW_DOES_NOT_EXIST == ci->value_.get_ext())
      {
        bret = true;
      }
      return bret;
    }

    int MemTableScanIter::next_cell()
    {
      int ret = OB_SUCCESS;
      if (is_iter_end_)
      {
        ret = OB_ITER_END;
      }
      else
      {
        ret = get_iter_.next_cell();
        if (OB_ITER_END == ret)
        {
          while (true)
          {
            ret = te_iter_.next();
            if (OB_SUCCESS != ret
                || te_iter_.get_key().table_id != table_id_)
            {
              ret = OB_ITER_END;
              is_iter_end_ = true;
              break;
            }
            //get_iter_.reset();
            get_iter_.set_(te_iter_.get_key(), te_iter_.get_value(), column_filter_, return_rowkey_column_, session_ctx_);
            ret = get_iter_.next_cell();
            if (OB_ITER_END == ret)
            {
              continue;
            }
            if (OB_SUCCESS != ret
                || !is_row_not_exist_(get_iter_))
            {
              break;
            }
          }
        }
      }
      if (OB_SUCCESS == ret)
      {
        is_iter_end_ = false;
      }
      return ret;
    }

    int MemTableScanIter::get_cell(ObCellInfo **cell_info)
    {
      return get_cell(cell_info, NULL);
    }

    int MemTableScanIter::get_cell(ObCellInfo **cell_info, bool *is_row_changed)
    {
      int ret = OB_SUCCESS;
      if (is_iter_end_)
      {
        ret = OB_ITER_END;
      }
      else
      {
        ret = get_iter_.get_cell(cell_info, is_row_changed);
      }
      return ret;
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////

    MemTableIterator::MemTableIterator() : scan_iter_(),
                                           get_iter_(),
                                           iter_(NULL)
    {
    }

    MemTableIterator::~MemTableIterator()
    {
    }

    void MemTableIterator::reset()
    {
      if (&scan_iter_ == iter_)
      {
        scan_iter_.reset();
      }
      if (&get_iter_ == iter_)
      {
        get_iter_.reset();
      }
      iter_ = NULL;
    }

    MemTableScanIter &MemTableIterator::get_scan_iter_()
    {
      iter_ = &scan_iter_;
      return scan_iter_;
    }

    MemTableGetIter &MemTableIterator::get_get_iter_()
    {
      iter_ = &get_iter_;
      return get_iter_;
    }

    int MemTableIterator::next_cell()
    {
      int ret = OB_SUCCESS;
      if (NULL == iter_)
      {
        ret = OB_ERROR;
      }
      else
      {
        ret = iter_->next_cell();
      }
      return ret;
    }

    int MemTableIterator::get_cell(ObCellInfo **cell_info)
    {
      return get_cell(cell_info, NULL);
    }

    int MemTableIterator::get_cell(ObCellInfo **cell_info, bool *is_row_changed)
    {
      int ret = OB_SUCCESS;
      if (NULL == iter_)
      {
        ret = OB_ERROR;
      }
      else
      {
        ret = iter_->get_cell(cell_info, is_row_changed);
      }
      return ret;
    }
  }
}

