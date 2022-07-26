#pragma once
#ifdef ZNS_PLUGIN_ENABLED
#ifndef ZNS_ZONEMETADATA_H
#define ZNS_ZONEMETADATA_H

#include "db/dbformat.h"

namespace ROCKSDB_NAMESPACE {
struct SSZoneMetaData {
  SSZoneMetaData()
      : refs(0), allowed_seeks(1 << 30), number(0), numbers(0), lba_count(0) {}
  int refs;
  int allowed_seeks;  // Seeks allowed until compaction
  uint64_t number;    // version identifier
  struct {
    uint64_t lba{0};  // Lba when no regions are used
  } L0;
  struct {
    uint8_t lba_regions{0};        // Number of start lbas (legal from 1 to 8)
    uint64_t lbas[8];              // start lbas (can be multiple, up to 8)
    uint64_t lba_region_sizes[8];  // Size in zones of an lbas region
  } LN;
  uint64_t numbers;      // number of kv pairs
  uint64_t lba_count;    // data size in lbas
  InternalKey smallest;  // Smallest internal key served by table
  InternalKey largest;   // Largest internal key served by table
};

}  // namespace ROCKSDB_NAMESPACE
#endif
#endif
