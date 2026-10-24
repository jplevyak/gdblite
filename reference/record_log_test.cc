#include "record_log.h"

#include <map>
using ::std::map;
#include <vector>
using ::std::vector;

#include "base/logging.h"
#include "util/rand.h"
#include "testing/base/public/benchmark.h"
#include "gtest/gtest.h"
#include "util/random/mt_random.h"

TEST(RecordLogTest, RecordLogRandom) {
  int n = 100000;
  auto r = GetRandomSkewed(n + 16);
  int s = kMaxVarInt5Size * n;
  char* buffer = new char[s];
  vector<uint64> out;
  out.resize(r.size());
  MTRandom rng(0);
  auto log = RecordLog::Make(buffer);
  auto e = GetRandomBool(n);
  uint64 id = 1;
  map<int, int> pos2size;
  int last = 0;
  for (; last < n;) {
    int x = 1 + rng.UnbiasedUniform(15);
    pos2size[last] = x;
    id = log->MakeTuple(id, e[last], x, &r[last]);
    last += x;
  }
  bool extra_bit = false;
  id = 1;
  for (int i = 0; i < n;) {
    CHECK(pos2size.count(i));
    CHECK_EQ(log->TupleSize(id), pos2size[i]);
    id = log->DecodeTuple(id, pos2size[i], &extra_bit, &out[i]);
    CHECK_EQ(extra_bit, e[i]);
    for (int j = 0; j < pos2size[i]; j++) {
      CHECK_EQ(out[i + j], r[i + j]);
    }
    i += pos2size[i];
  }
  for (int i = 0; i < last; i++) EXPECT_EQ(r[i], out[i]);
  delete[] buffer;
}

TEST(RecordLogTest, RecordLogCompressDecompressVersion1) {
  int n = 100000;
  auto random_ids = GetRandomUniform(1, n - 1, n + 16);
  int s = kMaxVarInt5Size * n;
  char* buffer = new char[s];
  vector<uint64> out;
  out.resize(random_ids.size());
  MTRandom rng(0);
  auto log = RecordLog::Make(buffer, n);
  auto extra_bits = GetRandomBool(n);
  uint64 id = n;
  map<int, int> pos2size;
  int last = 0;
  for (; last < n;) {
    int x = 1 + rng.UnbiasedUniform(10) + 5;
    pos2size[last] = x;
    id = log->MakeTuple(id, extra_bits[last], x, &random_ids[last]);
    last += x;
  }
  string compressed = CompressRecordLog(RecordLog::COMPRESSION_V1, log, id);
  uint64 next_id = 0;
  RecordLog* decompressed = DecompressRecordLog(compressed, 0, &next_id);
  CHECK_EQ(next_id, id);
  bool extra_bit = false;
  id = n;
  for (int i = 0; i < n;) {
    CHECK(pos2size.count(i));
    CHECK_EQ(decompressed->TupleSize(id), pos2size[i]);
    id = decompressed->DecodeTuple(id, pos2size[i], &extra_bit, &out[i]);
    CHECK_EQ(extra_bit, extra_bits[i]);
    for (int j = 0; j < pos2size[i]; j++) {
      CHECK_EQ(out[i + j], random_ids[i + j]);
    }
    i += pos2size[i];
  }
  for (int i = 0; i < last; i++) EXPECT_EQ(random_ids[i], out[i]);
  ::free(decompressed);
  delete[] buffer;
}

static void BM_RecordLogEncode(int iters) {
  StopBenchmarkTiming();
  int64 n = iters;
  auto r = GetRandomSkewed(n + 16);
  int s = kMaxVarInt5Size * ((n + 4) / 5);
  char* buffer = new char[s];
  vector<uint64> out;
  out.resize(r.size());
  MTRandom rng(0);
  auto log = RecordLog::Make(buffer);
  vector<int> len(n);
  for (int i = 0; i < n; i++) len[i] = 1 + rng.UnbiasedUniform(15);
  uint64 id = log->start_id();
  int64 last = 0;
  int64 items = 0;

  StartBenchmarkTiming();
  for (; last < n;) {
    int x = len[last];
    id = log->MakeTuple(id, false, x, &r[last]);
    last += x;
    items++;
  }
  StopBenchmarkTiming();
  SetBenchmarkItemsProcessed(items);
  SetBenchmarkBytesProcessed(last * sizeof(uint64));

  delete[] buffer;
}
BENCHMARK(BM_RecordLogEncode);

static void BM_RecordLogDecode(int iters) {
  StopBenchmarkTiming();
  int64 n = iters;
  auto r = GetRandomSkewed(n + 16);
  int s = kMaxVarInt5Size * ((n + 4) / 5);
  char* buffer = new char[s];
  vector<uint64> out;
  out.resize(r.size());
  MTRandom rng(0);
  auto log = RecordLog::Make(buffer);
  vector<int> len(n);
  for (int i = 0; i < n; i++) len[i] = 1 + rng.UnbiasedUniform(15);
  map<int, int> pos2size;
  uint64 id = log->start_id();
  int64 last = 0;
  int64 items = 0;

  for (; last < n;) {
    int x = len[last];
    pos2size[last] = x;
    id = log->MakeTuple(id, false, x, &r[last]);
    last += x;
    items++;
  }

  bool extra_bit = false;
  id = log->start_id();

  StartBenchmarkTiming();
  for (int i = 0; i < n;) {
    id = log->DecodeTuple(id, pos2size[i], &extra_bit, &out[i]);
    i += pos2size[i];
  }
  StopBenchmarkTiming();
  SetBenchmarkItemsProcessed(items);
  SetBenchmarkBytesProcessed(last * sizeof(uint64));

  delete[] buffer;
}
BENCHMARK(BM_RecordLogDecode);

static RecordLog* PreparseForCompressionTest(int iters, int* ids,
                                             uint64* next_id) {
  int n = iters * 100;
  auto r = GetRandomUniform(1, n - 1, n + 16);
  int s = kMaxVarInt5Size * n;
  char* buffer = static_cast<char*>(::malloc(s));
  vector<uint64> out;
  out.resize(r.size());
  MTRandom rng(0);
  auto log = RecordLog::Make(buffer, n);
  auto e = GetRandomBool(n);
  uint64 id = n;
  map<int, int> pos2size;
  int last = 0;
  for (; last < n;) {
    int x = 1 + rng.UnbiasedUniform(10) + 5;
    pos2size[last] = x;
    id = log->MakeTuple(id, e[last], x, &r[last]);
    last += x;
  }
  *ids = last;
  *next_id = id;
  return log;
}

static void BM_RecordLogCompressVersion1(int iters) {
  StopBenchmarkTiming();
  int ids;
  uint64 next_id;
  auto log = PreparseForCompressionTest(iters, &ids, &next_id);

  StartBenchmarkTiming();
  string compressed =
      CompressRecordLog(RecordLog::COMPRESSION_V1, log, next_id);
  StopBenchmarkTiming();
  SetBenchmarkItemsProcessed(ids);
  SetBenchmarkBytesProcessed(ids * sizeof(uint64));

  ::free(log);
}
BENCHMARK(BM_RecordLogCompressVersion1);

static void BM_RecordLogDecompressVersion1(int iters) {
  StopBenchmarkTiming();
  int ids;
  uint64 id;
  auto log = PreparseForCompressionTest(iters, &ids, &id);
  string compressed = CompressRecordLog(RecordLog::COMPRESSION_V1, log, id);
  uint64 next_id = 0;

  StartBenchmarkTiming();
  RecordLog* decompressed = DecompressRecordLog(compressed, 0, &next_id);
  StopBenchmarkTiming();
  SetBenchmarkItemsProcessed(ids);
  SetBenchmarkBytesProcessed(ids * sizeof(uint64));

  ::free(decompressed);
  ::free(log);
}
BENCHMARK(BM_RecordLogDecompressVersion1);
