#include "tupleindex.h"

#include <algorithm>
#include <unordered_map>
using std::unordered_map;
#define hash_map unordered_map
#include <map>
using ::std::map;
#include <memory>

#include "base/logging.h"
#include "codegen/schema.h"
#include "core/bootstrap.h"
#include "core/metaschema.h"
#include "core/tuplestore.h"
#include "pgi/local_pgi.h"
#include "strings/strcat.h"
#include "util/coding/coder.h"
#include "util/coding/varint.h"

using std::unique_ptr;

typedef b_bootstrap_constants bc;

const size_t kSPOKeyBufSize = Varint::kMax64 * 3;
const size_t kIntersectionKeyBufSize = 1 + Varint::kMax64 * 2;

const size_t kStackBufferMaxSpokeCount = 16;
const size_t kCSPOKeyBufSize = Varint::kMax64 * 2 * kStackBufferMaxSpokeCount;

PgiTable* Indexes::index(int i) const { return ts_->pgi()->index(i); }

UUID SPOKey(const Record& tuple) {
  return SPOKey(tuple.subject(), tuple.predicate(), tuple.object());
}

static bool SmallUUID(const Encoder& encoder, UUID *uuid) {
  if (encoder.length() <= sizeof(UUID)) {
    memcpy(reinterpret_cast<char*>(uuid), encoder.base(), encoder.length());
    if (encoder.length() < sizeof(UUID))
      memset(reinterpret_cast<char*>(uuid) + encoder.length(), 0,
             sizeof(UUID) - encoder.length());
    return true;
  }
  return false;
}

UUID SPOKey(ID s, ID p, ID o) {
  char buffer[kSPOKeyBufSize];
  Encoder encoder(buffer, sizeof(buffer));
  encoder.clear();
  encoder.put_varint64(s);
  encoder.put_varint64(p);
  encoder.put_varint64(o);
  UUID uuid;
  if (SmallUUID(encoder, &uuid)) return uuid;
  return StringToUUID(StringPiece(encoder.base(), encoder.length()));
}

UUID CSPOKey(const vector<IDPair>& source_spokes) {
  vector<IDPair> spokes = source_spokes;
  sort(spokes.begin(), spokes.end());

  char stack_buffer[kCSPOKeyBufSize];
  void* bufptr = stack_buffer;
  size_t bufsize = sizeof(stack_buffer);

  unique_ptr<char[]> heap_buffer;
  if (spokes.size() > kStackBufferMaxSpokeCount) {
    bufsize = Varint::kMax64 * 2 * spokes.size();
    heap_buffer.reset(new char[bufsize]);
    bufptr = heap_buffer.get();
  }

  Encoder encoder(bufptr, bufsize);
  encoder.clear();
  for (auto spoke : spokes) {
    encoder.put_varint64(spoke.first);   // predicate
    encoder.put_varint64(spoke.second);  // non-hub node
  }
  UUID uuid;
  if (SmallUUID(encoder, &uuid)) return uuid;
  return StringToUUID(StringPiece(encoder.base(), encoder.length()));
}

UUID IntersectionKey(ID x, ID y) {
  UUID uuid;
  uuid.uuid0 = x;
  uuid.uuid1 = y;
  return uuid;
}

int IntersectionIndex(Linkage x, Linkage y) {
  if (x == kSubject && y == kPredicate)
    return kSubjectPredicateIndex;
  else if (x == kPredicate && y == kObject)
    return kPredicateObjectIndex;
  return -1;
}

template <typename K>
static void ToKvs(const vector<K>& keys, PgiBufferPtr<pair<K, ID>> out_kvs) {
  CHECK_EQ(keys.size(), out_kvs.size());
  for (size_t i = 0; i < keys.size(); i++) {
    out_kvs[i].first = keys[i];
    out_kvs[i].second = kNoID;
  }
}

template <typename K>
static void ToKvs(const vector<K>& keys,
                  PgiBufferPtr<pair<K, PgiIDSet>> out_kvs) {
  CHECK_EQ(keys.size(), out_kvs.size());
  for (size_t i = 0; i < keys.size(); i++) {
    out_kvs[i].first = keys[i];
    out_kvs[i].second = PgiIDSet();
  }
}

template <typename K>
static void FromKvs(PgiBufferPtr<pair<K, ID>> kvs, vector<ID>* out_values) {
  size_t n = kvs.size();
  out_values->resize(n);
  for (size_t i = 0; i < n; i++)
    if (kvs[i].second) (*out_values)[i] = kvs[i].second;
}

template <typename K>
static void FromKvs(PgiBufferPtr<pair<K, PgiIDSet>> kvs,
                    vector<PgiIDSet>* out_values) {
  size_t n = kvs.size();
  out_values->resize(n);
  for (size_t i = 0; i < n; i++) {
    // TODO(davidespinosa): Conditional for base/ext layering?
    if (!kvs[i].second.IsEmpty()) (*out_values)[i] = kvs[i].second;
  }
}

template <typename K>
static void FromKvs(PgiBufferPtr<pair<K, PgiIDSet>> kvs,
                    vector<ID>* out_values) {
  size_t n = kvs.size();
  out_values->resize(n);
  for (size_t i = 0; i < n; i++) {
    // TODO(davidespinosa): Conditional for base/ext layering?
    ID id = kvs[i].second.Max();
    if (id) (*out_values)[i] = id;
  }
}

template <typename T>
static void WriteIDs(PgiTable* t, PgiBufferPtr<T> kvs, PgiCallback* done) {
  t->Insert(nullptr, kvs, new PgiCallbackFreeBufferPtr<T>(kvs, done));
}

template <typename T>
static void ReadIDs(PgiTable* t, PgiBufferPtr<T> kvs) {
  BlockingPgiCallback done;
  t->Find(kvs, &done);
  done.WaitCheckSuccess();
}

template <typename T>
static ID ReadID(PgiTable* t, T key) {
  ScopedPgiBufferPtr<pair<T, ID>> kvs(1);
  kvs[0].first = key;
  kvs[0].second = kNoID;
  ReadIDs(t, kvs);
  return kvs[0].second;
}

template <typename K>
static void ReadIDs(PgiTable* t, const vector<K>& keys,
                    vector<ID>* out_values) {
  ScopedPgiBufferPtr<pair<K, ID>> kvs(keys.size());
  ToKvs(keys, kvs);
  ReadIDs(t, kvs);
  FromKvs(kvs, out_values);
}

template <typename K>
static bool CheckUniqueIDs(PgiTable* t, PgiBufferPtr<pair<K, ID>> kvs) {
  ScopedPgiBufferPtr<pair<K, ID>> check_kvs(kvs.size());
  hash_map<K, ID> dup_check;
  for (size_t i = 0; i < kvs.size(); i++) {
    // Check that we aren't writing conflicting values in kvs.
    if (dup_check.count(kvs[i].first) &&
        dup_check[kvs[i].first] != kvs[i].second)
      return false;
    dup_check[kvs[i].first] = kvs[i].second;
    check_kvs[i] = make_pair(kvs[i].first, 0);
  }
  ReadIDs(t, check_kvs);
  for (size_t i = 0; i < kvs.size(); i++)
    if (check_kvs[i].second && check_kvs[i].second != kvs[i].second)
      return false;
  return true;
}

template <typename K>
static void WriteUniqueIDs(PgiTable* t, PgiBufferPtr<pair<K, ID>> kvs,
                           PgiCallback* done) {
  CHECK(CheckUniqueIDs(t, kvs));
  WriteIDs(t, kvs, done);
}

template <typename T>
static const PgiIDSet ReadSet(PgiTable* t, T key) {
  ScopedPgiBufferPtr<pair<T, PgiIDSet>> p(1);
  p[0] = make_pair(key, PgiIDSet());
  BlockingPgiCallback done;
  t->Find(p, &done);
  done.WaitCheckSuccess();
  return p[0].second;
}

template <typename K, typename V>
static void ReadSets(PgiTable* t, PgiBufferPtr<pair<K, V>>* kvs) {
  BlockingPgiCallback done;
  t->Find(*kvs, &done);
  done.WaitCheckSuccess();
}

template <typename K>
static void ReadSetsMax(PgiTable* t, const vector<K>& keys,
                        vector<ID>* out_values) {
  ScopedPgiBufferPtr<pair<K, PgiIDSet>> kvs(keys.size());
  ToKvs(keys, kvs);
  ReadSets(t, &kvs);
  FromKvs(kvs, out_values);
}

template <typename T>
static void WriteSets(PgiTable* t, T* key_to_id_set, PgiCallback* done) {
  t->Insert(nullptr, *key_to_id_set,
            new PgiCallbackDeleteValues<T>(key_to_id_set, done));
}

template <typename C>
void Get(const C& c, typename C::key_type k, typename C::mapped_type* d) {
  auto i = c.find(k);
  if (i != c.end()) *d = i->second;
}

PgiIDSet Indexes::GetSet(PgiTable* t, UUID k) const {
  auto i = t->map()->map_.find(k);
  if (i != t->map()->map_.end()) return PgiIDSet(k, t->map());
  return PgiIDSet();
}

template <typename C>
void GetMaxOrZero(const C& c, typename C::key_type k, ID* d) {
  auto i = c.find(k);
  if (i != c.end())
    *d = i->second.empty() ? 0 : *i->second.rbegin();
}

template <typename C>
void GetMax(const C& c, typename C::key_type k, ID* d) {
  auto i = c.find(k);
  if (i != c.end() && !i->second.empty()) *d = *i->second.rbegin();
}

template <typename C>
typename C::mapped_type Get(const C& c, typename C::key_type k) {
  auto i = c.find(k);
  if (i != c.end()) return i->second;
  return typename C::mapped_type();
}

template <typename C>
ID GetMax(const C& c, typename C::key_type k) {
  auto i = c.find(k);
  if (i != c.end())
    return i->second.empty() ? 0 : *i->second.rbegin();
  return 0;
}

ID Indexes::FindValueByString(StringPiece s) const {
  if (is_transient_)
    return Get(index(kValueIndex)->map()->uuid2id_map_, StringToUUID(s));
  return ReadID(index(kValueIndex), StringToUUID(s));
}

void Indexes::FindValueByString(const vector<UUID>& sids,
                                vector<ID>* out_value_ids) const {
  if (is_transient_) {
    out_value_ids->resize(sids.size());
    int i = 0;
    for (auto& uuid : sids)
      Get(index(kValueIndex)->map()->uuid2id_map_, uuid,
          &(*out_value_ids)[i++]);
    return;
  }
  ReadIDs(index(kValueIndex), sids, out_value_ids);
}

ID Indexes::FindLinkBySPO(UUID spo_key) const {
  if (is_transient_)
    return GetMax(index(kSPOIndex)->map()->map_, spo_key);
  return ReadSet(index(kSPOIndex), spo_key).Max();
}

ID Indexes::FindLinkBySPO(ID subject, ID predicate, ID object) const {
  return FindLinkBySPO(SPOKey(subject, predicate, object));
}

void Indexes::FindLinkBySPO(const vector<UUID>& keys,
                            vector<ID>* out_values) const {
  if (is_transient_) {
    out_values->resize(keys.size());
    int i = 0;
    for (auto& uuid : keys)
      GetMaxOrZero(index(kSPOIndex)->map()->map_, uuid, &(*out_values)[i++]);
    return;
  }
  ReadSetsMax(index(kSPOIndex), keys, out_values);
}

const PgiIDSet Indexes::FindIDsBySPO(UUID spo_key) const {
  if (is_transient_) return GetSet(index(kSPOIndex), spo_key);
  return ReadSet(index(kSPOIndex), spo_key);
}

const PgiIDSet Indexes::FindIDsBySPO(ID subject, ID predicate,
                                     ID object) const {
  return FindIDsBySPO(SPOKey(subject, predicate, object));
}

const PgiIDSet Indexes::FindIDsByIntersection(int i, UUID key) const {
  if (is_transient_) return GetSet(index(i), key);
  return ReadSet(index(i), key);
}

const PgiIDSet Indexes::FindIDsByIntersection(int i, ID x, ID y) const {
  return FindIDsByIntersection(i, IntersectionKey(x, y));
}

const ID Indexes::FindIDByIntersection(int i, UUID key) const {
  if (is_transient_) return GetMax(index(i)->map()->map_, key);
  return ReadID(index(i), key);
}

const ID Indexes::FindIDByIntersection(int i, ID x, ID y) const {
  return FindIDByIntersection(i, IntersectionKey(x, y));
}

void Indexes::FindIDByIntersection(int idx, const vector<UUID>& keys,
                                   vector<ID>* out_ids) const {
  if (is_transient_) {
    out_ids->resize(keys.size());
    auto m = index(idx)->map()->map_;
    int i = 0;
    for (auto& uuid : keys) GetMax(m, uuid, &(*out_ids)[i++]);
    return;
  }
  ReadSetsMax(index(idx), keys, out_ids);
}

ID Indexes::FindHubByCSPO(const vector<IDPair>& spokes) const {
  if (is_transient_)
    return Get(index(kCompoundIndex)->map()->uuid2id_map_, CSPOKey(spokes));
  return ReadID(index(kCompoundIndex), CSPOKey(spokes));
}

void Indexes::FindHubByCSPO(const vector<UUID>& cspos,
                            vector<ID>* out_hub_ids) const {
  if (is_transient_) {
    out_hub_ids->resize(cspos.size());
    int i = 0;
    for (auto& uuid : cspos)
      Get(index(kCompoundIndex)->map()->uuid2id_map_, uuid,
          &(*out_hub_ids)[i++]);
    return;
  }
  ReadIDs(index(kCompoundIndex), cspos, out_hub_ids);
}

hash_map<ID, vector<IDPair>> Indexes::MakeHubMap(
    const vector<Record>& tuples) const {
  hash_map<ID, vector<IDPair>> hub_to_spokes;
  for (const Record& tuple : tuples) {
    if (!tuple.IsLink()) continue;
    const SimpleProperty* sp = ts_->FindSimplePropertyByID(tuple.predicate());
    const CompoundProperty* cp = FindCompoundParent(*CHECK_NOTNULL(sp));
    if (!cp) continue;
    Linkage hub_linkage = GetHubLinkage(*cp, sp->tuple_id());
    ID hub_id = tuple.linkage(hub_linkage);
    ID non_hub_id = tuple.linkage(InverseLinkage(hub_linkage));
    hub_to_spokes[hub_id].push_back(IDPair(sp->tuple_id(), non_hub_id));
  }
  return hub_to_spokes;
}

const PgiIDSet Indexes::FindLinksWithIDAtLinkage(Linkage l, ID id) const {
  if (is_transient_) return GetSet(index(LinkageToIndex(l)), UUID(id));
  return ReadSet(index(LinkageToIndex(l)), id);
}

Indexes::Indexes(TupleStore* ts)
    : ts_(ts), is_transient_(ts->pgi()->IsTransient()) {}

void Indexes::IndexTuples(const vector<Record>& tuples) {
  CountingPgiCallback done(kNumBuiltinIndexes - 1);

  // Count values and tuples.
  int64 nvalues = 0;
  int64 ntuples = 0;
  int64 npks = 0;
  vector<IDPair> updates;
  for (const Record& tuple : tuples) {
    if (tuple.IsValue()) {
      nvalues++;
    } else if (tuple.IsLink()) {
      ntuples++;
      if (tuple.predicate() == bc::b_p_primary_key) npks++;
    }
  }

  if (is_transient_) {
    for (const Record& tuple : tuples) {
      if (tuple.IsValue()) {
        auto& map = index(kValueIndex)->map()->uuid2id_map_;
        auto it = map.insert(
            make_pair(StringToUUID(tuple.ToStringPiece()), tuple.id()));
        CHECK(it.second) << "Duplicate value '" << tuple.ToStringPiece() << "'";
        it.first->second = tuple.id();
      }
      if (tuple.IsLink()) {
        index(kSPOIndex)->map()->ElementInsert(SPOKey(tuple), tuple.id());
        for (auto l : kAllLinkages) {
          const ID linkage = tuple.linkage(l);
          if (!linkage) continue;
          index(LinkageToIndex(l))->map()->ElementInsert(UUID(linkage),
                                                         tuple.id());
        }
        index(kSubjectPredicateIndex)->map()->ElementInsert(
               IntersectionKey(tuple.subject(), tuple.predicate()), tuple.id());
        index(kPredicateObjectIndex)->map()->ElementInsert(
               IntersectionKey(tuple.predicate(), tuple.object()), tuple.id());
      }
    }
    for (auto kv : MakeHubMap(tuples))
      index(kCompoundIndex)->map()->uuid2id_map_[CSPOKey(kv.second)] =
          kv.first;
    return;
  }

  // Not Transient.
  PgiBufferPtr<pair<UUID, ID>> vals(nvalues);
  unique_ptr<UUIDToIDSetMap> spos(new UUIDToIDSetMap);
  vector<unique_ptr<IDToIDSetMap>> linkage_updates(kLinkageIndexes);
  for (auto l : kAllLinkages)
    linkage_updates[LinkageToIndex(l)].reset(new IDToIDSetMap);
  unique_ptr<UUIDToIDSetMap> intersections[2];
  intersections[0].reset(new UUIDToIDSetMap);
  intersections[1].reset(new UUIDToIDSetMap);

  int64 i_value = 0;
  for (const Record& tuple : tuples) {
    if (tuple.IsValue())
      // TODO(jplev): remove this gross indecency.
      vals[i_value++] =
          make_pair(StringToUUID(tuple.ToStringPiece()), tuple.id());
    if (tuple.IsLink()) {
      (*spos)[SPOKey(tuple)].insert(tuple.id());
      for (auto l : kAllLinkages) {
        const ID linkage = tuple.linkage(l);
        if (!linkage) continue;
        int index = LinkageToIndex(l);
        (*linkage_updates[index])[tuple.linkage(l)].insert(tuple.id());
      }
      (*intersections[0])[IntersectionKey(tuple.subject(), tuple.predicate())]
          .insert(tuple.id());
      (*intersections[1])[IntersectionKey(tuple.predicate(), tuple.object())]
          .insert(tuple.id());
    }
  }

  WriteUniqueIDs(index(kValueIndex), vals, &done);
  WriteSets(index(kSPOIndex), spos.release(), &done);
  for (auto l : kAllLinkages)
    WriteSets(index(LinkageToIndex(l)),
              linkage_updates[LinkageToIndex(l)].release(), &done);
  WriteSets(index(kSubjectPredicateIndex), intersections[0].release(), &done);
  WriteSets(index(kPredicateObjectIndex), intersections[1].release(), &done);

  done.WaitCheckSuccess();

  // Process compound index *after* the other indices
  // so it can find simple properties in the meta schema.
  BlockingPgiCallback done_compound;

  // Collect the new link id groups and CSPOKeys
  auto hubs = MakeHubMap(tuples);
  PgiBufferPtr<pair<UUID, ID>> cspo(hubs.size());
  int i_hubs = 0;
  for (auto kv : hubs) cspo[i_hubs++] = make_pair(CSPOKey(kv.second), kv.first);
  WriteIDs(index(kCompoundIndex), cspo, &done_compound);
  done_compound.WaitCheckSuccess();
}
