#include <stddef.h>
#include <unordered_map>
using std::unordered_map;
#define hash_map unordered_map
#include <string>
using ::std::string;
#include <utility>
#include <vector>
using ::std::vector;

#include "base/callback.h"
#include "base/integral_types.h"
#include "base/macros.h"
#include "common/common_types.h"
#include "common/record.h"
#include "pgi/pgi.h"
#include "pgi/pgi_callbacks.h"
#include "pgi/pgi_idset.h"
#include "pgi/pgi_resource_manager.h"
#include "strings/stringpiece.h"

class PgiTable;
class TupleStore;

const int kLinkageIndexes = 5;

enum BuiltinIndex {
  kSubjectIndex,
  kPredicateIndex,
  kObjectIndex,
  kAuthorIndex,
  kSubgraphIndex,
  kValueIndex,
  kSPOIndex,
  kSubjectPredicateIndex,
  kPredicateObjectIndex,
  kCompoundIndex,
  kNumBuiltinIndexes
};

inline int LinkageToIndex(Linkage l) {
  switch (l) {
    case kSubject:
      return kSubjectIndex;
    case kPredicate:
      return kPredicateIndex;
    case kObject:
      return kObjectIndex;
    case kAuthor:
      return kAuthorIndex;
    default:
      return kSubgraphIndex;  // Everything else is a subgraph.
  }
}

UUID SPOKey(const Record& tuple);
UUID SPOKey(ID s, ID p, ID o);
// The elements of parameter 'spokes' are (predicate, node id) pairs.
UUID CSPOKey(const vector<IDPair>& spokes);
UUID IntersectionKey(ID x, ID y);
int IntersectionIndex(Linkage l1, Linkage l2);  // Returns -1 if there is none.

struct Indexes {
  explicit Indexes(TupleStore* ts);

  void IndexTuples(const vector<Record>& tuples);

  const PgiIDSet FindLinksWithIDAtLinkage(Linkage l, ID id) const;

  ID FindValueByString(StringPiece sid) const;
  void FindValueByString(const vector<UUID>& sids, vector<ID>* value_ids) const;

  // Return ID of Link with (subject, predicate, object) or 0.
  ID FindLinkBySPO(ID subject, ID predicate, ID object) const;
  ID FindLinkBySPO(UUID spo_key) const;
  void FindLinkBySPO(const vector<UUID>& spo_keys, vector<ID>* link_ids) const;
  const PgiIDSet FindIDsBySPO(ID subject, ID predicate, ID object) const;
  const PgiIDSet FindIDsBySPO(UUID spo_key) const;

  const PgiIDSet FindIDsByIntersection(int index, ID x, ID y) const;
  const PgiIDSet FindIDsByIntersection(int index, UUID key) const;
  const ID FindIDByIntersection(int index, ID x, ID y) const;
  const ID FindIDByIntersection(int index, UUID key) const;
  void FindIDByIntersection(int index, const vector<UUID>& keys,
                            vector<ID>* out_ids) const;

  // Find the hub of a compound given the CSPOKeys.
  ID FindHubByCSPO(const vector<pair<ID, ID>>& spokes) const;
  void FindHubByCSPO(const vector<UUID>& cspo_strings,
                     vector<ID>* out_hub_ids) const;

 private:
  hash_map<ID, vector<IDPair>> MakeHubMap(const vector<Record>& tuples) const;
  PgiIDSet GetSet(PgiTable* t, UUID k) const;
  PgiTable* index(int i) const;

  static const size_t kStackBufferMaxSpokeCount;

  const TupleStore* ts_;
  const bool is_transient_;
};
