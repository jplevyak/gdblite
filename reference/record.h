#include <stddef.h>
#include <stdlib.h>
#include <sys/time.h>
#include <vector>
using ::std::vector;

#include "base/callback.h"  // DEPRECATED (See ForEachRecord below).
#include "common/common_types.h"
#include "common/record_log_ref.h"
#include "util/coder.h"
#include "util/record_log.h"
#include "util/tagged_ptr.h"
#include "strings/stringpiece.h"
#include "util/btree/btree_set.h"
#include "util/task/status.h"

class TuplePb;

enum Linkage {
  kSubject,
  kPredicate,
  kObject,
  kAuthor,
  kWhen,      // "when" is the creation time, not a regular linkage.
  kSubgraph,  // TODO(jplev): remove this when we have multiple sugraphs.
  kNumLinkages
};

enum class LinkState { kLive, kDead };

const int64 kLinkTimePerSecond = 1000;
const int kNumAllLinkages = 5;
const vector<Linkage> kAllLinkages{kSubject, kPredicate, kObject, kAuthor,
                                   kSubgraph};
const vector<const char*> kRecordFieldNames{"subject", "predicate", "object",
                                            "author",  "when",      "subgraph"};

Linkage InverseLinkage(Linkage l);
const char* GetLinkageName(int l);

struct Record : public RecordLogRef {
  Record() : id_(0) {}     // Construct an Empty Record.
  explicit Record(ID id);  // Construct a Node.
  Record(ID aid, const RecordLog::Entry* ptr, RecordLogRefType ref_type,
         bool ephemeral);
  Record(ID aid, RecordLogRef* swap);
  Record(ID aid, RecordLogRef rle);

  // Generic/Node API.
  ID id() const { return id_; }
  bool IsEmpty() const;
  bool IsNode() const;
  bool IsValue() const;
  bool IsLink() const;
  size_t bytes() const;
  ID next_id() const;

  // Value API.
  size_t ValueSize() const;
  StringPiece ToStringPiece() const;
  string ToString() const;
  char* c_str() const;
  UUID ValueUUID() const;

  // Link API.
  ID subject() const;
  ID predicate() const;
  ID object() const;
  ID author() const;
  LinkTime when() const;
  int LinkSize() const;
  bool dead() const;
  int subgraphs_size() const;
  vector<ID> subgraphs() const;
  ID linkage(int i) const;
  LinkState state() const;

  void set_id(ID id) { id_ = id; }
  // Copy and change one aspect.  The result is a RefCountedRecord.
  Record ChangeID(ID new_id) const MUST_USE_RESULT;
  Record ChangeValue(StringPiece new_value) const MUST_USE_RESULT;
  Record ChangeLinkage(Linkage i, ID new_linkage) const MUST_USE_RESULT;
  Record ChangeWhen(LinkTime new_when) const MUST_USE_RESULT;
  Record ChangeState(LinkState new_state) const MUST_USE_RESULT;

  static Record FromProto(const TuplePb& tuple_pb);
  void ToProto(TuplePb* tuple_pb) const;

  // Static Constructors.
  static Record Node(ID id) { return Record(id); }
  static Record Value(ID id, StringPiece value);
  static Record Link(ID id, LinkTime when, ID author, ID subject, ID predicate,
                     ID object, LinkState dead, const vector<ID>& subgraphs);
  // START DEPRECATED
  // TODO(jplev): remove these when we move to multiple subgraphs.
  static Record Link(ID id, LinkTime when, ID author, ID subgraph, ID subject,
                     ID predicate, ID object, LinkState dead) {
    return Link(id, when, author, subject, predicate, object, dead,
                vector<ID>{subgraph});
  }
  static Record Link(ID id, LinkTime when, ID author, ID subject, ID predicate,
                     ID object, LinkState dead) {
    return Link(id, when, author, subject, predicate, object, dead,
                vector<ID>());
  }
  ID subgraph() const {
    auto s = subgraphs();
    if (s.size()) return s[0];
    return 0;
  }
  // END DEPRECATED

  bool IsValidLink() const;
  string ToDebugString() const;
  bool operator==(const Record& r) { return id_ == r.id_; }
  static LinkTime Now() {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * kLinkTimePerSecond +
           ts.tv_nsec / (1000000000 / kLinkTimePerSecond);
  }

  static char kStaticNodeRecordLogEntry;  // Nodes contain no data.
  ID id_;
};
}  // namespace blythe

namespace std {
template <>
struct less<blythe::Record> {
  bool operator()(const blythe::Record& x, const blythe::Record& y) const {
    return x.id() < y.id();
  }
};
}  // std

namespace blythe {
class RecordSet : public util::btree::btree_set<Record> {
  typedef util::btree::btree_set<Record> Base;
  using Base::Base;

 public:
  Record Get(ID id) const { return *find(Record(id)); }
  string GetValue(ID id) const { return find(Record(id))->ToString(); }
  ID StartID() const { return begin()->id(); }
  ID NextID() const { return rbegin()->next_id(); }
  const_iterator LowerBound(ID id) const { return lower_bound(Record(id)); }
};

::util::Status WriteToTrio(const RecordSet& records, StringPiece filename);
std::unique_ptr<RecordSet> LoadRecordSet(StringPiece filename);

Record MakeRawRecord(ID id, const RecordLog* rlog);
Record MakeEphemeralRecord(ID id, RecordLog::Entry* entry);
Record MakeEphemeralRecord(ID id, RecordLog* rlog);

Record NewRefCountedRecord(ID id, StringPiece s);
Record NewRefCountedRecord(ID id, bool dead, int nlinkages, ID* linkages);
Record CopyToRefCountedRecord(ID id, RecordLog* log);
Record CopyToRefCountedRecord(Record r);

Record MakeRefCountedRecord(Record r);  // If not ref-counted, make ref-counted.
Record MakeLastingRecord(Record r);  // If ephemeral, make ref-counted.

// Utilities.

typedef Callback1<const Record&> ForEachRecordCallback;

RecordLog* LoadRecordLog(StringPiece, ID* next_id);
UUID StringToUUID(StringPiece s);
LinkTime TupleTime_Now();
bool SameContent(const Record& t1, const Record& t2);
ID RenumberID(hash_map<ID, ID>* map, ID id);
Record Renumber(const Record& t, ID* next_id, hash_map<ID, ID>* map);

#include "common/record-inl.h"
