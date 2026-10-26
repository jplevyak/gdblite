#include <string>
using ::std::string;
#include <tuple>
#include <utility>
#include <vector>
using ::std::vector;

#include "base/integral_types.h"
#include "base/logging.h"
#include "common/common_types.h"
#include "common/record.h"
#include "core/tupleindex.h"
#include "dataflow/data_interface.h"
#include "pgi/local_pgi.h"
#include "pgi/pgi.h"
#include "pgi/pgi_idset.h"
#include "pgi/pgi_resource_manager.h"
#include "util/btree/btree_set.h"
#include "util/task/status.h"

class Closure;

namespace dataflow {

typedef uint64 ID;

typedef pair<ID, ID> IDPair;
typedef std::tuple<ID, ID, ID> IDTriple;

class SetEngineDataInterface : public DataInterface {
 public:
  typedef PgiResourceManager ResourceManager;

  explicit SetEngineDataInterface(const PhysicalGraphInterface& pgi)
      : next_map_index_(1), pgi_(pgi) {}

  template <typename I>
  void Find(const I& i, typename I::BufferType buffer, Callback* done) const;
  template <typename I>
  void FindDone(const I& i, typename I::BufferType buffer) const {}

  template <typename C>
  void MakeSet(const typename C::MaterializedType& set_to_store, C* set);

 private:
  ID next_map_index_;
  LocalIDSetMap map_;
  const PhysicalGraphInterface& pgi_;
};

// A wrapper for sets.  Note, pgi currently only supports sets of ID,
// so support for other data types is local-only via the STL set<>.
// TODO: refcount Set<C> (reffed_ptr) to record their memory usage.
template <typename C>
class Set : public util::btree::btree_set<C> {
 public:
  typedef util::btree::btree_set<C> base;
  typedef C value_type;
  typedef SetEngineDataInterface DataInterfaceType;
  typedef base MaterializedType;
  typedef C* iterator;

  Set() {}
  template <typename K>
  explicit Set(const K& s) {
    std::copy(s.begin(), s.end(), std::inserter(values_, values_.begin()));
  }
  void Materialize(SetEngineDataInterface* di, MaterializedType* values) {
    *values = values_;
  }
  bool IsImmediate() const { return true; }

  Closure* Read(ID start, ID end, PgiIDSetReadCallback* callback) const {
    LOG(FATAL) << "should not be reached";
  }

  Closure* Probe(vector<PgiBufferPtr<ID>> buffer,
                 PgiIDSetProbeCallback* callback) const {
    LOG(FATAL) << "should not be reached";
  }

 private:
  MaterializedType values_;
};

// Wrapper for the existing Pgi and stl set.
// TODO: replace with combined representation.
template <>
class Set<ID> : public PgiIDSet {
 public:
  typedef PgiIDSet base;
  typedef ID value_type;
  typedef SetEngineDataInterface DataInterfaceType;
  typedef DataInterfaceType::Callback Callback;
  typedef IDSet MaterializedType;
  typedef ID* iterator;

  Set() {}
  Set(SetEngineDataInterface* di, const IDSet& s);
  explicit Set(const PgiIDSet& other) : PgiIDSet(other) {}

  void Materialize(SetEngineDataInterface* di, MaterializedType* values) {
    ToIDSet(values);
  }
};

typedef pair<ID, Set<ID>> ID2Set;

template <typename C>
void SetEngineDataInterface::MakeSet(
    const typename C::MaterializedType& set_to_store, C* set) {
  std::copy(set_to_store.begin(), set_to_store.end(),
            std::inserter(*set, set->begin()));
}

template <>
inline void SetEngineDataInterface::MakeSet(const IDSet& transient_set,
                                            Set<ID>* s) {
  ID id = next_map_index_++;
  map_.ElementInsert(UUID(id), transient_set);
  *s = Set<ID>(PgiIDSet(id, &map_));
}

inline Set<ID>::Set(SetEngineDataInterface* di, const IDSet& s) {
  di->MakeSet(s, this);
}

template <typename T>
struct ToPgiIDSet {
  typedef T C;
};
template <>
struct ToPgiIDSet<Set<ID>> {
  typedef PgiIDSet C;
};

template <typename K, typename V>
class SetEngineIndex {
 public:
  typedef K KeyType;
  typedef V ValueType;
  typedef pair<K, V> OutputType;
  typedef SetEngineDataInterface DataInterfaceType;
  typedef PgiBufferPtr<pair<K, V>> BufferType;
  typedef PgiBufferPtr<pair<K, typename ToPgiIDSet<V>::C>> FindBufferType;

  explicit SetEngineIndex(int pgi_index) : pgi_index_(pgi_index) {}

  int pgi_index() const { return pgi_index_; }
  static void SetKey(K k, OutputType* o) { o->first = k; }

 private:
  const int pgi_index_;
};
typedef SetEngineIndex<ID, Set<ID>> IDToSetIndex;
typedef SetEngineIndex<UUID, Set<ID>> UUIDToSetIndex;

class TupleIndex {
 public:
  typedef ID KeyType;
  typedef Record OutputType;
  typedef SetEngineDataInterface DataInterfaceType;
  typedef PgiBufferPtr<Record> BufferType;
  typedef PgiBufferPtr<Record> FindBufferType;

  static void SetKey(ID k, OutputType* o) { o->set_id(k); }
};

template <typename I>
inline void SetEngineDataInterface::Find(const I& index,
                                         typename I::BufferType b,
                                         Callback* done) const {
  pgi_.index(index.pgi_index())
      ->Find(*(reinterpret_cast<typename I::FindBufferType*>(&b)), done);
}

template <>
inline void SetEngineDataInterface::Find(const TupleIndex& index,
                                         PgiBufferPtr<Record> b,
                                         Callback* done) const {
  pgi_.records()->Get(b, done);
}

template <>
inline void SetEngineDataInterface::FindDone(const TupleIndex& index,
                                             TupleIndex::BufferType b) const {
  for (auto& e : b) e = MakeLastingRecord(e);
}
}  // namespace dataflow
