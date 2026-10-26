#include "dataflow/dataflow.h"
#include "dataflow/dataflow-inl.h"

#include <stddef.h>

#include "gtest/gtest.h"

namespace dataflow {

class DataFlowTest : public ::testing::Test {
 protected:
  DataFlowTest() {}
  virtual void SetUp() {}
};

TEST_F(DataFlowTest, TrivialStream) {
  int done = 0;
  class TrivialStream : public BaseStream {
   public:
    explicit TrivialStream(int* apdone)
        : pdone(apdone), run_handler(this, &TrivialStream::Run, NULL) {}
    void Start() { GetFlow()->Post(&run_handler); }
    void Run(void* a1) { *pdone = 1; }
    int* pdone;
    Handler<TrivialStream, void> run_handler;
  };
  BaseFlow flow;
  flow.Add(new TrivialStream(&done));
  flow.Run();
  ASSERT_EQ(done, 1);
}

TEST_F(DataFlowTest, PingPongStream) {
  class PingPongStream : public BaseStream {
   public:
    explicit PingPongStream(bool astart)
        : start(astart), todo(10), other(NULL) {}
    void Initialize() { run_handler.Set(other, &PingPongStream::Run, NULL); }
    void Start() {
      if (start) Run(0);
    }
    void Run(void* a1) {
      if (todo > 0) {
        todo--;
        GetFlow()->Post(&run_handler);
      }
    }
    bool start;
    int todo;
    PingPongStream* other;
    Handler<PingPongStream, void> run_handler;
  };
  auto ping_op = RootPtr(new PingPongStream(true));
  auto pong_op = RootPtr(new PingPongStream(false));
  ping_op->other = pong_op;
  pong_op->other = ping_op;
  BaseFlow flow;
  flow.Add(ping_op);
  flow.Add(pong_op);
  flow.Run();
  ASSERT_EQ(ping_op->todo, 0);
  ASSERT_EQ(pong_op->todo, 0);
}

TEST_F(DataFlowTest, PingPongNotifyStream) {
  class PingPongStream : public BaseStream {
   public:
    explicit PingPongStream(bool astart)
        : start(astart), todo(1000), other(NULL) {}
    void Initialize() { run_handler.Set(other, &PingPongStream::Run, NULL); }
    void Start() {
      if (start) Run(0);
    }
    void Run(void* a1) {
      if (todo > 0) {
        todo--;
        GetFlow()->Notify(&run_handler);
      }
    }
    bool start;
    int todo;
    PingPongStream* other;
    Handler<PingPongStream, void> run_handler;
  };
  auto ping_op = RootPtr(new PingPongStream(true));
  auto pong_op = RootPtr(new PingPongStream(false));
  ping_op->other = pong_op;
  pong_op->other = ping_op;
  BaseFlow flow;
  flow.Add(ping_op);
  flow.Add(pong_op);
  flow.Run();
  ASSERT_EQ(ping_op->todo, 0);
  ASSERT_EQ(pong_op->todo, 0);
}

TEST_F(DataFlowTest, PingPongStopStream) {
  class PingPongStream : public BaseStream {
   public:
    PingPongStream(bool astart, bool astop)
        : start(astart), stop(astop), todo(10), other(NULL) {}
    void Initialize() { run_handler.Set(other, &PingPongStream::Run, NULL); }
    void Start() {
      if (start) Run(0);
    }
    void Run(void* a1) {
      if (todo > 0) {
        todo--;
        if (todo == 5 && stop && !start)
          GetFlow()->Remove(this);
        else
          GetFlow()->Post(&run_handler);
      }
    }
    bool start;
    bool stop;
    int todo;
    PingPongStream* other;
    Handler<PingPongStream, void> run_handler;
  };
  auto ping_op = RootPtr(new PingPongStream(true, true));
  auto pong_op = RootPtr(new PingPongStream(false, true));
  ping_op->other = pong_op;
  pong_op->other = ping_op;
  auto ping2_op = RootPtr(new PingPongStream(true, false));
  auto pong2_op = RootPtr(new PingPongStream(false, false));
  ping2_op->other = pong2_op;
  pong2_op->other = ping2_op;
  BaseFlow flow;
  flow.Add(ping_op);
  flow.Add(pong_op);
  flow.Add(ping2_op);
  flow.Add(pong2_op);
  flow.Run();
  ASSERT_EQ(ping_op->todo, 5);
  ASSERT_EQ(pong_op->todo, 5);
  ASSERT_EQ(ping2_op->todo, 0);
  ASSERT_EQ(pong2_op->todo, 0);
}

TEST_F(DataFlowTest, PingPongAbortStream) {
  class PingPongStream : public BaseStream {
   public:
    PingPongStream(bool astart, bool astop)
        : start(astart), stop(astop), todo(10), other(NULL) {}
    void Initialize() { run_handler.Set(other, &PingPongStream::Run, NULL); }
    void Start() {
      if (start) Run(0);
    }
    void Run(void* a1) {
      if (todo > 0) {
        todo--;
        if (todo == 5 && stop && !start)
          GetFlow()->Abort(util::Status::OK);
        else
          GetFlow()->Post(&run_handler);
      }
    }
    bool start;
    bool stop;
    int todo;
    PingPongStream* other;
    Handler<PingPongStream, void> run_handler;
  };
  auto ping_op = RootPtr(new PingPongStream(true, true));
  auto pong_op = RootPtr(new PingPongStream(false, true));
  ping_op->other = pong_op;
  pong_op->other = ping_op;
  auto ping2_op = RootPtr(new PingPongStream(true, false));
  auto pong2_op = RootPtr(new PingPongStream(false, false));
  ping2_op->other = pong2_op;
  pong2_op->other = ping2_op;
  BaseFlow flow;
  flow.Add(ping_op);
  flow.Add(pong_op);
  flow.Add(ping2_op);
  flow.Add(pong2_op);
  flow.Run();
  ASSERT_EQ(ping_op->todo, 5);
  ASSERT_GE(pong_op->todo, 5);
  ASSERT_LE(pong_op->todo, 6);
  ASSERT_EQ(ping2_op->todo, 5);
  ASSERT_GE(pong2_op->todo, 5);
  ASSERT_LE(pong2_op->todo, 6);
}

TEST_F(DataFlowTest, PingPongOtherThreadStream) {
  class PingPongStream : public BaseStream {
   public:
    explicit PingPongStream(bool astart)
        : start(astart), todo(10), other(NULL) {}
    void Initialize() { run_handler.Set(other, &PingPongStream::Run, NULL); }
    void Start() {
      if (start) Run(0);
    }
    void Run(void* a1) {
      if (todo > 0) {
        todo--;
        GetFlow()->WaitFor(&run_handler);
        GetFlow()->PostFromOtherThread(&run_handler);
      }
    }
    bool start;
    int todo;
    PingPongStream* other;
    Handler<PingPongStream, void> run_handler;
  };
  auto ping_op = RootPtr(new PingPongStream(true));
  auto pong_op = RootPtr(new PingPongStream(false));
  ping_op->other = pong_op;
  pong_op->other = ping_op;
  BaseFlow flow;
  flow.Add(ping_op);
  flow.Add(pong_op);
  flow.Run();
  ASSERT_EQ(ping_op->todo, 0);
  ASSERT_EQ(pong_op->todo, 0);
}
}  // namespace dataflow
