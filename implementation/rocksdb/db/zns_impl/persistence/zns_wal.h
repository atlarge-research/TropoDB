#pragma once
#ifdef ZNS_PLUGIN_ENABLED
#ifndef ZNS_WAL_H
#define ZNS_WAL_H

#include "db/zns_impl/io/szd_port.h"
#include "db/zns_impl/memtable/zns_memtable.h"
#include "db/zns_impl/persistence/zns_committer.h"
#include "db/zns_impl/ref_counter.h"
#include "rocksdb/slice.h"
#include "rocksdb/status.h"
#include "rocksdb/types.h"

namespace ROCKSDB_NAMESPACE {
/**
 * @brief
 *
 */
class ZNSWAL : public RefCounter {
 public:
  ZNSWAL(SZD::SZDChannelFactory* channel_factory, const SZD::DeviceInfo& info,
         const uint64_t min_zone_nr, const uint64_t max_zone_nr);
  // No copying or implicits
  ZNSWAL(const ZNSWAL&) = delete;
  ZNSWAL& operator=(const ZNSWAL&) = delete;
  ~ZNSWAL();
  inline Status Append(const Slice& data) {
    return committer_.SafeCommit(data);
  }
  inline Status Reset() { return FromStatus(log_.ResetAll()); }
  inline Status Recover() { return FromStatus(log_.RecoverPointers()); }
  inline bool Empty() { return log_.Empty(); }
  inline bool SpaceLeft(const Slice& data) {
    return log_.SpaceLeft(data.size());
  }

  Status Replay(ZNSMemTable* mem, SequenceNumber* seq);
  void MarkInactive() { committer_.ClearBuffer();}

 private:
  // references
  SZD::SZDChannelFactory* channel_factory_;
  SZD::SZDOnceLog log_;
  ZnsCommitter committer_;
};
}  // namespace ROCKSDB_NAMESPACE
#endif
#endif
