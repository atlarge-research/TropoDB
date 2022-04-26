#pragma once
#ifdef ZNS_PLUGIN_ENABLED
#ifndef ZNS_SSTABLE_H
#define ZNS_SSTABLE_H

#include "db/zns_impl/io/szd_port.h"
#include "db/zns_impl/memtable/zns_memtable.h"
#include "db/zns_impl/ref_counter.h"
#include "db/zns_impl/table/zns_zonemetadata.h"
#include "rocksdb/iterator.h"
#include "rocksdb/slice.h"
#include "rocksdb/status.h"

namespace ROCKSDB_NAMESPACE {
enum class EntryStatus { found, deleted, notfound };

class ZNSSSTableManager;

class SSTableBuilder {
 public:
  SSTableBuilder(){};
  virtual ~SSTableBuilder(){};
  virtual Status Apply(const Slice& key, const Slice& value) = 0;
  virtual Status Finalise() = 0;
  virtual Status Flush() = 0;
  virtual uint64_t GetSize() = 0;
};

class ZnsSSTable {
 public:
  ZnsSSTable(SZD::SZDChannelFactory* channel_factory,
             const SZD::DeviceInfo& info, const uint64_t min_zone_head,
             uint64_t max_zone_head);
  virtual ~ZnsSSTable();
  virtual Status ReadSSTable(Slice* sstable, SSZoneMetaData* meta) = 0;
  virtual Status Get(const InternalKeyComparator& icmp, const Slice& key,
                     std::string* value, SSZoneMetaData* meta,
                     EntryStatus* entry) = 0;
  virtual bool EnoughSpaceAvailable(Slice slice) = 0;
  virtual Status InvalidateSSZone(SSZoneMetaData* meta) = 0;
  virtual SSTableBuilder* NewBuilder(SSZoneMetaData* meta) = 0;
  virtual Status WriteSSTable(Slice content, SSZoneMetaData* meta) = 0;
  virtual Iterator* NewIterator(SSZoneMetaData* meta) = 0;
  virtual void EncodeTo(std::string* dst) = 0;
  virtual bool EncodeFrom(Slice* data) = 0;
  inline uint64_t GetTail() { return write_tail_; }
  inline uint64_t GetHead() { return write_head_; }

 protected:
  friend class SSTableBuilder;
  void PutKVPair(std::string* dst, const Slice& key, const Slice& value);
  void GeneratePreamble(std::string* dst, uint32_t count);

  // data
  uint64_t zone_head_;
  uint64_t write_head_;
  uint64_t zone_tail_;
  uint64_t write_tail_;
  uint64_t min_zone_head_;
  uint64_t max_zone_head_;
  uint64_t zone_size_;
  uint64_t lba_size_;
  // references
  SZD::SZDChannelFactory* channel_factory_;
  SZD::SZDChannel* channel_;
};

int FindSS(const InternalKeyComparator& icmp,
           const std::vector<SSZoneMetaData*>& ss, const Slice& key);
}  // namespace ROCKSDB_NAMESPACE
#endif
#endif
