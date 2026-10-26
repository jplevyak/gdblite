#include <stddef.h>
#include <utility>

#include "base/callback.h"

namespace util {
class Status;
}  // namespace util

namespace dataflow {

// An abstraction for (potentially async) indexing and set actions.
class DataInterface {
 public:
  typedef Callback1<util::Status> Callback;

  // Load values from an Index.
  template <typename I>
  void Find(const I& i, typename I::BufferType buffer, Callback* done);
  template <typename I>
  void FindDone(const I& i, typename I::BufferType buffer);

  class MemoryBlock;

  class ResourceManager {
   public:
    MemoryBlock* Alloc(size_t amount);
    void Free(MemoryBlock* block);
    void* GetData(MemoryBlock* block);
  };
};

// Abstraction for a (potentially lazy) set/stream of values.  This should
// satisfy the following conditions and take the place of the template parameter
// S below.
// TODO: consider making this comments/documentation or enforcing it in
// some manner (e.g. C++ Concepts).
template <typename C>
class DataInterfaceStream {
 public:
  class DataInterfaceType;
  class MaterializedType;
  class StreamBufferType;
  class ValueBufferType;
  class size_type;
  class value_type;

  void Materialize(DataInterfaceType* di, MaterializedType* c);
};

// Abstraction for a (potentially async) index/map.  Each different type of
// index will have a different DataIndex subclass or instantiation.  Each
// instance of an index is determined by that class and a key (of KeyType). This
// should satisfy the following conditions and take the place of the template
// parameter I below.
// TODO: consider making this comments/documentation or enforcing it in
// some manner (e.g. C++ Concepts).
template <typename K, typename V>
class DataInterfaceIndex {
 public:
  typedef K KeyType;
  typedef V ValueType;
  typedef pair<K, V> OutputType;
  class BufferType;
  class DataInterfaceType;
};
}  // namespace dataflow
