/*
 * Copyright (c) 2014-2015, Hewlett-Packard Development Company, LP.
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details. You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * HP designates this particular file as subject to the "Classpath" exception
 * as provided by HP in the LICENSE.txt file that accompanied this code.
 */

#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <sys/wait.h>

#include <algorithm>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "foedus/engine.hpp"
#include "foedus/engine_options.hpp"
#include "foedus/error_stack.hpp"
#include "foedus/assorted/zipfian_random.hpp"
#include "foedus/debugging/debugging_supports.hpp"
#include "foedus/debugging/stop_watch.hpp"
#include "foedus/fs/filesystem.hpp"
#include "foedus/log/log_manager.hpp"
#include "foedus/memory/engine_memory.hpp"
#include "foedus/snapshot/snapshot_manager.hpp"
#include "foedus/soc/shared_memory_repo.hpp"
#include "foedus/soc/shared_mutex.hpp"
#include "foedus/soc/soc_manager.hpp"
#include "foedus/storage/masstree/masstree_cursor.hpp"
#include "foedus/storage/masstree/masstree_metadata.hpp"
#include "foedus/storage/masstree/masstree_page_impl.hpp"
#include "foedus/storage/masstree/masstree_storage.hpp"
#include "foedus/thread/thread.hpp"
#include "foedus/xct/xct.hpp"
#include "foedus/xct/xct_manager.hpp"
#include "foedus/ycsb/ycsb.hpp"

namespace foedus {
namespace ycsb {

ErrorStack ycsb_client_task(const proc::ProcArguments& args) {
  thread::Thread* context = args.context_;
  if (args.input_len_ != sizeof(YcsbClientTask::Inputs)) {
    return ERROR_STACK(kErrorCodeUserDefined);
  }
  if (args.output_buffer_size_ < sizeof(YcsbClientTask::Outputs)) {
    return ERROR_STACK(kErrorCodeUserDefined);
  }
  *args.output_used_ = sizeof(YcsbClientTask::Outputs);
  const YcsbClientTask::Inputs* inputs
    = reinterpret_cast<const YcsbClientTask::Inputs*>(args.input_buffer_);
  YcsbClientTask task(*inputs, reinterpret_cast<YcsbClientTask::Outputs*>(args.output_buffer_));

  auto result = task.run(context);
  if (result.is_error()) {
    LOG(ERROR) << "YCSB Client-" << task.worker_id() << " exit with an error:" << result;
  }
  ++get_channel(context->get_engine())->exit_nodes_;
  return result;
}

ErrorStack YcsbClientTask::run(thread::Thread* context) {
  context_ = context;
  ASSERT_ND(context_);
  engine_ = context_->get_engine();
  xct_manager_ = engine_->get_xct_manager();
#ifdef YCSB_HASH_STORAGE
  user_table_ = engine_->get_storage_manager()->get_hash("ycsb_user_table");
#else
  user_table_ = engine_->get_storage_manager()->get_masstree("ycsb_user_table");
#endif
  channel_ = get_channel(engine_);
  // TODO(tzwang): so far we only support homogeneous systems: each processor has exactly the same
  // amount of cores. Add support for heterogeneous processors later and let get_total_thread_count
  // figure out how many cores we have (basically by adding individual core counts up).
  uint32_t total_thread_count = engine_->get_options().thread_.get_total_thread_count();

  std::vector<YcsbKey> access_keys;
  access_keys.reserve(workload_.reps_per_tx_ + workload_.rmw_additional_reads_);

  // Wait for the driver's order
  channel_->exit_nodes_--;
  ASSERT_ND(channel_->exit_nodes_ <= total_thread_count);
  channel_->start_rendezvous_.wait();
  LOG(INFO) << "YCSB Client-" << worker_id_
    << " started working on workload " << workload_.desc_ << "!";
  while (!is_stop_requested()) {
    uint16_t xct_type = rnd_xct_select_.uniform_within(1, 100);
    // remember the random seed to repeat the same transaction on abort/retry.
    uint64_t rnd_seed = rnd_xct_select_.get_current_seed();
    uint64_t scan_length_rnd_seed = rnd_scan_length_select_.get_current_seed();

    // Get x different keys first
    if (access_keys.size() == 0) {
      for (int32_t i = 0; i < workload_.reps_per_tx_ + workload_.rmw_additional_reads_; i++) {
        YcsbKey k = build_rmw_key();
        while (std::find(access_keys.begin(), access_keys.end(), k) != access_keys.end()) {
          k = build_rmw_key();
        }
        access_keys.push_back(k);
      }
    }
    ASSERT_ND((int32_t)access_keys.size() == workload_.reps_per_tx_ + workload_.rmw_additional_reads_);

    // abort-retry loop
    while (!is_stop_requested()) {
      rnd_xct_select_.set_current_seed(rnd_seed);
      rnd_scan_length_select_.set_current_seed(scan_length_rnd_seed);
      WRAP_ERROR_CODE(xct_manager_->begin_xct(context, xct::kSerializable));
      ErrorCode ret = kErrorCodeOk;
      if (xct_type <= workload_.insert_percent_) {
        for (int32_t reps = 0; reps < workload_.reps_per_tx_; reps++) {
          YcsbKey key;
          uint32_t high = worker_id_;
          uint32_t* low = &local_key_counter_->key_counter_;
          if (random_inserts_) {
            high = rnd_record_select_.uniform_within(0, total_thread_count - 1);
            low = &(get_local_key_counter(engine_, high)->key_counter_);
          }
          ret = do_insert(build_key(worker_id_, *low));
          // Only increment the key counter if committed to avoid holes in the key space and
          // make sure other thread can get a valid key after peeking my counter
          if (ret == kErrorCodeOk) {
            if (random_inserts_) {
              __sync_fetch_and_add(low, 1);
            } else {
              (*low)++;
            }
          } else {
            break;
          }
        }
      } else {
        if (xct_type <= workload_.read_percent_) {
          for (int32_t reps = 0; reps < workload_.reps_per_tx_; reps++) {
            ret = do_read(access_keys[reps]);
            if (ret != kErrorCodeOk) {
              break;
            }
          }
        } else if (xct_type <= workload_.update_percent_) {
          for (int32_t reps = 0; reps < workload_.reps_per_tx_; reps++) {
            ret = do_update(access_keys[reps]);
            if (ret != kErrorCodeOk) {
              break;
            }
          }
        } else if (xct_type <= workload_.scan_percent_) {
#ifdef YCSB_HASH_STORAGE
          ret = kErrorCodeInvalidParameter;
          COERCE_ERROR_CODE(ret);
#else
          for (int32_t reps = 0; reps < workload_.reps_per_tx_; reps++) {
            auto nrecs = rnd_scan_length_select_.uniform_within(1, max_scan_length());
            increment_total_scans();
            ret = do_scan(access_keys[reps], nrecs);
            if (ret != kErrorCodeOk) {
              break;
            }
          }
#endif
        } else {  // read-modify-write
          for (int32_t i = 0; i < workload_.reps_per_tx_; i++) {
            ret = do_rmw(access_keys[i]);
            if (ret != kErrorCodeOk) {
              break;
            }
          }
          if (ret == kErrorCodeOk) {
            for (int32_t i = 0; i < workload_.rmw_additional_reads_; i++) {
              ret = do_read(access_keys[workload_.reps_per_tx_ + i]);
              if (ret != kErrorCodeOk) {
                break;
              }
            }
          }
        }
      }

      // Done with data access, try to commit
      Epoch commit_epoch;
      if (ret == kErrorCodeOk) {
        ret = xct_manager_->precommit_xct(context_, &commit_epoch);
        if (ret == kErrorCodeOk) {
          ASSERT_ND(!context->is_running_xct());
          access_keys.clear();
          break;
        }
      } else {
        ASSERT_ND(context->is_running_xct());
        WRAP_ERROR_CODE(xct_manager_->abort_xct(context));
      }

      ASSERT_ND(!context->is_running_xct());

      if (ret == kErrorCodeXctRaceAbort) {
        increment_race_aborts();
        continue;
      } else if (ret == kErrorCodeXctPageVersionSetOverflow ||
        ret == kErrorCodeXctPointerSetOverflow ||
        ret == kErrorCodeXctReadSetOverflow ||
        ret == kErrorCodeXctWriteSetOverflow) {
        // this usually doesn't happen, but possible.
        increment_largereadset_aborts();
        continue;
      } else if (random_inserts_ && ret == kErrorCodeStrKeyAlreadyExists) {
        increment_insert_conflict_aborts();
        continue;
      } else {
        increment_unexpected_aborts();
        LOG(WARNING) << "Unexpected error: " << get_error_name(ret);
        if (outputs_->unexpected_aborts_ > kMaxUnexpectedErrors) {
          LOG(ERROR) << "Too many unexpected errors. What's happening?" << get_error_name(ret);
          return ERROR_STACK(ret);
        } else {
          continue;
        }
      }
    }
    ++outputs_->processed_;
    if (UNLIKELY(outputs_->processed_ % (1U << 8) == 0)) {  // it's just stats. not too frequent
      outputs_->snapshot_cache_hits_ = context->get_snapshot_cache_hits();
      outputs_->snapshot_cache_misses_ = context->get_snapshot_cache_misses();
    }
  }
  outputs_->snapshot_cache_hits_ = context->get_snapshot_cache_hits();
  outputs_->snapshot_cache_misses_ = context->get_snapshot_cache_misses();
  return kRetOk;
}

ErrorCode YcsbClientTask::do_read(const YcsbKey& key) {
  YcsbRecord r;
  if (read_all_fields_) {
#ifdef YCSB_HASH_STORAGE
    uint16_t payload_len = sizeof(YcsbRecord);
#else
    foedus::storage::masstree::PayloadLength payload_len = sizeof(YcsbRecord);
#endif
    CHECK_ERROR_CODE(user_table_.get_record(context_, key.ptr(), key.size(), &r, &payload_len));
  } else {
    // Randomly pick one field to read
    uint32_t field = rnd_field_select_.uniform_within(0, kFields - 1);
    uint32_t offset = field * kFieldLength;
    CHECK_ERROR_CODE(user_table_.get_record_part(context_,
      key.ptr(), key.size(), &r.data_[offset], offset, kFieldLength));
  }
  return kErrorCodeOk;
}

ErrorCode YcsbClientTask::do_update(const YcsbKey& key) {
  if (write_all_fields_) {
    YcsbRecord r('b');
    CHECK_ERROR_CODE(
      user_table_.overwrite_record(context_, key.ptr(), key.size(), &r, 0, sizeof(r)));
  } else {
    // Randomly pick one filed to update
    uint32_t field = rnd_field_select_.uniform_within(0, kFields - 1);
    uint32_t offset = field * kFieldLength;
    char f[kFieldLength];
    YcsbRecord::initialize_field(f);
    CHECK_ERROR_CODE(
      user_table_.overwrite_record(context_, key.ptr(), key.size(), f, offset, kFieldLength));
  }
  return kErrorCodeOk;
}

ErrorCode YcsbClientTask::do_rmw(const YcsbKey& key) {
  YcsbRecord r;

  // Read
  if (read_all_fields_) {
#ifdef YCSB_HASH_STORAGE
    uint16_t payload_len = sizeof(YcsbRecord);
#else
    foedus::storage::masstree::PayloadLength payload_len = sizeof(YcsbRecord);
#endif
    CHECK_ERROR_CODE(user_table_.get_record(context_, key.ptr(), key.size(), &r, &payload_len));
  } else {
    // Randomly pick one field to read
    uint32_t field = rnd_field_select_.uniform_within(0, kFields - 1);
    uint32_t offset = field * kFieldLength;
    CHECK_ERROR_CODE(user_table_.get_record_part(context_,
      key.ptr(), key.size(), &r.data_[offset], offset, kFieldLength));
  }

  // Modify-Write
  if (write_all_fields_) {
    r = YcsbRecord('w');
    CHECK_ERROR_CODE(
      user_table_.overwrite_record(context_, key.ptr(), key.size(), &r, 0, sizeof(r)));
  } else {
    // Randomly pick one filed to update
    uint32_t field = rnd_field_select_.uniform_within(0, kFields - 1);
    uint32_t offset = field * kFieldLength;
    char* f = r.get_field(field);
    YcsbRecord::initialize_field(f);  // modify the field
    CHECK_ERROR_CODE(
      user_table_.overwrite_record(context_, key.ptr(), key.size(), f, offset, kFieldLength));
  }
  return kErrorCodeOk;
}

ErrorCode YcsbClientTask::do_insert(const YcsbKey& key) {
  YcsbRecord r('a');
  CHECK_ERROR_CODE(user_table_.insert_record(context_, key.ptr(), key.size(), &r, sizeof(r)));
  return kErrorCodeOk;
}

#ifndef YCSB_HASH_STORAGE
ErrorCode YcsbClientTask::do_scan(const YcsbKey& start_key, uint64_t nrecs) {
  storage::masstree::MasstreeCursor cursor(user_table_, context_);
  CHECK_ERROR_CODE(cursor.open(start_key.ptr(), start_key.size(), nullptr,
    foedus::storage::masstree::MasstreeCursor::kKeyLengthExtremum, true, false, true, false));
  while (nrecs-- && cursor.is_valid_record()) {
    const YcsbRecord *pr = reinterpret_cast<const YcsbRecord *>(cursor.get_payload());
    YcsbRecord r;
    memcpy(&r, pr, sizeof(r));
    increment_total_scan_length();
    cursor.next();
  }
  return kErrorCodeOk;
}
#endif

}  // namespace ycsb
}  // namespace foedus
