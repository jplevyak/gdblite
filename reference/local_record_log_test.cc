#include <map>
using ::std::map;
#include <memory>

#include "base/logging.h"
#include "common/common_types.h"
#include "common/record.h"
#include "pgi/pgi.h"
#include "pgi/local_pgi.h"
#include "util/rand.h"
#include "util/record_log.h"
#include "gtest/gtest.h"
#include "util/random/mt_random.h"
#include "util/task/status.h"


const int kSimpleSize = 10000;
const int kLinkagesPerTuple = 10;

TEST(LocalRecordLog, Simple) {
  string prefix = FLAGS_test_tmpdir + "Simple.";
  DeleteLocalRecordLog(prefix);
  CHECK_OK(CreateLocalRecordLog(prefix, 1000));
  std::unique_ptr<PgiRecordLog> l;
  CHECK_OK(OpenLocalRecordLog(prefix, true, &l));

  // Write 1000 tuples with random linkages.
  vector<ID> ids = GetRandomSkewed(kSimpleSize * kLinkagesPerTuple);
  ScopedPgiBufferPtr<Record> records(kSimpleSize);
  auto log = RecordLog::Make(
      ::malloc(kSimpleSize * RecordLog::MaxMakeTupleSize(kLinkagesPerTuple)));
  int id = 1;
  for (int i = 0; i < kSimpleSize; i++) {
    records[i].id_ = id;
    records[i].reset(MakeRawRecordLogRef(log->GetEntry(id)));
    id = log->MakeTuple(id, false, kLinkagesPerTuple,
                        &ids[i * kLinkagesPerTuple]);
  }
  l->Append(nullptr, records, nullptr);

  // Read back 4 and check that we got the right ones.
  ScopedPgiBufferPtr<Record> read_records(4);
  vector<ID> read_set = {13, 2145, 3, 9765};
  int i = 0;
  for (auto r : read_set) read_records[i++].id_= records[r].id();
  l->Get(read_records, nullptr);
  i = 0;
  for (auto r : read_set) {
    EXPECT_EQ(read_records[i].LinkSize(), 10);
    for (int j = 0; j < 5; j++) {
      EXPECT_EQ(read_records[i].linkage(j), ids[r * kLinkagesPerTuple + j]);
    }
    vector<uint64> sg = read_records[i].subgraphs();
    for (int j = 0; j < 5; j++) {
      EXPECT_EQ(sg.size(), 5);
      EXPECT_EQ(sg[j], ids[r * kLinkagesPerTuple + 5 + j]);
    }
    i++;
  }

  // Read back a range which is not complete, decode and check.
  vector<PgiBufferPtr<char>> buffers;
  vector<bool> buffer_is_ephemeral;
  l->GetRange(records[13].id(), records[2145].id() + 1, &buffers,
              &buffer_is_ephemeral, nullptr);
  vector<Record> byte_records;
  ID start = records[13].id();
  for (auto &b : buffers)
    start = PgiRecordLog::DecodeRange(start, b, false, &byte_records);
  EXPECT_EQ(start, records[2145].id());

  ::free(log);
}
