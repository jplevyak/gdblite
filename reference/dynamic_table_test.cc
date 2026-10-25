#include "dynamic_table.h"

#include <map>
using ::std::map;
#include <memory>
#include <set>
using ::std::set;
#include <string>
using ::std::string;
#include <utility>

#include "base/logging.h"
#include "file/base/file.h"
#include "file/base/fileutils.h"
#include "file/base/helpers.h"
#include "util/rand.h"
#include "testing/base/public/googletest.h"
#include "gtest/gtest.h"
#include "util/random/mt_random.h"

static const int kBigSize = 10000;
static constexpr float kCorruptionRate = 0.01;
typedef DynamicTable::LookupOp LookupOp;
typedef DynamicTable::InsertOp InsertOp;
typedef map<pair<uint64, uint64>, pair<bool, vector<uint64>>> Records;

TEST(DynamicTableTest, Simple) {
  string prefix = FLAGS_test_tmpdir + "Simple.";
  DynamicTable::Delete(prefix);
  std::unique_ptr<DynamicTable> t(DynamicTable::Open(prefix, true));
  uint64 values[8];
  vector<LookupOp> l{LookupOp(1234, false, values)};
  t->Lookup(&l);
  EXPECT_FALSE(l[0].bit);
  EXPECT_EQ(l[0].n, 0);
  values[0] = 12;
  values[1] = 1313131313;
  values[2] = 14;
  vector<InsertOp> i{InsertOp(1234, false, true, 3, values)};
  t->Insert(i);
  values[0] = 0;
  values[1] = 0;
  values[2] = 0;
  t->Lookup(&l);
  EXPECT_TRUE(l[0].bit);
  EXPECT_EQ(l[0].n, 3);
  EXPECT_EQ(values[0], 12);
  EXPECT_EQ(values[1], 1313131313);
  EXPECT_EQ(values[2], 14);
}

TEST(DynamicTableTest, Big) {
  string prefix = FLAGS_test_tmpdir + "Big.";
  DynamicTable::Delete(prefix);
  std::unique_ptr<DynamicTable> t(DynamicTable::Open(prefix, true));
  MTRandom rng(0);
  Records records;
  for (int i = 0; i < kBigSize; i++) {
    pair<pair<uint64, uint64>, pair<bool, vector<uint64>>> r;
    do {
      r.first.first = RandomSkewed60(&rng);
      if (rng.OneIn(2)) r.first.second = RandomSkewed60(&rng);
    } while (records.count(r.first));  // Do not duplicate insertion.
    r.second.first = rng.OneIn(2);
    int n = rng.Uniform(8) + 1;
    for (int j = 0; j < n; j++) r.second.second.push_back(RandomSkewed60(&rng));
    records.insert(r);
    vector<InsertOp> iop{InsertOp(r.first.first, r.first.second, false,
                                  r.second.first, r.second.second.size(),
                                  &r.second.second[0])};
    t->Insert(iop);
  }
  for (auto& r : records) {
    uint64 values[8];
    vector<LookupOp> lop{
        LookupOp(r.first.first, r.first.second, false, values)};
    t->Lookup(&lop);
    EXPECT_EQ(lop[0].bit, r.second.first);
    EXPECT_EQ(lop[0].n, r.second.second.size());
    for (int j = 0; j < lop[0].n; j++) {
      EXPECT_EQ(values[j], r.second.second[j]);
    }
  }
}

TEST(DynamicTableTest, InsertPrevious) {
  string prefix = FLAGS_test_tmpdir + "InsertPrevious.";
  DynamicTable::Delete(prefix);
  std::unique_ptr<DynamicTable> t(DynamicTable::Open(prefix, true));
  uint64 values[8];
  vector<LookupOp> l{LookupOp(1234, false, values)};
  t->Lookup(&l);
  EXPECT_FALSE(l[0].bit);
  EXPECT_EQ(l[0].n, 0);
  values[0] = 12;
  values[1] = 1313131313;
  vector<InsertOp> i{InsertOp(1234, false, true, 2, values)};
  t->Insert(i);
  values[0] = 13;
  values[1] = 1313131314;
  t->Insert(i);
  values[0] = 0;
  values[1] = 0;
  values[2] = 0;
  values[3] = 0;
  t->Lookup(&l);
  EXPECT_TRUE(l[0].bit);
  EXPECT_EQ(l[0].n, 4);
  EXPECT_EQ(values[0], 12);
  EXPECT_EQ(values[1], 1313131313);
  EXPECT_EQ(values[2], 13);
  EXPECT_EQ(values[3], 1313131314);
}

TEST(DynamicTableTest, Insert) {
  string prefix = FLAGS_test_tmpdir + "Insert.";
  DynamicTable::Delete(prefix);
  std::unique_ptr<DynamicTable> t(DynamicTable::Open(prefix, true));
  uint64 values[8];
  vector<LookupOp> l{LookupOp(1234, false, values)};
  t->Lookup(&l);
  EXPECT_FALSE(l[0].bit);
  EXPECT_EQ(l[0].n, 0);
  values[0] = 12;
  values[1] = 1313131313;
  vector<InsertOp> i{InsertOp(1234, false, true, 2, values)};
  t->Insert(i);
  values[0] = 10;
  values[1] = 1313131310;
  // Key-Values pairs with the same key but different key2 placed in the same
  // bucket.
  vector<InsertOp> i2{InsertOp(1234, 5678, false, true, 2, values)};
  values[0] = 13;
  values[1] = 1313131314;
  t->Insert(i);
  values[0] = 0;
  values[1] = 0;
  values[2] = 0;
  values[3] = 0;
  t->Lookup(&l);
  EXPECT_TRUE(l[0].bit);
  EXPECT_EQ(l[0].n, 4);
  EXPECT_EQ(values[0], 12);
  EXPECT_EQ(values[1], 1313131313);
  EXPECT_EQ(values[2], 13);
  EXPECT_EQ(values[3], 1313131314);
}

TEST(DynamicTableTest, ReplacePrevious) {
  string prefix = FLAGS_test_tmpdir + "ReplacePrevious.";
  DynamicTable::Delete(prefix);
  std::unique_ptr<DynamicTable> t(DynamicTable::Open(prefix, true));
  uint64 values[8];
  vector<LookupOp> l{LookupOp(1234, false, values)};
  t->Lookup(&l);
  EXPECT_FALSE(l[0].bit);
  EXPECT_EQ(l[0].n, 0);
  values[0] = 12;
  values[1] = 1313131313;
  vector<InsertOp> i{InsertOp(1234, false, true, 2, values)};
  t->Insert(i);
  values[0] = 13;
  values[1] = 1313131314;
  vector<InsertOp> i2{InsertOp(1234, true, true, 2, values)};
  t->Insert(i2);
  values[0] = 0;
  values[1] = 0;
  t->Lookup(&l);
  EXPECT_TRUE(l[0].bit);
  EXPECT_EQ(l[0].n, 2);
  EXPECT_EQ(values[0], 13);
  EXPECT_EQ(values[1], 1313131314);
}

TEST(DynamicTableTest, Replace) {
  string prefix = FLAGS_test_tmpdir + "Replace.";
  DynamicTable::Delete(prefix);
  std::unique_ptr<DynamicTable> t(DynamicTable::Open(prefix, true));
  uint64 values[8];
  vector<LookupOp> l{LookupOp(1234, false, values)};
  t->Lookup(&l);
  EXPECT_FALSE(l[0].bit);
  EXPECT_EQ(l[0].n, 0);
  values[0] = 12;
  values[1] = 1313131313;
  vector<InsertOp> i{InsertOp(1234, false, true, 2, values)};
  t->Insert(i);
  values[0] = 10;
  values[1] = 1313131310;
  // Key-Values pairs with the same key but different key2 placed in the same
  // bucket.
  vector<InsertOp> i2{InsertOp(1234, 5678, false, true, 2, values)};
  values[0] = 13;
  values[1] = 1313131314;
  vector<InsertOp> i3{InsertOp(1234, true, true, 2, values)};
  t->Insert(i3);
  values[0] = 0;
  values[1] = 0;
  t->Lookup(&l);
  EXPECT_TRUE(l[0].bit);
  EXPECT_EQ(l[0].n, 2);
  EXPECT_EQ(values[0], 13);
  EXPECT_EQ(values[1], 1313131314);
}

TEST(DynamicTableTest, DeletePrevious) {
  string prefix = FLAGS_test_tmpdir + "DeletePrevious.";
  DynamicTable::Delete(prefix);
  std::unique_ptr<DynamicTable> t(DynamicTable::Open(prefix, true));
  uint64 values[8];
  vector<LookupOp> l{LookupOp(1234, false, values)};
  t->Lookup(&l);
  EXPECT_FALSE(l[0].bit);
  EXPECT_EQ(l[0].n, 0);
  values[0] = 12;
  values[1] = 1313131313;
  vector<InsertOp> i{InsertOp(1234, false, true, 2, values)};
  t->Insert(i);
  // Delete is replace with n == 0.
  vector<InsertOp> i2{InsertOp(1234, true, true, 0, nullptr)};
  t->Insert(i2);
  t->Lookup(&l);
  EXPECT_FALSE(l[0].bit);
  EXPECT_EQ(l[0].n, 0);
}

TEST(DynamicTableTest, Delete) {
  string prefix = FLAGS_test_tmpdir + "Delete.";
  DynamicTable::Delete(prefix);
  std::unique_ptr<DynamicTable> t(DynamicTable::Open(prefix, true));
  uint64 values[8];
  vector<LookupOp> l{LookupOp(1234, false, values)};
  t->Lookup(&l);
  EXPECT_FALSE(l[0].bit);
  EXPECT_EQ(l[0].n, 0);
  values[0] = 12;
  values[1] = 1313131313;
  vector<InsertOp> i{InsertOp(1234, false, true, 2, values)};
  t->Insert(i);
  values[0] = 10;
  values[1] = 1313131310;
  // Key-Values pairs with the same key but different key2 placed in the same
  // bucket.
  vector<InsertOp> i2{InsertOp(1234, 5678, false, true, 2, values)};
  // Delete is replace with n == 0.
  vector<InsertOp> i3{InsertOp(1234, true, true, 0, nullptr)};
  t->Insert(i3);
  t->Lookup(&l);
  EXPECT_FALSE(l[0].bit);
  EXPECT_EQ(l[0].n, 0);
}

TEST(DynamicTableTest, Corruption) {
  string prefix = FLAGS_test_tmpdir + "Corruption.";
  DynamicTable::Delete(prefix);
  std::unique_ptr<DynamicTable> t(DynamicTable::Open(prefix, true));
  MTRandom rng(0);

  vector<InsertOp> ops1(kBigSize / 2);
  vector<InsertOp> ops2(kBigSize / 2);
  vector<uint64> ops_values(kBigSize * 8);
  int i_ops_values = 0;
  set<pair<uint64, uint64>> ops_done;
  for (int i = 0; i < kBigSize; i++) {
    auto& r = (i < kBigSize / 2) ? ops1[i] : ops2[i - (kBigSize / 2)];
    do {
      r.key = RandomSkewed60(&rng);
      r.key2 = 0;
      if (rng.OneIn(2)) r.key2 = RandomSkewed60(&rng);
    } while (ops_done.count(make_pair(r.key, r.key2)));
    r.replace = false;
    r.bit = rng.OneIn(2);
    r.n = rng.Uniform(8) + 1;
    r.values = &ops_values[i_ops_values];
    for (int j = 0; j < r.n; j++)
      ops_values[i_ops_values++] = RandomSkewed60(&rng);
    ops_done.insert(make_pair(r.key, r.key2));
  }
  t->Insert(ops1);  // Insert the first half.

  vector<InsertOp> corrupt_ops(kBigSize);
  vector<uint64> corrupt_values(kBigSize * 8);
  int i_corrupt_values = 0;
  set<pair<uint64, uint64>> corrupt_done;
  for (int i = 0; i < kBigSize; i++) {
    auto& r = corrupt_ops[i];
    do {
      r.key = RandomSkewed60(&rng);
      r.key2 = 0;
      if (rng.OneIn(2)) r.key2 = RandomSkewed60(&rng);
    } while (corrupt_done.count(make_pair(r.key, r.key2)) ||
             ops_done.count(make_pair(r.key, r.key2)));
    r.replace = false;
    r.bit = rng.OneIn(2);
    r.n = rng.Uniform(8) + 1;
    r.values = &corrupt_values[i_corrupt_values];
    for (int j = 0; j < r.n; j++)
      corrupt_values[i_corrupt_values++] = RandomSkewed60(&rng);
    corrupt_done.insert(make_pair(r.key, r.key2));
  }
  t->InsertCorrupt(corrupt_ops, &rng, kCorruptionRate);

  t->Insert(ops2);  // Insert the other half.

  for (int i = 0; i < kBigSize; i++) {
    auto& r = (i < kBigSize / 2) ? ops1[i] : ops2[i - (kBigSize / 2)];
    uint64 values[8];
    vector<LookupOp> lop{LookupOp(r.key, r.key2, false, values)};
    t->Lookup(&lop);
    CHECK_EQ(lop[0].n, r.n);
    CHECK_EQ(lop[0].bit, r.bit);
    EXPECT_EQ(lop[0].bit, r.bit);
    EXPECT_EQ(lop[0].n, r.n);
    for (int j = 0; j < lop[0].n; j++) {
      EXPECT_EQ(values[j], r.values[j]);
    }
  }
}

TEST(DynamicTableTest, Snaphot) {
  string prefix = FLAGS_test_tmpdir + "PreSnapshot.";
  DynamicTable::Delete(prefix);
  std::unique_ptr<DynamicTable> t(DynamicTable::Open(prefix, true));
  MTRandom rng(0);
  Records records;
  for (int i = 0; i < kBigSize; i++) {
    pair<pair<uint64, uint64>, pair<bool, vector<uint64>>> r;
    do {
      r.first.first = RandomSkewed60(&rng);
      if (rng.OneIn(2)) r.first.second = RandomSkewed60(&rng);
    } while (records.count(r.first));  // Do not duplicate insertion.
    r.second.first = rng.OneIn(2);
    int n = rng.Uniform(8) + 1;
    for (int j = 0; j < n; j++) r.second.second.push_back(RandomSkewed60(&rng));
    records.insert(r);
    vector<InsertOp> iop{InsertOp(r.first.first, r.first.second, false,
                                  r.second.first, r.second.second.size(),
                                  &r.second.second[0])};
    t->Insert(iop);
  }
  {
    FileCloser file(File::Open(FLAGS_test_tmpdir + "Snapshot", "w"));
    t->Snapshot([&file](StringPiece s) -> void {
      int64 ss = s.size();
      file->Write(&ss, sizeof(ss));
      file->Write(s.data(), ss);
    });
  }
  t.reset(nullptr);
  prefix = FLAGS_test_tmpdir + "PostSnapshot.";
  {
    FileCloser file(File::Open(FLAGS_test_tmpdir + "Snapshot", "r"));
    string tmp;
    t.reset(
        DynamicTable::OpenFromSnapshot(prefix, [&file, &tmp]() -> StringPiece {
          int64 ss;
          file->Read(&ss, sizeof(ss));
          tmp.resize(ss);
          file->Read(&tmp[0], ss);
          return tmp;
        }));
  }
  for (auto& r : records) {
    uint64 values[8];
    vector<LookupOp> lop{
        LookupOp(r.first.first, r.first.second, false, values)};
    t->Lookup(&lop);
    CHECK_EQ(lop[0].bit, r.second.first);
    CHECK_EQ(lop[0].n, r.second.second.size());
    for (int j = 0; j < lop[0].n; j++) {
      CHECK_EQ(values[j], r.second.second[j]);
    }
  }
}
