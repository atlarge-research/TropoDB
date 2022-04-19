// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/zns_impl/db_impl_zns.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "db/column_family.h"
#include "db/memtable.h"
#include "db/write_batch_internal.h"
#include "db/zns_impl/index/zns_compaction.h"
#include "db/zns_impl/index/zns_version.h"
#include "db/zns_impl/index/zns_version_set.h"
#include "db/zns_impl/io/device_wrapper.h"
#include "db/zns_impl/persistence/zns_manifest.h"
#include "db/zns_impl/table/zns_sstable_manager.h"
#include "port/port.h"
#include "rocksdb/db.h"
#include "rocksdb/file_checksum.h"
#include "rocksdb/listener.h"
#include "rocksdb/metadata.h"
#include "rocksdb/options.h"
#include "rocksdb/slice.h"
#include "rocksdb/status.h"
#include "rocksdb/transaction_log.h"
#include "rocksdb/write_batch.h"
#include "rocksdb/write_buffer_manager.h"
#include "util/coding.h"

namespace ROCKSDB_NAMESPACE {

const int kNumNonTableCacheFiles = 10;

DBImplZNS::DBImplZNS(const DBOptions& options, const std::string& dbname,
                     const bool seq_per_batch, const bool batch_per_txn,
                     bool read_only)
    : options_(options),
      name_(dbname),
      env_(options.env),
      internal_comparator_(BytewiseComparator()),
      mem_(nullptr),
      imm_(nullptr),
      versions_(nullptr),
      bg_work_finished_signal_(&mutex_),
      bg_compaction_scheduled_(false) {}

Status DBImplZNS::NewDB() { return Status::OK(); }

// Default implementations of convenience methods that subclasses of DB
// can call if they wish
Status DBImplZNS::Put(const WriteOptions& options, const Slice& key,
                      const Slice& value) {
  // create writebatch of size key + value + 24 bytes.8 bytes are taken by
  // header(sequence number), 4 bytes for count, 1 byte for type + 11 for size.
  // then write the batch. the batch does some stuff with cf, that we do not
  // care about.  See top write_batch.cc for some awesome insights. Some flags
  // are stored as well, protected information and save points and commits can
  // be made (friend classes..) Then it moves into WriteImpl. writes have a prio
  // which can be assigned (ignore for now I guess...) Get counts from
  // WriteBatch back. Then write to WAL
  //    Write to WAL: separate thread (WAL thread?) something with batch
  //    groups..
  // then one write to the memtable, which we can copy I guess.
  // multiple variants, unordered writes, pipelined writes etc.
  // NOTE, no flushing or compaction at all! probably a different threads
  WriteBatch batch;
  batch.Put(key, value);
  return Write(options, &batch);
}

Status DBImplZNS::Put(const WriteOptions& options,
                      ColumnFamilyHandle* column_family, const Slice& key,
                      const Slice& value) {
  return Status::NotSupported("Column families not supported");
}
Status DBImplZNS::Put(const WriteOptions& options,
                      ColumnFamilyHandle* column_family, const Slice& key,
                      const Slice& ts, const Slice& value) {
  return Status::NotSupported("Column families not supported");
}

Status DBImplZNS::Get(const ReadOptions& options, const Slice& key,
                      std::string* value) {
  MutexLock l(&mutex_);
  Status s;
  /* GetImpl
  1. tracing, we dont care.
  2. sequence number? not relevant as of now. probably problematic for
  flushes...
  3. memtable lookup, copy? requires the superversion...
  4. if IsMergeInProgress!, look in immutable as well.
  5. can also return merge operands??? then get does nothing on normal table
  (except return) and gets operands for imm when exists. SLOW!
  6. get version logic. think for sstable locations? differentiates on action.
  **/

  // This is absolutely necessary for locking logic because private pointers can
  // be changed in background work.
  ZNSMemTable* mem = mem_;
  ZNSMemTable* imm = imm_;
  ZnsVersion* current = versions_->current();
  mem->Ref();
  if (imm != nullptr) imm->Ref();
  current->Ref();
  LookupKey lkey(key, versions_->LastSequence());
  {
    mutex_.Unlock();
    if (mem->Get(options, lkey, value, &s)) {
    } else if (imm != nullptr && imm->Get(options, lkey, value, &s)) {
      printf("read from immutable!\n");
      // Done
    } else {
      s = current->Get(options, lkey, value);
    }
    mutex_.Lock();
  }

  // Ensures that old data can be removed.
  mem->Unref();
  if (imm != nullptr) imm->Unref();
  current->Unref();

  return s;
}

Status DBImplZNS::Get(const ReadOptions& options,
                      ColumnFamilyHandle* column_family, const Slice& key,
                      PinnableSlice* value) {
  return Status::NotSupported();
}

Status DBImplZNS::Delete(const WriteOptions& opt, const Slice& key) {
  WriteBatch batch;
  batch.Delete(key);
  return Write(opt, &batch);
}

Status DBImplZNS::Delete(const WriteOptions& options,
                         ColumnFamilyHandle* column_family, const Slice& key) {
  WriteBatch batch;
  batch.Delete(key);
  return Write(options, &batch);
}
Status DBImplZNS::Delete(const WriteOptions& options,
                         ColumnFamilyHandle* column_family, const Slice& key,
                         const Slice& ts) {
  return Status::NotSupported();
}

Status DBImplZNS::Write(const WriteOptions& options, WriteBatch* updates) {
  Status s;
  MutexLock l(&mutex_);
  // TODO: syncing

  s = MakeRoomForWrite();
  uint64_t last_sequence = versions_->LastSequence();

  // TODO make threadsafe for multiple writes and add writebatch
  // optimisations...

  // Write to what is needed
  if (s.ok() && updates != nullptr) {
    WriteBatchInternal::SetSequence(updates, last_sequence + 1);
    last_sequence += WriteBatchInternal::Count(updates);
    {
      // write to log (needs to be locked because log can be deleted)
      Slice log_entry = WriteBatchInternal::Contents(updates);
      wal_->Append(log_entry);
      // write to memtable
      mutex_.Unlock();
      assert(this->mem_ != nullptr);
      this->mem_->Write(options, updates);
      mutex_.Lock();
    }
    versions_->SetLastSequence(last_sequence);
  }
  return s;
}

Status DBImplZNS::MakeRoomForWrite() {
  mutex_.AssertHeld();
  Status s;
  bool allow_delay = true;
  while (true) {
    if (allow_delay && versions_->NumLevelZones(0) > 3) {
      mutex_.Unlock();
      env_->SleepForMicroseconds(1000);
      allow_delay = false;
      mutex_.Lock();
    } else if (!mem_->ShouldScheduleFlush()) {
      // space left in memory table
      break;
    } else if (imm_ != nullptr) {
      // flush is scheduled, wait...
      printf("is it done????\n");
      bg_work_finished_signal_.Wait();
    } else if (versions_->NumLevelZones(0) > 3) {
      printf("waiting for compaction\n");
      bg_work_finished_signal_.Wait();
    } else {
      // create new WAL
      s = wal_->Reset();
      printf("Reset WAL\n");
      // Switch to fresh memtable
      imm_ = mem_;
      mem_ = new ZNSMemTable(options_, internal_comparator_);
      mem_->Ref();
      MaybeScheduleCompaction();
    }
  }
  return Status::OK();
}

void DBImplZNS::MaybeScheduleCompaction() {
  printf("Scheduling?\n");
  mutex_.AssertHeld();
  if (bg_compaction_scheduled_) {
    return;
  }
  if (imm_ == nullptr && !versions_->NeedsCompaction()) {
    return;
  }
  bg_compaction_scheduled_ = true;
  env_->Schedule(&DBImplZNS::BGWork, this, rocksdb::Env::HIGH);
}

void DBImplZNS::BGWork(void* db) {
  reinterpret_cast<DBImplZNS*>(db)->BackgroundCall();
}

void DBImplZNS::BackgroundCall() {
  MutexLock l(&mutex_);
  assert(bg_compaction_scheduled_);
  {
    printf("starting background work\n");
    BackgroundCompaction();
  }
  bg_compaction_scheduled_ = false;
  // cascading.
  MaybeScheduleCompaction();
  bg_work_finished_signal_.SignalAll();
}

void DBImplZNS::BackgroundCompaction() {
  mutex_.AssertHeld();
  Status s;
  if (imm_ != nullptr) {
    printf("  Compact memtable...\n");
    s = CompactMemtable();
    return;
  }
  // Compaction itself does not require a lock. only once the changes become
  // visible.
  mutex_.Unlock();
  ZnsVersionEdit edit;
  {
    printf("  Compact LN...\n");
    ZnsCompaction compaction(versions_);
    versions_->Compact(&compaction);
    compaction.MarkStaleTargetsReusable(&edit);
    if (compaction.IsTrivialMove()) {
      s = compaction.DoTrivialMove(&edit);
    } else {
      s = compaction.DoCompaction(&edit);
    }
  }
  mutex_.Lock();
  if (!s.ok()) {
    printf("ERROR during compaction!!!\n");
    return;
  }
  s = s.ok() ? versions_->LogAndApply(&edit) : s;
  // s = s.ok() ? versions_->RemoveObsoleteZones(&edit) : s;
  printf("Compacted!!\n");
}

Status DBImplZNS::CompactMemtable() {
  mutex_.AssertHeld();
  assert(imm_ != nullptr);
  assert(bg_compaction_scheduled_);
  Status s;
  // Flush and set new version
  {
    ZnsVersionEdit edit;
    SSZoneMetaData meta;
    meta.number = versions_->NewSSNumber();
    s = FlushL0SSTables(&meta);
    int level = 0;
    if (s.ok() && meta.lba_count > 0) {
      edit.AddSSDefinition(level, meta.number, meta.lba, meta.lba_count,
                           meta.numbers, meta.smallest, meta.largest);
      s = versions_->LogAndApply(&edit);
    } else {
      printf("Fatal error \n");
    }
    imm_->Unref();
    imm_ = nullptr;
    printf("Flushed!!\n");
  }
  return s;
}

Status DBImplZNS::FlushL0SSTables(SSZoneMetaData* meta) {
  Status s;
  ss_manager_->Ref();
  s = ss_manager_->FlushMemTable(imm_, meta);
  ss_manager_->Unref();
  return s;
}

Status DBImplZNS::Merge(const WriteOptions& options,
                        ColumnFamilyHandle* column_family, const Slice& key,
                        const Slice& value) {
  return Status::OK();
};

bool DBImplZNS::SetPreserveDeletesSequenceNumber(SequenceNumber seqnum) {
  return false;
}

Status DBImplZNS::OpenDevice() {
  device_manager_ = new ZnsDevice::DeviceManager*;
  int rc = 0;
  if (!ZnsDevice::device_set) {
    rc = ZnsDevice::z_init(device_manager_, false);
    ZnsDevice::device_set = true;
  } else {
    rc = ZnsDevice::z_init(device_manager_, true);
  }
  if (rc != 0) {
    return Status::IOError("Error opening SPDK");
  }
  rc = ZnsDevice::z_open(*device_manager_, this->name_.c_str());
  if (rc != 0) {
    return Status::IOError("Error opening ZNS device");
  }
  qpair_factory_ = new QPairFactory(*device_manager_);
  qpair_factory_->Ref();
  return Status::OK();
}

Status DBImplZNS::InitDB(const DBOptions& options) {
  assert(device_manager_ != nullptr);
  ZnsDevice::DeviceInfo device_info = (*device_manager_)->info;
  manifest_ = new ZnsManifest(qpair_factory_, device_info, 0,
                              device_info.zone_size * 2);
  manifest_->Ref();
  wal_ = new ZNSWAL(qpair_factory_, device_info, device_info.zone_size * 2,
                    device_info.zone_size * 5);
  wal_->Ref();
  uint64_t zsize = device_info.zone_size;
  std::pair<uint64_t, uint64_t> ranges[7] = {
      std::make_pair(zsize * 5, zsize * 10),
      std::make_pair(zsize * 10, zsize * 15),
      std::make_pair(zsize * 15, zsize * 20),
      std::make_pair(zsize * 20, zsize * 25),
      std::make_pair(zsize * 25, zsize * 30),
      std::make_pair(zsize * 30, zsize * 35),
      std::make_pair(zsize * 35, zsize * 40)};
  ss_manager_ = new ZNSSSTableManager(qpair_factory_, device_info, ranges);
  ss_manager_->Ref();
  mem_ = new ZNSMemTable(options, this->internal_comparator_);
  mem_->Ref();
  versions_ = new ZnsVersionSet(internal_comparator_, ss_manager_, manifest_,
                                device_info.lba_size);
  return Status::OK();
}

Status DBImplZNS::ResetDevice() {
  qpair_factory_->Ref();
  ZnsDevice::QPair** qpair =
      (ZnsDevice::QPair**)calloc(1, sizeof(ZnsDevice::QPair*));
  qpair_factory_->register_qpair(qpair);
  int rc = ZnsDevice::z_reset(*qpair, 0, true);
  qpair_factory_->unregister_qpair(*qpair);
  qpair_factory_->Unref();
  delete qpair;
  return rc == 0 ? Status::OK() : Status::IOError("Error resetting device");
}

DBImplZNS::~DBImplZNS() {
  mutex_.Lock();
  while (bg_compaction_scheduled_) {
    printf("busy, wait before closing\n");
    bg_work_finished_signal_.Wait();
  }
  mutex_.Unlock();
  std::cout << versions_->DebugString();

  delete versions_;
  if (mem_ != nullptr) mem_->Unref();
  if (imm_ != nullptr) imm_->Unref();
  if (wal_ != nullptr) wal_->Unref();
  if (ss_manager_ != nullptr) ss_manager_->Unref();
  if (manifest_ != nullptr) manifest_->Unref();
  if (qpair_factory_ != nullptr) qpair_factory_->Unref();
  if (device_manager_ != nullptr) {
    ZnsDevice::z_shutdown(*device_manager_);
    free(device_manager_);
  }
}

Status DBImplZNS::Open(
    const DBOptions& db_options, const std::string& name,
    const std::vector<ColumnFamilyDescriptor>& column_families,
    std::vector<ColumnFamilyHandle*>* handles, DB** dbptr,
    const bool seq_per_batch, const bool batch_per_txn) {
  Status s;
  s = ValidateOptions(db_options);
  if (!s.ok()) {
    return s;
  }
  // We do not support column families, so we just clear them
  handles->clear();

  DBImplZNS* impl = new DBImplZNS(db_options, name);
  s = impl->OpenDevice();
  if (!s.ok()) return s;
  s = impl->InitDB(db_options);
  if (!s.ok()) return s;
  // setup WAL (WAL DIR)
  impl->Recover();
  // recover?
  //  !readonly: set directories, lockfile and check if current manifest exists
  //  create_if_missing (NewDB). verify options and system compability readonly
  //  find or error
  // recover version
  // setid
  // recover from WAL
  // mutex
  // s = impl->Recover(column_families, false, false, false, &recovered_seq);

  if (s.ok()) {
    // do something
    // new wall with higher version? max_write_buffer_size
    // increment superversion
  }
  // write options file
  if (s.ok()) {
    // persist_options_status = impl->WriteOptionsFile(
    //     false /*need_mutex_lock*/, false /*need_enter_write_thread*/);
    // // delete obsolete files, maybe schedule or flush
    *dbptr = (DB*)impl;
  }

  // get live files metadata

  // reserve disk bufferspace
  return s;
}

Status DBImplZNS::Recover() {
  MutexLock l(&mutex_);
  Status s;
  // WAL stuff
  s = wal_[0].Recover();
  SequenceNumber seqa;
  s = wal_[0].Replay(mem_, &seqa);
  versions_->SetLastSequence(seqa);

  // manifest stuff
  s = versions_->Recover();
  std::cout << versions_->DebugString();
  MaybeScheduleCompaction();
  return s;
}

Status DBImplZNS::ValidateOptions(const DBOptions& db_options) {
  if (db_options.db_paths.size() > 1) {
    return Status::NotSupported("We do not support multiple db paths.");
  }
  // We do not support most other options, but rather we ignore them for now.
  if (!db_options.use_zns_impl) {
    return Status::NotSupported("ZNS must be enabled to use ZNS.");
  }

  return Status::OK();
}

Status DBImplZNS::Close() { return Status::OK(); }

const Snapshot* DBImplZNS::GetSnapshot() { return nullptr; }

void DBImplZNS::ReleaseSnapshot(const Snapshot* snapshot) {}

Status DBImplZNS::GetMergeOperands(
    const ReadOptions& options, ColumnFamilyHandle* column_family,
    const Slice& key, PinnableSlice* merge_operands,
    GetMergeOperandsOptions* get_merge_operands_options,
    int* number_of_operands) {
  return Status::NotSupported();
}

std::vector<Status> DBImplZNS::MultiGet(
    const ReadOptions& options,
    const std::vector<ColumnFamilyHandle*>& column_family,
    const std::vector<Slice>& keys, std::vector<std::string>* values) {
  std::vector<Status> test;
  return test;
}
std::vector<Status> DBImplZNS::MultiGet(
    const ReadOptions& options,
    const std::vector<ColumnFamilyHandle*>& column_family,
    const std::vector<Slice>& keys, std::vector<std::string>* values,
    std::vector<std::string>* timestamps) {
  std::vector<Status> test;
  return test;
}

Status DBImplZNS::SingleDelete(const WriteOptions& options,
                               ColumnFamilyHandle* column_family,
                               const Slice& key, const Slice& ts) {
  return Status::NotSupported();
}

Status DBImplZNS::SingleDelete(const WriteOptions& options,
                               ColumnFamilyHandle* column_family,
                               const Slice& key) {
  return Status::NotSupported();
}

Iterator* DBImplZNS::NewIterator(const ReadOptions& options,
                                 ColumnFamilyHandle* column_family) {
  return NULL;
}
Status DBImplZNS::NewIterators(
    const ReadOptions& options,
    const std::vector<ColumnFamilyHandle*>& column_families,
    std::vector<Iterator*>* iterators) {
  return Status::NotSupported();
}
bool DBImplZNS::GetProperty(ColumnFamilyHandle* column_family,
                            const Slice& property, std::string* value) {
  return false;
}
bool DBImplZNS::GetMapProperty(ColumnFamilyHandle* column_family,
                               const Slice& property,
                               std::map<std::string, std::string>* value) {
  return false;
}
bool DBImplZNS::GetIntProperty(ColumnFamilyHandle* column_family,
                               const Slice& property, uint64_t* value) {
  return false;
}
bool DBImplZNS::GetAggregatedIntProperty(const Slice& property,
                                         uint64_t* aggregated_value) {
  return false;
};
Status DBImplZNS::GetApproximateSizes(const SizeApproximationOptions& options,
                                      ColumnFamilyHandle* column_family,
                                      const Range* range, int n,
                                      uint64_t* sizes) {
  return Status::NotSupported();
};
void DBImplZNS::GetApproximateMemTableStats(ColumnFamilyHandle* column_family,
                                            const Range& range,
                                            uint64_t* const count,
                                            uint64_t* const size){};
Status DBImplZNS::CompactRange(const CompactRangeOptions& options,
                               ColumnFamilyHandle* column_family,
                               const Slice* begin, const Slice* end) {
  return Status::NotSupported();
};
Status DBImplZNS::SetDBOptions(
    const std::unordered_map<std::string, std::string>& options_map) {
  return Status::NotSupported();
}
Status DBImplZNS::CompactFiles(
    const CompactionOptions& compact_options, ColumnFamilyHandle* column_family,
    const std::vector<std::string>& input_file_names, const int output_level,
    const int output_path_id, std::vector<std::string>* const output_file_names,
    CompactionJobInfo* compaction_job_info) {
  return Status::NotSupported();
}
Status DBImplZNS::PauseBackgroundWork() { return Status::NotSupported(); }
Status DBImplZNS::ContinueBackgroundWork() { return Status::NotSupported(); }
Status DBImplZNS::EnableAutoCompaction(
    const std::vector<ColumnFamilyHandle*>& column_family_handles) {
  return Status::NotSupported();
}
void DBImplZNS::EnableManualCompaction() {}
void DBImplZNS::DisableManualCompaction() {}
int DBImplZNS::NumberLevels(ColumnFamilyHandle* column_family) { return 0; }
int DBImplZNS::MaxMemCompactionLevel(ColumnFamilyHandle* column_family) {
  return 0;
}
int DBImplZNS::Level0StopWriteTrigger(ColumnFamilyHandle* column_family) {
  return 0;
}
const std::string& DBImplZNS::GetName() const { return name_; }
Env* DBImplZNS::GetEnv() const { return env_; }
Options DBImplZNS::GetOptions(ColumnFamilyHandle* column_family) const {
  Options options(options_, ColumnFamilyOptions());
  return options;
}
DBOptions DBImplZNS::GetDBOptions() const {
  Options options(options_, ColumnFamilyOptions());
  return options;
};
Status DBImplZNS::Flush(const FlushOptions& options,
                        ColumnFamilyHandle* column_family) {
  return Status::NotSupported();
}
Status DBImplZNS::Flush(
    const FlushOptions& options,
    const std::vector<ColumnFamilyHandle*>& column_families) {
  return Status::NotSupported();
}

Status DBImplZNS::SyncWAL() { return Status::NotSupported(); }

SequenceNumber DBImplZNS::GetLatestSequenceNumber() const {
  SequenceNumber n = 0;
  return n;
}

Status DBImplZNS::DisableFileDeletions() { return Status::NotSupported(); }

Status DBImplZNS::IncreaseFullHistoryTsLow(ColumnFamilyHandle* column_family,
                                           std::string ts_low) {
  return Status::NotSupported();
}

Status DBImplZNS::GetFullHistoryTsLow(ColumnFamilyHandle* column_family,
                                      std::string* ts_low) {
  return Status::NotSupported();
}

Status DBImplZNS::EnableFileDeletions(bool force) {
  return Status::NotSupported();
}

Status DBImplZNS::GetLiveFiles(std::vector<std::string>&,
                               uint64_t* manifest_file_size,
                               bool flush_memtable) {
  return Status::NotSupported();
}
Status DBImplZNS::GetSortedWalFiles(VectorLogPtr& files) {
  return Status::NotSupported();
}
Status DBImplZNS::GetCurrentWalFile(
    std::unique_ptr<LogFile>* current_log_file) {
  return Status::NotSupported();
}
Status DBImplZNS::GetCreationTimeOfOldestFile(uint64_t* creation_time) {
  return Status::NotSupported();
}

Status DBImplZNS::GetUpdatesSince(
    SequenceNumber seq_number, std::unique_ptr<TransactionLogIterator>* iter,
    const TransactionLogIterator::ReadOptions& read_options) {
  return Status::NotSupported();
}

Status DBImplZNS::DeleteFile(std::string name) {
  return Status::NotSupported();
}

Status DBImplZNS::GetLiveFilesChecksumInfo(FileChecksumList* checksum_list) {
  return Status::NotSupported();
}

Status DBImplZNS::GetLiveFilesStorageInfo(
    const LiveFilesStorageInfoOptions& opts,
    std::vector<LiveFileStorageInfo>* files) {
  return Status::NotSupported();
}

Status DBImplZNS::IngestExternalFile(
    ColumnFamilyHandle* column_family,
    const std::vector<std::string>& external_files,
    const IngestExternalFileOptions& ingestion_options) {
  return Status::NotSupported();
}

Status DBImplZNS::IngestExternalFiles(
    const std::vector<IngestExternalFileArg>& args) {
  return Status::NotSupported();
}

Status DBImplZNS::CreateColumnFamilyWithImport(
    const ColumnFamilyOptions& options, const std::string& column_family_name,
    const ImportColumnFamilyOptions& import_options,
    const ExportImportFilesMetaData& metadata, ColumnFamilyHandle** handle) {
  return Status::NotSupported();
}

Status DBImplZNS::VerifyChecksum(const ReadOptions& /*read_options*/) {
  return Status::NotSupported();
}

Status DBImplZNS::GetDbIdentity(std::string& identity) const {
  return Status::NotSupported();
}

Status DBImplZNS::GetDbSessionId(std::string& session_id) const {
  return Status::NotSupported();
}

ColumnFamilyHandle* DBImplZNS::DefaultColumnFamily() const { return NULL; }

Status DBImplZNS::GetPropertiesOfAllTables(ColumnFamilyHandle* column_family,
                                           TablePropertiesCollection* props) {
  return Status::NotSupported();
}
Status DBImplZNS::GetPropertiesOfTablesInRange(
    ColumnFamilyHandle* column_family, const Range* range, std::size_t n,
    TablePropertiesCollection* props) {
  return Status::NotSupported();
}

Status DBImplZNS::DestroyDB(const std::string& dbname, const Options& options) {
  // Destroy "all" files from the DB. Since we do not use multitenancy, we might
  // as well reset the device.
  Status s;
  DBImplZNS* impl = new DBImplZNS(options, dbname);
  s = impl->OpenDevice();
  if (!s.ok()) return s;
  s = impl->InitDB(options);
  if (!s.ok()) return s;
  s = impl->ResetDevice();
  if (!s.ok()) return s;
  printf("Reset device\n");
  delete impl;
  return s;
}

}  // namespace ROCKSDB_NAMESPACE