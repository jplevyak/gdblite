#include "record.h"

#include <string.h>

#include "base/file.h"
#include "file/base/file_closer.h"
#include "file/base/helpers.h"
#include "common/common_types.h"
#include "proto/tuple.pb.h"
#include "security/util/lite/sha256.h"
#include "strings/strcat.h"
#include "strings/stringpiece.h"

const int kStackBufferLimit = 200;

char Record::kStaticNodeRecordLogEntry = 0;

UUID StringToUUID(StringPiece s) {
  UUID uuid;
  uint64 hash[4];
  SHA256_hash(s.data(), static_cast<unsigned int>(s.length()),
              reinterpret_cast<uint8*>(&hash[0]));
  uuid.uuid0 = hash[0] ^ hash[1];
  uuid.uuid1 = hash[2] ^ hash[3];
  return uuid;
}

RecordLogRef NewRefCountedValue(StringPiece s) {
  RecordLogRef rlogentry(
      RefCountedRecordLogRef::Alloc(RecordLog::MaxMakeValueSize(s)),
      kRefCounted, false);
  RecordLog::EncodeValue(rlogentry->chars(), s);
  return rlogentry;
}

RecordLogRef NewRefCountedTuple(ID id, bool dead, int nlinkages, ID* linkages) {
  char buffer[kStackBufferLimit];
  char* b = &buffer[0];
  size_t s = RecordLog::MaxMakeTupleSize(nlinkages);
  if (s > kStackBufferLimit) b = static_cast<char*>(::malloc(s));
  auto n =
      RecordLog::EncodeTupleWithOffset(b, id, id, dead, nlinkages, linkages);
  // TODO(jplev): with a custom allocator we can avoid this padding.
  size_t size = n - id + RecordLog::kPaddingRequired;
  RecordLogRef rlogentry(RefCountedRecordLogRef::Alloc(size), kRefCounted,
                         false);
  memcpy(rlogentry->chars(), b, size);
  if (b != &buffer[0]) ::free(b);
  return rlogentry;
}

RecordLogRef CopyToRefCountedRecordLogRef(RecordLog::Entry* e) {
  size_t size = e->bytes();
  // TODO(jplev): with a custom allocator we can avoid this padding.
  RecordLogRef rlogentry(
      RefCountedRecordLogRef::Alloc(size + RecordLog::kPaddingRequired),
      kRefCounted, false);
  memcpy(rlogentry->chars(), e->chars(), size);
  return rlogentry;
}

Record NewRefCountedRecord(ID id, StringPiece s) {
  RecordLogRef rlogentry(NewRefCountedValue(s));
  return Record(id, &rlogentry);
}

Record NewRefCountedRecord(ID id, bool dead, int nlinkages, ID* linkages) {
  RecordLogRef rlogentry(NewRefCountedTuple(id, dead, nlinkages, linkages));
  return Record(id, &rlogentry);
}

Record CopyToRefCountedRecord(ID id, RecordLog* log) {
  RecordLogRef rlogentry(CopyToRefCountedRecordLogRef(id, log));
  return Record(id, &rlogentry);
}

Record CopyToRefCountedRecord(Record r) {
  if (r.IsNode()) return Record(r.id());
  RecordLogRef record_log_ref(CopyToRefCountedRecordLogRef(r));
  return Record(r.id(), &record_log_ref);
}

Record Record::ChangeID(ID new_id) const {
  if (!IsLink()) return Record(new_id, *this);
  DCHECK(this->IsLink());
  vector<ID> ids;
  auto tuple = (*this)->ToTuple();
  int n = tuple->size();
  ids.resize(tuple->size());
  bool dead;
  tuple->Decode(id(), n, &dead, &ids[0]);
  return NewRefCountedRecord(new_id, dead, n, &ids[0]);
}

UUID Record::ValueUUID() const { return StringToUUID(ToStringPiece()); }

Record Record::ChangeValue(StringPiece new_value) const {
  DCHECK_NE(new_value, "");
  return NewRefCountedRecord(id(), new_value);
}

Record Record::ChangeLinkage(Linkage i, ID new_linkage) const {
  DCHECK(this->IsLink());
  vector<ID> ids;
  auto tuple = (*this)->ToTuple();
  int n = tuple->size();
  ids.resize(tuple->size());
  bool dead;
  tuple->Decode(id(), n, &dead, &ids[0]);
  ids[i] = new_linkage;
  return NewRefCountedRecord(id(), dead, n, &ids[0]);
}

Record Record::ChangeState(LinkState new_state) const {
  DCHECK(this->IsLink());
  vector<ID> ids;
  auto tuple = (*this)->ToTuple();
  int n = tuple->size();
  ids.resize(tuple->size());
  bool dead;
  tuple->Decode(id(), n, &dead, &ids[0]);
  dead = new_state == LinkState::kDead;
  return NewRefCountedRecord(id(), dead, n, &ids[0]);
}

Record Record::ChangeWhen(LinkTime new_when) const {
  if (IsLink()) return ChangeLinkage(kWhen, new_when);
  return *this;
}

Record Record::FromProto(const TuplePb& tuple_pb) {
  if (tuple_pb.has_value())
    return NewRefCountedRecord(tuple_pb.id(), tuple_pb.value());
  if (tuple_pb.has_predicate() && tuple_pb.predicate() != 0) {
    vector<ID> linkages;
    linkages.resize(5 + tuple_pb.subgraph_size());
    linkages[0] = tuple_pb.subject();
    linkages[1] = tuple_pb.predicate();
    linkages[2] = tuple_pb.object();
    linkages[3] = tuple_pb.author();
    linkages[4] = tuple_pb.when();
    for (int i = 0; i < tuple_pb.subgraph_size(); i++)
      linkages[5 + i] = tuple_pb.subgraph(i);
    ID id = tuple_pb.id();
    for (int i = 0; i < linkages.size(); i++) {
      if (i != kWhen) {
        CHECK_LT(linkages[i], id);
      }
    }
    return NewRefCountedRecord(tuple_pb.id(), tuple_pb.dead(), linkages.size(),
                               &linkages[0]);
  }
  return Record(tuple_pb.id());
}

Record Record::Link(ID id, LinkTime when, ID author, ID subject, ID predicate,
                    ID object, LinkState dead, const vector<ID>& xsubgraphs) {
  vector<ID> linkages;
  // TODO(jplev): remove this kludge when we truely have multiple subgraphs.
  auto subgraphs = (xsubgraphs.size() == 1 && xsubgraphs[0] == 0) ? vector<ID>()
                                                                  : xsubgraphs;
  linkages.resize(5 + subgraphs.size());
  linkages[0] = subject;
  linkages[1] = predicate;
  linkages[2] = object;
  linkages[3] = author;
  linkages[4] = when;
  for (int i = 0; i < subgraphs.size(); i++)
    linkages[5 + i] = subgraphs[i];
  for (int i = 0; i < linkages.size(); i++) {
    if (i != kWhen) {
      CHECK_LT(linkages[i], id);
    }
  }
  return NewRefCountedRecord(id, dead == LinkState::kDead, linkages.size(),
                             &linkages[0]);
}

void Record::ToProto(TuplePb* tuple_pb) const {
  tuple_pb->Clear();

  tuple_pb->set_id(id());
  if (IsLink()) {
    if (linkage(kSubject)) tuple_pb->set_subject(linkage(kSubject));
    if (linkage(kPredicate)) tuple_pb->set_predicate(linkage(kPredicate));
    if (linkage(kObject)) tuple_pb->set_object(linkage(kObject));
    if (linkage(kAuthor)) tuple_pb->set_author(linkage(kAuthor));
    if (linkage(kSubgraph)) tuple_pb->add_subgraph(linkage(kSubgraph));
    if (when()) tuple_pb->set_when(when());
    if (dead()) tuple_pb->set_dead(dead());
  } else if (IsValue()) {
    tuple_pb->set_value(ToString());
  }
}

bool Record::IsValidLink() const {
  auto n = LinkSize();
  vector<ID> ids(n);
  bool dummy;
  (*this)->ToTuple()->Decode(id(), n, &dummy, &ids[0]);
  for (int i = 0; i < LinkSize(); i++)
    if (i != kWhen && ids[i] >= id()) return false;
  return true;
}

string Record::ToDebugString() const {
  TuplePb tuple_pb;
  ToProto(&tuple_pb);
  return tuple_pb.DebugString();
}

bool SameContent(const Record& t1, const Record& t2) {
  if (t1.IsNode() && t2.IsNode()) {
    return true;
  } else if (t1.IsValue() && t2.IsValue()) {
    return t1.ToStringPiece() == t2.ToStringPiece();
  } else if (t1.IsLink() && t2.IsLink()) {
    int n = t1.LinkSize();
    if (n != t2.LinkSize()) return false;
    vector<ID> r1(n), r2(n);
    bool dummy;
    t1->ToTuple()->Decode(t1.id(), n, &dummy, &r1[0]);
    t2->ToTuple()->Decode(t2.id(), n, &dummy, &r2[0]);
    return r1 == r2;
  } else {
    return false;
  }
}


RecordLog* LoadRecordLog(StringPiece path, uint64* next) {
  FileCloser file(File::Open(path, "r"));
  if (!file) return nullptr;
  int64 filesize = file->Size();
  char* input = static_cast<char*>(::malloc(filesize));
  if (file->Read(input, filesize) != filesize) return nullptr;
  auto rlog = RecordLog::Adopt(input);
  auto& header = rlog->GetHeader();
  if (header.magic != RecordLog::kMagic) return nullptr;
  if (header.version != RecordLog::kVersion) return nullptr;
  if (header.compression > RecordLog::COMPRESSION_LAST) return nullptr;
    *next = rlog->NextID(filesize);
  return rlog;
}

ID RenumberID(hash_map<ID, ID>* map, ID id) {
  auto it = map->find(id);
  if (it != map->end()) return it->second;
  return id;
}

Record Renumber(const Record& t, ID* next_id, hash_map<ID, ID>* map) {
  if (t.id() != *next_id) (*map)[t.id()] = *next_id;
  if (!map->size()) {  // Not renumbering.
    *next_id += t.bytes();
    return t;
  } else {
    // Renumber the IDs wrt map except for kWhen.
    if (t.IsLink()) {
      int n = t.LinkSize();
      vector<ID> ids(n);
      bool dead;
      t->ToTuple()->Decode(t.id(), n, &dead, &ids[0]);
      for (int i = 0; i < n; i++)
        if (i != kWhen) ids[i] = RenumberID(map, ids[i]);
      Record result(NewRefCountedRecord(*next_id, dead, n, &ids[0]));
      *next_id += result->bytes();
      return result;
    } else {
      Record result(t);
      result.set_id(*next_id);
      *next_id += result.bytes();
      return result;
    }
  }
}
