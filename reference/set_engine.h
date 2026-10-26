#include <string.h>
#include <deque>
using ::std::deque;
#include <string>
using ::std::string;
#include <tuple>
#include <utility>
#include <vector>
using ::std::vector;

#include "base/callback.h"
#include "base/integral_types.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/mutex.h"
#include "base/thread_annotations.h"
#include "common/common_types.h"
#include "common/record.h"
#include "core/tupleindex.h"
#include "core/tuplestore.h"
#include "dataflow/dataflow.h"
#include "dataflow/dataflow-inl.h"
#include "dataflow/handler.h"
#include "dataflow/set_engine_data_interface.h"
#include "pgi/pgi_idset.h"
#include "pgi/pgi_resource_manager.h"
#include "util/btree/btree_container.h"
#include "util/refcount/reffed_ptr.h"
#include "util/task/status.h"

namespace blythe {
namespace dataflow {
typedef SetEngineDataInterface DI;

typedef ConditionMutex Mutex;
// Only works for the constructor.
#define MutexLock ConditionMutexLock

// TODO: handle the extension.
template <>
class Flow<DI> : public BaseFlow {
 public:
  explicit Flow(const TupleStore& store)
      : data_interface_(*CHECK_NOTNULL(store.pgi())), store_(store) {}
  DI* GetDataInterface() { return &data_interface_; }

 private:
  DI data_interface_;
  const TupleStore& store_;
};
typedef Flow<DI> SetEngineFlow;

typedef Stream<ID, DI> IDStream;
typedef Stream<UUID, DI> UUIDStream;
typedef Stream<Record, DI> TupleStream;
typedef Channel<ID, DI> IDChannel;
typedef ChannelReader<ID, DI> IDReader;
typedef Stream<string, DI> StringStream;
typedef IndexStream<IDToSetIndex> IDToSetIndexStream;
typedef IndexStream<UUIDToSetIndex> UUIDToSetIndexStream;

template <>
class SetStream<Set<ID>, SetEngineDataInterface>
    : public Stream<typename Set<ID>::value_type, SetEngineDataInterface> {
 public:
  typedef SetStream<Set<ID>, SetEngineDataInterface> T;
  typedef Set<ID> S;  // Specialied template parameter.
  typedef SetEngineDataInterface D;
  typedef typename S::value_type C;
  typedef Stream<C, D> base;

  SetStream(S* s, C start, C end)
      : set_(s),
        start_(start),
        end_(end),
        read_callback_(this, &T::ReadCallback),
        reader_(nullptr) {}
  explicit SetStream(S* s)
      : set_(s),
        start_(0),
        end_(kuint64max),
        read_callback_(this, &T::ReadCallback),
        reader_(nullptr) {}
  explicit SetStream(const typename S::MaterializedType& values)
      : set_(nullptr),
        values_(values),
        read_callback_(this, &T::ReadCallback),
        reader_(nullptr) {
    cursor_ = values_.begin();
  }
  ~SetStream() {
    for (auto& b : buffers_) b.free();
  }

  void Initialize() {
    if (set_ && set_->IsImmediate()) {
      // TODO: copy directly into the target buffer rather than staging
      // into values_.
      set_->Materialize(this->GetDataInterface(), &values_);
      cursor_ = values_.begin();
    }
    this->Register(WRITABLE, &T::Run, this->GetOutput());
  }

  void Start() {
    MutexLock l(&mutex_);
    StartInternal();
  }

  void StartInternal() EXCLUSIVE_LOCKS_REQUIRED(mutex_) {
    if (set_ && !set_->IsImmediate()) {
      read_handler_.Set(this, &T::ReadHandler);
      close_handler_.Set(this, &T::ReadHandler);
      WaitFor(&close_handler_);
      read_offset_ = 0;
      buffers_available_ = kBuffersPerRead;
      reader_ = set_->Read(start_, end_, &read_callback_);
    }
  }

  void Reset() {
    {
      MutexLock l(&mutex_);
      if (!set_ || set_->IsImmediate()) {
        cursor_ = values_.begin();
      } else {
        CHECK(!reader_);  // Can't reset an outstanding async operation.
        for (auto& b : buffers_) b.free();
        buffers_.clear();
        StartInternal();
      }
    }
    base::Reset();
  }

  void Run(Channel<C, D>* channel) {
    if (!set_ || set_->IsImmediate()) {
      while (cursor_ != values_.end()) {
        ChannelBlock<C, D>* b = channel->GetBlock();
        if (!b || b->IsFull()) return;
        cursor_ = b->insert(cursor_, values_.end());
        channel->SomeWritten();
      }
      channel->Done();
    } else {
      bool first = true;
      bool was_paused = false;
      while (true) {
        MutexLock l(&mutex_);
        if (first) {
          was_paused = buffers_available_ <= 0;
          first = false;
        }
        if (buffers_.empty()) {
          if (!reader_) channel->Done();
          return;
        }
        auto b = channel->GetBlock();
        if (!b || b->IsFull()) return;
        auto begin = buffers_.front().begin() + read_offset_;
        int64 available = buffers_.front().end() - begin;
        int64 space = b->end() - b->inserter();
        int64 todo = available < space ? available : space;
        memcpy(b->inserter(), begin, todo * sizeof(ID));
        read_offset_ += todo;
        if (buffers_.front().begin() + read_offset_ >= buffers_.front().end()) {
          // TODO: reuse these buffers.
          buffers_.front().free();
          buffers_.pop_front();
          read_offset_ = 0;
          buffers_available_++;
          if (reader_ && was_paused) {
            // Transitioning from PAUSED to not-PAUSED.
            was_paused = false;
            reader_->Run();
          }
        }
      }
    }
  }

  void ReadHandler() { Run(this->GetOutput()); }

  // TODO: consider some way to share this code with JoinStream.
  AsyncStreamResult ReadCallback(util::Status status, PgiBufferPtr<C> b) {
    AsyncStreamResult r = ASYNC_CONTINUE;
    {
      MutexLock l(&mutex_);
      buffers_.push_back(b);
      buffers_available_--;
      if (buffers_available_ <= 0) r = ASYNC_PAUSE;
      if (status.error_code() == util::Status::CANCELLED_CODE) {
        reader_ = nullptr;
        GetFlow()->PostFromOtherThread(&close_handler_);
        r = ASYNC_DONE;
      }
    }
    if ((!status.ok()) && status.error_code() != util::Status::CANCELLED_CODE) {
      GetFlow()->Abort(status);
      GetFlow()->PostFromOtherThread(&close_handler_);
      r = ASYNC_DONE;
    }
    if (r != ASYNC_DONE) GetFlow()->PostFromOtherThread(&read_handler_);
    return r;
  }

 private:
  S* set_;
  C start_;
  C end_;
  typename S::MaterializedType values_;
  typename S::MaterializedType::iterator cursor_;
  _MemberResultCallback_0_2<false, AsyncStreamResult, T, util::Status,
                            PgiBufferPtr<C>> read_callback_;
  SimpleHandler<T> read_handler_;
  SimpleHandler<T> close_handler_;
  int read_offset_;
  // TODO: this mutex_ could be avoided if we controlled the Executor
  // in SetMapReader so that it was an Executor on this thread.
  Mutex mutex_;
  Closure* reader_ GUARDED_BY(mutex_);  // Used to restart the read.
  int buffers_available_ GUARDED_BY(mutex_);
  deque<PgiBufferPtr<ID>> buffers_ GUARDED_BY(mutex_);
};

// Stream the values from a set of IDs.
typedef SetStream<Set<ID>, DI> IDSetStream;
inline IDSetStream* IDSetOp(Set<ID>* s) { return new IDSetStream(s); }
inline IDSetStream* IDSetOp(const IDSet& ids) { return new IDSetStream(ids); }
inline IDSetStream* IDSetOp(ID id) {
  IDSet ids;
  ids.insert(id);
  return IDSetOp(ids);
}

// Stream the values from a set of strings.
typedef SetStream<Set<string>, DI> StringSetStream;
inline StringSetStream* StringSetOp(Set<string>* s) {
  return new StringSetStream(s);
}

// General operator to transform a stream of values of type I to those of type
// O.
template <typename I, typename O, typename D>
class TransformStream : public Stream<O, D> {
 public:
  typedef typename ChannelBlock<I, D>::iterator in_iterator;
  typedef typename ChannelBlock<O, D>::iterator out_iterator;
  explicit TransformStream(Stream<I, D>* op) {
    input_ = op->Input(this);
    this->Register(READABLE, &TransformStream::Run, input_);
    this->Register(WRITABLE, &TransformStream::Run, this->GetOutput());
  }
  virtual void Transform(in_iterator* in, in_iterator in_end, out_iterator* out,
                         out_iterator out_end) = 0;
  void Run(ChannelReader<I, D>* reader) {
    if (!input_->GetBlock()) {
      input_->GetChannel()->Read();
      return;
    }
    while (true) {
      if (input_->IsEmpty()) {
        if (input_->IsDone()) this->GetOutput()->Done();
        return;
      }
      auto b = this->GetOutput()->GetBlock();
      if (!b || b->IsFull()) return;
      auto in = input_->begin();
      auto out = b->inserter();
      Transform(&in, input_->end(), &out, b->end());
      input_->Consume(in);
      this->GetOutput()->Fill(out);
    }
  }
  void Run(Channel<O, D>* channel) { Run(input_); }

 protected:
  ChannelReader<I, D>* input_;
};

// A map operation run a function f which takes one element and
// outputs exactly one corresponding element
template <typename I, typename O, typename D>
class MapStream : public TransformStream<I, O, D> {
 public:
  typedef typename TransformStream<I, O, D>::in_iterator in_iterator;
  typedef typename TransformStream<I, O, D>::out_iterator out_iterator;
  explicit MapStream(Stream<I, D>* op) : TransformStream<I, O, D>(op) {}
  void Transform(in_iterator* pin, in_iterator in_end, out_iterator* pout,
                 out_iterator out_end) {
    auto in = *pin;
    auto out = *pout;
    for (; in != in_end && out != out_end; ++in, ++out) *out = Run(*in);
    *pin = in;
    *pout = out;
  }

  virtual O Run(const I& element) ABSTRACT;
};

// Given a stream of tuples (or pairs), returns the i-th element.
template <typename I, int i, typename D>
class ProjectStream
    : public MapStream<I, typename std::tuple_element<i, I>::type, D> {
 public:
  typedef typename std::tuple_element<i, I>::type O;
  explicit ProjectStream(Stream<I, D>* op) : MapStream<I, O, D>(op) {}
  inline O Run(const I& elem) { return std::get<i>(elem); }
};

// Filter things out by specified Pred on the fly.
// TODO: we should be able to replace the other on the fly filters
// with this template.
template <typename I, typename Pred, typename D>
class FunctionFilterStream : public TransformStream<I, I, D> {
 public:
  typedef I O;
  typedef typename TransformStream<I, O, D>::in_iterator in_iterator;
  typedef typename TransformStream<I, O, D>::out_iterator out_iterator;
  FunctionFilterStream(Stream<I, D>* op, Pred* pred)
      : TransformStream<I, O, D>(op), pred_(CHECK_NOTNULL(pred)) {}

  void Transform(in_iterator* pin, in_iterator in_end, out_iterator* pout,
                 out_iterator out_end) {
    auto in = *pin;
    auto out = *pout;
    for (; in != in_end && out != out_end; ++in) {
      if (!(*pred_)(*in)) *out++ = *in;
    }
    *pin = in;
    *pout = out;
  }

 private:
  Pred* pred_;
};

template <typename I, typename Pred, typename D>
inline FunctionFilterStream<I, Pred, D>* FunctionFilterOp(Stream<I, D>* op,
                                                          Pred* pred) {
  return new FunctionFilterStream<I, Pred, D>(op, pred);
}

template <typename I, int i, typename D>
inline ProjectStream<I, i, D>* ProjectOp(Stream<I, D>* op) {
  return new ProjectStream<I, i, D>(op);
}
template <typename I, typename D>
inline ProjectStream<I, 0, D>* FirstOp(Stream<I, D>* op) {
  return new ProjectStream<I, 0, D>(op);
}
template <typename I, typename D>
inline ProjectStream<I, 1, D>* SecondOp(Stream<I, D>* op) {
  return new ProjectStream<I, 1, D>(op);
}
template <typename I, typename D>
inline ProjectStream<I, 2, D>* ThirdOp(Stream<I, D>* op) {
  return new ProjectStream<I, 2, D>(op);
}

// Given a stream of tuples (or pairs), returns the pair of i,j-th elements.
template <typename I, int i, int j, typename D>
class ProjectPairStream
    : public TransformStream<I, pair<typename std::tuple_element<i, I>::type,
                                     typename std::tuple_element<j, I>::type>,
                             D> {
 public:
  typedef typename std::tuple_element<i, I>::type E1;
  typedef typename std::tuple_element<j, I>::type E2;
  typedef pair<E1, E2> O;
  typedef typename TransformStream<I, O, D>::in_iterator in_iterator;
  typedef typename TransformStream<I, O, D>::out_iterator out_iterator;
  explicit ProjectPairStream(Stream<I, D>* op) : TransformStream<I, O, D>(op) {}
  void Transform(in_iterator* pin, in_iterator in_end, out_iterator* pout,
                 out_iterator out_end) {
    auto in = *pin;
    auto out = *pout;
    for (; in != in_end && out != out_end; ++in, ++out)
      *out = make_pair(std::get<i>(*in), std::get<j>(*in));
    *pin = in;
    *pout = out;
  }
};
template <typename I, typename D>
inline ProjectPairStream<I, 0, 2, D>* ProjectFirstThirdOp(Stream<I, D>* op) {
  return new ProjectPairStream<I, 0, 2, D>(op);
}

template <typename O, typename D>
class JoinStream : public TransformStream<Set<O>, O, D> {
 public:
  typedef Set<O> I;
  typedef typename I::MaterializedType MaterializedType;
  typedef typename MaterializedType::iterator set_iterator;
  typedef typename TransformStream<I, O, D>::in_iterator in_iterator;
  typedef typename TransformStream<I, O, D>::out_iterator out_iterator;
  explicit JoinStream(Stream<I, D>* s) : TransformStream<I, O, D>(s) {}
  void Transform(in_iterator* pin, in_iterator in_end, out_iterator* pout,
                 out_iterator out_end) {
    auto in = *pin;
    auto out = *pout;
    for (; in != in_end && out != out_end; ++in) {
      if (cur_ == set_iterator()) {
        in->Materialize(this->GetDataInterface(), &values_);
        cur_ = values_.begin();
      }
      for (; cur_ != values_.end() && out != out_end; ++cur_, ++out)
        *out = *cur_;
      if (cur_ != values_.end()) break;
      cur_ = set_iterator();
    }
    *pin = in;
    *pout = out;
  }

 private:
  MaterializedType values_;
  set_iterator cur_;
};

template <>
class JoinStream<ID, SetEngineDataInterface>
    : public Stream<ID, SetEngineDataInterface> {
 public:
  typedef ID O;
  typedef SetEngineDataInterface D;
  typedef Set<O> I;
  typedef JoinStream<O, D> T;
  typedef I::MaterializedType MaterializedType;
  typedef typename MaterializedType::iterator set_iterator;
  typedef typename TransformStream<I, O, D>::in_iterator in_iterator;
  typedef typename TransformStream<I, O, D>::out_iterator out_iterator;

  explicit JoinStream(Stream<I, D>* op)
      : Stream<O, D>(),
        read_callback_(this, &T::ReadCallback),
        read_handler_(this, &T::RunHandler),
        close_handler_(this, &T::RunHandler) {
    input_ = op->Input(this);
    this->Register(READABLE, &T::Run, input_);
    this->Register(WRITABLE, &T::Run, this->GetOutput());
  }

  void Run(ChannelReader<I, D>* reader) {
    if (!input_->GetBlock()) {
      input_->GetChannel()->Read();
      return;
    }
    while (true) {
    LCheckBuffers:
      if (input_->IsEmpty()) {
        if (input_->IsDone()) this->GetOutput()->Done();
        return;
      }
      auto b = this->GetOutput()->GetBlock();
      if (!b || b->IsFull()) return;
      auto in = input_->begin();
      auto in_end = input_->end();
      auto out = b->inserter();
      auto out_end = b->end();
      while (in != in_end && out != out_end) {  // While input and output data.
        if (in->IsImmediate()) {
          if (cur_ == set_iterator()) {  // No current set.
            // TODO: this could and should be avoidable.
            values_.clear();
            in->Materialize(this->GetDataInterface(), &values_);
            cur_ = values_.begin();
          }
          for (; cur_ != values_.end() && out != out_end; ++cur_, ++out)
            *out = *cur_;
          this->GetOutput()->Fill(out);
          if (cur_ == values_.end()) {
            cur_ = set_iterator();
            in++;  // Done with this bit of input.
            input_->Consume(in);
          }
          goto LCheckBuffers;
        }
        // Non-immediate set.
        bool was_paused = false;
        while (true) {
          Closure* run_reader = nullptr;
          {
            MutexLock l(&mutex_);
            if (!reading_) {
              StartReading(in);
              return;  // Wait for data from PGI callback.
            }
            was_paused = buffers_available_ <= 0;
            if (buffers_.empty()) {
              if (!reader_) {
                in++;
                input_->Consume(in);
                reading_ = false;
                goto LCheckBuffers;
              }
              return;  // Wait for more data from PGI callback.
            }
            auto begin = buffers_.front().begin() + read_offset_;
            int64 available = buffers_.front().end() - begin;
            int64 space = out_end - out;
            if (space <= 0) goto LCheckBuffers;
            int64 todo = available < space ? available : space;
            memcpy(out, begin, todo * sizeof(ID));
            read_offset_ += todo;
            out += todo;
            this->GetOutput()->Fill(out);
            if (buffers_.front().begin() + read_offset_ >=
                buffers_.front().end()) {
              // TODO: reuse these buffers.
              buffers_.front().free();
              buffers_.pop_front();
              read_offset_ = 0;
              buffers_available_++;
              if (reader_ && was_paused) {
                // Transitioning from PAUSED to not-PAUSED.
                was_paused = false;
                run_reader = reader_;
              }
            }
          }
          if (run_reader) {
            run_reader->Run();
            Run(reader);
            return;
          }
        }
      }
    }
  }

  void Run(Channel<O, D>* channel) { Run(input_); }
  void RunHandler() { Run(input_); }

  // TODO: consider some way to share this code with SetStream.
  AsyncStreamResult ReadCallback(util::Status status, PgiBufferPtr<O> b) {
    AsyncStreamResult r = ASYNC_CONTINUE;
    {
      MutexLock l(&mutex_);
      buffers_.push_back(b);
      buffers_available_--;
      if (buffers_available_ <= 0) r = ASYNC_PAUSE;
      if (status.error_code() == util::Status::CANCELLED_CODE) {
        reader_ = nullptr;
        // This call must be within this lock so that we stop waiting for
        // close_handler_ before Transform() above can switch to the next set
        // and call WaitFor(&close_handler_) in StartReading() below.
        GetFlow()->PostFromOtherThread(&close_handler_);
        r = ASYNC_DONE;
      }
    }
    if ((!status.ok()) && status.error_code() != util::Status::CANCELLED_CODE) {
      GetFlow()->Abort(status);
      GetFlow()->PostFromOtherThread(&close_handler_);
      r = ASYNC_DONE;
    }
    if (r != ASYNC_DONE) GetFlow()->PostFromOtherThread(&read_handler_);
    return r;
  }

 private:
  void StartReading(Set<O>* s) EXCLUSIVE_LOCKS_REQUIRED(mutex_) {
    WaitFor(&close_handler_);
    read_offset_ = 0;
    buffers_available_ = kBuffersPerRead;
    reading_ = true;
    reader_ = s->Read(kuint64max, 0, &read_callback_);
  }

  ChannelReader<I, D>* input_;
  MaterializedType values_;
  set_iterator cur_;
  _MemberResultCallback_0_2<false, AsyncStreamResult, T, util::Status,
                            PgiBufferPtr<O>> read_callback_;
  SimpleHandler<T> read_handler_;
  SimpleHandler<T> close_handler_;
  int read_offset_;
  bool reading_ = false;
  // TODO(: this mutex_ could be avoided if we controlled the Executor
  // in SetMapReader so that it was an Executor on this thread.
  Mutex mutex_;
  Closure* reader_ GUARDED_BY(mutex_) = nullptr;  // Used to restart the read.
  int buffers_available_ GUARDED_BY(mutex_);
  deque<PgiBufferPtr<ID>> buffers_ GUARDED_BY(mutex_);
};
template <typename O, typename D>
inline JoinStream<O, D>* JoinOp(Stream<Set<O>, D>* op) {
  return new JoinStream<O, D>(op);
}

class TuplesStream : public IndexStream<TupleIndex> {
 public:
  explicit TuplesStream(IDStream* op) : IndexStream(TupleIndex(), op) {}
};
// Given a stream of IDs, produces a stream of Tuples.
inline TupleStream* TuplesOp(IDStream* op) { return new TuplesStream(op); }

typedef Stream<pair<ID, Set<ID>>, DI> IDToSetStream;
// Given a stream of IDs, produces a stream of Set<ID> of the links with the
// corresponding id at the given linkage.
class LinkageIndexStream : public IDToSetIndexStream {
 public:
  LinkageIndexStream(IDStream* op, Linkage linkage)
      : IDToSetIndexStream(IDToSetIndex(LinkageToIndex(linkage)), op) {}
};
inline IDToSetStream* LinkageIndexOp(IDStream* op, Linkage linkage) {
  return new LinkageIndexStream(op, linkage);
}
// Produces the set of links with a given id at a given linkage.
// TODO: optimize
inline Stream<ID, DI>* LinkageIndexOp(Linkage linkage, ID id) {
  return JoinOp(SecondOp(LinkageIndexOp(IDSetOp(id), linkage)));
}

// Given a stream of IDs, produces a stream of Tuples of the links with the
// corresponding id at the given linkage.
// TODO(: optimize.
inline TupleStream* LinksOp(IDStream* op, Linkage linkage) {
  return TuplesOp(JoinOp(SecondOp(LinkageIndexOp(op, linkage))));
}

class SPOTupleKeyStream : public TransformStream<Record, UUID, DI> {
 public:
  typedef TransformStream<Record, UUID, DI>::in_iterator in_iterator;
  typedef TransformStream<Record, UUID, DI>::out_iterator out_iterator;
  explicit SPOTupleKeyStream(TupleStream* op)
      : TransformStream<Record, UUID, DI>(op) {}
  void Transform(in_iterator* pin, in_iterator in_end, out_iterator* pout,
                 out_iterator out_end) {
    auto in = *pin;
    auto out = *pout;
    for (; in != in_end && out != out_end; ++in, ++out) *out = SPOKey(*in);
    *pin = in;
    *pout = out;
  }
};
inline Stream<UUID, DI>* SPOKeyOp(TupleStream* op) {
  return new SPOTupleKeyStream(op);
}

class SPOTripleKeyStream : public TransformStream<IDTriple, UUID, DI> {
 public:
  typedef TransformStream<IDTriple, UUID, DI>::in_iterator in_iterator;
  typedef TransformStream<IDTriple, UUID, DI>::out_iterator out_iterator;
  explicit SPOTripleKeyStream(Stream<IDTriple, DI>* op) : TransformStream(op) {}
  void Transform(in_iterator* pin, in_iterator in_end, out_iterator* pout,
                 out_iterator out_end) {
    auto in = *pin;
    auto out = *pout;
    for (; in != in_end && out != out_end; ++in, ++out)
      *out = SPOKey(std::get<0>(*in), std::get<1>(*in), std::get<2>(*in));
    *pin = in;
    *pout = out;
  }
};
inline Stream<UUID, DI>* SPOKeyOp(Stream<IDTriple, DI>* op) {
  return new SPOTripleKeyStream(op);
}

class FilterZeroStream : public TransformStream<ID, ID, DI> {
 public:
  typedef TransformStream<ID, ID, DI>::in_iterator in_iterator;
  typedef TransformStream<ID, ID, DI>::out_iterator out_iterator;
  explicit FilterZeroStream(Stream<ID, DI>* op) : TransformStream(op) {}
  void Transform(in_iterator* pin, in_iterator in_end, out_iterator* pout,
                 out_iterator out_end) {
    auto in = *pin;
    auto out = *pout;
    for (; in != in_end && out != out_end; ++in)
      if (*in) *out++ = *in;
    *pin = in;
    *pout = out;
  }
};
inline Stream<ID, DI>* FilterZeroOp(Stream<ID, DI>* op) {
  return new FilterZeroStream(op);
}

// Given s stream of Record or IDTriple, return a stream of Record for the
// corresponding SPO indexes.
class SPOSetIndexStream : public UUIDToSetIndexStream {
 public:
  explicit SPOSetIndexStream(Stream<UUID, DI>* op)
      : IndexStream(UUIDToSetIndex(kSPOIndex), op) {}
};
inline TupleStream* SPOOp(TupleStream* op) {
  return TuplesOp(JoinOp(SecondOp(new SPOSetIndexStream(SPOKeyOp(op)))));
}
inline TupleStream* SPOOp(Stream<IDTriple, DI>* op) {
  return TuplesOp(JoinOp(SecondOp(new SPOSetIndexStream(SPOKeyOp(op)))));
}

// Extract linkages from a stream of link ID(s).  This should be more efficient
// than just reading the full tuple.
// TODO: complete
class LinkLinkageStream : public Stream<IDPair, DI> {};

// Produces a set of mappings from ID to IDPair.
// TODO: complete
class LinkLinkagePairStream : public Stream<pair<ID, IDPair>, DI> {};

// Produces a set of mappings from ID to IDPair.
// TODO: complete
class LinkLinkageTripleStream : public Stream<pair<ID, IDTriple>, DI> {
 public:
  LinkLinkageTripleStream(IDStream* op, Linkage l1, Linkage l2, Linkage l3);
};

// Given a stream of Tuples, produces a stream of ID at the given linkage.
class LinkageStream : public TransformStream<Record, ID, DI> {
 public:
  LinkageStream(TupleStream* op, Linkage l) : TransformStream(op), l_(l) {}
  typedef TransformStream<Record, ID, DI>::in_iterator in_iterator;
  typedef TransformStream<Record, ID, DI>::out_iterator out_iterator;
  explicit LinkageStream(TupleStream* op)
      : TransformStream<Record, ID, DI>(op) {}
  void Transform(in_iterator* pin, in_iterator in_end, out_iterator* pout,
                 out_iterator out_end) {
    auto in = *pin;
    auto out = *pout;
    for (; in != in_end && out != out_end; ++in, ++out) *out = in->linkage(l_);
    *pin = in;
    *pout = out;
  }

 private:
  Linkage l_;
};
inline LinkageStream* LinkageOp(TupleStream* op, Linkage l1) {
  return new LinkageStream(op, l1);
}
inline LinkageStream* SubjectOp(TupleStream* op) {
  return new LinkageStream(op, kSubject);
}
inline LinkageStream* PredicateOp(TupleStream* op) {
  return new LinkageStream(op, kPredicate);
}
inline LinkageStream* ObjectOp(TupleStream* op) {
  return new LinkageStream(op, kObject);
}
// Adapter for IDs.
// TODO: replace with direct loading of just the requested linkages.
inline LinkageStream* LinkageOp(Stream<ID, DI>* op, Linkage l1) {
  return LinkageOp(TuplesOp(op), l1);
}

// Extract a pair of linkages from a stream of Tuples.
class LinkagePairStream : public TransformStream<Record, IDPair, DI> {
 public:
  LinkagePairStream(TupleStream* op, Linkage l1, Linkage l2)
      : TransformStream(op), l1_(l1), l2_(l2) {}
  typedef TransformStream<Record, IDPair, DI>::in_iterator in_iterator;
  typedef TransformStream<Record, IDPair, DI>::out_iterator out_iterator;
  explicit LinkagePairStream(TupleStream* op)
      : TransformStream<Record, IDPair, DI>(op) {}
  void Transform(in_iterator* pin, in_iterator in_end, out_iterator* pout,
                 out_iterator out_end) {
    auto in = *pin;
    auto out = *pout;
    for (; in != in_end && out != out_end; ++in, ++out) {
      out->first = in->linkage(l1_);
      out->second = in->linkage(l2_);
    }
    *pin = in;
    *pout = out;
  }

 private:
  Linkage l1_;
  Linkage l2_;
};
inline LinkagePairStream* LinkagePairOp(TupleStream* op, Linkage l1,
                                        Linkage l2) {
  return new LinkagePairStream(op, l1, l2);
}
// Adapter for IDs.
// TODO: replace with direct loading of just the requested linkages.
inline LinkagePairStream* LinkPairOp(Stream<ID, DI>* op, Linkage l1,
                                     Linkage l2) {
  return LinkagePairOp(TuplesOp(op), l1, l2);
}

// Extract a triple of linkages from a stream of link ID(s).
class LinkageTripleStream : public TransformStream<Record, IDTriple, DI> {
 public:
  LinkageTripleStream(TupleStream* op, Linkage l1, Linkage l2, Linkage l3)
      : TransformStream(op), l1_(l1), l2_(l2), l3_(l3) {}
  typedef TransformStream<Record, IDTriple, DI>::in_iterator in_iterator;
  typedef TransformStream<Record, IDTriple, DI>::out_iterator out_iterator;
  explicit LinkageTripleStream(TupleStream* op)
      : TransformStream<Record, IDTriple, DI>(op) {}
  void Transform(in_iterator* pin, in_iterator in_end, out_iterator* pout,
                 out_iterator out_end) {
    auto in = *pin;
    auto out = *pout;
    for (; in != in_end && out != out_end; ++in, ++out)
      *out =
          std::make_tuple(in->linkage(l1_), in->linkage(l2_), in->linkage(l3_));
    *pin = in;
    *pout = out;
  }

 private:
  Linkage l1_;
  Linkage l2_;
  Linkage l3_;
};
inline LinkageTripleStream* LinkageTripleOp(TupleStream* op, Linkage l1,
                                            Linkage l2, Linkage l3) {
  return new LinkageTripleStream(op, l1, l2, l3);
}
// Adapter for IDs.
// TODO: replace with direct loading of just the requested linkages.
inline LinkageTripleStream* LinkTripleOp(Stream<ID, DI>* op, Linkage l1,
                                         Linkage l2, Linkage l3) {
  return LinkageTripleOp(TuplesOp(op), l1, l2, l3);
}
// Convenience function for SPO.
inline LinkageTripleStream* LinkageSPOOp(TupleStream* op) {
  return LinkageTripleOp(op, kSubject, kPredicate, kObject);
}
// Adapter for IDs.
// TODO: replace with direct loading of just the requested linkages.
inline LinkageTripleStream* LinkSPOOp(Stream<ID, DI>* op) {
  return LinkageTripleOp(TuplesOp(op), kSubject, kPredicate, kObject);
}

// Consumes a stream of pair<ID,Set<ID> >(s) produces pair<ID,int64>(s).
// TODO: generalize/parallelize?
template <typename D>
class EstimatedSetSizesStream : public Stream<pair<ID, int64>, D> {
 public:
  typedef pair<ID, Set<ID>> InputType;
  typedef pair<ID, int64> OutputType;
  EstimatedSetSizesStream(Stream<InputType, D>* op) {
    input_ = op->Input(this);
    this->Register(READABLE, &EstimatedSetSizesStream::Run, input_);
    this->Register(WRITABLE, &EstimatedSetSizesStream::Run, this->GetOutput());
  }
  void Run(ChannelReader<InputType, D>* reader) {
    if (!input_->GetBlock()) {
      input_->GetChannel()->Read();
      return;
    }
    while (true) {
      if (input_->IsEmpty()) {
        if (input_->IsDone()) this->GetOutput()->Done();
        return;
      }
      auto begin = input_->begin(), end = input_->end();
      auto b = this->GetOutput()->GetBlock();
      if (!b || b->IsFull()) return;
      auto i = b->inserter(), e = b->end();
      // TODO: parallelize this.
      for (; begin != end && i != e; ++begin, ++i) {
        i->first = begin->first;
        i->second = begin->second.size();
      }
      input_->Consume(begin);
      b->Fill(i);
    }
  }
  void Run(Channel<OutputType, D>* channel) { Run(input_); }

 private:
  ChannelReader<InputType, D>* input_;
};
template <typename D>
inline EstimatedSetSizesStream<D>* EstimatedSetSizesOp(
    Stream<pair<ID, Set<ID>>, D>* op) {
  return new EstimatedSetSizesStream<D>(op);
}

// Consumes an Op and returns the exact size.
class ExactSizeStream : public BaseStream {
 public:
  ExactSizeStream(BaseStream* op, int64* presult);
};

// Consumes a stream of pair<ID,Set>(s) produces pair<ID,int64>(s).
template <typename D>
class ExactIndexSizesStream : public Stream<pair<ID, int64>, D> {};

// Removes dead ids from a stream of ID(s).
class RemoveDeadStream : public IDStream {};

// Given a stream of strings, lookup the value ids.
class ValueIDStream : public IDStream {
 public:
  explicit ValueIDStream();
};

// Given a stream of ID(s) produces a stream of strings.
class ValueStream : public Stream<const string, DI> {
 public:
  explicit ValueStream();
};

// Compute the union of a set of sets.
class UnionStream : public IDStream {
 public:
  explicit UnionStream(const vector<IDStream*> inputs);
};

// Same as above but with the hint that it is a disjoint union.
class DisjointUnionStream : public IDStream {
  explicit DisjointUnionStream(const vector<IDStream*> inputs);
};

// Compute the intersection of a set of sets.
class IntersectionStream : public IDStream {
 public:
  explicit IntersectionStream(const vector<IDStream*> inputs);
  explicit IntersectionStream(const vector<IDToSetIndexStream*> inputs);
};
// Compute the union and intersection of a pair of sets of sets.
class UnionIntersectionStream : public IDStream {
 public:
  UnionIntersectionStream(const vector<IDStream*> unions,
                          const vector<IDStream*> intersections);
};
// Compute the difference between a set and another set.
class DifferenceStream : public IDStream {
 public:
  DifferenceStream(IDStream* s, IDStream* diff);
};

// Using a limited size buffer, filter out some duplicate IDs from a stream
// of IDs.  This is used for non-disjoint unions.
class FilterSomeDupIDsStream : IDStream {};

// Filter from a stream of Tuples, those which do not have the given linkages.
class FilterLinkageTupleStream : public TupleStream {
 public:
  FilterLinkageTupleStream(TupleStream* op, Linkage l, ID id);
  FilterLinkageTupleStream(TupleStream* op, Linkage l, Set<ID> ids);
  FilterLinkageTupleStream(TupleStream* op, Linkage l1, ID id1, Linkage l2,
                           ID id2);
  FilterLinkageTupleStream(TupleStream* op, Linkage l1, Set<ID> ids1,
                           Linkage l2, Set<ID> ids2);
};

// Materialize the stream as a local set so that it will efficiently support
// Probe.
// TODO: add support for out-of-core Probe.
template <typename C, typename D>
class MaterializeStream : public Stream<C, D> {
 public:
  explicit MaterializeStream(Stream<C, D>* op) {
    input_ = op->Input(this);
    this->Register(READABLE, &MaterializeStream<C, D>::In, input_);
    this->Register(WRITABLE, &MaterializeStream<C, D>::Out, this->GetOutput());
  }
  typename Set<C>::MaterializedType* GetSet() { return &set_; }
  void In(ChannelReader<C, D>* reader) {
    while (true) {
      if (input_->IsEmpty()) {
        if (input_->IsDone()) {
          cursor_ = set_.begin();
          Out(this->GetOutput());
        }
        return;
      }
      set_.insert(input_->begin(), input_->end());
      input_->Next();
    }
  }
  void Out(Channel<C, D>* channel) {
    if (cursor_ == typename Set<C>::MaterializedType::iterator()) {
      input_->GetChannel()->Read();
      return;
    }
    while (cursor_ != set_.end()) {
      ChannelBlock<C, D>* b = this->GetOutput()->GetBlock();
      if (b->IsFull()) return;
      cursor_ = b->insert(cursor_, set_.end());
      this->GetOutput()->SomeWritten();
    }
    this->GetOutput()->Done();
  }

 private:
  ChannelReader<C, D>* input_;
  typename Set<C>::MaterializedType set_;
  typename Set<C>::MaterializedType::iterator cursor_;
};
template <typename C, typename D>
inline MaterializeStream<C, D>* MaterializeOp(Stream<C, D>* op) {
  return new MaterializeStream<C, D>(op);
}

// Filter from a stream of tuples, those without matching elements.
// TODO: convert this into two forms, one which does Probe and one
// which loads a materialized set.
template <int i, typename C, typename D>
class FilterStream : public TransformStream<C, C, D> {
 public:
  typedef typename std::tuple_element<i, C>::type E;
  typedef typename TransformStream<C, C, D>::in_iterator iterator;
  FilterStream(Stream<C, D>* op, E e) : TransformStream<C, C, D>(op) {
    set_.insert(e);
  }
  FilterStream(Stream<C, D>* op, Set<E> s) : TransformStream<C, C, D>(op) {
    set_ = s;
  }
  FilterStream(Stream<C, D>* op, MaterializeStream<E, D>* filter)
      : TransformStream<C, C, D>(op) {
    filter_ = filter;
    filter_input_ = filter->Input(this);
    this->Register(READABLE, &FilterStream::FilterRun, filter_input_);
    // Override superclass handlers.
    this->Register(WRITABLE, &FilterStream::FilterRun, this->GetOutput());
    this->Register(READABLE, &FilterStream::FilterNotReady, this->input_);
  }
  void FilterNotReady(ChannelReader<C, D>* channel) {}
  void FilterRun(ChannelReader<E, D>* channel) {
    set_ = *filter_->GetSet();  // TODO: make more efficient.
    filter_input_ = NULL;       // Indicate that we have the filter set now.
    this->Run(this->input_);
  }
  void FilterRun(Channel<C, D>* channel) {
    if (filter_input_) {  // Still need to read the filter.
      this->input_->GetChannel()->Read();
    } else {  // Done reading the filter, register & pass to TransformStream.
      this->Register(READABLE, &TransformStream<C, C, D>::Run, this->input_);
      this->Run(this->input_);
    }
  }
  void Transform(iterator* pin, iterator in_end, iterator* pout,
                 iterator out_end) {
    auto in = *pin;
    auto out = *pout;
    // TODO: this could be a Probe of set_.
    for (; in != in_end && out != out_end; ++in)
      if (set_.count(std::get<i>(*in)) != 0) *out++ = *in;
    *pin = in;
    *pout = out;
  }

 private:
  MaterializeStream<E, D>* filter_;
  ChannelReader<E, D>* filter_input_;
  typename Set<E>::MaterializedType set_;
};
template <int i, typename C, typename D>
inline FilterStream<i, C, D>* FilterOp(Stream<C, D>* op, ID id) {
  return new FilterStream<i, C, D>(op, id);
}
template <int i, typename C, typename D>
inline FilterStream<i, C, D>* FilterOp(
    Stream<C, D>* op,
    MaterializeStream<typename std::tuple_element<i, C>::type, D>* filter) {
  return new FilterStream<i, C, D>(op, filter);
}

// Sort an unordered stream.
template <typename C>
class ForwardSortStream : public Stream<C, DI> {};
template <typename C>
class ReverseSortStream : public Stream<C, DI> {};
class ForwardSortIDStream : public ForwardSortStream<ID> {};
class ReverseSortIDStream : public ReverseSortStream<ID> {};
typedef ReverseSortIDStream SortIDStream;

// Uniquify an unordered stream.
template <typename C>
class ForwardUniqueStream : public Stream<C, DI> {};
template <typename C>
class ReverseUniqueStream : public Stream<C, DI> {};
class ForwardUniqueIDStream : public ForwardUniqueStream<ID> {};
class ReverseUniqueIDStream : public ReverseUniqueStream<ID> {};
typedef ReverseUniqueIDStream UniqueIDStream;

// Sort and uniquify an unordered stream.
template <typename C>
class ForwardSortUniqueStream : public Stream<C, DI> {};
template <typename C>
class ReverseSortUniqueStream : public Stream<C, DI> {};
class ForwardSortUniqueIDStream : public ForwardSortUniqueStream<ID> {};
class ReverseSortUniqueIDStream : public ReverseSortUniqueStream<ID> {};
typedef ReverseSortUniqueIDStream SortUniqueIDStream;

SetEngineFlow* NewFlow(const TupleStore& store);
}  // namespace dataflow
