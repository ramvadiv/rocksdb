//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#pragma once
#include <stdio.h>
#include <memory>
#include <string>
#include <utility>
#include "db/lookup_key.h"
#include "db/merge_context.h"
#include "logging/logging.h"
#include "monitoring/perf_context_imp.h"
#include "rocksdb/comparator.h"
#include "rocksdb/db.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/slice.h"
#include "rocksdb/slice_transform.h"
#include "rocksdb/table.h"
#include "rocksdb/types.h"
#include "util/coding.h"
#include "util/user_comparator_wrapper.h"

namespace ROCKSDB_NAMESPACE {

// The file declares data structures and functions that deal with internal
// keys.
// Each internal key contains a user key, a sequence number (SequenceNumber)
// and a type (ValueType), and they are usually encoded together.
// There are some related helper classes here.

class InternalKey;

// Value types encoded as the last component of internal keys.
// DO NOT CHANGE THESE ENUM VALUES: they are embedded in the on-disk
// data structures.
// The highest bit of the value type needs to be reserved to SST tables
// for them to do more flexible encoding.
enum ValueType : unsigned char {
  kTypeDeletion = 0x0,
  kTypeValue = 0x1,
  kTypeMerge = 0x2,
  kTypeLogData = 0x3,               // WAL only.
  kTypeColumnFamilyDeletion = 0x4,  // WAL only.
  kTypeColumnFamilyValue = 0x5,     // WAL only.
  kTypeColumnFamilyMerge = 0x6,     // WAL only.
  kTypeSingleDeletion = 0x7,
  kTypeColumnFamilySingleDeletion = 0x8,  // WAL only.
  kTypeBeginPrepareXID = 0x9,             // WAL only.
  kTypeEndPrepareXID = 0xA,               // WAL only.
  kTypeCommitXID = 0xB,                   // WAL only.
  kTypeRollbackXID = 0xC,                 // WAL only.
  kTypeNoop = 0xD,                        // WAL only.
  kTypeColumnFamilyRangeDeletion = 0xE,   // WAL only.
  kTypeRangeDeletion = 0xF,               // meta block
  kTypeColumnFamilyBlobIndex = 0x10,      // Blob DB only
  kTypeBlobIndex = 0x11,                  // Blob DB only
  // When the prepared record is also persisted in db, we use a different
  // record. This is to ensure that the WAL that is generated by a WritePolicy
  // is not mistakenly read by another, which would result into data
  // inconsistency.
  kTypeBeginPersistedPrepareXID = 0x12,  // WAL only.
  // Similar to kTypeBeginPersistedPrepareXID, this is to ensure that WAL
  // generated by WriteUnprepared write policy is not mistakenly read by
  // another.
  kTypeBeginUnprepareXID = 0x13,  // WAL only.
  kTypeDeletionWithTimestamp = 0x14,
  kMaxValue = 0x7F  // Not used for storing records.
};

// Defined in dbformat.cc
extern const ValueType kValueTypeForSeek;
extern const ValueType kValueTypeForSeekForPrev;

// Checks whether a type is an inline value type
// (i.e. a type used in memtable skiplist and sst file datablock).
inline bool IsValueType(ValueType t) {
  return t <= kTypeMerge || t == kTypeSingleDeletion || t == kTypeBlobIndex ||
         kTypeDeletionWithTimestamp == t;
}

// Checks whether a type is from user operation
// kTypeRangeDeletion is in meta block so this API is separated from above
inline bool IsExtendedValueType(ValueType t) {
  return IsValueType(t) || t == kTypeRangeDeletion;
}

// We leave eight bits empty at the bottom so a type and sequence#
// can be packed together into 64-bits.
static const SequenceNumber kMaxSequenceNumber = ((0x1ull << 56) - 1);

static const SequenceNumber kDisableGlobalSequenceNumber = port::kMaxUint64;

constexpr uint64_t kNumInternalBytes = 8;

// The data structure that represents an internal key in the way that user_key,
// sequence number and type are stored in separated forms.
struct ParsedInternalKey {
  Slice user_key;
  SequenceNumber sequence;
  ValueType type;

  ParsedInternalKey()
      : sequence(kMaxSequenceNumber),
        type(kTypeDeletion)  // Make code analyzer happy
  {}                         // Intentionally left uninitialized (for speed)
  // u contains timestamp if user timestamp feature is enabled.
  ParsedInternalKey(const Slice& u, const SequenceNumber& seq, ValueType t)
      : user_key(u), sequence(seq), type(t) {}
  // std::string DebugString(bool log_err_key, bool hex = false) const;
  std::string DebugString(bool log_err_key, bool hex) const;

  void clear() {
    user_key.clear();
    sequence = 0;
    type = kTypeDeletion;
  }
};

// Return the length of the encoding of "key".
inline size_t InternalKeyEncodingLength(const ParsedInternalKey& key) {
  return key.user_key.size() + kNumInternalBytes;
}

// Pack a sequence number and a ValueType into a uint64_t
inline uint64_t PackSequenceAndType(uint64_t seq, ValueType t) {
  assert(seq <= kMaxSequenceNumber);
  assert(IsExtendedValueType(t));
  return (seq << 8) | t;
}

// Given the result of PackSequenceAndType, store the sequence number in *seq
// and the ValueType in *t.
inline void UnPackSequenceAndType(uint64_t packed, uint64_t* seq,
                                  ValueType* t) {
  *seq = packed >> 8;
  *t = static_cast<ValueType>(packed & 0xff);

  assert(*seq <= kMaxSequenceNumber);
  assert(IsExtendedValueType(*t));
}

EntryType GetEntryType(ValueType value_type);

// Append the serialization of "key" to *result.
extern void AppendInternalKey(std::string* result,
                              const ParsedInternalKey& key);

// Append the serialization of "key" to *result, replacing the original
// timestamp with argument ts.
extern void AppendInternalKeyWithDifferentTimestamp(
    std::string* result, const ParsedInternalKey& key, const Slice& ts);

// Serialized internal key consists of user key followed by footer.
// This function appends the footer to *result, assuming that *result already
// contains the user key at the end.
extern void AppendInternalKeyFooter(std::string* result, SequenceNumber s,
                                    ValueType t);

// Attempt to parse an internal key from "internal_key".  On success,
// stores the parsed data in "*result", and returns true.
//
// On error, returns false, leaves "*result" in an undefined state.
extern Status ParseInternalKey(const Slice& internal_key,
                               ParsedInternalKey* result,
                               bool log_err_key = true);

// Returns the user key portion of an internal key.
inline Slice ExtractUserKey(const Slice& internal_key) {
  assert(internal_key.size() >= kNumInternalBytes);
  return Slice(internal_key.data(), internal_key.size() - kNumInternalBytes);
}

inline Slice ExtractUserKeyAndStripTimestamp(const Slice& internal_key,
                                             size_t ts_sz) {
  assert(internal_key.size() >= kNumInternalBytes + ts_sz);
  return Slice(internal_key.data(),
               internal_key.size() - kNumInternalBytes - ts_sz);
}

inline Slice StripTimestampFromUserKey(const Slice& user_key, size_t ts_sz) {
  assert(user_key.size() >= ts_sz);
  return Slice(user_key.data(), user_key.size() - ts_sz);
}

inline Slice ExtractTimestampFromUserKey(const Slice& user_key, size_t ts_sz) {
  assert(user_key.size() >= ts_sz);
  return Slice(user_key.data() + user_key.size() - ts_sz, ts_sz);
}

inline uint64_t ExtractInternalKeyFooter(const Slice& internal_key) {
  assert(internal_key.size() >= kNumInternalBytes);
  const size_t n = internal_key.size();
  return DecodeFixed64(internal_key.data() + n - kNumInternalBytes);
}

inline ValueType ExtractValueType(const Slice& internal_key) {
  uint64_t num = ExtractInternalKeyFooter(internal_key);
  unsigned char c = num & 0xff;
  return static_cast<ValueType>(c);
}

// A comparator for internal keys that uses a specified comparator for
// the user key portion and breaks ties by decreasing sequence number.
class InternalKeyComparator
#ifdef NDEBUG
    final
#endif
    : public Comparator {
 private:
  UserComparatorWrapper user_comparator_;
  std::string name_;

 public:
  // `InternalKeyComparator`s constructed with the default constructor are not
  // usable and will segfault on any attempt to use them for comparisons.
  InternalKeyComparator() = default;

  // @param named If true, assign a name to this comparator based on the
  //    underlying comparator's name. This involves an allocation and copy in
  //    this constructor to precompute the result of `Name()`. To avoid this
  //    overhead, set `named` to false. In that case, `Name()` will return a
  //    generic name that is non-specific to the underlying comparator.
  explicit InternalKeyComparator(const Comparator* c, bool named = true)
      : Comparator(c->timestamp_size()), user_comparator_(c) {
    if (named) {
      name_ = "rocksdb.InternalKeyComparator:" +
              std::string(user_comparator_.Name());
    }
  }
  virtual ~InternalKeyComparator() {}

  virtual const char* Name() const override;
  virtual int Compare(const Slice& a, const Slice& b) const override;
  // Same as Compare except that it excludes the value type from comparison
  virtual int CompareKeySeq(const Slice& a, const Slice& b) const;
  virtual void FindShortestSeparator(std::string* start,
                                     const Slice& limit) const override;
  virtual void FindShortSuccessor(std::string* key) const override;

  const Comparator* user_comparator() const {
    return user_comparator_.user_comparator();
  }

  int Compare(const InternalKey& a, const InternalKey& b) const;
  int Compare(const ParsedInternalKey& a, const ParsedInternalKey& b) const;
  // In this `Compare()` overload, the sequence numbers provided in
  // `a_global_seqno` and `b_global_seqno` override the sequence numbers in `a`
  // and `b`, respectively. To disable sequence number override(s), provide the
  // value `kDisableGlobalSequenceNumber`.
  int Compare(const Slice& a, SequenceNumber a_global_seqno, const Slice& b,
              SequenceNumber b_global_seqno) const;
  virtual const Comparator* GetRootComparator() const override {
    return user_comparator_.GetRootComparator();
  }
};

// The class represent the internal key in encoded form.
class InternalKey {
 private:
  std::string rep_;

 public:
  InternalKey() {}  // Leave rep_ as empty to indicate it is invalid
  InternalKey(const Slice& _user_key, SequenceNumber s, ValueType t) {
    AppendInternalKey(&rep_, ParsedInternalKey(_user_key, s, t));
  }

  // sets the internal key to be bigger or equal to all internal keys with this
  // user key
  void SetMaxPossibleForUserKey(const Slice& _user_key) {
    AppendInternalKey(
        &rep_, ParsedInternalKey(_user_key, 0, static_cast<ValueType>(0)));
  }

  // sets the internal key to be smaller or equal to all internal keys with this
  // user key
  void SetMinPossibleForUserKey(const Slice& _user_key) {
    AppendInternalKey(&rep_, ParsedInternalKey(_user_key, kMaxSequenceNumber,
                                               kValueTypeForSeek));
  }

  bool Valid() const {
    ParsedInternalKey parsed;
    return (ParseInternalKey(Slice(rep_), &parsed).ok());
  }

  void DecodeFrom(const Slice& s) { rep_.assign(s.data(), s.size()); }
  Slice Encode() const {
    assert(!rep_.empty());
    return rep_;
  }

  Slice user_key() const { return ExtractUserKey(rep_); }
  size_t size() { return rep_.size(); }

  void Set(const Slice& _user_key, SequenceNumber s, ValueType t) {
    SetFrom(ParsedInternalKey(_user_key, s, t));
  }

  void SetFrom(const ParsedInternalKey& p) {
    rep_.clear();
    AppendInternalKey(&rep_, p);
  }

  void Clear() { rep_.clear(); }

  // The underlying representation.
  // Intended only to be used together with ConvertFromUserKey().
  std::string* rep() { return &rep_; }

  // Assuming that *rep() contains a user key, this method makes internal key
  // out of it in-place. This saves a memcpy compared to Set()/SetFrom().
  void ConvertFromUserKey(SequenceNumber s, ValueType t) {
    AppendInternalKeyFooter(&rep_, s, t);
  }

  // std::string DebugString(bool hex = false) const;
  std::string DebugString(bool hex) const;
};

inline int InternalKeyComparator::Compare(const InternalKey& a,
                                          const InternalKey& b) const {
  return Compare(a.Encode(), b.Encode());
}

inline Status ParseInternalKey(const Slice& internal_key,
                               ParsedInternalKey* result, bool log_err_key) {
  const size_t n = internal_key.size();

  if (n < kNumInternalBytes) {
    return Status::Corruption(
        "Corrupted Key: Internal Key too small. Size=" + std::to_string(n) + ". ");
  }

  uint64_t num = DecodeFixed64(internal_key.data() + n - kNumInternalBytes);
  unsigned char c = num & 0xff;
  result->sequence = num >> 8;
  result->type = static_cast<ValueType>(c);
  assert(result->type <= ValueType::kMaxValue);
  result->user_key = Slice(internal_key.data(), n - kNumInternalBytes);

  if (IsExtendedValueType(result->type)) {
    return Status::OK();
  } else {
    // return ReturnCorruptKeyInfo(result, log_err_key);
    return Status::Corruption("Corrupted Key",
                              result->DebugString(log_err_key, true));
  }
}

// Update the sequence number in the internal key.
// Guarantees not to invalidate ikey.data().
inline void UpdateInternalKey(std::string* ikey, uint64_t seq, ValueType t) {
  size_t ikey_sz = ikey->size();
  assert(ikey_sz >= kNumInternalBytes);
  uint64_t newval = (seq << 8) | t;

  // Note: Since C++11, strings are guaranteed to be stored contiguously and
  // string::operator[]() is guaranteed not to change ikey.data().
  EncodeFixed64(&(*ikey)[ikey_sz - kNumInternalBytes], newval);
}

// Get the sequence number from the internal key
inline uint64_t GetInternalKeySeqno(const Slice& internal_key) {
  const size_t n = internal_key.size();
  assert(n >= kNumInternalBytes);
  uint64_t num = DecodeFixed64(internal_key.data() + n - kNumInternalBytes);
  return num >> 8;
}

// The class to store keys in an efficient way. It allows:
// 1. Users can either copy the key into it, or have it point to an unowned
//    address.
// 2. For copied key, a short inline buffer is kept to reduce memory
//    allocation for smaller keys.
// 3. It tracks user key or internal key, and allow conversion between them.
class IterKey {
 public:
  IterKey()
      : buf_(space_),
        key_(buf_),
        key_size_(0),
        buf_size_(sizeof(space_)),
        is_user_key_(true) {}
  // No copying allowed
  IterKey(const IterKey&) = delete;
  void operator=(const IterKey&) = delete;

  ~IterKey() { ResetBuffer(); }

  // The bool will be picked up by the next calls to SetKey
  void SetIsUserKey(bool is_user_key) { is_user_key_ = is_user_key; }

  // Returns the key in whichever format that was provided to KeyIter
  Slice GetKey() const { return Slice(key_, key_size_); }

  Slice GetInternalKey() const {
    assert(!IsUserKey());
    return Slice(key_, key_size_);
  }

  Slice GetUserKey() const {
    if (IsUserKey()) {
      return Slice(key_, key_size_);
    } else {
      assert(key_size_ >= kNumInternalBytes);
      return Slice(key_, key_size_ - kNumInternalBytes);
    }
  }

  size_t Size() const { return key_size_; }

  void Clear() { key_size_ = 0; }

  // Append "non_shared_data" to its back, from "shared_len"
  // This function is used in Block::Iter::ParseNextKey
  // shared_len: bytes in [0, shard_len-1] would be remained
  // non_shared_data: data to be append, its length must be >= non_shared_len
  void TrimAppend(const size_t shared_len, const char* non_shared_data,
                  const size_t non_shared_len) {
    assert(shared_len <= key_size_);
    size_t total_size = shared_len + non_shared_len;

    if (IsKeyPinned() /* key is not in buf_ */) {
      // Copy the key from external memory to buf_ (copy shared_len bytes)
      EnlargeBufferIfNeeded(total_size);
      memcpy(buf_, key_, shared_len);
    } else if (total_size > buf_size_) {
      // Need to allocate space, delete previous space
      char* p = new char[total_size];
      memcpy(p, key_, shared_len);

      if (buf_ != space_) {
        delete[] buf_;
      }

      buf_ = p;
      buf_size_ = total_size;
    }

    memcpy(buf_ + shared_len, non_shared_data, non_shared_len);
    key_ = buf_;
    key_size_ = total_size;
  }

  Slice SetKey(const Slice& key, bool copy = true) {
    // is_user_key_ expected to be set already via SetIsUserKey
    return SetKeyImpl(key, copy);
  }

  Slice SetUserKey(const Slice& key, bool copy = true) {
    is_user_key_ = true;
    return SetKeyImpl(key, copy);
  }

  Slice SetInternalKey(const Slice& key, bool copy = true) {
    is_user_key_ = false;
    return SetKeyImpl(key, copy);
  }

  // Copies the content of key, updates the reference to the user key in ikey
  // and returns a Slice referencing the new copy.
  Slice SetInternalKey(const Slice& key, ParsedInternalKey* ikey) {
    size_t key_n = key.size();
    assert(key_n >= kNumInternalBytes);
    SetInternalKey(key);
    ikey->user_key = Slice(key_, key_n - kNumInternalBytes);
    return Slice(key_, key_n);
  }

  // Copy the key into IterKey own buf_
  void OwnKey() {
    assert(IsKeyPinned() == true);

    Reserve(key_size_);
    memcpy(buf_, key_, key_size_);
    key_ = buf_;
  }

  // Update the sequence number in the internal key.  Guarantees not to
  // invalidate slices to the key (and the user key).
  void UpdateInternalKey(uint64_t seq, ValueType t) {
    assert(!IsKeyPinned());
    assert(key_size_ >= kNumInternalBytes);
    uint64_t newval = (seq << 8) | t;
    EncodeFixed64(&buf_[key_size_ - kNumInternalBytes], newval);
  }

  bool IsKeyPinned() const { return (key_ != buf_); }

  void SetInternalKey(const Slice& key_prefix, const Slice& user_key,
                      SequenceNumber s,
                      ValueType value_type = kValueTypeForSeek,
                      const Slice* ts = nullptr) {
    size_t psize = key_prefix.size();
    size_t usize = user_key.size();
    size_t ts_sz = (ts != nullptr ? ts->size() : 0);
    EnlargeBufferIfNeeded(psize + usize + sizeof(uint64_t) + ts_sz);
    if (psize > 0) {
      memcpy(buf_, key_prefix.data(), psize);
    }
    memcpy(buf_ + psize, user_key.data(), usize);
    if (ts) {
      memcpy(buf_ + psize + usize, ts->data(), ts_sz);
    }
    EncodeFixed64(buf_ + usize + psize + ts_sz,
                  PackSequenceAndType(s, value_type));

    key_ = buf_;
    key_size_ = psize + usize + sizeof(uint64_t) + ts_sz;
    is_user_key_ = false;
  }

  void SetInternalKey(const Slice& user_key, SequenceNumber s,
                      ValueType value_type = kValueTypeForSeek,
                      const Slice* ts = nullptr) {
    SetInternalKey(Slice(), user_key, s, value_type, ts);
  }

  void Reserve(size_t size) {
    EnlargeBufferIfNeeded(size);
    key_size_ = size;
  }

  void SetInternalKey(const ParsedInternalKey& parsed_key) {
    SetInternalKey(Slice(), parsed_key);
  }

  void SetInternalKey(const Slice& key_prefix,
                      const ParsedInternalKey& parsed_key_suffix) {
    SetInternalKey(key_prefix, parsed_key_suffix.user_key,
                   parsed_key_suffix.sequence, parsed_key_suffix.type);
  }

  void EncodeLengthPrefixedKey(const Slice& key) {
    auto size = key.size();
    EnlargeBufferIfNeeded(size + static_cast<size_t>(VarintLength(size)));
    char* ptr = EncodeVarint32(buf_, static_cast<uint32_t>(size));
    memcpy(ptr, key.data(), size);
    key_ = buf_;
    is_user_key_ = true;
  }

  bool IsUserKey() const { return is_user_key_; }

 private:
  char* buf_;
  const char* key_;
  size_t key_size_;
  size_t buf_size_;
  char space_[32];  // Avoid allocation for short keys
  bool is_user_key_;

  Slice SetKeyImpl(const Slice& key, bool copy) {
    size_t size = key.size();
    if (copy) {
      // Copy key to buf_
      EnlargeBufferIfNeeded(size);
      memcpy(buf_, key.data(), size);
      key_ = buf_;
    } else {
      // Update key_ to point to external memory
      key_ = key.data();
    }
    key_size_ = size;
    return Slice(key_, key_size_);
  }

  void ResetBuffer() {
    if (buf_ != space_) {
      delete[] buf_;
      buf_ = space_;
    }
    buf_size_ = sizeof(space_);
    key_size_ = 0;
  }

  // Enlarge the buffer size if needed based on key_size.
  // By default, static allocated buffer is used. Once there is a key
  // larger than the static allocated buffer, another buffer is dynamically
  // allocated, until a larger key buffer is requested. In that case, we
  // reallocate buffer and delete the old one.
  void EnlargeBufferIfNeeded(size_t key_size) {
    // If size is smaller than buffer size, continue using current buffer,
    // or the static allocated one, as default
    if (key_size > buf_size_) {
      EnlargeBuffer(key_size);
    }
  }

  void EnlargeBuffer(size_t key_size);
};

// Convert from a SliceTranform of user keys, to a SliceTransform of
// user keys.
class InternalKeySliceTransform : public SliceTransform {
 public:
  explicit InternalKeySliceTransform(const SliceTransform* transform)
      : transform_(transform) {}

  virtual const char* Name() const override { return transform_->Name(); }

  virtual Slice Transform(const Slice& src) const override {
    auto user_key = ExtractUserKey(src);
    return transform_->Transform(user_key);
  }

  virtual bool InDomain(const Slice& src) const override {
    auto user_key = ExtractUserKey(src);
    return transform_->InDomain(user_key);
  }

  virtual bool InRange(const Slice& dst) const override {
    auto user_key = ExtractUserKey(dst);
    return transform_->InRange(user_key);
  }

  const SliceTransform* user_prefix_extractor() const { return transform_; }

 private:
  // Like comparator, InternalKeySliceTransform will not take care of the
  // deletion of transform_
  const SliceTransform* const transform_;
};

// Read the key of a record from a write batch.
// if this record represent the default column family then cf_record
// must be passed as false, otherwise it must be passed as true.
extern bool ReadKeyFromWriteBatchEntry(Slice* input, Slice* key,
                                       bool cf_record);

// Read record from a write batch piece from input.
// tag, column_family, key, value and blob are return values. Callers own the
// Slice they point to.
// Tag is defined as ValueType.
// input will be advanced to after the record.
extern Status ReadRecordFromWriteBatch(Slice* input, char* tag,
                                       uint32_t* column_family, Slice* key,
                                       Slice* value, Slice* blob, Slice* xid);

// When user call DeleteRange() to delete a range of keys,
// we will store a serialized RangeTombstone in MemTable and SST.
// the struct here is a easy-understood form
// start/end_key_ is the start/end user key of the range to be deleted
struct RangeTombstone {
  Slice start_key_;
  Slice end_key_;
  SequenceNumber seq_;
  RangeTombstone() = default;
  RangeTombstone(Slice sk, Slice ek, SequenceNumber sn)
      : start_key_(sk), end_key_(ek), seq_(sn) {}

  RangeTombstone(ParsedInternalKey parsed_key, Slice value) {
    start_key_ = parsed_key.user_key;
    seq_ = parsed_key.sequence;
    end_key_ = value;
  }

  // be careful to use Serialize(), allocates new memory
  std::pair<InternalKey, Slice> Serialize() const {
    auto key = InternalKey(start_key_, seq_, kTypeRangeDeletion);
    Slice value = end_key_;
    return std::make_pair(std::move(key), std::move(value));
  }

  // be careful to use SerializeKey(), allocates new memory
  InternalKey SerializeKey() const {
    return InternalKey(start_key_, seq_, kTypeRangeDeletion);
  }

  // The tombstone end-key is exclusive, so we generate an internal-key here
  // which has a similar property. Using kMaxSequenceNumber guarantees that
  // the returned internal-key will compare less than any other internal-key
  // with the same user-key. This in turn guarantees that the serialized
  // end-key for a tombstone such as [a-b] will compare less than the key "b".
  //
  // be careful to use SerializeEndKey(), allocates new memory
  InternalKey SerializeEndKey() const {
    return InternalKey(end_key_, kMaxSequenceNumber, kTypeRangeDeletion);
  }
};

inline int InternalKeyComparator::Compare(const Slice& akey,
                                          const Slice& bkey) const {
  // Order by:
  //    increasing user key (according to user-supplied comparator)
  //    decreasing sequence number
  //    decreasing type (though sequence# should be enough to disambiguate)
  int r = user_comparator_.Compare(ExtractUserKey(akey), ExtractUserKey(bkey));
  if (r == 0) {
    const uint64_t anum =
        DecodeFixed64(akey.data() + akey.size() - kNumInternalBytes);
    const uint64_t bnum =
        DecodeFixed64(bkey.data() + bkey.size() - kNumInternalBytes);
    if (anum > bnum) {
      r = -1;
    } else if (anum < bnum) {
      r = +1;
    }
  }
  return r;
}

inline int InternalKeyComparator::CompareKeySeq(const Slice& akey,
                                                const Slice& bkey) const {
  // Order by:
  //    increasing user key (according to user-supplied comparator)
  //    decreasing sequence number
  int r = user_comparator_.Compare(ExtractUserKey(akey), ExtractUserKey(bkey));
  if (r == 0) {
    // Shift the number to exclude the last byte which contains the value type
    const uint64_t anum =
        DecodeFixed64(akey.data() + akey.size() - kNumInternalBytes) >> 8;
    const uint64_t bnum =
        DecodeFixed64(bkey.data() + bkey.size() - kNumInternalBytes) >> 8;
    if (anum > bnum) {
      r = -1;
    } else if (anum < bnum) {
      r = +1;
    }
  }
  return r;
}

inline int InternalKeyComparator::Compare(const Slice& a,
                                          SequenceNumber a_global_seqno,
                                          const Slice& b,
                                          SequenceNumber b_global_seqno) const {
  int r = user_comparator_.Compare(ExtractUserKey(a), ExtractUserKey(b));
  if (r == 0) {
    uint64_t a_footer, b_footer;
    if (a_global_seqno == kDisableGlobalSequenceNumber) {
      a_footer = ExtractInternalKeyFooter(a);
    } else {
      a_footer = PackSequenceAndType(a_global_seqno, ExtractValueType(a));
    }
    if (b_global_seqno == kDisableGlobalSequenceNumber) {
      b_footer = ExtractInternalKeyFooter(b);
    } else {
      b_footer = PackSequenceAndType(b_global_seqno, ExtractValueType(b));
    }
    if (a_footer > b_footer) {
      r = -1;
    } else if (a_footer < b_footer) {
      r = +1;
    }
  }
  return r;
}

// Wrap InternalKeyComparator as a comparator class for ParsedInternalKey.
struct ParsedInternalKeyComparator {
  explicit ParsedInternalKeyComparator(const InternalKeyComparator* c)
      : cmp(c) {}

  bool operator()(const ParsedInternalKey& a,
                  const ParsedInternalKey& b) const {
    return cmp->Compare(a, b) < 0;
  }

  const InternalKeyComparator* cmp;
};

}  // namespace ROCKSDB_NAMESPACE
