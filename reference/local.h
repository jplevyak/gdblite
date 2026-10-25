#include <stddef.h>
#include <memory>
#include <utility>
#include <vector>
using ::std::vector;

#include "base/integral_types.h"
#include "base/port.h"
#include "common/common_types.h"
#include "common/record.h"
#include "pgi/pgi.h"
#include "pgi/pgi_callbacks.h"
#include "pgi/pgi_idset.h"
#include "pgi/pgi_resource_manager.h"
#include "util/local_sets.h"
#include "util/mmap_backing.h"
#include "strings/stringpiece.h"
#include "util/task/status.h"

class Closure;

class DynamicTable;
class Indexes;
class LocalTable;
class Transaction;
class RecordLog;

// Storage for Transient PGI Indexes and caching for backed PGI Indexes.
class LocalIDSetMap {
 public:
  virtual ~LocalIDSetMap() {}

  virtual void Acquire(UUID set_key) {}
  virtual void Release(UUID set_key) {}
  virtual int64 GetNumberOfIDSetsInUse() { return map_.size(); }

  // Free those not in use that are still in indexes.
  virtual void FreeUnusedMemory() {}

  virtual size_t ElementSize(UUID set_key) const;

  // Returns member with smallest value.
  virtual ID ElementMin(UUID set_key) const;

  // Returns member with largest value.
  virtual ID ElementMax(UUID set_key) const;

  virtual bool ElementIsImmediate(UUID set_key) const;

  virtual Closure* ElementRead(UUID set_key, ID first, ID last,
                               IDSetReadCallback* callback) const;

  virtual Closure* ElementProbe(UUID set_key,
                                const vector<BufferPtr<ID>>& buffers,
                                IDSetProbeCallback* callback) const;

  virtual void ElementToIDSet(UUID set_key, IDSet* out) const;

  virtual void ElementForEachIDForward(UUID set_key,
                                       ForEachIDCallback* callback) const;

  virtual void ElementForEachIDReverse(UUID set_key,
                                       ForEachIDCallback* callback) const;

  virtual void ElementInsert(UUID set_key, ID id);
  virtual void ElementInsert(UUID set_key, const IDSet& ids);

  void Insert(Transaction* t, const IDToIDSetMap& values, Callback* done);
  void Find(BufferPtr<pair<ID, IDSet>> values, Callback* done) const;
  void Insert(Transaction* t, const UUIDToIDSetMap& values,
              Callback* done);
  void Find(BufferPtr<pair<UUID, IDSet>> values, Callback* done) const;

  void ReadSnapshot(StringPiece path);
  void WriteSnapshot(StringPiece path);

  static bool Exists(StringPiece path) { return true; }
  static bool IsValid(StringPiece path) { return true; }

 private:
  friend LocalTable;
  friend Indexes;

  UUIDToIDSetMap map_;
  IDToIDMap id2id_map_;
  UUIDToIDMap uuid2id_map_;
};

class LocalRecordLog {
 public:
  static const int kEstimatedBytesPerTuple = 30;  // Just a guess.
  static const int64 kDefaultCreateSize = 0;
  static const int64 kDoNotCreateSize = -1;  // Error if not preexisting.

  virtual void Append(Transaction* t, BufferPtr<Record> records,
                      Callback* done);
  virtual void Get(BufferPtr<Record> records, Callback* done,
                   size_t limit = 0) const;
  virtual void GetRange(ID start_id, ID end_id,
                        vector<BufferPtr<char>>* buffers,
                        vector<bool>* buffer_is_ephemeral,
                        Callback* done) const;
  virtual void ReadMetadata(Callback* done);
  virtual const Metadata& GetMetadata() const { return metadata_; }
  virtual void WriteMetadata(const Metadata& metadata, Callback* done);

  ID& start() { return metadata_.start_id; }
  ID& next() { return metadata_.next_id; }
  void AppendRecord(const Record& tuple);
  void WriteSnapshot(StringPiece path) const;
  ::util::Status Initialize(StringPiece path, bool writer, ID start_id,
                            int64 create_size = kDefaultCreateSize);

  static bool Exists(StringPiece path);
  static bool IsValid(StringPiece path);

  virtual ~LocalRecordLog();

 private:
  friend ::util::Status OpenLocalRecordLogFromSnapshot(
      StringPiece prefix, StringPiece snapshot_path,
      std::unique_ptr<LocalRecordLog>* result);

  struct LogFile {
    RecordLog* log = nullptr;
    size_t size = 0;
    int fd = -1;
  };

  bool Expand(ID new_last_id);
  ID ExpandSize(ID new_last_id);
  void UpdateStartID(ID start_id);
  // When the prefix is "", the RecordLog is transient/mapped anonymously.
  bool AnonymousMMaps() { return prefix_.empty(); }
  void SetFlags(bool create);
  bool LoadFiles(ID new_last_id);
  RecordLog* last_log() const;

  MMapFlags mmap_flags_;
  string prefix_;
  bool writer_ = false;
  Metadata metadata_;
  int metadata_fd_ = -1;
  size_t metadata_size_ = 0;
  Metadata* mmapped_metadata_ = nullptr;
  vector<LogFile> files_;
  ID last_id_ = 0;  // Last id given the size of the last file.
};

// Local implemenation of the Table interface on top of dynamic_table.*.
class LocalTable : public Table {
 public:
  static const int kEstimatedBytesPerTuple = 10;  // Just a guess.

  virtual void Insert(Transaction* t, const IDToIDSetMap& values,
                      Callback* done);
  virtual void Insert(Transaction* t, const UUIDToIDSetMap& values,
                      Callback* done);
  // Under the assumption that there is at most one value for each
  // key, insert a key value pair.
  virtual void Insert(Transaction* t, BufferPtr<pair<ID, ID>> values,
                      Callback* done);
  virtual void Insert(Transaction* t, BufferPtr<pair<UUID, ID>> values,
                      Callback* done);
  // The pair is <key, value> where the value is replaced if the key is found.
  virtual void Find(BufferPtr<pair<ID, IDSet>> values,
                    Callback* done) const;
  virtual void Find(BufferPtr<pair<UUID, IDSet>> values,
                    Callback* done) const;
  // Find at most one ID.
  virtual void Find(BufferPtr<pair<ID, ID>> values, Callback* done) const;
  virtual void Find(BufferPtr<pair<UUID, ID>> values,
                    Callback* done) const;
  virtual void Tick();

  virtual LocalIDSetMap* map() const { return &map_; }

  void WriteSnapshot(StringPiece path) const;
  ::util::Status status();

  static bool Exists(StringPiece path);
  static bool IsValid(StringPiece path);

  typedef LocalSets<20, 488, 2048> LocalSetsType;

  LocalTable(StringPiece prefix, bool writer);
  LocalTable(DynamicTable* dt, LocalSetsType* lst);
  virtual ~LocalTable();

 private:
  template <typename Key>
  void InternalInsert(Transaction* t, const ToIDSetMap<Key>& values,
                      Callback* done);
  template <typename Key>
  void InternalInsert(Transaction* t, BufferPtr<pair<Key, ID>> values,
                      Callback* done);
  template <typename Key>
  void InternalFind(BufferPtr<pair<Key, IDSet>> values,
                    Callback* done) const;
  template <typename Key>
  void InternalFind(BufferPtr<pair<Key, ID>> values,
                    Callback* done) const;
  template <typename Key>
  hash_map<Key, ID>* FindMap() const;

  std::unique_ptr<DynamicTable> dt_;
  mutable LocalIDSetMap map_;
  std::unique_ptr<LocalSetsType> sets_;
};

// The prefix is the pathname prefix of all the database files.
// If the prefix is empty the log will be ephemeral.
::util::Status CreateLocal(StringPiece prefix, int64 tuple_limit);
// These should likely return ::util::Status.
std::unique_ptr<PhysicalGraphInterface> OpenLocal(StringPiece prefix,
                                                     bool writer,
                                                     ID start_id = kDBStartID);
std::unique_ptr<PhysicalGraphInterface> OpenLocalFromSnapshot(
    StringPiece prefix, StringPiece snapshot_path);
// Read the snapshot to the graph_path.
::util::Status ReadLocalSnapshot(StringPiece prefix,
                                    StringPiece snapshot_path);

bool DeleteLocal(StringPiece prefix);
bool LocalExists(StringPiece prefix);
bool LocalIsValid(StringPiece prefix);

// LocalTV function.
::util::Status CreateLocalRecordLog(StringPiece prefix,
                                    uint64 initial_size = 0) MUST_USE_RESULT;
::util::Status OpenLocalRecordLog(StringPiece prefix, bool writer,
                                  std::unique_ptr<RecordLog>* result)
    MUST_USE_RESULT;
::util::Status OpenLocalRecordLogFromSnapshot(
    StringPiece prefix, StringPiece snapshot_path,
    std::unique_ptr<LocalRecordLog>* result) MUST_USE_RESULT;
void DeleteLocalRecordLog(StringPiece prefix);

// LocalTable function.
::util::Status CreateLocalTable(StringPiece prefix,
                                uint64 initial_size) MUST_USE_RESULT;
::util::Status OpenLocalTable(StringPiece prefix, bool writer,
                              std::unique_ptr<Table>* table) MUST_USE_RESULT;
::util::Status OpenLocalTableFromSnapshot(
    StringPiece prefix, StringPiece snapshot_path,
    std::unique_ptr<LocalTable>* table) MUST_USE_RESULT;
void DeleteLocalTable(StringPiece prefix);
