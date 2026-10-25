#include "tupleindex.h"

#include <memory>

#include "tuplestore.h"
#include "pgi/pgi.h"
#include "strings/strcat.h"
#include "gtest/gtest.h"

using std::unique_ptr;

class BlytheTupleIndexTest : public ::testing::Test {
 protected:
  BlytheTupleIndexTest() {}
  virtual ~BlytheTupleIndexTest() {}

  virtual void SetUp() {}
  virtual void TearDown() {}

  // Objects declared here can be used by all TEST_Fs in the test case
};

TEST_F(BlytheTupleIndexTest, TestWriteDuplicateValue) {
  // This test verifies that ValueInternPool fails in UpdateSince()
  // if the TupleSequence parameter includes duplicate values.
  unique_ptr<TupleStore> ts(TupleStore::NewTransientEmpty());
  vector<Record> tuples;
  tuples.push_back(Record::Value(1, "foo"));
  tuples.push_back(Record::Value(2, "foo"));
  // Expect this to fail badly.
  ASSERT_DEATH(ts->indexes_.IndexTuples(tuples), "");
}
