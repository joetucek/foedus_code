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
#ifndef FOEDUS_XCT_XCT_ID_HPP_
#define FOEDUS_XCT_XCT_ID_HPP_
#include <stdint.h>

#include <iosfwd>

#include "foedus/assert_nd.hpp"
#include "foedus/compiler.hpp"
#include "foedus/cxx11.hpp"
#include "foedus/epoch.hpp"
#include "foedus/assorted/assorted_func.hpp"
#include "foedus/assorted/atomic_fences.hpp"
#include "foedus/assorted/endianness.hpp"
#include "foedus/assorted/raw_atomics.hpp"
#include "foedus/storage/fwd.hpp"
#include "foedus/thread/fwd.hpp"
#include "foedus/thread/thread_id.hpp"

/**
 * @file foedus/xct/xct_id.hpp
 * @brief Definitions of IDs in this package and a few related constant values.
 * @ingroup XCT
 */
namespace foedus {
namespace xct {

/**
 * @brief Specifies the level of isolation during transaction processing.
 * @ingroup XCT
 * @details
 * May add:
 * \li COMMITTED_READ: see-epoch and read data -> fence -> check-epoch, then forget the read set
 * \li REPEATABLE_READ: assuming no-repeated-access (which we do assume), same as COMMITTED_READ
 *
 * but probably they are superseded either by kDirtyRead or kSnapshot.
 */
enum IsolationLevel {
  /**
   * @brief No guarantee at all for reads, for the sake of best performance and scalability.
   * @details
   * This avoids checking and even storing read set, thus provides the best performance.
   * However, concurrent transactions might be modifying the data the transaction is now reading.
   * So, this has a chance of reading half-changed data.
   * This mode prefers volatile pages if both a snapshot page and a volatile page is available.
   * In other words, more recent but more inconsistent reads compared to kSnapshot.
   */
  kDirtyRead,

  /**
   * @brief Snapshot isolation (SI), meaning the transaction reads a consistent and complete image
   * of the database as of the previous snapshot.
   * @details
   * Writes are same as kSerializable, but all reads
   * simply follow snapshot-pointer from the root, so there is no race, no abort, no verification.
   * Hence, higher scalability than kSerializable.
   * However, this level can result in \e write \e skews.
   * Choose this level if you want highly consistent reads and very high performance.
   * TASK(Hideaki): Allow specifying which snapshot we should be based on. Low priority.
   */
  kSnapshot,

  /**
   * @brief Protects against all anomalies in all situations.
   * @details
   * This is the most expensive level, but everything good has a price.
   * Choose this level if you want full correctness.
   */
  kSerializable,
};

/** Index in thread-local MCS block. 0 means not locked. */
typedef uint32_t McsBlockIndex;
/**
 * When MCS lock contains this value, this means the lock is held by a non-regular guest
 * that doesn't have a context.
 */
const uint32_t kMcsGuestId = -1;

/**
 * Represents an MCS node, a pair of node-owner (thread) and its block index.
 */
union McsNodeUnion {
  uint64_t word;
  struct Components {
    uint32_t      thread_id_;
    McsBlockIndex block_;
  } components;

  bool is_valid() const ALWAYS_INLINE { return components.block_ != 0; }
  bool is_valid_atomic() const ALWAYS_INLINE {
    McsBlockIndex block = assorted::atomic_load_seq_cst<McsBlockIndex>(&components.block_);
    return block != 0;
  }
  void clear() ALWAYS_INLINE { word = 0; }
  void clear_atomic() ALWAYS_INLINE { set_atomic(0, 0); }
  void clear_release() ALWAYS_INLINE { set_release(0, 0); }
  void set(uint32_t thread_id, McsBlockIndex block) ALWAYS_INLINE {
    McsNodeUnion new_value;
    new_value.components.thread_id_ = thread_id;
    new_value.components.block_ = block;
    word = new_value.word;  // don't do *this = new_value. this must be an assignment of one int
  }
  void set_atomic(uint32_t thread_id, McsBlockIndex block) ALWAYS_INLINE {
    McsNodeUnion new_value;
    new_value.components.thread_id_ = thread_id;
    new_value.components.block_ = block;
    // The following is inlined as far as the compile-unit (caller) is compiled with C++11.
    // We observed 5%~ performance difference in TPCC with/without the inlining.
    assorted::atomic_store_seq_cst<uint64_t>(&this->word, new_value.word);
  }
  void set_release(uint32_t thread_id, McsBlockIndex block) ALWAYS_INLINE {
    McsNodeUnion new_value;
    new_value.components.thread_id_ = thread_id;
    new_value.components.block_ = block;
    assorted::atomic_store_release<uint64_t>(&this->word, new_value.word);
  }
};

/** Pre-allocated MCS block. we so far pre-allocate at most 2^16 nodes per thread. */
struct McsBlock {
  /**
   * The successor of MCS lock queue after this thread (in other words, the thread that is
   * waiting for this thread). Successor is represented by thread ID and block,
   * the index in mcs_blocks_.
   */
  McsNodeUnion  successor_;

  /// setter/getter for successor_.
  inline bool             has_successor() const ALWAYS_INLINE { return successor_.is_valid(); }
  inline bool has_successor_atomic() const ALWAYS_INLINE { return successor_.is_valid_atomic(); }
  inline uint32_t         get_successor_thread_id() const ALWAYS_INLINE {
    return successor_.components.thread_id_;
  }
  inline McsBlockIndex    get_successor_block() const ALWAYS_INLINE {
    return successor_.components.block_;
  }
  inline void             clear_successor() ALWAYS_INLINE { successor_.clear(); }
  inline void             clear_successor_atomic() ALWAYS_INLINE { successor_.clear_atomic(); }
  inline void             clear_successor_release() ALWAYS_INLINE { successor_.clear_release(); }
  inline void set_successor(thread::ThreadId thread_id, McsBlockIndex block) ALWAYS_INLINE {
    successor_.set(thread_id, block);
  }
  inline void set_successor_atomic(thread::ThreadId thread_id, McsBlockIndex block) ALWAYS_INLINE {
    successor_.set_atomic(thread_id, block);
  }
  inline void set_successor_release(thread::ThreadId thread_id, McsBlockIndex block) ALWAYS_INLINE {
    successor_.set_release(thread_id, block);
  }
};

struct McsRwBlock {
  static const uint8_t kStateClassMask       = 3U;        // [LSB + 1, LSB + 2]
  static const uint8_t kStateClassReaderFlag = 1U;        // LSB binary = 01
  static const uint8_t kStateClassWriterFlag = 2U;        // LSB binary = 10

  static const uint8_t kStateBlockedFlag     = 1U << 7U;  // MSB binary = 1
  static const uint8_t kStateBlockedMask     = 1U << 7U;

  static const uint8_t kSuccessorClassReader = 1U;
  static const uint8_t kSuccessorClassWriter = 2U;
  static const uint8_t kSuccessorClassNone   = 3U;        // LSB binary 11

  union Self {
    uint16_t data_;                       // +2 => 2
    struct Components {
      uint8_t successor_class_;
      // state_ covers:
      // Bit 0-1: my **own** class (am I a reader or writer?)
      // Bit 7: blocked (am I waiting for the lock or acquired?)
      uint8_t state_;
    } components_;
  } self_;
  // TODO(tzwang): make these two fields 8 bytes by themselves. Now we need
  // to worry about sub-word writes (ie have to use atomic ops even when
  // changing only these two fields because they are in the same byte as data_).
  thread::ThreadId successor_thread_id_;  // +2 => 4
  McsBlockIndex successor_block_index_;   // +4 => 8

  inline void init_reader() {
    self_.components_.state_ = kStateClassReaderFlag | kStateBlockedFlag;
    init_common();
  }
  inline void init_writer() {
    self_.components_.state_ = kStateClassWriterFlag | kStateBlockedFlag;
    init_common();
  }
  inline void init_common() ALWAYS_INLINE {
    self_.components_.successor_class_ = kSuccessorClassNone;
    successor_thread_id_ = 0;
    successor_block_index_ = 0;
    assorted::memory_fence_release();
  }

  inline bool is_reader() ALWAYS_INLINE {
    return (self_.components_.state_ & kStateClassMask) == kStateClassReaderFlag;
  }

  inline void unblock() ALWAYS_INLINE {
    ASSERT_ND(
      assorted::atomic_load_acquire<uint8_t>(&self_.components_.state_) & kStateBlockedFlag);
    assorted::raw_atomic_fetch_and_bitwise_and<uint8_t>(
      &self_.components_.state_,
      static_cast<uint8_t>(~kStateBlockedMask));
    ASSERT_ND(
      !(assorted::atomic_load_acquire<uint8_t>(&self_.components_.state_) & kStateBlockedMask));
  }
  inline bool is_blocked() ALWAYS_INLINE {
    return assorted::atomic_load_acquire<uint8_t>(&self_.components_.state_) & kStateBlockedMask;
  }

  inline void set_successor_class_writer() {
    // In case the caller is a reader appending after a writer or waiting reader,
    // the requester should have already set the successor class to "reader" through by CASing
    // self_.data_ from [no-successor, blocked] to [reader successor, blocked].
    ASSERT_ND(self_.components_.successor_class_ == kSuccessorClassNone);
    assorted::atomic_store_release<uint8_t>(
      &self_.components_.successor_class_,
      kSuccessorClassWriter);
  }
  inline void set_successor_next_only(thread::ThreadId thread_id, McsBlockIndex block_index) {
    McsRwBlock tmp;
    tmp.self_.data_ = 0;
    tmp.successor_thread_id_ = thread_id;
    tmp.successor_block_index_ = block_index;
    ASSERT_ND(successor_thread_id_ == 0);
    ASSERT_ND(successor_block_index_ == 0);
    uint64_t *address = reinterpret_cast<uint64_t*>(this);
    uint64_t mask = *reinterpret_cast<uint64_t*>(&tmp);
    assorted::raw_atomic_fetch_and_bitwise_or<uint64_t>(address, mask);
  }
  inline bool has_successor() {
    return assorted::atomic_load_acquire<uint8_t>(
      &self_.components_.successor_class_) != kSuccessorClassNone;
  }
  inline bool successor_is_ready() {
    // Check block index only - thread ID could be 0
    return assorted::atomic_load_acquire<McsBlockIndex>(&successor_block_index_) != 0;
  }
  inline bool has_reader_successor() {
    uint8_t s = assorted::atomic_load_acquire<uint8_t>(&self_.components_.successor_class_);
    return s == kSuccessorClassReader;
  }
  inline bool has_writer_successor() {
    uint8_t s = assorted::atomic_load_acquire<uint8_t>(&self_.components_.successor_class_);
    return s == kSuccessorClassWriter;
  }

  uint16_t make_blocked_with_reader_successor_state() {
    // Only using the class bit, which doesn't change, so no need to use atomic ops.
    uint8_t state = self_.components_.state_ | kStateBlockedFlag;
    return (uint16_t)state << 8 | kSuccessorClassReader;
  }
  uint16_t make_blocked_with_no_successor_state() {
    uint8_t state = self_.components_.state_ | kStateBlockedFlag;
    return (uint16_t)state << 8 | kSuccessorClassNone;
  }
};

/**
 * @brief An MCS lock data structure.
 * @ingroup XCT
 * @details
 * This is the minimal unit of locking in our system.
 * Unlike SILO, we employ MCS locking that scales much better on big machines.
 * This object stores \e tail-waiter, which indicates the thread that is in the tail of the queue
 * lock, which \e might be the owner of the lock.
 * The MCS-lock nodes are pre-allocated for each thread and placed in shared memory.
 */
struct McsLock {
  McsLock() { data_ = 0; unused_ = 0; }
  McsLock(thread::ThreadId tail_waiter, McsBlockIndex tail_waiter_block) {
    reset(tail_waiter, tail_waiter_block);
    unused_ = 0;
  }

  McsLock(const McsLock& other) CXX11_FUNC_DELETE;
  McsLock& operator=(const McsLock& other) CXX11_FUNC_DELETE;

  /** Used only for sanity check */
  uint8_t   last_1byte_addr() const ALWAYS_INLINE {
    // address is surely a multiply of 4. omit that part.
    ASSERT_ND(reinterpret_cast<uintptr_t>(reinterpret_cast<const void*>(this)) % 4 == 0);
    return reinterpret_cast<uintptr_t>(reinterpret_cast<const void*>(this)) / 4;
  }
  bool      is_locked() const { return (data_ & 0xFFFFU) != 0; }

  /** Equivalent to context->mcs_acquire_lock(this). Actually that's more preferred. */
  McsBlockIndex acquire_lock(thread::Thread* context);
  /** This doesn't use any atomic operation to take a lock. only allowed when there is no race */
  McsBlockIndex initial_lock(thread::Thread* context);
  /** Equivalent to context->mcs_release_lock(this). Actually that's more preferred. */
  void          release_lock(thread::Thread* context, McsBlockIndex block);

  /// The followings are implemented in thread_pimpl.cpp along with the above methods,
  /// but these don't use any of Thread's context information.
  void          ownerless_initial_lock();
  void          ownerless_acquire_lock();
  void          ownerless_release_lock();


  thread::ThreadId get_tail_waiter() const ALWAYS_INLINE { return data_ >> 16U; }
  McsBlockIndex get_tail_waiter_block() const ALWAYS_INLINE { return data_ & 0xFFFFU; }

  /** used only while page initialization */
  void  reset() ALWAYS_INLINE { data_ = 0; }

  void  reset_guest_id_release() {
    assorted::atomic_store_release<uint32_t>(&data_, kMcsGuestId);
  }

  /** used only for initial_lock() */
  void  reset(thread::ThreadId tail_waiter, McsBlockIndex tail_waiter_block) ALWAYS_INLINE {
    data_ = to_int(tail_waiter, tail_waiter_block);
  }

  void  reset_atomic() ALWAYS_INLINE { reset_atomic(0, 0); }
  void  reset_atomic(thread::ThreadId tail_waiter, McsBlockIndex tail_waiter_block) ALWAYS_INLINE {
    uint32_t data = to_int(tail_waiter, tail_waiter_block);
    assorted::atomic_store_seq_cst<uint32_t>(&data_, data);
  }
  void  reset_release() ALWAYS_INLINE { reset_release(0, 0); }
  void  reset_release(thread::ThreadId tail_waiter, McsBlockIndex tail_waiter_block) ALWAYS_INLINE {
    uint32_t data = to_int(tail_waiter, tail_waiter_block);
    assorted::atomic_store_release<uint32_t>(&data_, data);
  }

  static uint32_t to_int(
    thread::ThreadId tail_waiter,
    McsBlockIndex tail_waiter_block) ALWAYS_INLINE {
    ASSERT_ND(tail_waiter_block <= 0xFFFFU);
    return static_cast<uint32_t>(tail_waiter) << 16 | (tail_waiter_block & 0xFFFFU);
  }

  friend std::ostream& operator<<(std::ostream& o, const McsLock& v);

  // these two will become one 64-bit integer.
  uint32_t data_;
  uint32_t unused_;
};

/**
 * @brief An MCS reader-writer lock data structure.
 * @ingroup XCT
 * @details
 * This implements a fair reader-writer lock by the original authors of MCS lock [PPoPP 1991].
 * The version implemented here includes a bug fix due to Keir Fraser (University of Cambridge).
 * See https://www.cs.rochester.edu/research/synchronization/pseudocode/rw.html#s_f for
 * the original pseudocode with the fix.
 *
 * The major use case so far is row-level locking for 2PL.
 *
 * The assumption is that a thread at any instant can be **waiting** for only one MCS lock,
 * so knowing the thread ID suffices to locate the block index as well.
 *
 * TODO(tzwang): add the ownerless variant.
 */
struct McsRwLock {
  static const thread::ThreadId kNextWriterNone = 0xFFFFU;

  McsRwLock() { reset(); }

  McsRwLock(const McsRwLock& other) CXX11_FUNC_DELETE;
  McsRwLock& operator=(const McsRwLock& other) CXX11_FUNC_DELETE;

  McsBlockIndex reader_acquire(thread::Thread* context);
  McsBlockIndex reader_release(thread::Thread* context, McsBlockIndex block);

  McsBlockIndex writer_acquire(thread::Thread* context);
  McsBlockIndex writer_release(thread::Thread* context, McsBlockIndex block);

  void  reset() ALWAYS_INLINE {
    tail_ = readers_count_ = 0;
    next_writer_ = kNextWriterNone;
  }
  void increment_readers_count() ALWAYS_INLINE {
    assorted::raw_atomic_fetch_add<uint16_t>(&readers_count_, 1);
  }
  uint16_t decrement_readers_count() ALWAYS_INLINE {
    return assorted::raw_atomic_fetch_add<uint16_t>(&readers_count_, -1);
  }
  bool is_locked() const { return (tail_ & 0xFFFFU) != 0; }

  static uint32_t to_tail_int(
    thread::ThreadId tail_waiter,
    McsBlockIndex tail_waiter_block) ALWAYS_INLINE {
    ASSERT_ND(tail_waiter_block <= 0xFFFFU);
    return static_cast<uint32_t>(tail_waiter) << 16 | (tail_waiter_block & 0xFFFFU);
  }

  McsBlockIndex get_tail_waiter_block() const ALWAYS_INLINE { return tail_ & 0xFFFFU; }
  thread::ThreadId get_tail_waiter() const ALWAYS_INLINE { return tail_ >> 16U; }
  bool has_next_writer() const ALWAYS_INLINE {return next_writer_ != kNextWriterNone; }

  uint32_t tail_;                 // +4 => 4
  /* FIXME(tzwang): ThreadId starts from 0, so we use 0xFFFF as the "invalid"
   * marker, unless we make the lock even larger than 8 bytes. This essentially
   * limits the largest allowed number of cores we support to 256 sockets x 256
   * cores per socket - 1.
   */
  thread::ThreadId next_writer_;  // +2 => 6
  uint16_t readers_count_;        // +2 => 8

  friend std::ostream& operator<<(std::ostream& o, const McsRwLock& v);
};

const uint64_t kXctIdDeletedBit     = 1ULL << 63;
const uint64_t kXctIdMovedBit       = 1ULL << 62;
const uint64_t kXctIdBeingWrittenBit = 1ULL << 61;
const uint64_t kXctIdNextLayerBit    = 1ULL << 60;
const uint64_t kXctIdMaskSerializer = 0x0FFFFFFFFFFFFFFFULL;
const uint64_t kXctIdMaskEpoch      = 0x0FFFFFFF00000000ULL;
const uint64_t kXctIdMaskOrdinal    = 0x00000000FFFFFFFFULL;

/**
 * @brief Maximum value of in-epoch ordinal.
 * @ingroup XCT
 * @details
 * We reserve 4 bytes in XctId, but in reality 3 bytes are more than enough.
 * By restricting it to within 3 bytes, we can pack more information in a few places.
 */
const uint64_t kMaxXctOrdinal       = (1ULL << 24) - 1U;

/**
 * @brief Persistent status part of Transaction ID
 * @ingroup XCT
 * @details
 * Unlike what [TU13] Sec 4.2 defines, FOEDUS's TID is 128 bit to contain more information.
 * XctId represents a half (64bit) of TID that is used to represent persistent status of the record,
 * such as record versions. The locking-mechanism part is separated to another half; CombinedLock.
 *
 * @par Bit Assignments
 * <table>
 * <tr><th>Bits</th><th>Name</th><th>Description</th></tr>
 * <tr><td>1</td><td>Psuedo-delete bit</td><td>Whether te key is logically non-existent.</td></tr>
 * <tr><td>2</td><td>Moved bit</td><td>This is used for the Master-tree foster-twin protocol.
 * when a record is moved from one page to another during split.</td></tr>
 * <tr><td>3</td><td>BeingWritten</td><td>Before we start applying modifications to a record,
 * we set true to this so that optimistic-read can easily check for half-updated value.
 * After the modification, we set false to this. Of course with appropriate fences.</td></tr>
 * <tr><td>4</td><td>NextLayer</td><td>This is used only in Masstree. This bit indicates whether
 * the record represents a pointer to next layer. False if it is a tuple itself. We put this
 * information as part of XctId because we sometimes have to transactionally know whether the
 * record is a next-layer pointer or not. There is something wrong if a read-set or write-set
 * contains an XctId whose NextLayer bit is ON, because then the record is not a logical tuple.
 * In other words, a reading transaction can efficiently protect their reads on a record that
 * might become a next-layer pointer with a simple check after the usual read protocol.</td></tr>
 * <tr><td>5..32</td><td>Epoch</td><td>The recent owning transaction was in this Epoch.
 * We don't consume full 32 bits for epoch.
 * Assuming 20ms per epoch, 28bit still represents 1 year. All epochs will be refreshed by then
 * or we can have some periodic mantainance job to make it sure.</td></tr>
 * <tr><td>33..64</td><td>Ordinal</td><td>The recent owning transaction had this ordinal
 * in the epoch. We assign 32 bits. Thus we no longer have the case where we have to
 * increment current epoch even when there are many dependencies between transactions.
 * We still have the machanism to do so, but in reality it won't be triggered.
 * </td></tr>
 * </table>
 *
 * @par Greater than/Less than as 64-bit integer
 * The last 60 bits represent the serialization order of the transaction. Sometimes not exactly
 * the chronological order, but enough to assure serializability, see discussion in Sec 4.2 of
 * [TU13]. This class thus provides before() method to check \e strict order of
 * two instantances. Be aware of the following things, though:
 *  \li Epoch might be invalid/uninitialized (zero). An invalid epoch is \e before everything else.
 *  \li Epoch might wrap-around. We use the same wrap-around handling as foedus::Epoch.
 *  \li Ordinal is not a strict ordinal unless there is a dependency between transactions
 * in different cores. In that case, commit protocol adjusts the ordinal for serializability.
 * See [TU13] or their code (gen_commit_tid() in proto2_impl.h).
 *  \li We can \e NOT provide "equals" semantics via simple integer comparison. 61th- bits are
 * status bits, thus we have to mask it. equals_serial_order() does it.
 *
 * @par No Thread-ID
 * This is one difference from SILO. FOEDUS's XctID does not store thread-ID of last commit.
 * We don't use it for any purpose.
 *
 * @par POD
 * This is a POD struct. Default destructor/copy-constructor/assignment operator work fine.
 */
struct XctId {
  XctId() : data_(0) {}

  void set(Epoch::EpochInteger epoch_int, uint32_t ordinal) {
    ASSERT_ND(epoch_int < Epoch::kEpochIntOverflow);
    ASSERT_ND(ordinal <= kMaxXctOrdinal);
    data_ = static_cast<uint64_t>(epoch_int) << 32 | ordinal;
  }

  Epoch   get_epoch() const ALWAYS_INLINE { return Epoch(get_epoch_int()); }
  void    set_epoch(Epoch epoch) ALWAYS_INLINE { set_epoch_int(epoch.value()); }
  Epoch::EpochInteger get_epoch_int() const ALWAYS_INLINE {
    return (data_ & kXctIdMaskEpoch) >> 32;
  }
  void    set_epoch_int(Epoch::EpochInteger epoch_int) ALWAYS_INLINE {
    ASSERT_ND(epoch_int < Epoch::kEpochIntOverflow);
    data_ = (data_ & ~kXctIdMaskEpoch) | (static_cast<uint64_t>(epoch_int) << 32);
  }
  bool    is_valid() const ALWAYS_INLINE { return get_epoch_int() != Epoch::kEpochInvalid; }


  uint32_t  get_ordinal() const ALWAYS_INLINE {
    ASSERT_ND(static_cast<uint32_t>(data_) <= kMaxXctOrdinal);
    return static_cast<uint32_t>(data_);
  }
  void      set_ordinal(uint32_t ordinal) ALWAYS_INLINE {
    ASSERT_ND(ordinal <= kMaxXctOrdinal);
    data_ = (data_ & (~kXctIdMaskOrdinal)) | ordinal;
  }
  void      increment_ordinal() ALWAYS_INLINE {
    uint32_t ordinal = get_ordinal();
    set_ordinal(ordinal + 1U);
  }
  /**
   * Returns -1, 0, 1 when this is less than, same, larger than other in terms of epoch/ordinal
   * @pre this->is_valid(), other.is_valid()
   * @pre this->get_ordinal() != 0, other.get_ordinal() != 0
   */
  int       compare_epoch_and_orginal(const XctId& other) const ALWAYS_INLINE {
    // compare epoch
    if (get_epoch_int() != other.get_epoch_int()) {
      Epoch this_epoch = get_epoch();
      Epoch other_epoch = other.get_epoch();
      ASSERT_ND(this_epoch.is_valid());
      ASSERT_ND(other_epoch.is_valid());
      if (this_epoch < other_epoch) {
        return -1;
      } else {
        ASSERT_ND(this_epoch > other_epoch);
        return 1;
      }
    }

    // if the epoch is the same, compare in_epoch_ordinal_.
    ASSERT_ND(get_epoch() == other.get_epoch());
    if (get_ordinal() < other.get_ordinal()) {
      return -1;
    } else if (get_ordinal() > other.get_ordinal()) {
      return 1;
    } else {
      return 0;
    }
  }

  void    set_being_written() ALWAYS_INLINE { data_ |= kXctIdBeingWrittenBit; }
  void    set_write_complete() ALWAYS_INLINE { data_ &= (~kXctIdBeingWrittenBit); }
  void    set_deleted() ALWAYS_INLINE { data_ |= kXctIdDeletedBit; }
  void    set_notdeleted() ALWAYS_INLINE { data_ &= (~kXctIdDeletedBit); }
  void    set_moved() ALWAYS_INLINE { data_ |= kXctIdMovedBit; }
  void    set_next_layer() ALWAYS_INLINE {
    // Delete-bit has no meaning for a next-layer record. To avoid confusion, turn it off.
    data_ = (data_ & (~kXctIdDeletedBit)) | kXctIdNextLayerBit;
  }
  // note, we should not need this method because becoming a next-layer-pointer is permanent.
  // we never revert it, which simplifies a concurrency control.
  // void    set_not_next_layer() ALWAYS_INLINE { data_ &= (~kXctIdNextLayerBit); }

  bool    is_being_written() const ALWAYS_INLINE { return (data_ & kXctIdBeingWrittenBit) != 0; }
  bool    is_deleted() const ALWAYS_INLINE { return (data_ & kXctIdDeletedBit) != 0; }
  bool    is_moved() const ALWAYS_INLINE { return (data_ & kXctIdMovedBit) != 0; }
  bool    is_next_layer() const ALWAYS_INLINE { return (data_ & kXctIdNextLayerBit) != 0; }
  /** is_moved() || is_next_layer() */
  bool    needs_track_moved() const ALWAYS_INLINE {
    return (data_ & (kXctIdMovedBit | kXctIdNextLayerBit)) != 0;
  }


  bool operator==(const XctId &other) const ALWAYS_INLINE { return data_ == other.data_; }
  bool operator!=(const XctId &other) const ALWAYS_INLINE { return data_ != other.data_; }

  /**
   * @brief Kind of std::max(this, other).
   * @details
   * This relies on the semantics of before(). Thus, this can't differentiate two XctId that
   * differ only in status bits. This method is only used for XctId generation at commit time,
   * so that's fine.
   */
  void store_max(const XctId& other) ALWAYS_INLINE {
    if (!other.is_valid()) {
      return;
    }

    if (before(other)) {
      operator=(other);
    }
  }

  /**
   * Returns if this XctId is \e before other in serialization order, meaning this is either an
   * invalid (unused) epoch or strictly less than the other.
   * @pre other.is_valid()
   */
  bool before(const XctId &other) const ALWAYS_INLINE {
    ASSERT_ND(other.is_valid());
    // compare epoch, then ordinal
    if (get_epoch_int() != other.get_epoch_int()) {
      return get_epoch().before(other.get_epoch());
    }
    return get_ordinal() < other.get_ordinal();
  }

  void clear_status_bits() { data_ &= kXctIdMaskSerializer; }

  friend std::ostream& operator<<(std::ostream& o, const XctId& v);

  uint64_t            data_;
};

/**
 * @brief Transaction ID, a 128-bit data to manage record versions and provide locking mechanism.
 * @ingroup XCT
 * @details
 * This object contains a quite more information compared to SILO [TU13]'s TID.
 * We spend more bits on ordinals and epochs for larger environments, and also employ MCS-locking
 * to be more scalable. Thus, now it's 128-bits.
 * It's not a negligible size, but still compact. Also, 16-bytes sometimes reduce false cacheline
 * sharing (well, then you might ask making it 64 bytes... but that's too much).
 *
 * @par CombinedLock and XctId
 * CombinedLock provides the locking mechanism, namely MCS locking.
 * XctId provides the record version information protected by the lock.
 *
 * @par POD
 * This is a POD struct. Default destructor/copy-constructor/assignment operator work fine.
 */
struct LockableXctId {
  /** the first 64bit: Locking part of TID */
  McsLock       lock_;
  /** the second 64bit: Persistent status part of TID. */
  XctId         xct_id_;

  McsLock* get_key_lock() ALWAYS_INLINE { return &lock_; }
  bool is_keylocked() const ALWAYS_INLINE { return lock_.is_locked(); }
  bool is_deleted() const ALWAYS_INLINE { return xct_id_.is_deleted(); }
  bool is_moved() const ALWAYS_INLINE { return xct_id_.is_moved(); }
  bool is_next_layer() const ALWAYS_INLINE { return xct_id_.is_next_layer(); }
  bool needs_track_moved() const ALWAYS_INLINE { return xct_id_.needs_track_moved(); }
  bool is_being_written() const ALWAYS_INLINE { return xct_id_.is_being_written(); }

  /** used only while page initialization */
  void    reset() ALWAYS_INLINE {
    lock_.reset();
    xct_id_.data_ = 0;
  }
  friend std::ostream& operator<<(std::ostream& o, const LockableXctId& v);
};

/**
 * @brief The MCS reader-writer lock variant of LockableXctId.
 */
struct RwLockableXctId {
  /** the first 64bit: Locking part of TID */
  McsRwLock       lock_;

  /** the second 64bit: Persistent status part of TID. */
  XctId         xct_id_;

  McsRwLock* get_key_lock() ALWAYS_INLINE { return &lock_; }
  bool is_keylocked() const ALWAYS_INLINE { return lock_.is_locked(); }
  bool is_deleted() const ALWAYS_INLINE { return xct_id_.is_deleted(); }
  bool is_moved() const ALWAYS_INLINE { return xct_id_.is_moved(); }
  bool is_next_layer() const ALWAYS_INLINE { return xct_id_.is_next_layer(); }
  bool needs_track_moved() const ALWAYS_INLINE { return xct_id_.needs_track_moved(); }
  bool is_being_written() const ALWAYS_INLINE { return xct_id_.is_being_written(); }

  /** used only while page initialization */
  void    reset() ALWAYS_INLINE {
    lock_.reset();
    xct_id_.data_ = 0;
  }
  friend std::ostream& operator<<(std::ostream& o, const RwLockableXctId& v);
};

/**
 * @brief Auto-release object for MCS locking.
 * @ingroup XCT
 */
struct McsLockScope {
  McsLockScope();
  McsLockScope(
    thread::Thread* context,
    LockableXctId* lock,
    bool acquire_now = true,
    bool non_racy_acquire = false);
  McsLockScope(
    thread::Thread* context,
    McsLock* lock,
    bool acquire_now = true,
    bool non_racy_acquire = false);
  ~McsLockScope();

  /// scope object is movable, but not copiable.
  McsLockScope(const McsLockScope& other) CXX11_FUNC_DELETE;
#ifndef DISABLE_CXX11_IN_PUBLIC_HEADERS
  McsLockScope(McsLockScope&& other);
  McsLockScope& operator=(McsLockScope&& other);
#endif  // DISABLE_CXX11_IN_PUBLIC_HEADERS

  void initialize(thread::Thread* context, McsLock* lock, bool acquire_now, bool non_racy_acquire);

  bool is_valid() const { return lock_; }
  bool is_locked() const { return block_ != 0; }

  /** Acquires the lock. Does nothing if already acquired or !is_valid(). */
  void acquire(bool non_racy_acquire);
  /** Release the lock if acquired. Does nothing if not or !is_valid(). */
  void release();

  /** Just for PageVersionLockScope(McsLockScope*) */
  void move_to(storage::PageVersionLockScope* new_owner);

 private:
  thread::Thread* context_;
  McsLock*        lock_;
  /** Non-0 when locked. 0 when already released or not yet acquired. */
  McsBlockIndex   block_;
};

struct McsRwLockScope {
  explicit McsRwLockScope(bool as_reader);
  McsRwLockScope(
    thread::Thread* context,
    RwLockableXctId* lock,
    bool as_reader,
    bool acquire_now = true);
  McsRwLockScope(
    thread::Thread* context,
    McsRwLock* lock,
    bool as_reader,
    bool acquire_now = true);
  ~McsRwLockScope();

  /// scope object is movable, but not copiable.
  McsRwLockScope(const McsRwLockScope& other) CXX11_FUNC_DELETE;
#ifndef DISABLE_CXX11_IN_PUBLIC_HEADERS
  McsRwLockScope(McsRwLockScope&& other);
  McsRwLockScope& operator=(McsRwLockScope&& other);
#endif  // DISABLE_CXX11_IN_PUBLIC_HEADERS

  void initialize(
    thread::Thread* context,
    McsRwLock* lock,
    bool as_reader,
    bool acquire_now);

  bool is_valid() const { return lock_; }
  bool is_locked() const { return block_ != 0; }

  /** Acquires the lock. Does nothing if already acquired or !is_valid(). */
  void acquire();
  /** Release the lock if acquired. Does nothing if not or !is_valid(). */
  void release();

  /** Just for PageVersionLockScope(McsRwLockScope*) */
  void move_to(storage::PageVersionLockScope* new_owner);

 private:
  thread::Thread* context_;
  McsRwLock*      lock_;
  /** Non-0 when locked. 0 when already released or not yet acquired. */
  McsBlockIndex   block_;
  bool            as_reader_;
};

class McsOwnerlessLockScope {
 public:
  McsOwnerlessLockScope();
  McsOwnerlessLockScope(
    McsLock* lock,
    bool acquire_now = true,
    bool non_racy_acquire = false);
  ~McsOwnerlessLockScope();

  bool is_valid() const { return lock_; }
  bool is_locked_by_me() const { return locked_by_me_; }

  /** Acquires the lock. Does nothing if already acquired or !is_valid(). */
  void acquire(bool non_racy_acquire);
  /** Release the lock if acquired. Does nothing if not or !is_valid(). */
  void release();

 private:
  McsLock*        lock_;
  bool            locked_by_me_;
};

/** Result of track_moved_record(). When failed to track, both null. */
struct TrackMovedRecordResult {
  TrackMovedRecordResult()
    : new_owner_address_(CXX11_NULLPTR), new_payload_address_(CXX11_NULLPTR) {}
  TrackMovedRecordResult(LockableXctId* new_owner_address, char* new_payload_address)
    : new_owner_address_(new_owner_address), new_payload_address_(new_payload_address) {}

  LockableXctId* new_owner_address_;
  char* new_payload_address_;
};


// sizeof(XctId) must be 64 bits.
STATIC_SIZE_CHECK(sizeof(XctId), sizeof(uint64_t))
STATIC_SIZE_CHECK(sizeof(McsLock), 8)
STATIC_SIZE_CHECK(sizeof(LockableXctId), 16)

}  // namespace xct
}  // namespace foedus
#endif  // FOEDUS_XCT_XCT_ID_HPP_
