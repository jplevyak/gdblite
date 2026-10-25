#include <memory>
#include <string>
using ::std::string;
#include <utility>
#include <vector>
using ::std::vector;

#include "file/base/path.h"
#include "common/common_types.h"
#include "core/tuplesequence.h"
#include "core/tuplestore.h"
#include "local.h"
#include "idset.h"
#include "resource_manager.h"
#include "util/rand.h"
#include "testing/base/public/googletest.h"
#include "gtest/gtest.h"
#include "util/btree/btree_set.h"
#include "util/task/status.h"

using std::unique_ptr;

TEST(LocalPGI, Snapshot) {
  auto db_path = file::JoinPath(
      FLAGS_test_srcdir,
      "google3/testing/testdata/ex_woody_allen_data.db.trio");
  auto graph_path = file::JoinPath(FLAGS_test_tmpdir, "/local_pgi_test_graph");
  PhysicalGraphInterface::Delete(graph_path);
  auto db = TupleStore::Load(db_path, graph_path);
  ASSERT_TRUE(db);
  string pre_tuples;
  db->CopyTuples()->ToProtoString(&pre_tuples, false);
  BlockingCallback done;
  db->pgi()->WriteSnapshot(FLAGS_test_tmpdir, &done);
  db.reset(nullptr);  // Delete the old one before creating the new one.
  db = TupleStore::OpenFromSnapshot(graph_path, FLAGS_test_tmpdir);
  ASSERT_TRUE(db);
  string post_tuples;
  db->CopyTuples()->ToProtoString(&post_tuples, false);
  EXPECT_EQ(pre_tuples, post_tuples);
}

TEST(LocalPGI, TransientSnapshot) {
  auto db_path = file::JoinPath(
      FLAGS_test_srcdir,
      "google3/testing/testdata/ex_woody_allen_data.db.trio");
  auto db = TupleStore::Load(db_path, "");
  ASSERT_TRUE(db);
  string pre_tuples;
  db->CopyTuples()->ToProtoString(&pre_tuples, false);
  BlockingCallback done;
  db->pgi()->WriteSnapshot(FLAGS_test_tmpdir, &done);
  db.reset(nullptr);  // Delete the old one before creating the new one.
  db = TupleStore::OpenFromSnapshot("", FLAGS_test_tmpdir);
  ASSERT_TRUE(db);
  string post_tuples;
  db->CopyTuples()->ToProtoString(&post_tuples, false);
  EXPECT_EQ(pre_tuples, post_tuples);
}
