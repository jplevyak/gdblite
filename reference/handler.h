#include "base/callback.h"
#include "base/macros.h"
#include "util/invasive_queue.h"
#include "util/task/status.h"

namespace dataflow {

class BaseStream;
template <typename C, typename D>
class Channel;
template <typename C, typename D>
class ChannelReader;

template <typename F>
struct QueueWaitingLink {
  static F*& Next(F* x) { return x->waiting_link_.next; }
  static F*& Prev(F* x) { return x->waiting_link_.prev; }
};

// Base class for an event handler (Closure).
class BaseHandler : public Closure {
 public:
    BaseHandler() : Closure(), debug_string_(nullptr) {}

  virtual bool IsRepeatable() const { return true; }
  virtual void CheckIsRepeatable() const {}
  virtual void Run() ABSTRACT;
  virtual BaseStream* GetStream() ABSTRACT;

  const char* debug_string_;

 protected:
  friend class BaseFlow;
  friend class QueueLink<BaseHandler>;
  friend class QueueWaitingLink<BaseHandler>;

  virtual BaseStream* GetStream2() { return nullptr; }

  Link<BaseHandler> link_;          // link for posted or pending.
  Link<BaseHandler> waiting_link_;  // link for waiting.
};

template <typename T>
class SimpleHandler : public BaseHandler {
 public:
  typedef void (T::*MemberSignature)();

  SimpleHandler() : object_(nullptr), member_(nullptr) {}
  SimpleHandler(T* op, MemberSignature member) : object_(op), member_(member) {}
  virtual void Run() { (object_->*this->member_)(); }
  BaseStream* GetStream() { return object_; }
  void Set(T* object, MemberSignature member) {
    object_ = object;
    member_ = member;
  }

 private:
  T* object_;
  MemberSignature member_;
};

// A handler which takes a single argument of arbitray type.
template <typename T, typename C>
class Handler : public BaseHandler {
 public:
  typedef C* A1;
  typedef BaseHandler base;
  typedef void (T::*MemberSignature)(A1);

  Handler() : object_(nullptr), member_(nullptr), a1_(nullptr) {}
  Handler(T* object, MemberSignature member, A1 a1)
      : object_(object), member_(member), a1_(a1) {}

  void Set(T* object, MemberSignature member, A1 a1) {
    object_ = object;
    member_ = member;
    a1_ = a1;
  }

  virtual void Run() { (object_->*this->member_)(a1_); }
  virtual T* GetStream() { return object_; }

 protected:
  virtual BaseStream* GetStream2() { return nullptr; }

  T* object_;
  MemberSignature member_;
  A1 a1_;
};

// A handler specialized for ChannelReaders.
template <typename T, typename C, typename D>
class ChannelReaderHandler : public Handler<T, ChannelReader<C, D> > {
  typedef Handler<T, ChannelReader<C, D> > base;

 public:
  ChannelReaderHandler() {}
  ChannelReaderHandler(T* object, typename base::MemberSignature member,
                       ChannelReader<C, D>* reader)
      : base(object, member, reader) {}

 protected:
  virtual BaseStream* GetStream2();
};

// A handler specialized for Channels.
template <typename T, typename C, typename D>
class ChannelHandler : public Handler<T, Channel<C, D> > {
  typedef Handler<T, Channel<C, D> > base;

 public:
  ChannelHandler() {}
  ChannelHandler(T* object, typename base::MemberSignature member,
                 Channel<C, D>* channel)
      : base(object, member, channel) {}

 protected:
  virtual BaseStream* GetStream2();
};

// A handler specialized for Streams.
template <typename T>
class StreamHandler : public Handler<T, BaseStream> {
  typedef Handler<T, BaseStream> base;

 public:
  StreamHandler() {}
  StreamHandler(T* object, typename base::MemberSignature member,
                BaseStream* op)
      : base(object, member, op) {}

 protected:
  virtual BaseStream* GetStream2();
};

template <typename T, typename C>
class StatusHandler : public Callback1<util::Status> {
 public:
  typedef void (T::*MemberSignature)(C* arg);
  StatusHandler(T* op, MemberSignature member, C* c)
        : Callback1(), done_(op, member, c) {}
  virtual void Run(util::Status status) {
    if (!status.ok()) done_.GetStream()->GetFlow()->Abort(status);
    done_.GetStream()->GetFlow()->PostFromOtherThread(&done_);
  }
  BaseHandler* GetDoneCallback() { return &done_; }

 private:
  Handler<T, C> done_;
};

}  // namespace dataflow
