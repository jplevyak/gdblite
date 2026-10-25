#include "common/record.h"

#include "common/common_types.h"
#include "util/rand.h"
#include "gtest/gtest.h"


const int kLinkagesPerTuple = 10;

int LinkageOffset(int i, Linkage l) {
  return i * kLinkagesPerTuple + static_cast<int>(l);

// The Record tests include large tests, this just tests the new features
// of Record.
TEST(RecordTest, Simple) {
  RecordLog* log = RecordLog::Make(malloc(100000));
  vector<ID> ids = GetRandomSkewed(4 * kLinkagesPerTuple);
  int i = 0;  // ids in use.
  int id = log->start_id();

  ID t1 = id;
  ID t2 = log->MakeTuple(t1, false, kLinkagesPerTuple, &ids[i]);
  i += kLinkagesPerTuple;
  ID t3 = log->MakeTuple(t2, false, kLinkagesPerTuple, &ids[i]);
  i += kLinkagesPerTuple;
  ID t4 = log->MakeTuple(t3, false, kLinkagesPerTuple, &ids[i]);
  i += kLinkagesPerTuple;
  log->MakeTuple(t4, false, kLinkagesPerTuple, &ids[i]);

  Record v1{MakeRawRecord(t1, log)};
  Record v2{MakeEphemeralRecord(t2, log)};
  Record v3{CopyToRefCountedRecord(t3, log)};
  Record v4;
  v4 = v3;
  v3 = CopyToRefCountedRecord(t4, log);

  EXPECT_EQ(v1.subject(), ids[LinkageOffset(0, kSubject)]);
  EXPECT_EQ(v2.object(), ids[LinkageOffset(1, kObject)]);
  EXPECT_EQ(v3.predicate(), ids[LinkageOffset(3, kPredicate)]);
  EXPECT_EQ(v4.subgraphs()[0], ids[LinkageOffset(2, kSubgraph)]);

  EXPECT_TRUE(MakeRefCountedRecord(v2).IsRefCounted());
  Record r3(MakeRefCountedRecord(v3));
  EXPECT_EQ(r3.get(), v3.get());
  Record r2(MakeRefCountedRecord(v2));
  EXPECT_NE(r2.get(), v2.get());
  ::free(log);
}
