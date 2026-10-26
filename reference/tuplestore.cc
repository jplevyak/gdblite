#include "core/tuplestore.h"

#include <string>
using ::std::string;

#include "base/callback.h"
#include "base/logging.h"
#include "file/base/file.h"
#include "common/record_reader.h"
#include "common/format.h"
#include "core/tuplesequence.h"
#include "pgi/pgi.h"
#include "proto/tuple.pb.h"
#include "common/record_reader.h"
#include "google/protobuf/text_format.h"

using std::unique_ptr;

static const float kTrioBytesPerTuple = 29.0;  // Empirical.
static const float kRecordLogBytesPerTuple = 20.0;  // Empirical.
static const float kCompressedRecordLogBytesPerTuple = 6.2;  // Empirical.
static const float kTuplesOverestimate = 1.1;  // Estimate 10% more.

TupleStore::TupleStore(PhysicalGraphInterface* pgi, const TupleStore* base)
    : pgi_(pgi),
      base_(base),
      tuples_(pgi->records()),
      indexes_(this),
      first_unindexed_tuple_(next_id()) {
  if (base) {
    FindNodeByPrimaryKeyCallback cb = std::bind(
        &TupleStore::FindNodeByPrimaryKey, base, std::placeholders::_1);
    // TODO(davidespinosa): handle this error.
    CHECK(bootstrap_constants_.Init(cb));
  }
}

TupleStore::TupleStore(unique_ptr<PhysicalGraphInterface> pgi,
                       const TupleStore* base)
    : TupleStore(pgi.release(), base) {}

bool TupleStore::RestoreFromProtoString(const string& proto_string) {
  unique_ptr<TupleSequence> ts(new TupleSequence);
  CHECK(ts->InitFromProtoStr(proto_string));
  return Restore(*ts);
}

bool TupleStore::InitModules() {
  FindNodeByPrimaryKeyCallback cb =
      std::bind(&TupleStore::FindNodeByPrimaryKey, this, std::placeholders::_1);
  return bootstrap_constants_.Init(cb) && doc_constants_.Init(cb) &&
         mql_constants_.Init(cb) && query_constants_.Init(cb) &&
         system_constants_.Init(cb);
}

unique_ptr<TupleStore> TupleStore::NewTransientEmpty() {
  unique_ptr<TupleStore> store(
      new TupleStore(PhysicalGraphInterface::NewTransient(), nullptr));
  return store;
}

unique_ptr<TupleStore> TupleStore::NewTransientWithBootstrap() {
  unique_ptr<TupleStore> store(NewTransientEmpty());
  if (!store->RestoreFromProtoString(b_bootstrap_constants::AsProto())) {
    return nullptr;
  }
  FindNodeByPrimaryKeyCallback cb = std::bind(
      &TupleStore::FindNodeByPrimaryKey, store.get(), std::placeholders::_1);
  if (!store->bootstrap_constants_.Init(cb)) {
    return nullptr;
  }
  return store;
}

unique_ptr<TupleStore> TupleStore::NewTransientWithSystem() {
  unique_ptr<TupleStore> store(NewTransientEmpty());
  if (!store->RestoreFromProtoString(b_system_constants::AsProto())) {
    return nullptr;
  }
  if (!store->InitModules()) {
    return nullptr;
  }
  return store;
}

unique_ptr<TupleStore> TupleStore::NewTransientExtending(
    const TupleStore* base) {
  ID start_id = base ? base->next_id() : kDBStartID;
  unique_ptr<TupleStore> store(
      new TupleStore(PhysicalGraphInterface::NewTransient(start_id), base));
  // Don't check result -- store may not have modules.
  store->InitModules();
  return store;
}

unique_ptr<TupleStore> TupleStore::Open(StringPiece path, bool writer) {
  unique_ptr<PhysicalGraphInterface> pgi;
  auto status = PhysicalGraphInterface::Open(path, writer, &pgi);
  if (!status.ok()) {
    LOG(ERROR) << status.error_message();
    return nullptr;
  }
  unique_ptr<TupleStore> store(new TupleStore(pgi.release(), nullptr));
  // Don't check result -- store may not have modules.
  store->InitModules();
  return store;
}

unique_ptr<TupleStore> TupleStore::CreateEmpty(StringPiece path,
                                               int64 tuple_limit) {
  auto status = PhysicalGraphInterface::Create(path, tuple_limit);
  LOG(INFO) << "Creating " << path.ToString() << " tuple_limit " << tuple_limit;
  if (!status.ok()) {
    LOG(ERROR) << status.error_message();
    return nullptr;
  }
  unique_ptr<PhysicalGraphInterface> pgi;
  status = PhysicalGraphInterface::Open(path, true, &pgi);
  if (!status.ok()) {
    LOG(ERROR) << status.error_message();
    return nullptr;
  }

  unique_ptr<TupleStore> store(new TupleStore(pgi.release(), nullptr));
  return store;
}

unique_ptr<TupleStore> TupleStore::CreateWithBootstrap(StringPiece path,
                                                       int64 tuple_limit) {
  unique_ptr<TupleStore> store(CreateEmpty(path, tuple_limit));
  if (!store.get()) {
    return nullptr;
  }
  if (!store->RestoreFromProtoString(b_bootstrap_constants::AsProto())) {
    return nullptr;
  }
  return store;
}

unique_ptr<TupleStore> TupleStore::CreateWithSystem(StringPiece path,
                                                    int64 tuple_limit) {
  unique_ptr<TupleStore> store(CreateEmpty(path, tuple_limit));
  if (!store.get()) {
    return nullptr;
  }
  if (!store->RestoreFromProtoString(b_system_constants::AsProto())) {
    return nullptr;
  }
  if (!store->InitModules()) {
    return nullptr;
  }
  return store;
}

unique_ptr<TupleStore> TupleStore::OpenFromSnapshot(StringPiece graph_path,
                                                    StringPiece snapshot_path) {
  PhysicalGraphInterface::Delete(graph_path);
  unique_ptr<PhysicalGraphInterface> pgi;
  auto status =
      PhysicalGraphInterface::OpenFromSnapshot(graph_path, snapshot_path, &pgi);
  if (!status.ok()) return nullptr;
  unique_ptr<TupleStore> ts(new TupleStore(pgi.release(), nullptr));
  if (!ts->InitModules()) return nullptr;
  return ts;
}

static int64 EstimateTupleCount(StringPiece db_file_path) {
  if (db_file_path.empty()) return 0;
  int64 s = File::GetFileSize(db_file_path.ToString());
  FormatType type = FormatType::RLOG;
  InferFormat(db_file_path, &type);
  if (type == FormatType::TUPLES)
    return (s / kTrioBytesPerTuple) * kTuplesOverestimate;
    else if  (type == FormatType::RLOG)
    return (s / kRecordLogBytesPerTuple) * kTuplesOverestimate;
  return 0;
}

unique_ptr<TupleStore> TupleStore::Load(StringPiece db_file_path,
                                        StringPiece graph_path) {
  const int64 size = EstimateTupleCount(db_file_path);

  unique_ptr<TupleStore> store = graph_path.empty()
                                     ? NewTransientWithBootstrap()
                                     : CreateWithBootstrap(graph_path, size);

  if (!store.get()) {
    LOG(ERROR) << "Unable to open "
               << (graph_path.empty() ? "TransientTupleStore"
                                      : graph_path.ToString().c_str());
    return nullptr;
  }

  if (db_file_path.empty()) {
    if (!store->RestoreFromProtoString(b_system_constants::AsProto())) {
      LOG(ERROR) << "Unable to restore system constants.";
      return nullptr;
    }
  } else {
    if (!store->Restore(db_file_path)) {
      LOG(ERROR) << "Unable to load from '" << db_file_path.ToString() << "'.";
      return nullptr;
    }
  }

  if (!store->InitModules()) {
    LOG(ERROR) << "Unable to init system constants.";
    return nullptr;
  }

  return store;
}

string TupleStore::GetValue(const Record& tuple) const {
  if (base_ && tuple.id() < start_id_ext()) {
    return base_->GetValue(tuple);
  } else {
    return PtsGetValue(tuple);
  }
}

bool TupleStore::Write(const TupleSequence& source, IDToIDMap* out_id_map) {
  TupleStore ts(PhysicalGraphInterface::NewTransient(next_id()), this);
  return ts.WriteExtension(source, out_id_map) && Restore(ts);
}

ID TupleStore::NextTupleID(const Record& t) const { return t.id() + t.bytes(); }

Record TupleStore::GetTuple(ID id) const {
  if (base_ && id < start_id_ext()) {
    return base_->GetTuple(id);
  } else {
    return PtsGetTuple(id);
  }
}

bool TupleStore::HasTuple(const Record& tuple) const {
  if (!HasTuple(tuple.id())) {
    return false;
  }
  Record internal_tuple = GetTuple(tuple.id());
  return SameContent(internal_tuple, tuple);
}

void CopyTuplesCb(TupleSequence* copy, const Record& tuple) {
  copy->AppendRecord(CopyToRefCountedRecord(tuple));
}

unique_ptr<TupleSequence> TupleStore::CopyTuples() const {
  unique_ptr<TupleSequence> copy =
      base_ ? base_->CopyTuples()
            : unique_ptr<TupleSequence>(new TupleSequence);
  unique_ptr<ForEachRecordCallback> callback(
      NewPermanentCallback(&CopyTuplesCb, copy.get()));
  PtsForEachTuple(callback.get());
  return copy;
}

void TupleStore::ToProtoCb(TupleArrayPb* tuple_array_pb,
                           const Record& tuple) const {
  TuplePb* tuple_pb = tuple_array_pb->add_tuples();
  tuple.ToProto(tuple_pb);
}

void TupleStore::ToProto(TupleArrayPb* tuple_array_pb) const {
  unique_ptr<ForEachRecordCallback> callback(
      NewPermanentCallback(this, &TupleStore::ToProtoCb, tuple_array_pb));
  ForEachRecord(callback.get());
}

void TupleStore::ToProtoString(string* output, bool text_format) const {
  TupleArrayPb tuple_array_pb;
  ToProto(&tuple_array_pb);
  if (text_format) {
    google::protobuf::TextFormat::PrintToString(tuple_array_pb, output);
  } else {
    tuple_array_pb.SerializeToString(output);
  }
}

PgiStats TupleStore::GetPgiStats() const {
  PgiStats stats = GetPgiStatsExt();
  if (base_) {
    stats.Add(base_->GetPgiStats());
  }
  return stats;
}
