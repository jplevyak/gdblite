#include "local.h"

#include <string.h>
#include <deque>
using ::std::deque;
#include <unordered_map>
using std::unordered_map;
#define hash_map unordered_map
#include <memory>
#include <string>
using ::std::string;
#include <utility>
#include <vector>
using ::std::vector;

#include "base/callback.h"
#include "base/casts.h"
#include "base/integral_types.h"
#include "base/logging.h"
#include "base/macros.h"
#include "file/base/file.h"
#include "file/base/fileutils.h"
#include "file/base/helpers.h"
#include "common/errors.h"
#include "core/tupleindex.h"
#include "strings/interntable.h"
#include "strings/strcat.h"
#include "strings/stringpiece.h"
#include "util/btree/btree_set.h"
#include "util/gtl/map_util.h"
#include "util/random/secure_random.h"
#include "util/task/status.h"

using std::unique_ptr;
using util::Status;

class LocalInterface : public Interface {
 public:
  LocalInterface() : writer_(false) {}
  LocalInterface(StringPiece name, bool writer,
                              ID start_id = kDBStartID);
  virtual ~LocalInterface() {}

  // Close returns true on success.
  virtual bool Close() { return true; }

  virtual RecordLog* records() { return record_log_.get(); }
  virtual const RecordLog* records() const { return record_log_.get(); }
  virtual ResourceManager* resources() { return &pgi_resource_manager; }
  virtual bool IsLocal() const { return true; }
  virtual bool IsTransient() const { return path_ == ""; }

  virtual int NewIndex() {
    string name;
    if (!path_.empty()) name = StrCat(path_, "table", tables_.size());
    tables_.emplace_back(new LocalTable(name, writer_));
    return tables_.size() - 1;
  }
  virtual Table* index(int i) const { return tables_[i].get(); }

  virtual Transaction* NewTransaction() { return nullptr; }
  virtual void StartCommit(ID new_next_id, Callback* done) {
    auto metadata = record_log_->GetMetadata();
    metadata.pre_next_id = new_next_id;
    record_log_->WriteMetadata(metadata, done);
  }
  virtual void CommitTransaction(Transaction* t, Callback* done) {
    done->Run(util::Status::OK);
  }
  virtual void CompleteCommit(Callback* done) {
    auto metadata = record_log_->GetMetadata();
    metadata.next_id = metadata.pre_next_id;
    record_log_->WriteMetadata(metadata, done);
  }
  virtual void WriteSnapshot(StringPiece snapshot_path, Callback* done);
  virtual const string& GetPath() const { return path_; }

  Status ReadSnapshot(StringPiece path, StringPiece snapshot_path);

 private:
  string path_;
  bool writer_;
  unique_ptr<LocalRecordLog> record_log_;
  vector<unique_ptr<LocalTable>> tables_;
};

void LocalIDSetMap::WriteSnapshot(StringPiece filename) {
  auto f = File::Open(filename, "w");
  int64 s = map_.size();
  f->Write(&s, sizeof(s));
  for (auto& p : map_) {
    f->Write(&p.first, sizeof(p.first));
    s = p.second.size();
    f->Write(&s, sizeof(s));
    for (auto& id : p.second) f->Write(&id, sizeof(id));
  }
  s = id2id_map_.size();
  f->Write(&s, sizeof(s));
  for (auto& p : id2id_map_) f->Write(&p, sizeof(p));
  s = uuid2id_map_.size();
  f->Write(&s, sizeof(s));
  for (auto& p : uuid2id_map_) f->Write(&p, sizeof(p));
  f->Close();
}

// TODO(jplev): detect and return errors.
void LocalIDSetMap::ReadSnapshot(StringPiece filename) {
  FileCloser f(File::Open(filename, "r"));
  map_.clear();
  id2id_map_.clear();
  uuid2id_map_.clear();
  int64 s;
  f->Read(&s, sizeof(s));
  for (int64 i = 0; i < s; i++) {
    UUID key;
    f->Read(&key, sizeof(key));
    int64 ss;
    f->Read(&ss, sizeof(ss));
    IDSet& set = map_[key];
    for (int64 j = 0; j < ss; j++) {
      ID id;
      f->Read(&id, sizeof(id));
      set.insert(id);
    }
  }
  f->Read(&s, sizeof(s));
  for (int64 i = 0; i < s; i++) {
    ID key;
    f->Read(&key, sizeof(key));
    ID id;
    f->Read(&id, sizeof(id));
    id2id_map_[key] = id;
  }
  f->Read(&s, sizeof(s));
  for (int64 i = 0; i < s; i++) {
    UUID key;
    f->Read(&key, sizeof(key));
    ID id;
    f->Read(&id, sizeof(id));
    uuid2id_map_[key] = id;
  }
}

size_t LocalIDSetMap::ElementSize(UUID set_key) const {
  const IDSet* id_set = FindOrNull(map_, set_key);
  return id_set ? id_set->size() : 0;
}

ID LocalIDSetMap::ElementMin(UUID set_key) const {
  const IDSet* id_set = FindOrNull(map_, set_key);
  if (!id_set) return 0;
  const IDSet::const_iterator it = id_set->begin();
  if (it != id_set->end()) return *it;
  return 0;
}

ID LocalIDSetMap::ElementMax(UUID set_key) const {
  const IDSet* id_set = FindOrNull(map_, set_key);
  if (!id_set) return 0;
  const IDSet::const_reverse_iterator it = id_set->rbegin();
  if (it != id_set->rend()) return *it;
  return 0;
}

bool LocalIDSetMap::ElementIsImmediate(UUID set_key) const { return true; }

static Closure* ElementProbeLoop(const IDSet& id_set,
                                 deque<BufferPtr<ID>>* q,
                                 IDSetProbeCallback* callback,
                                 Closure* continue_closure);

// Note: it is up to the caller to ensure that it holds the id_set live (by
// having a live IDSet) until it signals ASYNC_DONE.
class ElementProbeContinuation : public Closure {
 public:
  ElementProbeContinuation(const IDSet& id_set, deque<BufferPtr<ID>>* q,
                           IDSetProbeCallback* callback)
      : id_set_(id_set), callback_(callback) {
    q_.swap(*q);
  }
  virtual void Run() {
    if (!ElementProbeLoop(id_set_, &q_, callback_, this)) delete this;
  }

 private:
  const IDSet& id_set_;
  deque<BufferPtr<ID>> q_;
  IDSetProbeCallback* callback_;
};

static Closure* ElementProbeLoop(const IDSet& id_set,
                                 deque<BufferPtr<ID>>* q,
                                 IDSetProbeCallback* callback,
                                 Closure* continue_closure) {
  while (true) {
    BufferPtr<ID> b = q->front();
    q->pop_front();
    for (auto it = b.begin(); it != b.end(); ++it)
      if (!id_set.count(*it)) *it = 0;
    AsyncStreamResult r = callback->Run(util::Status::OK, &b);
    if (r != ASYNC_CONTINUE) {
      if (r == ASYNC_DONE) return nullptr;
      DCHECK(r == ASYNC_PAUSE);
      if (continue_closure) return continue_closure;
      return new ElementProbeContinuation(id_set, q, callback);
    }
    if (b.size()) q->push_back(b);
  }
  return nullptr;
}

Closure* LocalIDSetMap::ElementProbe(UUID set_key,
                                     const vector<BufferPtr<ID>>& buffers,
                                     IDSetProbeCallback* callback) const {
  const IDSet* id_set = FindOrNull(map_, set_key);
  if (!id_set) {
    return nullptr;
  }
  deque<BufferPtr<ID>> q(buffers.begin(), buffers.end());
  return ElementProbeLoop(*id_set, &q, callback, nullptr);
}

Closure* LocalIDSetMap::ElementRead(UUID set_key, ID first, ID last,
                                    IDSetReadCallback* callback) const {
  if (first <= last) {
    const IDSet* id_set = FindOrNull(map_, set_key);
    if (!id_set) {
      return nullptr;
    }
    auto first_it = id_set->lower_bound(first);
    auto last_it = id_set->upper_bound(last);
    int64 s = 0;  // This could be done faster.
    for (auto tmp = first_it; tmp != last_it; ++tmp) ++s;
    ScopedBufferPtr<ID> ids(s);
    auto it = ids.begin();
    for (; first_it != last_it; ++first_it, ++it) {
      *it = *first_it;
      ++it;
    }
    DCHECK(callback->Run(util::Status::CANCELLED, ids.release()) == ASYNC_DONE);
  } else {
    const IDSet* id_set = FindOrNull(map_, set_key);
    if (!id_set) {
      return nullptr;
    }
    auto first_it = id_set->lower_bound(last);
    auto last_it = id_set->upper_bound(first);
    int64 s = 0;  // This could be done faster.
    for (auto tmp = first_it; tmp != last_it; ++tmp) ++s;
    ScopedBufferPtr<ID> ids(s);
    auto it = ids.end();
    for (; first_it != last_it; ++first_it) {
      --it;
      *it = *first_it;
    }
    DCHECK(callback->Run(util::Status::CANCELLED, ids.release()) == ASYNC_DONE);
  }
  return nullptr;
}

void LocalIDSetMap::ElementForEachIDForward(UUID set_key,
                                            ForEachIDCallback* callback) const {
  const IDSet* id_set = FindOrNull(map_, set_key);
  if (!id_set) {
    return;
  }
  for (auto id : *id_set) {
    bool read_more = callback->Run(id);
    if (!read_more) {
      break;
    }
  }
}

void LocalIDSetMap::ElementForEachIDReverse(UUID set_key,
                                            ForEachIDCallback* callback) const {
  const IDSet* id_set = FindOrNull(map_, set_key);
  if (!id_set) {
    return;
  }
  for (auto it = id_set->rbegin(); it != id_set->rend(); ++it) {
    bool read_more = callback->Run(*it);
    if (!read_more) {
      break;
    }
  }
}

void LocalIDSetMap::ElementInsert(UUID set_key, ID id) {
  map_[set_key].insert(id);
}

void LocalIDSetMap::ElementInsert(UUID set_key, const IDSet& ids) {
  map_[set_key].insert(ids.begin(), ids.end());
}

void LocalIDSetMap::Insert(Transaction* t, const IDToIDSetMap& values,
                           Callback* done) {
  for (const auto& entry : values)
    ElementInsert(UUID(entry.first, 0), entry.second);
  if (done) done->Run(util::Status::OK);
}

void LocalIDSetMap::Find(BufferPtr<pair<ID, IDSet>> values,
                         Callback* done) const {
  for (size_t i = 0; i < values.size(); i++) {
    auto set_key = UUID(values[i].first, 0);
    values[i].second = ContainsKey(map_, set_key)
                           ? IDSet(set_key, const_cast<LocalIDSetMap*>(this))
                           : IDSet();
  }
  if (done) done->Run(util::Status::OK);
}

void LocalIDSetMap::Insert(Transaction* t, const UUIDToIDSetMap& values,
                           Callback* done) {
  for (const auto& entry : values) ElementInsert(entry.first, entry.second);
  if (done) done->Run(util::Status::OK);
}

void LocalIDSetMap::Find(BufferPtr<pair<UUID, IDSet>> values,
                         Callback* done) const {
  for (size_t i = 0; i < values.size(); i++) {
    auto set_key = values[i].first;
    values[i].second = ContainsKey(map_, set_key)
                           ? IDSet(set_key, const_cast<LocalIDSetMap*>(this))
                           : IDSet();
  }
  if (done) done->Run(util::Status::OK);
}

void LocalIDSetMap::ElementToIDSet(UUID set_key, IDSet* out) const {
  const IDSet* s = FindOrNull(map_, set_key);
  if (s)
    *out = *s;
  else
    out->clear();
}

static string LocalRecordLogPath(StringPiece path) {
  if (path == "") return "";  // Transient.
  return path.ToString() + "record_log";
}

static string LocalTablePath(StringPiece path, int i) {
  if (path == "") return "";  // Transient.
  return StrCat(path, "index", i, ".");
}

static string LocalRecordLogSnapshotPath(StringPiece path) {
  return path.ToString() + "record_log_snap";
}

static string LocalTableSnapshotPath(StringPiece path, int i) {
  return StrCat(path, "index", i, "_snap");
}

LocalInterface::LocalInterface(StringPiece name,
                                                         bool writer,
                                                         ID start_id)
    : path_(name.ToString()), writer_(writer), record_log_(new LocalRecordLog) {
  CHECK_OK(record_log_->Initialize(LocalRecordLogPath(name), writer, start_id));
  for (int i = 0; i < kNumBuiltinIndexes; i++)
    tables_.emplace_back(new LocalTable(LocalTablePath(name, i), writer));
}

Status CreateLocal(StringPiece path, int64 tuple_limit) {
  if (path == "") return Status::OK;  // Transients are infinite.
  if (NotOK status{CreateLocalRecordLog(
          LocalRecordLogPath(path),
          tuple_limit * LocalRecordLog::kEstimatedBytesPerTuple)})
    return status;
  for (int i = 0; i < kNumBuiltinIndexes; i++) {
    if (NotOK status{CreateLocalTable(
            LocalTablePath(path, i),
            tuple_limit * LocalTable::kEstimatedBytesPerTuple)})
      return status;
  }
  return Status::OK;
}

unique_ptr<Interface> OpenLocal(StringPiece path, bool writer,
                                                ID start_id) {
  return unique_ptr<Interface>(
      new LocalInterface(path, writer, start_id));
}

Status LocalInterface::ReadSnapshot(StringPiece path,
                                                 StringPiece snapshot_path) {
  path_ = path.ToString();
  writer_ = true;
  if (NotOK status{OpenLocalRecordLogFromSnapshot(
          LocalRecordLogPath(path), LocalRecordLogSnapshotPath(snapshot_path),
          &record_log_)})
    return status;
  tables_.resize(kNumBuiltinIndexes);
  for (int i = 0; i < kNumBuiltinIndexes; i++)
    if (NotOK status{OpenLocalTableFromSnapshot(
            LocalTablePath(path, i), LocalTableSnapshotPath(snapshot_path, i),
            &tables_[i])})
      return status;
  return Status::OK;
}

unique_ptr<Interface> OpenLocalFromSnapshot(
    StringPiece path, StringPiece snapshot_path) {
  unique_ptr<LocalInterface> pgi(
      new LocalInterface());
  if (NotOK status{pgi->ReadSnapshot(path, snapshot_path)}) return nullptr;
  return unique_ptr<Interface>(pgi.release());
}

bool DeleteLocal(StringPiece path) {
  if (path.empty()) return false;
  DeleteLocalRecordLog(LocalRecordLogPath(path));
  for (int i = 0; i < kNumBuiltinIndexes; i++)
    DeleteLocalTable(LocalTablePath(path, i));
  return true;
}

bool LocalExists(StringPiece path) {
  if (path.empty()) return false;
  bool exists = LocalRecordLog::Exists(LocalRecordLogPath(path));
  for (int i = 0; i < kNumBuiltinIndexes; i++)
    exists = LocalTable::Exists(LocalTablePath(path, i)) && exists;
  return exists;
}

bool LocalIsValid(StringPiece path) {
  if (path.empty()) return false;
  bool exists = LocalRecordLog::IsValid(LocalRecordLogPath(path));
  for (int i = 0; i < kNumBuiltinIndexes; i++)
    exists = LocalTable::IsValid(LocalTablePath(path, i)) && exists;
  return exists;
}

void LocalInterface::WriteSnapshot(StringPiece path,
                                                Callback* done) {
  record_log_->WriteSnapshot(LocalRecordLogSnapshotPath(path));
  int i = 0;
  for (auto& t : tables_) {
    t->WriteSnapshot(LocalTableSnapshotPath(path, i));
    i++;
  }
  done->Run(util::Status::OK);
}

Status ReadLocalSnapshot(StringPiece path, StringPiece snapshot_path) {
  auto local_pgi = OpenLocalFromSnapshot(path, snapshot_path);
  if (local_pgi == nullptr)
    return Status(::util::error::INTERNAL, "unable to restore snapshot");
  return Status::OK;
}
