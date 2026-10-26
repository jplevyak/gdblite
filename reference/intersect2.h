#include <set>
using ::std::set;
#include <string>
using ::std::string;
#include <utility>
#include <vector>
using ::std::vector;

#include "base/logging.h"
#include "common/common_types.h"
#include "common/record.h"
#include "core/tupleindex.h"
#include "dataflow/dataflow.h"
#include "dataflow/dataflow-inl.h"
#include "dataflow/handler.h"
#include "dataflow/set_engine.h"
#include "dataflow/set_engine_data_interface.h"
#include "util/btree/btree_set.h"
#include "util/gtl/map_util.h"
#include "util/refcount/reffed_ptr.h"

using dataflow::Channel;
using dataflow::ChannelBlock;
using dataflow::ChannelReader;
using dataflow::ChannelReaderHandler;
using dataflow::DI;
using dataflow::ID;
using dataflow::IDChannel;
using dataflow::IDReader;
using dataflow::IDStream;
using dataflow::IDToSetIndex;
using dataflow::IDToSetIndexStream;
using dataflow::JoinOp;
using dataflow::LinkageIndexStream;
using dataflow::MapStream;
using dataflow::READABLE;
using dataflow::SecondOp;
using dataflow::Stream;
using dataflow::UUIDStream;
using dataflow::UUIDToSetIndex;
using dataflow::UUIDToSetIndexStream;
using dataflow::WRITABLE;

template <typename InputOp, typename Output>
class IntersectionBase : public Stream<Output, DI> {
 public:
  typedef Stream<Output, DI> Base;
  typedef ChannelReader<typename InputOp::DataType, DI> ReaderT;
  typedef ChannelBlock<Output, DI> OutputBlockT;
  explicit IntersectionBase(const vector<InputOp*>& ops) {
    writable_output_ = true;
    for (auto op : ops) {
      inputs_.push_back(op->Input(this));
    }
  }
  virtual ~IntersectionBase() {}

  void Initialize() {
    for (auto i : inputs_) {
      this->Register(READABLE, &IntersectionBase::Readable, i);
    }
    this->Register(WRITABLE, &IntersectionBase::Writable, Base::GetOutput());
  }
  bool Ready() {
    return writable_output_ && readable_inputs_.size() == inputs_.size();
  }
  void Readable(ReaderT* reader) {
    readable_inputs_.insert(reader);
    if (Ready()) Run();
  }
  void Writable(Channel<Output, DI>* writer) {
    writable_output_ = true;
    if (Ready()) {
      Run();
    } else {
      for (auto i : inputs_)
        if (!i->GetBlock()) {
          i->GetChannel()->Read();
        }
    }
  }
  void Run() {
    while (true) {
      auto b = Base::GetOutput()->GetBlock();
      if (b->IsFull()) {
        writable_output_ = false;
        return;
      }
      for (auto i : inputs_) {
        if (i->IsEmpty()) {
          if (i->IsDone()) {
            Base::GetOutput()->Done();
          } else {
            readable_inputs_.erase(i);
          }
          return;
        }
      }
      ProduceOutput(b);
    }
  }

 protected:
  // TODO: make it work for N ranges, if we decide not to implement
  // VIP indexes.  Also, remove the NOLINT here.
  typedef typename OutputBlockT::iterator OutIter;
  template <typename IDIter>
  void Intersect2Ranges(const IDIter& e1, const IDIter& e2, const OutIter& e3,
                        IDIter& s1, IDIter& s2, OutIter& s3) {  // NOLINT
    while (s1 != e1 && s2 != e2 && s3 != e3) {
      if (*s1 < *s2) {
        s1++;
      } else if (*s2 < *s1) {
        s2++;
      } else {
        *s3++ = *s1;
        s1++;
        s2++;
      }
    }
  }

  virtual void ProduceOutput(OutputBlockT* b) = 0;

  vector<ReaderT*> inputs_;
  set<ReaderT*> readable_inputs_;
  bool writable_output_;
};

// Intersect 2 sets of ids.
class Intersect2Stream : public IntersectionBase<IDStream, ID> {
 public:
  typedef IntersectionBase<IDStream, ID> Base;
  explicit Intersect2Stream(const vector<IDStream*>& ops) : Base(ops) {
    CHECK_EQ(2, ops.size());
  }

 private:
  void ProduceOutput(OutputBlockT* b) {
    auto s1 = inputs_[0]->begin(), s2 = inputs_[1]->begin();
    auto s3 = b->inserter();
    Intersect2Ranges(inputs_[0]->end(), inputs_[1]->end(), b->end(), s1, s2,
                     s3);
    GetOutput()->Fill(s3);
    inputs_[0]->Consume(s1);
    inputs_[1]->Consume(s2);
  }
};

// Intersect a large set with N small sets, union the intersection results
// as final results, and store rejections if some small set has 0 intersection
// with the large set.
class UXStream : public IntersectionBase<dataflow::IDToSetStream, ID> {
 public:
  typedef IntersectionBase<dataflow::IDToSetStream, ID> Base;
  explicit UXStream(const vector<dataflow::IDToSetStream*>& ops) : Base(ops) {
    CHECK_EQ(2, ops.size());
  }

 private:
  // TODO: 1, optimize by partial read. 2, only materialize large_set
  // once and cache it.
  void ProduceOutput(OutputBlockT* b) {
    typedef dataflow::Set<ID> SetType;
    typedef SetType::MaterializedType MemSet;
    auto s1 = inputs_[0]->begin(), e1 = inputs_[0]->end(),
         s2 = inputs_[1]->begin(), e2 = inputs_[1]->end();
    CHECK_EQ(1, e1 - s1);
    MemSet large_set;
    s1->second.Materialize(this->GetDataInterface(), &large_set);
    while (s2 != e2) {
      MemSet small_set;
      s2->second.Materialize(this->GetDataInterface(), &small_set);
      auto ss1 = large_set.begin();
      auto ss2 = small_set.begin();
      auto ss3 = b->inserter();
      Intersect2Ranges(large_set.end(), small_set.end(), b->end(), ss1, ss2,
                       ss3);
      GetOutput()->Fill(ss3);
      ++s2;
    }
    inputs_[1]->Consume(s2);
  }
};

class CartesianProductStream : public Stream<pair<ID, ID>, DI> {
 public:
  typedef ChannelReader<ID, DI> ReaderT;

  explicit CartesianProductStream(const vector<IDStream*>& ops) {
    CHECK_EQ(2, ops.size());
    writable_output_ = true;
    for (auto op : ops) {
      inputs_.push_back(op->Input(this));
    }
  }
  ~CartesianProductStream() {}

  void Initialize() {
    for (auto i : inputs_) {
      this->Register(READABLE, &CartesianProductStream::Readable, i);
    }
    this->Register(WRITABLE, &CartesianProductStream::Writable, GetOutput());
  }

  bool Ready() {
    return writable_output_ &&
           (started_ || readable_inputs_.size() == inputs_.size());
  }

  void Readable(ReaderT* reader) {
    readable_inputs_.insert(reader);
    if (Ready()) Run();
  }
  void Writable(Channel<pair<ID, ID>, DI>* writer) {
    writable_output_ = true;
    if (Ready()) {
      Run();
    } else {
      for (auto i : inputs_)
        if (!i->GetBlock()) {
          i->GetChannel()->Read();
        }
    }
  }

  void Run() {
    started_ = true;
    while (true) {
      auto b = GetOutput()->GetBlock();
      if (!b || b->IsFull()) {
        writable_output_ = false;
        return;
      }
      bool exists_valid_input = false;
      for (auto i : inputs_) {
        if (!ContainsKey(readable_inputs_, i)) continue;
        if (i->IsEmpty()) {
          if (i->IsDone()) {
            inputs_done_.insert(i);
            if (inputs_done_.size() >= inputs_.size()) {
              GetOutput()->Done();
              return;
            }
          }
          readable_inputs_.erase(i);
          down_to_one_input_ = true;
          first_input_done_ = (i == inputs_[0]);
        } else {
          exists_valid_input = true;
        }
      }
      if (exists_valid_input) {
        ProduceOutput(b);
      } else {
        return;
      }
    }
  }

 private:
  typedef ChannelBlock<pair<ID, ID>, DI> OutputBlockT;

  void ProduceOutputWhenOneLeft(ID** s1, const ID* const e1, ID** s2,
                                const ID* const e2, pair<ID, ID>** s3,
                                const pair<ID, ID>* const e3) {
    // When there's only one input left we have the small input
    // already in memory. We have to avoid loading the entire large
    // input into memory.
    ID** begin;
    const ID* end;
    IDSet* smaller_set;
    if (first_input_done_) {
      begin = s2;
      end = e2;
      smaller_set = &first_old_values_;
      second_old_values_.clear();
    } else {
      begin = s1;
      end = e1;
      smaller_set = &second_old_values_;
      first_old_values_.clear();
    }

    // We no longer have to insert the new values into
    // first/second_old_values_ because we're streaming these.
    while (*s3 != e3 && *begin != end) {
      ID val = **begin;
      (*begin)++;
      for (ID other : *smaller_set) {
        pair<ID, ID> key = first_input_done_ ? pair<ID, ID>(other, val)
                                             : pair<ID, ID>(val, other);
        if (*s3 != e3) {
          **s3 = key;
          (*s3)++;
        } else {
          to_output_.insert(key);
        }
      }
    }
  }

  void ProduceOutputWhenBothLeft(ID** s1, const ID* const e1, ID** s2,
                                 const ID* const e2, pair<ID, ID>** s3,
                                 const pair<ID, ID>* const e3) {
    // For each new element we see, we generate all pairs with
    // the past elements we've seen.
    bool on_s1 = true;
    bool s1_done = false, s2_done = false;
    while (*s3 != e3 && !(s1_done && s2_done)) {
      if (on_s1) {
        if (*s1 == e1) {
          s1_done = true;
        } else {
          auto new_val = **s1;
          (*s1)++;
          first_old_values_.insert(new_val);
          for (auto& second : second_old_values_) {
            pair<ID, ID> key = pair<ID, ID>(new_val, second);
            if (*s3 != e3) {
              **s3 = key;
              (*s3)++;
            } else {
              to_output_.insert(key);
            }
          }
        }
      } else {
        if (*s2 == e2) {
          s2_done = true;
        } else {
          auto new_val = **s2;
          (*s2)++;
          second_old_values_.insert(new_val);
          for (auto& first : first_old_values_) {
            pair<ID, ID> key = pair<ID, ID>(first, new_val);
            if (*s3 != e3) {
              **s3 = key;
              (*s3)++;
            } else {
              to_output_.insert(key);
            }
          }
        }
      }

      on_s1 = !on_s1;
    }
  }

  void ProduceOutput(OutputBlockT* b) {
    auto s1 = inputs_[0]->begin(), s2 = inputs_[1]->begin();
    auto s3 = b->inserter();
    auto e1 = inputs_[0]->end(), e2 = inputs_[1]->end();
    auto e3 = b->end();

    // If we weren't able to put all the output on the output block
    // before, let's do so now.
    if (!to_output_.empty()) {
      auto oiter = to_output_.begin();
      for (; s3 != e3 && oiter != to_output_.end(); s3++, oiter++) {
        *s3 = *oiter;
      }
      to_output_.erase(to_output_.begin(), oiter);
    }

    if (down_to_one_input_) {
      ProduceOutputWhenOneLeft(&s1, e1, &s2, e2, &s3, e3);
    } else {
      ProduceOutputWhenBothLeft(&s1, e1, &s2, e2, &s3, e3);
    }

    // Register what was consumed and what was produced. The
    // ProductOutputWhen... functions will change the values of s1,
    // s2, and s3.
    GetOutput()->Fill(s3);
    inputs_[0]->Consume(s1);
    inputs_[1]->Consume(s2);
  }

  vector<ReaderT*> inputs_;
  set<ReaderT*> readable_inputs_;
  bool writable_output_;

  set<ReaderT*> inputs_done_;
  bool started_ = false;
  bool down_to_one_input_ = false;
  bool first_input_done_ = false;

  IDSet first_old_values_;
  IDSet second_old_values_;
  // The outputs we didn't have enough room for in the output block.
  util::btree::btree_set<pair<ID, ID>> to_output_;
};

// Given two sets of IDs, produces a stream of IDSets where the id is
// a intersection key and the resulting set is the lookup in the
// IntersectionLinkage index. Note that the key in the key value pair
// will be the string id of the IntersectionKey string.
class LinkageIntersectionIndexStream : public UUIDToSetIndexStream {
 public:
  LinkageIntersectionIndexStream(UUIDStream* op, int index)
      : UUIDToSetIndexStream(UUIDToSetIndex(index), op) {}
};

class IntersectionKeyStream : public MapStream<pair<ID, ID>, UUID, DI> {
 public:
  explicit IntersectionKeyStream(Stream<pair<ID, ID>, DI>* op, Linkage l1,
                                 Linkage l2)
      : MapStream<pair<ID, ID>, UUID, DI>(op), l1_(l1), l2_(l2) {}

  UUID Run(const pair<ID, ID>& tuple) {
    if (l2_ == kSubject)
      return IntersectionKey(tuple.second, tuple.first);
    else
      return IntersectionKey(tuple.first, tuple.second);
  }

 private:
  Linkage l1_;
  Linkage l2_;
};

inline LinkageIntersectionIndexStream* LinkageIntersectionIndexOp(IDStream* op1,
                                                                  IDStream* op2,
                                                                  Linkage l1,
                                                                  Linkage l2) {
  return new LinkageIntersectionIndexStream(
      new IntersectionKeyStream(new CartesianProductStream({op1, op2}), l1, l2),
      l2 == kSubject ? IntersectionIndex(l2, l1) : IntersectionIndex(l1, l2));
}

inline IDStream* Intersect2Op(IDStream* op1, IDStream* op2) {
  vector<IDStream*> ops;
  ops.push_back(op1);
  ops.push_back(op2);
  return new Intersect2Stream(ops);
}

inline IDStream* UnionIntersectOp(dataflow::IDToSetStream* op1,
                                  dataflow::IDToSetStream* op2) {
  vector<dataflow::IDToSetStream*> ops;
  ops.push_back(op1);
  ops.push_back(op2);
  return new UXStream(ops);
}

