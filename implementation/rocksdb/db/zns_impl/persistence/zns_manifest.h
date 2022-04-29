#pragma once
#ifdef ZNS_PLUGIN_ENABLED
#ifndef ZNS_MANIFEST_H
#define ZNS_MANIFEST_H

#include "db/zns_impl/io/szd_port.h"
#include "db/zns_impl/persistence/zns_committer.h"
#include "db/zns_impl/ref_counter.h"
#include "rocksdb/slice.h"
#include "rocksdb/status.h"

namespace ROCKSDB_NAMESPACE {
class ZnsManifest : public RefCounter {
 public:
  ZnsManifest(SZD::SZDChannelFactory* channel_factory,
              const SZD::DeviceInfo& info, const uint64_t min_zone_head,
              const uint64_t max_zone_head);
  ~ZnsManifest();
  Status Scan();
  Status NewManifest(const Slice& record);
  Status ReadManifest(std::string* manifest);
  Status GetCurrentWriteHead(uint64_t* current);
  Status SetCurrent(uint64_t current_lba);
  Status Recover();
  Status RemoveObsoleteZones();
  Status Reset();

 private:
  Status RecoverLog();
  Status TryGetCurrent(uint64_t* start_manifest, uint64_t* end_manifest);
  Status TryParseCurrent(uint64_t slba, uint64_t* start_manifest);
  Status ValidateManifestPointers();

  // State
  uint64_t current_lba_;
  uint64_t manifest_start_;
  uint64_t manifest_end_;
  // Log
  uint64_t zone_head_;
  uint64_t write_head_;
  uint64_t zone_tail_;
  // const after init
  const uint64_t min_zone_head_;
  const uint64_t max_zone_head_;
  const uint64_t zone_size_;
  const uint64_t lba_size_;
  // references
  SZD::SZDChannelFactory* channel_factory_;
  SZD::SZDChannel* channel_;
  ZnsCommitter* committer_;
};
}  // namespace ROCKSDB_NAMESPACE
#endif
#endif
