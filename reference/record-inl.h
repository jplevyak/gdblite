#include <vector>
using ::std::vector;

#include "common/common_types.h"
#include "common/record.h"
#include "common/record_log_ref.h"
#include "util/coder.h"
#include "util/record_log.h"
#include "util/tagged_ptr.h"
#include "strings/stringpiece.h"

inline Linkage InverseLinkage(Linkage l) {
  switch (l) {
    case kSubject:
      return kObject;
    case kObject:
      return kSubject;
    default:
      LOG(FATAL) << "No inverse for: " << l;
  }
}

inline const char* GetLinkageName(int l) {
  DCHECK_GE(l, 0);
  if (l >= kSubgraph) return kRecordFieldNames[kSubgraph];
  return kRecordFieldNames[l];
}

inline Record::Record(ID aid, const RecordLog::Entry* ptr,
                      RecordLogRefType ref_type, bool ephemeral)
    : RecordLogRef(const_cast<RecordLog::Entry*>(ptr), ref_type, ephemeral),
      id_(aid) {}

inline Record::Record(ID id) : id_(id) {
  reset(RecordLogRef(
      reinterpret_cast<RecordLog::Entry*>(&kStaticNodeRecordLogEntry), kRaw,
      false));
}

inline Record::Record(ID aid, RecordLogRef* swap) : id_(aid) {
  auto ptr = this->ptr_;
  this->ptr_ = reinterpret_cast<Record*>(swap)->ptr_;
  reinterpret_cast<Record*>(swap)->ptr_ = ptr;
}

inline bool Record::IsEmpty() const { return !ptr_; }
inline bool Record::IsNode() const {
  return (*this)->IsValue() && (*this)->ToValue()->size() == 0;
}
inline bool Record::IsValue() const {
  return (*this)->IsValue() && (*this)->ToValue()->size() != 0;
}
inline bool Record::IsLink() const { return (*this)->IsTuple(); }
inline ID Record::next_id() const { return id_ + bytes(); }

inline LinkState Record::state() const {
  return dead() ? LinkState::kDead : LinkState::kLive;
}

inline Record::Record(ID aid, RecordLogRef rle) : id_(aid) { reset(rle); }

inline size_t Record::ValueSize() const { return (*this)->ToValue()->size(); }
inline StringPiece Record::ToStringPiece() const {
  return (*this)->ToValue()->ToStringPiece();
}
inline string Record::ToString() const {
  return (*this)->ToValue()->ToString();
}
inline char* Record::c_str() const { return (*this)->ToValue()->c_str(); }

inline ID Record::subject() const { return (*this)->ToTuple()->id0(id_); }
inline ID Record::predicate() const { return (*this)->ToTuple()->id1(id_); }
inline ID Record::object() const { return (*this)->ToTuple()->id2(id_); }
inline ID Record::author() const { return (*this)->ToTuple()->id3(id_); }
inline LinkTime Record::when() const { return (*this)->ToTuple()->id4(id_); }
inline int Record::LinkSize() const { return (*this)->ToTuple()->size(); }
inline bool Record::dead() const { return (*this)->ToTuple()->extra_bit(); }
inline ID Record::linkage(int i) const {
  switch (i) {
    case kSubject:
      return subject();
    case kPredicate:
      return predicate();
    case kObject:
      return this->object();
    case kAuthor:
      return this->author();
    case kWhen:
      return this->when();
    // TODO(jplev): remove this when we move to only vectors of subgraphs.
    case kSubgraph: {
      auto sg = subgraphs();
      if (sg.size()) return sg[0];
      return 0;
    }
    default:
      return 0;
  }
}

inline size_t Record::bytes() const {
  if (IsNode())
    return 1;
  else
    return (*this)->bytes();  // Delegate to ReocrdLog::Entry.
}

inline int Record::subgraphs_size() const {
  if (LinkSize() <= kSubgraph) return 0;
  return LinkSize() - kSubgraph;
}

inline vector<ID> Record::subgraphs() const {
  vector<ID> result;
  auto p = SkipGroupVarInt5((*this)->ToTuple()->first());
  int n = subgraphs_size();
  if (n <= 0) return result;
  result.resize(n);
  ID* ids = &result[0];
  bool unused;
  for (; n >= 5; n -= 5, ids += 5) p = DecodeGroupVarInt5(p, id_, &unused, ids);
  switch (n) {
    case 0:
      break;
    case 1:
      p = DecodeGroupVarInt2Safe1(p, id_, &unused, ids);
      break;
    case 2:
      p = DecodeGroupVarInt2(p, id_, &unused, ids);
      break;
    case 3:
      p = DecodeGroupVarInt5Safe3(p, id_, &unused, ids);
      break;
    case 4:
      p = DecodeGroupVarInt5Safe4(p, id_, &unused, ids);
      break;
  }
  return result;
}

inline Record MakeRawRecord(ID id, const RecordLog* rlog) {
  return Record(id, rlog->GetEntry(id), kRaw, false);
}
inline Record MakeEphemeralRecord(ID id, RecordLog::Entry* entry) {
  return Record(id, entry, kRaw, true);
}
inline Record MakeEphemeralRecord(ID id, RecordLog* rlog) {
  return Record(id, rlog->GetEntry(id), kRaw, true);
}
inline Record MakeRefCountedRecord(Record r) {  // Copy if not ref counted.
  if (!r.IsRefCounted()) return CopyToRefCountedRecord(r);
  return r;
}
inline Record Record::Value(ID id, StringPiece value) {
  return NewRefCountedRecord(id, value);
}
inline Record MakeLastingRecord(Record r) {
  if (!r.IsEphemeral()) return r;
  return MakeRefCountedRecord(r);
}
