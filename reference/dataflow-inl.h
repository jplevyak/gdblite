#include <utility>

#include "base/integral_types.h"
#include "base/logging.h"
#include "dataflow/dataflow.h"
#include "util/gtl/map_util.h"

template <typename T>
class reffed_ptr;
template <typename T>
class root_ptr;

namespace blythe {
namespace dataflow {

typedef ConditionMutex Mutex;
// Only works for the constructor.
#define MutexLock ConditionMutexLock

class BaseChannel;
class BaseFlow;
class BaseHandler;
class BaseStream;
template <typename C, typename D>
class Channel;
template <typename C, typename D>
class ChannelReader;
template <typename C, typename D>
struct ChannelBlock;
template <typename T, typename C, typename D>
class ChannelHandler;
template <typename T, typename C, typename D>
class ChannelReaderHandler;
template <typename T>
class StreamHandler;

inline bool BaseFlow::SomePending() {
  return !pending_events_.Empty();
}

inline void BaseStream::WaitFor(BaseHandler* h) { GetFlow()->WaitFor(h); }

inline void BaseFlow::WaitFor(BaseHandler* h) {
  CHECK(!h->waiting_link_.IsInQueue());
  waiting_events_.Enqueue(h);
}

inline void BaseFlow::Timeout() {
  LOG(ERROR) << "Timeout: ";
  for (auto h = waiting_events_.head; h; h = waiting_events_.Next(h)) {
    LOG(ERROR) << "Waiting for " << h << " "
               << (h->debug_string_ ? h->debug_string_ : "<UNKNOWN>");
  }
  aborted_ = true;  // TODO: record reasons for aborts.
}

inline void BaseFlow::HandlePendingEvents(bool post) {
  while (auto e = pending_events_.Pop())
    if (post) Post(e);
}

inline void BaseFlow::WaitForAll(bool post) {
  Condition some_pending(this, &BaseFlow::SomePending);
  MutexLock l(&mutex_);
  while (!pending_events_.Empty() || !waiting_events_.Empty()) {
    HandlePendingEvents(post);
    if (!waiting_events_.Empty())
      if (!mutex_.AwaitWithTimeout(some_pending, kDefaultTimeoutMsec))
        Timeout();
  }
}

inline void BaseFlow::Remove(BaseStream* op) {
  if (!operations_.count(op))  // Prevent recursion.
    return;
  WaitForAllAndPost();
  for (auto h = posted_events_.head; h;)
    if (h->GetStream() == op || h->GetStream2() == op)
      h = posted_events_.Remove(h);
    else
      h = h->link_.next;
  auto protect = RootPtr(op);
  while (op->attached_.size()) {
    auto a = *op->attached_.begin();
    op->attached_.erase(a);
    a->DetachedInternal(op);
  }
  operations_.erase(op);
  root_operations_.erase(op);
}

inline void BaseFlow::Run() {
  aborted_ = false;
  InitializeRun();
  for (auto& op : root_operations_) op->Start();
  Condition some_pending(this, &BaseFlow::SomePending);
  while (!aborted_) {
    {
      MutexLock l(&mutex_);
      HandlePendingEvents(true);
      // If we have active operations, but no posted events, wait till one or
      // more pending events are enqueued.
      if (posted_events_.Empty() && !waiting_events_.Empty()) {
        if (!mutex_.AwaitWithTimeout(some_pending, kDefaultTimeoutMsec))
          Timeout();
        HandlePendingEvents(true);
      }
      // If we have nothing to do, break out of the loop.
      if (posted_events_.Empty() && waiting_events_.Empty() &&
          pending_events_.Empty())
        break;
    }
    // Run posted_events.
    while (!posted_events_.Empty() && !aborted_) posted_events_.Pop()->Run();
  }
  if (aborted_) {
    while (!posted_events_.Empty())  // Clear posted_events events.
      posted_events_.Pop();
    WaitForAllAndDoNotPost();
    for (auto op : operations_) op->Aborted();
  }
  {
    // Safely clear out ref_ptrs when the scope exist.
    set<root_ptr<BaseStream>> roots_tmp;
    roots_tmp.swap(root_operations_);
  }
  operations_.clear();
}

inline BaseChannel* BaseStream::Attach(BaseStream* op) {
  attached_.insert(op);
  return nullptr;
}

inline void BaseStream::Ref() { ref_count_++; }

inline bool BaseStream::Unref() {
  ref_count_--;
  CHECK_GE(ref_count_, 0);
  if (!ref_count_) {
    delete this;
    return false;
  }
  return true;
}

inline void BaseStream::Root() { rooted_++; }

inline void BaseStream::Unroot() {
  rooted_--;
  CHECK_GE(rooted_, 0);
  if (!rooted_) {
    RootOrDetach();
  }
}

inline bool BaseStream::RootOrDetach() {
  root_ = nullptr;
  for (auto a : attached_) a->ClearRoot(this);
  for (auto a : attached_)
    if (a->FindRoot()) root_ = a;
  if (!root_) {
    if (GetFlow()) {
      GetFlow()->Remove(this);
    } else {
      reffed_ptr<BaseStream> prevent_delete(this, CREATE_A_REF);
      while (attached_.size()) {
        auto a = *attached_.begin();
        attached_.erase(a);
        a->DetachedInternal(this);
      }
    }
    return false;
  }
  return true;
}

inline void BaseStream::ClearRoot(BaseStream* old_root) {
  if (root_ == old_root) {
    root_ = nullptr;
    for (auto a : attached_) a->ClearRoot(this);
  }
}

inline bool BaseStream::FindRoot() {
  if (root_ == kFindRootInProgress) return false;
  if (root_) return true;
  root_ = kFindRootInProgress;
  for (auto a : attached_)
    if (a->FindRoot()) root_ = a;
  if (root_ == kFindRootInProgress) root_ = nullptr;
  return root_ != nullptr;
}

template <typename C, typename D>
inline void Channel<C, D>::SetRange(const K& start, const K& end) {
  range_set_ = true;
  start_ = start;
  end_ = end;
}

template <typename C, typename D>
inline ChannelReader<C, D>* Channel<C, D>::Reader(BaseStream* op) {
  ChannelReader<C, D>* reader = readers_[op];
  if (reader) return reader;
  reader = new ChannelReader<C, D>(this);
  readers_[op] = reader;
  return reader;
}

template <typename C, typename D>
inline void Channel<C, D>::DetachReader(BaseStream* op) {
  if (readers_.count(op)) {
    delete readers_[op];
    readers_.erase(op);
  }
}

inline BaseStream* BaseChannelReader::GetStream() {
  return GetChannel()->GetStream();
}

inline BaseFlow* BaseChannelReader::GetFlow() { return GetStream()->GetFlow(); }

inline BaseFlow* BaseChannel::GetFlow() { return GetStream()->GetFlow(); }

template <typename T, typename C, typename D>
inline BaseStream* ChannelReaderHandler<T, C, D>::GetStream2() {
  return this->a1_->GetChannel()->GetStream();
}

template <typename T, typename C, typename D>
inline BaseStream* ChannelHandler<T, C, D>::GetStream2() {
  return this->a1_->GetStream();
}

template <typename T>
inline BaseStream* StreamHandler<T>::GetStream2() {
  return this->a1_;
}

inline void BaseChannelReader::OnEvent(EventType et, BaseHandler* h) {
  event_handlers_[et] = h;
}

inline bool BaseChannelReader::PostEvent(EventType et) {
  BaseHandler* h = FindPtrOrNull(event_handlers_, et);
  if (h) this->GetFlow()->Post(h);
  return h != nullptr;
}

inline bool BaseChannelReader::NotifyEvent(EventType et) {
  BaseHandler* h = FindPtrOrNull(event_handlers_, et);
  if (h) this->GetFlow()->Notify(h);
  return h != nullptr;
}

inline void BaseChannel::OnEvent(EventType et, BaseHandler* h) {
  event_handlers_[et] = h;
}

inline bool BaseChannel::PostEvent(EventType et) {
  BaseHandler* h = FindPtrOrNull(event_handlers_, et);
  if (h) this->GetFlow()->Post(h);
  return h != nullptr;
}

inline bool BaseChannel::NotifyEvent(EventType et) {
  BaseHandler* h = FindPtrOrNull(event_handlers_, et);
  if (h) this->GetFlow()->Notify(h);
  return h != nullptr;
}

inline void BaseStream::OnEvent(EventType et, BaseHandler* h) {
  event_handlers_[et] = h;
}

inline bool BaseStream::PostEvent(EventType et) {
  BaseHandler* h = FindPtrOrNull(event_handlers_, et);
  if (h) this->GetFlow()->Post(h);
  return h != nullptr;
}

inline bool BaseStream::NotifyEvent(EventType et) {
  BaseHandler* h = FindPtrOrNull(event_handlers_, et);
  if (h) this->GetFlow()->Notify(h);
  return h != nullptr;
}

template <typename C, typename D>
inline int64 Channel<C, D>::UsedCapacity() {
  // If we are the only one holdibng a reference, decrease used_capacity_
  // and drop the reference.   NOTE: this does not support sharing blocks
  // between channels.
  while (head_ != nullptr && head_->ref_count == 1) {
    head_.reset(head_->next.get(), CREATE_A_REF);
    used_capacity_ -= kDefaultBlockSize;
    if (head_ == nullptr) tail_.reset(nullptr);
  }
  CHECK(head_ != nullptr || used_capacity_ == 0);
  return used_capacity_;
}

template <typename C, typename D>
inline ChannelBlock<C, D>* Channel<C, D>::GetBlock() {
  if (tail_ != nullptr && !tail_->IsFull()) return tail_.get();
  return Next();
}

template <typename C, typename D>
inline ChannelBlock<C, D>* Channel<C, D>::Next() {
  CHECK(tail_ == nullptr || !tail_->done);
  bool wrote = tail_ != nullptr && tail_->written != tail_->size;
  if (wrote) tail_->written = tail_->size;
  if (UsedCapacity() < capacity_) {
    reffed_ptr<ChannelBlock<C, D>> b(new ChannelBlock<C, D>(kDefaultBlockSize),
                                     CREATE_A_REF);
    used_capacity_ += kDefaultBlockSize;
    if (tail_ != nullptr) {
      tail_->next.reset(b.release());
      tail_.reset(tail_->next.get(), CREATE_A_REF);
    } else {
      tail_.reset(b.release());
      head_.reset(tail_.get(), CREATE_A_REF);
      for (auto p : readers_) {
        CHECK(p.second->block_.get() == nullptr);
        p.second->block_.reset(head_.get(), CREATE_A_REF);
      }
    }
  }
  if (wrote) PostEventToReaders(READABLE);
  return tail_.get();
}

template <typename C, typename D>
inline typename ChannelReader<C, D>::iterator ChannelReader<C, D>::begin() {
  ChannelBlock<C, D>* b = GetBlock();
  return b->begin() + offset_;
}

template <typename C, typename D>
inline typename ChannelReader<C, D>::iterator ChannelReader<C, D>::end() {
  ChannelBlock<C, D>* b = GetBlock();
  return b->begin() + b->written;
}

template <typename C, typename D>
inline bool ChannelReader<C, D>::IsEmpty() {
  ChannelBlock<C, D>* b = GetBlock();
  return b->written <= offset_;
}

template <typename C, typename D>
inline void ChannelReader<C, D>::SetBlock(
    const reffed_ptr<ChannelBlock<C, D>>& b) {
  block_.reset(b.get(), CREATE_A_REF);
  offset_ = 0;
}

template <typename C, typename D>
inline bool ChannelReader<C, D>::IsDone() {
  return GetBlock()->done;
}

template <typename C, typename D>
inline ChannelBlock<C, D>* ChannelReader<C, D>::GetBlock() {
  if (block_ == nullptr) {
    if (GetChannel()->head_ != nullptr) {
      SetBlock(GetChannel()->head_);
      CHECK_GT(block_->written, offset_);
    }
  } else {
    if (block_->size <= offset_) Next();
  }
  return block_.get();
}

template <typename C, typename D>
inline bool ChannelReader<C, D>::Next() {
  if (block_ == nullptr) {
    if (GetChannel()->head_ != nullptr) {
      SetBlock(GetChannel()->head_);
      return true;
    }
    return false;
  }
  if (block_->next != nullptr) {
    SetBlock(block_->next);
    GetChannel()->SomeRead();
    return true;
  } else {
    offset_ = block_->written;
    return false;
  }
}

template <typename C, typename D>
inline void ChannelReader<C, D>::Consume(iterator new_begin) {
  auto old_begin = begin();
  if (new_begin != old_begin) {
    offset_ += new_begin - old_begin;
    GetChannel()->SomeRead();
  }
}

// Convenience function to insert a collection of values using iterators.
template <typename C, typename D>
template <typename input_iterator>
inline input_iterator ChannelBlock<C, D>::insert(input_iterator first,
                                                 input_iterator last) {
  iterator e = end();
  iterator it = inserter();
  for (; first != last && it != e; ++first) *it++ = *first;
  Fill(it);
  return first;
}

// Convenience function to insert a single value.
template <typename C, typename D>
inline bool ChannelBlock<C, D>::insert(const C& value) {
  if (!IsFull()) {
    *inserter() = value;
    written++;
    return true;
  } else {
    return false;
  }
}

inline void BaseFlow::Post(BaseHandler* h) {
  if (h->link_.IsInQueue()) return;
  posted_events_.Enqueue(h);
}

inline void BaseFlow::Notify(BaseHandler* h) {
  if (notify_depth_ < kNotifyLimit) {
    notify_depth_++;
    if (h->link_.IsInQueue()) posted_events_.Remove(h);
    h->Run();
    notify_depth_--;
  } else {
    // Insert at the front.
    if (h->link_.IsInQueue()) posted_events_.Remove(h);
    posted_events_.Push(h);
  }
}

inline void BaseFlow::PostFromOtherThread(BaseHandler* h) {
  MutexLock _l(&mutex_);
  waiting_events_.Remove(h);
  if (h->link_.IsInQueue()) return;
  pending_events_.Enqueue(h);
}

inline bool BaseChannelReader::PostEventToChannel(EventType et) {
  return GetChannel()->PostEvent(et);
}

template <typename C, typename D>
inline bool Channel<C, D>::Read() {
  return PostEvent(WRITABLE);
}

template <typename C, typename D>
inline bool Channel<C, D>::Splits() {
  return PostEvent(SPLITS_REQUEST);
}

template <typename C, typename D>
inline bool Channel<C, D>::Seek(const typename Channel<C, D>::K& start) {
  seek_ = start;
  return PostEvent(SEEK_REQUEST);
}

template <typename C, typename D>
inline void Channel<C, D>::SomeRead() {
  if (UsedCapacity() < capacity_) PostEvent(WRITABLE);
}

template <typename C, typename D>
inline void Channel<C, D>::Fill(iterator it) {
  if (tail_->Fill(it)) this->SomeWritten();
}

template <typename C, typename D>
inline void Channel<C, D>::SomeWritten() {
  CHECK(tail_->written <= tail_->size);
  PostEventToReaders(READABLE);
}

template <typename C, typename D>
inline bool Channel<C, D>::PostEventToReaders(EventType et) {
  bool ret = false;
  for (auto p : readers_) ret = p.second->PostEvent(et) || ret;
  return ret;
}

// TODO: calling Done() from a reader results in a READBLE event to
// that same reader.  This could be avoided, by passing in the Reader here.
template <typename C, typename D>
inline void Channel<C, D>::Done() {
  if (tail_ == nullptr) {
    reffed_ptr<ChannelBlock<C, D>> b(new ChannelBlock<C, D>(0), CREATE_A_REF);
    tail_.reset(b.release());
    head_.reset(tail_.get(), CREATE_A_REF);
    for (auto p : readers_) {
      CHECK(p.second->block_.get() == nullptr);
      p.second->block_.reset(head_.get(), CREATE_A_REF);
    }
  }
  if (!tail_->done) {
    tail_->done = true;
    PostEventToReaders(READABLE);
  }
}

template <typename T, typename C, typename D>
void BaseStream::Register(EventType et, void (T::*member)(Channel<C, D>*),
                          Channel<C, D>* channel) {
  ChannelHandler<T, C, D>* h =
      new ChannelHandler<T, C, D>(static_cast<T*>(this), member, channel);
  InsertAndDeleteExisting(&handlers_, make_pair(et, channel), h);
  channel->OnEvent(et, h);
}

template <typename T, typename C, typename D>
void BaseStream::Register(EventType et, void (T::*member)(ChannelReader<C, D>*),
                          ChannelReader<C, D>* reader) {
  ChannelReaderHandler<T, C, D>* h =
      new ChannelReaderHandler<T, C, D>(static_cast<T*>(this), member, reader);
  InsertAndDeleteExisting(&handlers_, make_pair(et, reader), h);
  reader->OnEvent(et, h);
}

template <typename T>
void BaseStream::Register(EventType et,
                          void (T::*member)(BaseStream*),  // NOLINT
                          BaseStream* op) {
  StreamHandler<T>* h = new StreamHandler<T>(static_cast<T*>(this), member, op);
  InsertAndDeleteExisting(&handlers_, make_pair(et, op), h);
  op->OnEvent(et, h);
}

template <typename C, typename D>
inline void ChannelBlock<C, D>::Ref() {
  ref_count++;
}

template <typename C, typename D>
inline void ChannelBlock<C, D>::Unref() {
  ref_count--;
  CHECK_GE(ref_count, 0);
  if (!ref_count) delete this;
}
}  // namespace dataflow
