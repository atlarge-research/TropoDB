// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
/**
 * This logic is heavily based on the TwoLevelIterator from LevelDB
 */
#pragma once
#ifdef ZNS_PLUGIN_ENABLED
#ifndef ZNS_SSTABLE_LN_ITERATOR_H
#define ZNS_SSTABLE_LN_ITERATOR_H

#include "db/dbformat.h"
#include "db/zns_impl/table/iterators/iterator_wrapper.h"
#include "db/zns_impl/table/zns_sstable_manager.h"
#include "db/zns_impl/table/zns_zonemetadata.h"
#include "rocksdb/iterator.h"
#include "rocksdb/slice.h"
#include "rocksdb/status.h"

namespace ROCKSDB_NAMESPACE {
/**
 * Iterates over individual SSTables in a Vector of ZNSMetadata.
 */
class LNZoneIterator : public Iterator {
 public:
  LNZoneIterator(const Comparator* cmp,
                 const std::vector<SSZoneMetaData*>* slist,
                 const uint8_t level);
  ~LNZoneIterator();
  bool Valid() const override { return index_ < slist_->size(); }
  Slice key() const override {
    assert(Valid());
    return (*slist_)[index_]->largest.Encode();
  }
  Slice value() const override {
    assert(Valid());
    EncodeFixed64(value_buf_, (*slist_)[index_]->lba_regions);
    for (size_t i = 0; i < (*slist_)[index_]->lba_regions; i++) {
      EncodeFixed64(value_buf_ + 8 * i * 16, (*slist_)[index_]->lbas[i]);
      EncodeFixed64(value_buf_ + 16 * i * 16,
                    (*slist_)[index_]->lba_region_sizes[i]);
    }
    EncodeFixed64(value_buf_ + 8 + 16 * 8, (*slist_)[index_]->lba_count);
    EncodeFixed8(value_buf_ + 16 + 16 * 8, level_);
    return Slice(value_buf_, sizeof(value_buf_));
  }
  Status status() const override { return Status::OK(); }
  void Seek(const Slice& target) override;
  void SeekForPrev(const Slice& target) override;
  void SeekToFirst() override;
  void SeekToLast() override;
  void Next() override;
  void Prev() override;

 private:
  const Comparator* cmp_;
  const uint8_t level_;
  const std::vector<SSZoneMetaData*>* const slist_;
  // Iterator
  size_t index_;
  // This is mutable because value and key are const... As in LevelDB.
  mutable char value_buf_[sizeof(SSZoneMetaData)];
};

typedef Iterator* (*NewZoneIteratorFunction)(void*, const Slice&,
                                             const Comparator*);
class LNIterator : public Iterator {
 public:
  LNIterator(Iterator* ln_iterator, NewZoneIteratorFunction zone_function,
             void* arg, const Comparator* cmp);
  ~LNIterator() override;
  bool Valid() const override { return data_iter_.Valid(); }
  Slice key() const override {
    assert(Valid());
    return data_iter_.key();
  }
  Slice value() const override {
    assert(Valid());
    return data_iter_.value();
  }
  Status status() const override { return Status::OK(); }
  void Seek(const Slice& target) override;
  void SeekForPrev(const Slice& target) override;
  void SeekToFirst() override;
  void SeekToLast() override;
  void Next() override;
  void Prev() override;

 private:
  void SkipEmptyDataLbasForward();
  void SkipEmptyDataLbasBackward();
  void SetDataIterator(Iterator* data_iter);
  void InitDataZone();

  NewZoneIteratorFunction zone_function_;
  void* arg_;
  IteratorWrapper index_iter_;
  IteratorWrapper data_iter_;
  std::string data_zone_handle_;
  const Comparator* cmp_;
};
}  // namespace ROCKSDB_NAMESPACE

#endif
#endif