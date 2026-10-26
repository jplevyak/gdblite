// A framework for data flow and event driven computation on general graphs
// with flow control.
//
// Example: simple intersection of two sets (see set_engine_test.cc for details)
//
//   Set<ID> six_set, ten_set;
//   set<ID> result_set;
//   PopulateIDSet(0, 100, 6, &six_set);
//   PopulateIDSet(0, 100, 10, &ten_set);
//   auto sink = RootPtr(StoreOp(
//       new Intersect2Stream(
//           IDSetOp(&six_set), IDSetOp(&ten_set)),
//       &result_set, true));
//   Flow flow;
//   flow.Add(sink);
//   flow.Run(1);
//   ASSERT_EQ(result_set.size(), 4);

#include <algorithm>
#include <map>
using ::std::map;
#include <memory>
#include <new>
#include <set>
using ::std::set;
#include <string>
using ::std::string;
#include <utility>

#include "base/integral_types.h"
#include "base/logging.h"
#include "base/mutex.h"
#include "base/thread_annotations.h"
#include "dataflow/handler.h"
#include "util/invasive_queue.h"
#include "util/ref_ptr.h"
#include "util/gtl/stl_util.h"
#include "util/refcount/reffed_ptr.h"
#include "util/task/status.h"

namespace dataflow {

typedef ConditionMutex Mutex;
// Only works for the constructor.
#define MutexLock ConditionMutexLock

typedef uint64 ID;

class BaseChannel;
class BaseStream;
template <typename C, typename D>
class Channel;
template <typename C, typename D>
class ChannelBlock;

const int kDefaultCapacity = (1 << 18);   // 256K (2 blocks).
const int kDefaultBlockSize = (1 << 17);  // 128K.
const int kBuffersPerRead = 4;

// Extensible set of events.
enum EventType {
  RANGE_REQUEST,
  SEEK_REQUEST,
  SPLITS_REQUEST,
  READABLE,
  WRITABLE,
  NUM_BASE_EVENT_TYPES
};

// Main loop for data flow graph.  D is a DataInterface.
class BaseFlow {
 public:
  BaseFlow() : aborted_(false), notify_depth_(0) {}

  // Add a root operation and all transitively Attach()ed operations to the data
  // flow graph.  Multiple roots can be added for parallel evaluation.
  void Add(BaseStream* op) { root_operations_.insert(op); }

  // Remove the given operation, cancel all outstanding events and detach
  // the operation form the data flow graph.  This needs to be reentrant for
  // recursion from DetachedInternal().
  void Remove(BaseStream* op);

  // Call Start() on directly Add()ed opeartions.  Returns on an Abort() or
  // there are no more active operations.
  void Run();

  // Post an event to be called from the main event loop. This is for all
  // non-urgent events, e.g. READABLE, WRITABLE, etc.
  void Post(BaseHandler* h);

  // Notify a high-priority event either by direct call or by adding to the
  // front of the event queue.  This is for non-data status events which,
  // if processed ahead of data events, could result in avoiding work:
  // e.g. SEEK_REQUEST.
  // Note: the caller should return immediately as the object could be deleted.
  void Notify(BaseHandler* h);

  // Wait for a PostFromOtherThread for this handler.
  void WaitFor(BaseHandler* h);

  // Post an event from another thread (i.e. and RPC or Remote networking
  // thread).
  void PostFromOtherThread(BaseHandler* h) LOCKS_EXCLUDED(mutex_);

  // Stop the entire data flow graph evaluation, signal all operations by
  // calling Aborted() on them.
  void Abort(util::Status status) {
    status_ = status;
    aborted_ = true;
  }

  util::Status GetStatus() { return status_; }

 private:
  void InitializeRun();
  bool SomePending() EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  void WaitForAll(bool post);  // Wait for all outstanding off-thread events.
  // Stop waiting and optionally Post().
  void HandlePendingEvents(bool post) EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  void WaitForAllAndPost() { WaitForAll(true); }
  void WaitForAllAndDoNotPost() { WaitForAll(false); }
  void Timeout();

  static const int kDefaultTimeoutMsec;
  static const int kNotifyLimit;

  bool aborted_;
  util::Status status_;
  set<ref_ptr<BaseStream>> operations_;
  set<root_ptr<BaseStream>> root_operations_;
  Queue<BaseHandler> posted_events_;
  Queue<BaseHandler, QueueWaitingLink<BaseHandler>> waiting_events_;
  int notify_depth_;
  Mutex mutex_;
  // TODO: convert this to use an atomic queue.
  Queue<BaseHandler> pending_events_ GUARDED_BY(mutex_);
};

// Specialize for each D.
template <typename D>
class Flow : public BaseFlow {
 public:
  D* GetDataInterface();
};

// Base class for a channel reader.
class BaseChannelReader {
 public:
  explicit BaseChannelReader(BaseChannel* channel) : channel_(channel) {}

  void OnEvent(EventType et, BaseHandler* h);  // Register for an event.
  bool PostEvent(EventType et);
  bool NotifyEvent(EventType et);
  bool PostEventToChannel(EventType et);

  BaseChannel* GetChannel() { return channel_; }
  BaseStream* GetStream();  // Get the reading Stream.
  BaseFlow* GetFlow();

 protected:
  friend class BaseChannel;
  friend class BaseFlow;

  map<EventType, BaseHandler*> event_handlers_;
  BaseChannel* channel_;
};

// Base class for a data channel between two data flow Stream(s).
class BaseChannel {
 public:
  explicit BaseChannel(BaseStream* op)
      : operation_(op), capacity_(kDefaultCapacity), used_capacity_(0) {}

  void OnEvent(EventType et, BaseHandler* h);  // Register for an event.
  bool PostEvent(EventType et);
  bool NotifyEvent(EventType et);

  BaseStream* GetStream() { return operation_; }  // Writing Stream.
  BaseFlow* GetFlow();

  void ExpandCapacityTo(int64 capacity) { capacity_ = capacity; }
  int64 GetCapacity();

 protected:
  friend class BaseChannelReader;
  template <typename C, typename D>
  friend class ChannelReader;
  friend class BaseStream;
  friend class BaseFlow;

  map<EventType, BaseHandler*> event_handlers_;
  BaseStream* operation_;
  int64 capacity_;
  int64 used_capacity_;
  int64 estimated_size_;
  int64 estimated_cost_;
};

// Channel reader for a channel over data values of type C.
template <typename C, typename D>
class ChannelReader : public BaseChannelReader {
 public:
  typedef typename ChannelBlock<C, D>::iterator iterator;
  explicit ChannelReader(BaseChannel* channel)
      : BaseChannelReader(channel), offset_(0) {}

  ChannelBlock<C, D>* GetBlock();
  bool Next();  // Consume all, try move to the next block, return !IsEmpty.
  iterator begin();                  // Invalidated on Next()!
  iterator end();                    // Invalidated on Next()!
  bool IsEmpty();                    // There is no data ready to read.
  bool IsDone();                     // All available data is ready to read.
  void Consume(iterator new_start);  // Indicae partial read.
  void Reset() {
    offset_ = 0;
    block_.reset(nullptr);
  }

  Channel<C, D>* GetChannel() { return static_cast<Channel<C, D>*>(channel_); }

 private:
  friend class Channel<C, D>;

  void SetBlock(const reffed_ptr<ChannelBlock<C, D>>& b);

  int offset_;
  reffed_ptr<ChannelBlock<C, D>> block_;
};

// Base class for data flow operations which read and write channels.
// Note: use BaseStream via reffed_ptr and record their memory usage.
// TODO: In order to handle cycles, we need to flood the connected
// Attach graph from a root: either the set Add(ed) to a Flow or an external
// client pointer.
class BaseStream {
 public:
  BaseStream() : ref_count_(0), rooted_(0), root_(nullptr), flow_(nullptr) {}
  virtual ~BaseStream() {
    STLDeleteContainerPairSecondPointers(handlers_.begin(), handlers_.end());
  }

  void OnEvent(EventType et, BaseHandler* h);  // Register for an event.
  bool PostEvent(EventType et);
  bool NotifyEvent(EventType et);

  // Build and query the data flow graph.  This should be efficient, as
  // flattening a stream of Set(s) will require adding new SetReader operations
  // for any non-local set.  Returns the default output channel (if any).
  virtual BaseChannel* Attach(BaseStream* op);

  BaseFlow* GetFlow() { return flow_; }

  // Registration helpers.  Automatically construct callback.
  template <typename T, typename C, typename D>
  void Register(EventType et, void (T::*member)(Channel<C, D>*),
                Channel<C, D>* channel);
  template <typename T, typename C, typename D>
  void Register(EventType et, void (T::*member)(ChannelReader<C, D>*),
                ChannelReader<C, D>* reader);
  template <typename T>
  void Register(EventType et, void (T::*member)(BaseStream*),  // NOLINT
                BaseStream* op);

  // Called at the start of Flow::Run() on all nodes in the data flow graph.
  // Use this (or the constructor) to register for events.
  // Do *not* use this to cause data to flow (e.g. by calling Read()).
  virtual void Initialize() {}

  // Called on the nodes directly passed to Add().
  // Use this to start the data flow (e.g. call Read());
  virtual void Start() {}

  // Reset the operation, clearing the buffers and resetting any cursors.
  virtual void Reset() {}

  // Called on each operation when Abort() has been called.
  virtual void Aborted() {}

  // Called when an attached operation has Remove()ed.
  virtual void Detached(BaseStream* op) {}

  // Tell the Flow about outstanding asynchronous events (e.g. RPCs) for
  // which this Stream is waiting (for a PostFromOtherThread()).
  void WaitFor(BaseHandler* h);

  // ref_ptr/root_ptr interface.
  void Ref();
  bool Unref();
  void Root();
  void Unroot();

  // For resource management by flow_computation.
  template <typename C>
  void ReleaseAttached(C* container) {
    for (auto it = attached_.begin(); it != attached_.end();) {
      ref_ptr<BaseStream>& ptr = const_cast<ref_ptr<BaseStream>&>(*it);
      BaseStream* op = ptr.release();
      container->insert(container->begin(), op);
      ++it;
      DetachedInternal(op);
    }
  }

 protected:
  friend class BaseChannel;
  friend class BaseChannelReader;
  friend class BaseFlow;

  bool RootOrDetach();
  void ClearRoot(BaseStream* old_root);
  bool FindRoot();
  virtual void DetachedInternal(BaseStream* op) {
    attached_.erase(op);
    Detached(op);
  }

  static BaseStream* const kFindRootInProgress;

  int ref_count_;
  int rooted_;
  BaseStream* root_;  // Parent in spanning tree.
  map<EventType, BaseHandler*> event_handlers_;
  set<ref_ptr<BaseStream>> attached_;
  BaseFlow* flow_;
  map<pair<EventType, void*>, BaseHandler*> handlers_;
};

// Single output type Stream.  Note: can have multiple readers of the output.
template <typename C, typename D>
class Stream : public BaseStream {
 public:
  typedef C DataType;
  Stream() : output_(this) {}
  ~Stream() {}

  ChannelReader<C, D>* Input(BaseStream* op) {
    op->Attach(this);
    return Attach(op)->Reader(op);
  }
  Channel<C, D>* GetOutput() { return &output_; }
  virtual void Reset() { output_.Reset(); }

  // Read from the attached channel and probe for each element on the input
  // returning those that are found on the output.   The data will appear on
  // the output channel.
  virtual bool Probe(ChannelReader<C, D>* channel) { return false; }

  Flow<D>* GetFlow() { return static_cast<Flow<D>*>(flow_); }
  D* GetDataInterface() { return GetFlow()->GetDataInterface(); }

 protected:
  Channel<C, D>* Attach(BaseStream* op) {
    BaseStream::Attach(op);
    return &output_;
  }

 private:
  virtual void DetachedInternal(BaseStream* op) {
    output_.DetachReader(op);
    BaseStream::DetachedInternal(op);
    if (output_.NoReaders() && GetFlow()) GetFlow()->Remove(this);
  }
  Channel<C, D> output_;
};

// Partially specialize this template to specify the primary key for a type.
template <typename C>
struct PrimaryKey {
  typedef C type;
};

// Channel for data values of type C with DataInterface of type D.
template <typename C, typename D>
class Channel : public BaseChannel {
 public:
  typedef typename ChannelBlock<C, D>::iterator iterator;
  typedef typename PrimaryKey<C>::type K;
  explicit Channel(BaseStream* op) : BaseChannel(op), range_set_(false) {}
  virtual ~Channel() { CHECK(readers_.empty()); }

  bool PostEventToReaders(EventType et);

  ChannelBlock<C, D>* GetBlock();  // Returns NULL if no more blocks available.
  ChannelBlock<C, D>* Next();      // Returns NULL if no more blocks available.
  void Fill(iterator it);          // Indicate partial write and tell readers.
  void SomeWritten();  // Tell the readers that some has been written.
  void Done();         // Tell the readers that the stream is Done.
  void Reset() {
    head_.reset(nullptr);
    tail_.reset(nullptr);
  }

  virtual void SetRange(const K& start, const K& end);

  // Start data flowing.
  virtual bool Read();

  // Skip through the data stream until the first element not before 'start'
  // and start reading from that point.  NOTE: this *does not* guaratee that
  // the readers will see only values after the given value, only that they
  //* may* not.
  virtual bool Seek(const K& start);

  // The output of this channel will be a set of element which represent
  // natural divisions of the stream into generally similar amounts.  For
  // a B-Tree, this would correspond to the root block.
  virtual bool Splits();

  ChannelReader<C, D>* Reader(BaseStream* op);
  void DetachReader(BaseStream* op);
  bool NoReaders() { return readers_.size() == 0; }

 private:
  friend class BaseStream;
  friend class ChannelReader<C, D>;

  void SomeRead();       // Signal from Readers.
  int64 UsedCapacity();  // The amount of its capacity used by this channel.

  map<BaseStream*, ChannelReader<C, D>*> readers_;
  bool range_set_;
  K seek_;
  K start_;
  K end_;
  reffed_ptr<ChannelBlock<C, D>> head_;
  reffed_ptr<ChannelBlock<C, D>> tail_;
};

// Reference counted container of a block of channel values.
// NOTE: this doesn't support sharing between channels, nor is it threads-safe.
// We may want to reconsider theses restructions. We could introduce another
// level of indirection to a ChannelData structure which would be reference
// counted but could be shared by multiple ChannelBlock(s) to permit zero-copy
// forwarding of data from one Channel to another.
template <typename C, typename D>
struct ChannelBlock {
  typedef C* iterator;

  ChannelBlock()
      : ref_count(0), size(0), written(0), done(false), buffer(nullptr) {}
  explicit ChannelBlock(int n)
      : ref_count(0), size(n), written(0), done(false) {
    size = n / sizeof(C);
    memory_block = D::ResourceManager::Alloc(sizeof(C) * size);
    buffer = static_cast<C*>(D::ResourceManager::GetData(memory_block));
    for (int64 i = 0; i < size; i++) new (&buffer[i]) C();
  }
  ~ChannelBlock() {
    if (buffer) {
      for (int i = 0; i < written; i++) buffer[i].~C();
    }
    D::ResourceManager::Free(memory_block);
  }

  iterator inserter() { return iterator(buffer + written); }
  iterator begin() { return iterator(buffer); }
  iterator end() { return iterator(buffer + size); }

  bool insert(const C& x);  // Returns false if unable to insert.
  template <typename input_iterator>
  input_iterator insert(input_iterator first,
                        input_iterator last);  // Returns first uninserted.

  bool IsFull() { return written == size; }
  bool Fill(iterator it) {  // Returns true if something was written.
    iterator b = begin();
    written = it - b;
    return it != b;
  }

  void Ref();     // reffed_ptr<> interface.
  void Unref();   // reffed_ptr<> interface.
  int ref_count;  // reffed_ptr<> implementation.

  int size;     // In sizeof(C).
  int written;  // In sizeof(C).
  bool done;
  C* buffer;
  typename D::ResourceManager::MemoryBlock* memory_block;
  reffed_ptr<ChannelBlock> next;
};

// An operation which writes the contents of a lazy set to its output.
template <typename S, typename D>
class SetStream : public Stream<typename S::value_type, D> {
 public:
  typedef typename S::value_type C;
  typedef Stream<C, D> base;
  // TODO: Support these options.
  enum SetStreamOptions {
    SET_OP_UNORDERED,
    SET_OP_FORWORD_ORDER,
    SET_OP_REVERSE_ORDER  // Default.
  };
  SetStream(S* s, int options) : set_(s), options_(options) {}
  explicit SetStream(S* s) : set_(s), options_(SET_OP_REVERSE_ORDER) {}
  explicit SetStream(const typename S::MaterializedType& values)
      : set_(nullptr), options_(SET_OP_UNORDERED), values_(values) {
    cursor_ = values_.begin();
  }
  ~SetStream() {}
  void Initialize() {
    if (set_) {
      set_->Materialize(this->GetDataInterface(), &values_);
      cursor_ = values_.begin();
    }
    this->Register(WRITABLE, &SetStream<S, D>::Run, this->GetOutput());
  }
  void Reset() {
    cursor_ = values_.begin();
    base::Reset();
  }
  void Run(Channel<C, D>* channel) {
    while (cursor_ != values_.end()) {
      ChannelBlock<C, D>* b = this->GetOutput()->GetBlock();
      if (b->IsFull()) return;
      cursor_ = b->insert(cursor_, values_.end());
      this->GetOutput()->SomeWritten();
    }
    this->GetOutput()->Done();
  }

 private:
  S* set_;
  int options_;
  typename S::MaterializedType values_;
  typename S::MaterializedType::iterator cursor_;
};

// An operation which reads from another operation and stores the values into
// a container, optionally passing the values to its output.
// Container can be a map when C == pair<>.
template <typename E, typename C, typename D>
class StoreStream : public Stream<E, D> {
 public:
  StoreStream(Stream<E, D>* op, C* s) : container_(s), remove_on_done_(true) {
    input_ = op->Input(this);
    this->Register(READABLE, &StoreStream<E, C, D>::Run, input_);
  }
  // remove_on_done: if NoReaders() when the input is Done, Remove from Flow.
  // TODO: use cxx11 call constructor from another constructor.
  StoreStream(Stream<E, D>* op, C* s, bool remove_on_done)
      : container_(s), remove_on_done_(remove_on_done) {
    input_ = op->Input(this);
    this->Register(READABLE, &StoreStream<E, C, D>::Run, input_);
    this->Register(WRITABLE, &StoreStream<E, C, D>::Run, this->GetOutput());
  }
  void Start() {
    if (this->GetOutput()->NoReaders()) input_->GetChannel()->Read();
  }
  void Run(ChannelReader<E, D>* reader) {
    if (!input_->GetBlock()) {
      input_->GetChannel()->Read();
      return;
    }
    while (true) {
      if (input_->IsEmpty()) {
        if (input_->IsDone()) {
          DoneStoreResults();
          if (!this->GetOutput()->NoReaders()) {
            this->GetOutput()->Done();
          } else if (remove_on_done_) {
            this->GetFlow()->Remove(this);
          }
        }
        return;
      }
      auto begin = input_->begin(), end = input_->end();
      if (!this->GetOutput()->NoReaders()) {
        auto b = this->GetOutput()->GetBlock();
        if (!b || b->IsFull()) return;
        auto new_begin = b->insert(begin, end);
        container_->insert(begin, new_begin);
        input_->Consume(new_begin);
        if (b->Fill(new_begin)) this->GetOutput()->SomeWritten();
      } else {
        container_->insert(begin, end);
        input_->Next();
      }
    }
  }
  void Run(Channel<E, D>* channel) { Run(input_); }

 private:
  // TODO: find a better way of notifying.
  // Notify that new results are stored.
  virtual void DoneStoreResults() {}
  C* container_;
  ChannelReader<E, D>* input_;
  bool remove_on_done_;
};
template <typename E, typename C, typename D>
inline StoreStream<E, C, D>* StoreOp(Stream<E, D>* op, C* set) {
  return new StoreStream<E, C, D>(op, set);
}
template <typename E, typename C, typename D>
inline StoreStream<E, C, D>* StoreOp(Stream<E, D>* op, C* set, bool remove) {
  return new StoreStream<E, C, D>(op, set, remove);
}

// An operation to lookup a stream of keys in an index.
template <typename I>
class IndexStream
    : public Stream<typename I::OutputType, typename I::DataInterfaceType> {
 public:
  typedef IndexStream<I> T;
  typedef typename I::DataInterfaceType D;
  typedef typename I::KeyType K;
  typedef typename I::BufferType B;
  typedef typename I::OutputType OutputType;
  typedef Stream<OutputType, D> base;
  explicit IndexStream(const I& i, Stream<K, D>* op)
      : index_(i),
        lookup_handler_(this, &T::LookupsDone, &buffer_),
        block_(nullptr) {
    input_ = op->Input(this);
    this->Register(READABLE, &IndexStream<I>::Run, input_);
    this->Register(WRITABLE, &IndexStream<I>::Run, this->GetOutput());
  }
  ~IndexStream() { CHECK(!block_.get()); }
  void Run(ChannelReader<K, D>* reader) {
    if (block_.get())  // Outstanding lookup.
      return;
    if (!input_->GetBlock()) {
      input_->GetChannel()->Read();
      return;
    }
    while (true) {  // TODO: use this loop to pipeline this operation.
      if (input_->IsEmpty()) {
        if (input_->IsDone()) this->GetOutput()->Done();
        return;
      }
      auto begin = input_->begin(), end = input_->end();
      auto b = this->GetOutput()->GetBlock();
      if (!b || b->IsFull()) return;
      size_ = min(end - begin, b->end() - b->inserter());
      block_.reset(b, CREATE_A_REF);
      this->WaitFor(lookup_handler_.GetDoneCallback());
      buffer_ = B(b->memory_block, b->inserter() - b->begin(), size_);
      buffer_.init();
      for (int64 i = 0; i < size_; i++)
        I::SetKey(begin[i], &buffer_.begin()[i]);
      input_->Consume(begin + size_);
      this->GetDataInterface()->template Find(index_, buffer_,
                                              &lookup_handler_);
      return;
    }
  }
  void Run(Channel<OutputType, D>* channel) { Run(input_); }
  void LookupsDone(B* buffer) {
    // Remove CHECK when pipelined.
    CHECK(this->GetOutput()->GetBlock() == block_.get());
    this->GetDataInterface()->FindDone(index_, *buffer);
    this->GetOutput()->Fill(block_->inserter() + size_);
    block_.reset(nullptr);
    Run(input_);
  }

 protected:
  const I index_;
  ChannelReader<K, D>* input_;
  B buffer_;
  StatusHandler<T, B> lookup_handler_;
  int size_;
  reffed_ptr<ChannelBlock<OutputType, D>> block_;
};

// For debugging the intermediate results of flow machine. A typical usage is:
//  IDStream* op = Intersect2Op(op1, op2);
//  DebugOp(op1, new IDSet, "Input1 to intersect2: ");
//  DebugOp(op2, new IDSet, "Input2 to intersect2: ");
//  DebugOp(op, new IDSet, "Output of intersect2: ");
// As "op" is executed, its inputs and outputs will be logged for
// debugging.
// An example of controlling the debug via command line option is in
// graql/eval_plan.cc.

template <typename E, typename C, typename D>
class DebugStream : public StoreStream<E, C, D> {
 public:
  typedef StoreStream<E, C, D> BASE;
  typedef Stream<E, D> BaseOp;
  DebugStream(BaseOp* op, C* s, StringPiece id)
      : BASE(op, s, true), results_(s), id_(id.data()) {
    CHECK(s->empty());
  }

 private:
  void DoneStoreResults() {
    LOG(INFO) << id_ << ": " << ToDebugString(*results_);
  }
  std::unique_ptr<C> results_;
  string id_;
};

template <typename E, typename C, typename D>
inline Stream<E, D>* DebugOp(Stream<E, D>* op, C* s, StringPiece id) {
  return new DebugStream<E, C, D>(op, s, id);
}

}  // namespace dataflow
