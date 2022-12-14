#pragma once
#ifdef ZNS_PLUGIN_ENABLED
#ifndef ZNS_DIAGNOSTICS_H
#define ZNS_DIAGNOSTICS_H

#include <vector>

#include "rocksdb/rocksdb_namespace.h"
#include "rocksdb/slice.h"

namespace ROCKSDB_NAMESPACE {
struct ZNSDiagnostics {
  std::string name_;
  uint64_t bytes_written_;
  uint64_t append_operations_counter_;
  uint64_t bytes_read_;
  uint64_t read_operations_counter_;
  uint64_t zones_erased_counter_;
  std::vector<uint64_t> zones_erased_;
  std::vector<uint64_t> append_operations_;
};
}  // namespace ROCKSDB_NAMESPACE

#endif
#endif
