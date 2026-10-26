#include <algorithm>
#include <deque>
using ::std::deque;
#include <utility>

#include "base/callback.h"
#include "gflags/gflags.h"
#include "base/logging.h"
#include "file/base/path.h"
#include "core/bootstrap.h"
#include "core/tuplesequence.h"
#include "core/tuplestore.h"
#include "testing/commands.h"
#include "util/set.h"
#include "strings/cord.h"
#include "strings/numbers.h"
#include "testing/base/public/benchmark.h"
#include "testing/base/public/googletest.h"
#include "gtest/gtest.h"
#include "util/btree/btree_set.h"
#include "util/gtl/map_util.h"

using std::unique_ptr;

typedef b_bootstrap_constants bc;

class TupleStoreTest : public ::testing::Test {
 protected:
  TupleStoreTest() {}

  virtual void SetUp();

  unique_ptr<TupleSequence> CreateRestorableTupleSequence(uint64 size);

  void AddTriggerProperty(ID subject, TupleSequence* tuples);

  unique_ptr<TupleSequence> MakeNodeAndPropertyWithKeys(string* node_pk,
                                                        string* property_pk);

  static ID AppendNode(TupleStore* ts) { return ts->AppendNode(); }

  static ID AppendValue(TupleStore* ts, StringPiece value) {
    return ts->AppendValue(value);
  }

  static ID AppendLink(TupleStore* ts, ID author, ID subgraph, ID subject,
                       ID predicate, ID object, LinkState state) {
    ID assigned_id =
        ts->AppendLink(author, subgraph, subject, predicate, object, state);
    ts->CommitAndIndex();
    return assigned_id;
  }

  static void ToProtoString(const TupleStore& ts, string* output,
                            bool text_format) {
    ts.ToProtoString(output, text_format);
  }

  // Example sources.
  unique_ptr<ExampleSource> bootstrap_trio_example_source_;
  unique_ptr<ExampleSource> bootstrap_db_example_source_;
};

void TupleStoreTest::SetUp() {
  static const char* bootstrap_manifest = "all_bootstrap_examples.txt";
  static const char* trio_extension = ".trio";
  // static const char* db_extension = ".db.trio";

  bootstrap_trio_example_source_.reset(new ExampleSource(
      FLAGS_test_srcdir.c_str(), bootstrap_manifest, trio_extension));

  bootstrap_db_example_source_.reset(new ExampleSource(
      FLAGS_test_srcdir.c_str(), bootstrap_manifest, trio_extension));  // ***
}

unique_ptr<TupleSequence> TupleStoreTest::CreateRestorableTupleSequence(
    uint64 size) {
  unique_ptr<TupleStore> db = TupleStore::NewTransient();
  const uint64 bootstrap_size = db->next_id();
  // Pattern is: [Node, Value, Link]
  const uint64 kPatternSize = 3;
  const uint64 pattern_base_id =
      ((bootstrap_size / kPatternSize) + 1) * kPatternSize;

  ID id = 1;
  for (uint64 j = 1; j <= size; ++j) {
    if (j < pattern_base_id) {
      // Pad from bootstrap end to pattern begin.
      id = db->AppendNode();
    } else {
      switch (j % kPatternSize) {
        case 0:
          id = db->AppendNode();
          break;
        case 1:
          id = db->AppendValue(SimpleItoa(j));
          break;
        case 2:
          id = db->AppendLink(0, 0, id - 1, db->BootstrapConstants().b_p_gloss,
                              id, LinkState::kLive);
          break;
        default:
          LOG(FATAL) << "Pattern generator is broken.";
      }
    }
  }

  return db->CopyTuples();
}

void CollectValuesCb(IDSet* out_value_ids, const Record& tuple) {
  if (tuple.IsValue()) {
    out_value_ids->insert(tuple.id());
  }
}

void TupleStoreTest::AddTriggerProperty(ID subject, TupleSequence* tuples) {
  // Add a property to trigger a primary key write - else
  // subject is treated as an unreferenced node and is not written.
  unique_ptr<TupleStore> db = TupleStore::NewTransient();
  ID a_value = tuples->AppendValue("whatever");
  tuples->AppendLink(0, 0, subject, db->BootstrapConstants().b_p_gloss,
                     a_value);
}

TEST_F(TupleStoreTest, TestValueInterning) {
  // Re-assert all values - should yield interned id every time.
  unique_ptr<TupleStore> db = TupleStore::NewTransient();
  IDSet all_values;
  unique_ptr<ForEachRecordCallback> callback(
      NewPermanentCallback(&blythe::CollectValuesCb, &all_values));
  db->ForEachRecord(callback.get());
  for (auto value_id : all_values) {
    const Record value_tuple = db->GetTuple(value_id);
    ID added_id = AppendValue(db.get(), db->GetValue(value_tuple));
    ASSERT_EQ(added_id, value_id);
  }
}

TEST_F(TupleStoreTest, TestBaseExtensionCardinality) {
  // Test cardinality conflict detection across base + extension.

  const int kNumCases = 14;
  const Linkage foo_linkage[kNumCases] = {
      kObject, kObject,  kSubject, kSubject, kObject, kObject,  kObject,
      kObject, kSubject, kSubject, kSubject, kObject, kSubject, kSubject};
  const char* base_trio[kNumCases] = {
      "ex_card00_case01_base", "ex_card00_case02_base", "ex_card00_case03_base",
      "ex_card00_case04_base", "ex_card01_case01_base", "ex_card01_case02_base",
      "ex_card02_case01_base", "ex_card02_case02_base", "ex_card10_case01_base",
      "ex_card10_case02_base", "ex_card11_case01_base", "ex_card11_case02_base",
      "ex_card20_case01_base", "ex_card20_case02_base"};
  const char* extension_trio[kNumCases] = {
      "ex_card00_case01_extension", "ex_card00_case02_extension",
      "ex_card00_case03_extension", "ex_card00_case04_extension",
      "ex_card01_case01_extension", "ex_card01_case02_extension",
      "ex_card02_case01_extension", "ex_card02_case02_extension",
      "ex_card10_case01_extension", "ex_card10_case02_extension",
      "ex_card11_case01_extension", "ex_card11_case02_extension",
      "ex_card20_case01_extension", "ex_card20_case02_extension"};
  const int num_all_props[kNumCases] = {8, 8, 8, 8, 5, 5, 4,
                                        4, 5, 5, 5, 5, 4, 4};
  const int num_live_props[kNumCases] = {0, 0, 0, 0, 1, 1, 2,
                                         2, 1, 1, 1, 1, 2, 2};

  for (int i = 0; i < kNumCases; ++i) {
    unique_ptr<const TupleSequence> base_tuples(
        bootstrap_trio_example_source_->FetchExample(base_trio[i]));
    unique_ptr<const TupleSequence> extension_tuples(
        bootstrap_trio_example_source_->FetchExample(extension_trio[i]));

    unique_ptr<TupleStore> db0 = TupleStore::NewTransient();
    LOG(INFO) << "writing base " << base_trio[i];
    bool ok = db0->Write(*base_tuples, nullptr);
    ASSERT_TRUE(ok);
    LOG(INFO) << "writing extension " << extension_trio[i];
    unique_ptr<TupleStore> db1 = TupleStore::NewTransientExtending(db0.get());
    ok = db1->Write(*extension_tuples, nullptr);
    ASSERT_TRUE(ok);

    ID predicate = db1->FindNodeByPrimaryKey("/prop/p");
    ID foo = db1->FindNodeByPrimaryKey("foo");
    ID qux = db1->FindNodeByPrimaryKey("qux");
    ASSERT_GT(predicate, 0);
    IDSet all_props;
    db1->FindLinksWithIDAtLinkage(kPredicate, predicate, &all_props);
    EXPECT_EQ(num_all_props[i], all_props.size());
    IDSet live_props;
    live_props = db1->RemoveDeadTuples(all_props);
    ASSERT_TRUE(Includes(all_props, live_props));
    EXPECT_EQ(num_live_props[i], live_props.size());
    for (ID link_id : live_props) {
      Record link = db1->GetTuple(link_id);
      ID node_id = link.linkage(foo_linkage[i]);
      ID other_node_id = link.linkage(InverseLinkage(foo_linkage[i]));
      ASSERT_EQ(foo, node_id);
      if (num_live_props[i] == 1) {
        ASSERT_EQ(qux, other_node_id);
      } else if (num_live_props[i] == 2) {
        ID baz = db1->FindNodeByPrimaryKey("baz");
        ASSERT_TRUE((other_node_id == baz) || (other_node_id == qux));
      }
    }
  }
}

// A cardinality stress test. For each cardinality=[1..] put in a
// random number of tuples, then take some out, then put some in, etc,
// etc. The results should be the same as the obvious "count how many
// tuples are alive in here and compare that to the cardinality".
TEST_F(TupleStoreTest, CardinalityStressTest) {
  // Let's try a bunch of combinations of inserting <s,p,o> for
  // possible nodes as the object and mix in some deletion nodes and
  // finally some non-effective deletions (absent_nodes). First
  // element of pair states whether or not to the delete the tuple
  // before inserting. Note that all cases are run independently on
  // different tuplestores.
  ID possible_nodes[100];
  ID absent_nodes[100];

#define DELETE(n) \
  { true, &possible_nodes[n] }
#define DELETE_ABSENT(n) \
  { true, &absent_nodes[n] }
#define INSERT(n) \
  { false, &possible_nodes[n] }
  vector<pair<bool, ID*>> cases[] = {
      // Baby steps.
      {INSERT(4)},
      {INSERT(3), INSERT(4)},
      // Deleting elements that aren't present shouldn't muck with anything.
      {DELETE_ABSENT(0), INSERT(4)},
      {INSERT(3), DELETE_ABSENT(0), INSERT(4)},
      // Regular deletes and inserts.
      {INSERT(3), INSERT(2), INSERT(1), DELETE(2), DELETE(3), INSERT(4)},
      {INSERT(3), INSERT(2), INSERT(1), DELETE(2), INSERT(4)},
      {INSERT(3), INSERT(2), INSERT(1), DELETE(2)},
      // How do absent deletes interact with longer sequences.
      {INSERT(3), INSERT(2), INSERT(1), DELETE_ABSENT(0)},
      {INSERT(3), INSERT(2), INSERT(1), DELETE_ABSENT(0), INSERT(5)},
      {INSERT(3), INSERT(2), DELETE_ABSENT(1), INSERT(1), DELETE_ABSENT(0),
       DELETE_ABSENT(2), INSERT(5)},
      // Let's test sequences where there are multiple real deletes in a row.
      {INSERT(3), INSERT(2), INSERT(1), DELETE(3), DELETE(1), INSERT(5)},
      {INSERT(3), INSERT(2), INSERT(1), DELETE(3), DELETE(1), INSERT(5),
       INSERT(6)},
      // Check that cardinality eviction acts similarly to LRU cache.
      {INSERT(0), INSERT(1), INSERT(2), INSERT(3), INSERT(4), INSERT(5),
       INSERT(6), INSERT(7), INSERT(8), INSERT(9), INSERT(10), INSERT(11)},
      // Annoying case with many inserts and delets that we can't
      // bail out of quickly.
      {INSERT(0), INSERT(1), INSERT(2), DELETE(2), INSERT(6), INSERT(7)},
      // Medium case.
      {INSERT(0), INSERT(1), INSERT(2), DELETE(2), INSERT(3), DELETE(3),
       INSERT(6), INSERT(7)},
      // Bigger case.
      {INSERT(0), INSERT(1), INSERT(2), DELETE(2), INSERT(3), DELETE(3),
       INSERT(4), DELETE(4), INSERT(5), DELETE(5), INSERT(6), INSERT(7)}, };
#undef DELETE
#undef DELETE_ABSENT
#undef INSERT

  // We're only futzing with the object cardinality
  for (int cardinality = 1; cardinality < 10; cardinality++) {
    for (int i = 0; i < arraysize(cases); i++) {
      LOG(INFO) << "Dealing with cardinality " << cardinality << " in case "
                << i;

      TupleSequence tuples(19999);
      unique_ptr<TupleStore> db = TupleStore::NewTransient();

      string the_subject_key = "the eternal subject";

      // Test writing simple properties.
      tuples.AppendNode();             // author -- not used -- t0999
      ID t20000 = tuples.AppendNode();  // the eternal subject
      tuples.AppendNode();              // not used -- t20001
      ID t20002 = tuples.AppendNode();  // property metaproperty
      ID t20003 = tuples.AppendValue("/prop/p");
      tuples.AppendLink(0, 0, t20002, bc::b_p_primary_key, t20003);
      ID t20005 = tuples.AppendNode();  // metaproperty subject
      ID t20006 = tuples.AppendNode();  // metaproperty object
      tuples.AppendLink(0, 0, t20002, db->BootstrapConstants().b_p_subject_meta,
                        t20005);
      tuples.AppendLink(0, 0, t20002, db->BootstrapConstants().b_p_object_meta,
                        t20006);
      // Set subject cardinality to the desired cardinality.
      ID t20009 = tuples.AppendValue(std::to_string(cardinality));
      tuples.AppendLink(0, 0, t20005, db->BootstrapConstants().b_p_cardinality,
                        t20009);
      // Set object cardinality.
      ID t20011 = tuples.AppendValue("0");
      tuples.AppendLink(0, 0, t20006, db->BootstrapConstants().b_p_cardinality,
                        t20011);

      // set primary key for subject
      ID val = tuples.AppendValue(the_subject_key);
      tuples.AppendLink(t20000, bc::b_p_primary_key, val);

      // Create nodes that we can play with as objects of our subject
      // predicate relationship.
      for (int i = 0; i < arraysize(possible_nodes); i++) {
        possible_nodes[i] = tuples.AppendNode();
      }
      // The absent nodes are nodes that spurious deletes will point to.
      for (int i = 0; i < arraysize(absent_nodes); i++) {
        absent_nodes[i] = tuples.AppendNode();
      }

      ID current_max_id = tuples.AppendNode();

      // Before we've written the tuples, the subject and predicate are
      // the sparse numbers
      ID the_subject = t20000;
      ID the_predicate = t20002;

      // This will simulate a fixed length buffer that we can push
      // objects onto. We'll push new items onto the back and pop the
      // oldest items off the front. Could use top_n instead of a
      // deque, but I'm not sure how to erase from it.
      deque<ID> live_objects;

      for (auto& pair : cases[i]) {
        bool to_delete = pair.first;
        ID object = *pair.second;

        if (to_delete) {
          // Append a dead link.
          tuples.AppendRecord(Record::Link(++current_max_id, 0, 0, 0,
                                           the_subject, the_predicate, object,
                                           LinkState::kDead));
          auto it = find(live_objects.begin(), live_objects.end(), object);
          if (it != live_objects.end()) {
            live_objects.erase(it);
          }
        } else {
          tuples.AppendLink(the_subject, the_predicate, object);
          current_max_id++;
          live_objects.push_back(object);
          if (live_objects.size() > cardinality) {
            live_objects.erase(live_objects.begin());
          }
        }
      }

      // Now let's write all the tuples and see which ones cardinality killed
      IDToIDMap old_tuple_to_written_tuple;
      bool write_ok = db->Write(tuples, &old_tuple_to_written_tuple);
      ASSERT_TRUE(write_ok);

      // We have to re-find the subject and predicate we care about in
      // the new graph.
      the_subject = db->FindNodeByPrimaryKey(the_subject_key);
      the_predicate = db->FindNodeByPrimaryKey("/prop/p");

      // When we get all the live tuples for this subject predicate
      // combination, it should be equal to the objects in
      // live_objects.
      IDSet only_the_subject;
      only_the_subject.insert(the_subject);
      IDSet out_link_ids;
      db->QueryProperty(only_the_subject, kSubject, the_predicate, 0,
                        &out_link_ids, nullptr);
      out_link_ids = db->RemoveDeadTuples(out_link_ids);
      IDSet objects = db->ProjectLinkageToSet(out_link_ids, kObject);

      // live_objects can't possibly have more than cardinality values.
      CHECK_LE(live_objects.size(), cardinality);
      // Similarly, objects can't have more than cardinality values either.
      CHECK_LE(objects.size(), cardinality);
      CHECK_EQ(objects.size(), live_objects.size());
      for (auto old_object_id : live_objects) {
        EXPECT_TRUE(
            ContainsKey(objects, old_tuple_to_written_tuple[old_object_id]));
      }
    }
  }
}

TEST_F(TupleStoreTest, SpuriousDeadTuplesEffectOnCardinality) {
  // Dead tuples that don't match up to live tuples before them can
  // complicate more naive optimizations for cardinality.

  unique_ptr<const TupleSequence> tuples(
      bootstrap_trio_example_source_->FetchExample("ex_dead_tuple_card"));
  unique_ptr<TupleStore> db = TupleStore::NewTransient();
  IDToIDMap map;
  bool ok = db->Write(*tuples, &map);
  // This does not pass the cardinality check of 1, but the write will succeed.
  EXPECT_TRUE(ok);
  IDSet out_prop_p;
  ID prop_p = db->FindNodeByPrimaryKey("/prop/p");
  ASSERT_NE(prop_p, 0);
  db->FindLinksWithIDAtLinkage(kPredicate, prop_p, &out_prop_p);
  // bar-qux, bar-foo (dead), bar-bar, bar-qux (dead, because of cardinality)
  EXPECT_EQ(out_prop_p.size(), 4);
  IDSet out_prop_p_no_dead = db->RemoveDeadTuples(out_prop_p);
  EXPECT_EQ(out_prop_p_no_dead.size(), 1);  // bar-bar is left
  IDSet objects = db->ProjectLinkageToSet(out_prop_p_no_dead, kObject);
  for (auto id : objects) {
    EXPECT_EQ(id, db->FindNodeByPrimaryKey("bar"));
  }
}

TEST_F(TupleStoreTest, TestWriteSimpleProperty) {
  TupleSequence tuples(999);
  unique_ptr<TupleStore> db = TupleStore::NewTransient();

  // Test writing simple properties.
  ID t0999 = tuples.AppendNode();  // author
  ID t1000 = tuples.AppendNode();
  ID t1001 = tuples.AppendNode();
  ID t1002 = tuples.AppendNode();  // property metaproperty
  ID t1003 = tuples.AppendValue("mp");
  tuples.AppendLink(0, 0, t1002, bc::b_p_primary_key, t1003);
  ID t1005 = tuples.AppendNode();  // metaproperty subject
  ID t1006 = tuples.AppendNode();  // metaproperty object
  tuples.AppendLink(0, 0, t1002, db->BootstrapConstants().b_p_subject_meta,
                    t1005);
  tuples.AppendLink(0, 0, t1002, db->BootstrapConstants().b_p_object_meta,
                    t1006);
  ID t1009 = tuples.AppendValue("0");
  tuples.AppendLink(0, 0, t1005, db->BootstrapConstants().b_p_cardinality,
                    t1009);
  ID t1011 = tuples.AppendValue("1");
  tuples.AppendLink(0, 0, t1006, db->BootstrapConstants().b_p_cardinality,
                    t1011);

  // Write metaproperty.
  tuples.AppendLink(0, 0, t1000, t1002, t1001);
  ID t1014 = tuples.AppendNode();

  // Cardinality conflict requires delete.
  tuples.AppendLink(0, 0, t1014, t1002, t1001);

  // Re-assert the original link.
  tuples.AppendLink(0, 0, t1000, t1002, t1001);
  ID t1017 = tuples.AppendValue("author");
  tuples.AppendLink(0, 0, t0999, bc::b_p_primary_key, t1017);

  bool write_ok = db->Write(tuples, nullptr);
  ASSERT_TRUE(write_ok);
  ID mp_id = db->FindNodeByPrimaryKey("mp");
  ASSERT_GT(mp_id, 0);
  IDSet all_mp;
  db->FindLinksWithIDAtLinkage(kPredicate, mp_id, &all_mp);
  IDSet live_mp;
  live_mp = db->RemoveDeadTuples(all_mp);
  ASSERT_TRUE(Includes(all_mp, live_mp));
  ASSERT_EQ(live_mp.size(), 1);
  // 1. t1000, t1002, t1001
  // 2. t1000, t1002, t1001 DEAD
  // 3. t1014, t1002, t1001
  // 4. t1014, t1002, t1001 DEAD
  // 5. t1000, t1002, t1001
  ASSERT_EQ(all_mp.size(), 5);
}

void CheckIDMap(const IDToIDMap& id_map, const IDSet& expecting_success,
                const IDSet& expecting_error) {
  for (ID id : expecting_success) {
    ASSERT_NE(0, FindOrDie(id_map, id)) << "id: " << id;
  }
  for (ID id : expecting_error) {
    ASSERT_EQ(0, FindOrDie(id_map, id)) << "id: " << id;
  }
}

TEST_F(TupleStoreTest, TestSimpleWritePartialFailure) {
  // Test that Write() fails after encountering errors, but records them all.
  TupleSequence tuples(20000);

  unique_ptr<TupleStore> db = TupleStore::NewTransient();

  // TODO(bendrees):
  // Test Write() with invalid source.
  // Happily, it is difficult to create an invalid source to test against.

  // Test Write() with undefined ids.
  ID n1 = tuples.AppendNode();

  // Test Write() where predicate refers to an incomplete metaproperty.
  ID n2 = tuples.AppendNode();
  ID p1 = tuples.AppendNode();
  ID link_malformed_metaproperty = tuples.AppendLink(n1, p1, n2);

  // Test Write() /b/p/cardinality with bad cardinality values.
  ID cardinality_pred = db->FindNodeByPrimaryKey(bc::b_p_cardinality__pk);
  ID negative_cardinality = tuples.AppendValue("-2");
  ID link_negative_cardinality =
      tuples.AppendLink(n1, cardinality_pred, negative_cardinality);
  ID non_numeric_cardinality = tuples.AppendValue("x");
  ID link_non_numeric_cardinality =
      tuples.AppendLink(n1, cardinality_pred, non_numeric_cardinality);

  // Write() something that should succeed.
  ID n3 = tuples.AppendNode();
  ID v2 = tuples.AppendValue("/test/v2");
  ID link_primary_key = tuples.AppendLink(n3, bc::b_p_primary_key, v2);
  ID n4 = tuples.AppendNode();
  ID rb_pred = db->FindNodeByPrimaryKey(bc::b_p_replaced_by__pk);
  tuples.AppendLink(n3, rb_pred, n4);

  IDSet expecting_success;
  expecting_success.insert(link_primary_key);

  IDSet expecting_failure;
  expecting_failure.insert(link_negative_cardinality);
  expecting_failure.insert(link_non_numeric_cardinality);
  expecting_failure.insert(link_malformed_metaproperty);

  size_t size_before_write = db->size();

  IDToIDMap id_map;
  bool write_ok = db->Write(tuples, &id_map);
  ASSERT_FALSE(write_ok);
  CheckIDMap(id_map, expecting_success, expecting_failure);
  ASSERT_EQ(db->size(), size_before_write);

  // TODO(bendrees):
  // Test Write() attempt where predicate refers to a link or value.
  // Test Write(), alter metaproperty cardinality, Write() some more.
}

typedef vector<pair<Linkage, ID>> Path;

IDToSetMap PropertyChainToMap(const TupleStore& ts, const IDSet& ids,
                              const Path& path) {
  IDToSetMap result;
  for (ID id : ids) {
    IDSet this_level;
    this_level.insert(id);
    for (auto hop : path) {
      Linkage from_linkage = hop.first;
      ID pred = hop.second;
      Linkage to_linkage = InverseLinkage(from_linkage);
      IDSet fan_out = ts.LinksTo(from_linkage, this_level);
      if (!fan_out.size()) {
        this_level.clear();
        break;
      }
      IDSet pred_ids;
      pred_ids.insert(pred);
      IDSet links = Intersection(
          ts.FindLinksWithIDAtLinkage(kPredicate, pred_ids), fan_out);
      if (!links.size()) {
        this_level.clear();
        break;
      }
      this_level = ts.ProjectLinkageToSet(links, to_linkage);
      if (!this_level.size()) {
        break;
      }
    }
    if (this_level.size()) {
      result[id].swap(this_level);
    }
  }
  return result;
}

TEST_F(TupleStoreTest, TestSubgraphUnion) {
  // Test subgraph unions.
  unique_ptr<const TupleSequence> tuples(
      bootstrap_trio_example_source_->FetchExample("ex_subgraph"));
  CHECK_NOTNULL(tuples);

  unique_ptr<TupleStore> db = TupleStore::NewTransient();
  bool write_ok = db->Write(*tuples, nullptr);
  ASSERT_TRUE(write_ok);

  IDSet su_ids;
  db->FindLinksWithIDAtLinkage(
      kPredicate, db->BootstrapConstants().b_p_subgraph_union, &su_ids);
  // 2 from the the bootstrap, 10 from the example.
  CHECK_EQ(12, su_ids.size());

  ID source = db->FindNodeByPrimaryKey("source");
  ID source_a = db->FindNodeByPrimaryKey("source_a");
  ID source_b = db->FindNodeByPrimaryKey("source_b");
  ID source_c = db->FindNodeByPrimaryKey("source_c");

  IDSet all_sources;
  all_sources.insert(source_a);
  all_sources.insert(source_b);
  all_sources.insert(source_c);

  Path path;
  // One hop from sources to subgraphs.
  path.push_back(pair<Linkage, ID>(kObject, source));
  // One hop From subgraphs to subgraph unions.
  path.push_back(
      pair<Linkage, ID>(kObject, db->BootstrapConstants().b_p_subgraph_union));
  IDToSetMap source_map = PropertyChainToMap(*db, all_sources, path);
  CHECK_EQ(source_map.size(), 3);

  // Just one instance of s1 U s2 u s3
  IDSet abc_sus;
  for (auto it = source_map.begin(); it != source_map.end(); ++it) {
    if (it == source_map.begin()) {
      abc_sus = it->second;
    } else {
      abc_sus = Intersection(abc_sus, it->second);
    }
  }
  CHECK_EQ(1, abc_sus.size());

  // Just one link with that subgraph
  ID abc_su = *abc_sus.begin();
  IDSet links_with_subgraph_abc;
  db->FindLinksWithIDAtLinkage(kSubgraph, abc_su, &links_with_subgraph_abc);
  links_with_subgraph_abc = db->RemoveDeadTuples(links_with_subgraph_abc);
  CHECK_EQ(1, links_with_subgraph_abc.size());

  ID s3_union = db->FindNodeByPrimaryKey("s3_union");
  IDSet s3_links;
  db->FindLinksWithIDAtLinkage(kSubgraph, s3_union, &s3_links);
  s3_links = db->RemoveDeadTuples(s3_links);
  CHECK_EQ(1, s3_links.size());
}

TEST_F(TupleStoreTest, TestSubgraphErrors) {
  // Test that various types of subgraph errors are handled.
  TupleSequence tuples(20000);

  unique_ptr<TupleStore> db = TupleStore::NewTransient();

  ID n1 = tuples.AppendNode();
  ID v1 = tuples.AppendValue("/test/v1");

  // Undefined author.
  ID link_undefined_author =
      tuples.AppendLink(259, 0, n1, bc::b_p_primary_key, v1);

  // Undefined subgraph.
  ID link_undefined_subgraph =
      tuples.AppendLink(0, 260, n1, bc::b_p_primary_key, v1);

  ID link_primary_key = tuples.AppendLink(0, 0, n1, bc::b_p_primary_key, v1);
  ID n2 = tuples.AppendNode();
  ID v2 = tuples.AppendValue("blah blah");

  // Subgraph is a Value.
  ID link_subgraph_is_value =
      tuples.AppendLink(0, v1, n2, db->BootstrapConstants().b_p_gloss, v2);

  // Subgraph is a Link.
  ID link_subgraph_is_link = tuples.AppendLink(
      0, link_primary_key, n2, db->BootstrapConstants().b_p_gloss, v2);

  // Subgraph is an empty union.
  ID link_subgraph_is_empty_union =
      tuples.AppendLink(0, n1, n2, db->BootstrapConstants().b_p_gloss, v2);

  // Subgraph is a non-empty union.
  ID link_subgraph_union_spoke = tuples.AppendLink(
      0, n2, n2, db->BootstrapConstants().b_p_subgraph_union, n1);
  ID link_gloss =
      tuples.AppendLink(0, n2, n2, db->BootstrapConstants().b_p_gloss, v2);

  // Subgraph is disallowed for primary key links.
  ID n3 = tuples.AppendNode();
  ID v3 = tuples.AppendValue("blah blah blah");
  ID link_primary_key_with_subgraph =
      tuples.AppendLink(0, n2, n3, bc::b_p_primary_key, v3);

  size_t size_before_write = db->size();

  IDSet expecting_success;
  expecting_success.insert(link_primary_key);
  expecting_success.insert(link_subgraph_union_spoke);
  expecting_success.insert(link_gloss);

  IDSet expecting_failure;
  expecting_failure.insert(link_undefined_author);
  expecting_failure.insert(link_undefined_subgraph);
  expecting_failure.insert(link_subgraph_is_value);
  expecting_failure.insert(link_subgraph_is_link);
  expecting_failure.insert(link_subgraph_is_empty_union);
  expecting_failure.insert(link_primary_key_with_subgraph);

  IDToIDMap id_map;
  bool write_ok = db->Write(tuples, &id_map);
  ASSERT_FALSE(write_ok);
  CheckIDMap(id_map, expecting_success, expecting_failure);
  ASSERT_EQ(db->size(), size_before_write);
}

TEST_F(TupleStoreTest, TestImmutableCompoundCardinalityOverflow) {
  // Test handling of 'extra' spokes in immutable compounds.
  unique_ptr<const TupleSequence> tuples(
      bootstrap_trio_example_source_->FetchExample(
          "ex_compound_card_overflow_i_case01"));
  unique_ptr<TupleStore> db = TupleStore::NewTransient();

  // Write() should fail the whole thing.
  ID next_id = db->next_id();
  bool ok = db->Write(*tuples, nullptr);
  ASSERT_FALSE(ok);
  ASSERT_EQ(next_id, db->next_id());

  // WriteExtension() should produce the subset of all valid writes.
  unique_ptr<TupleStore> ext(TupleStore::NewTransientExtending(db.get()));
  ok = ext->WriteExtension(*tuples, nullptr);
  ASSERT_FALSE(ok);
  ASSERT_NE(next_id, ext->next_id());

  ID lat_pred = ext->FindNodeByPrimaryKey("/prop/lat");
  ASSERT_GT(lat_pred, 0);
  ID long_pred = ext->FindNodeByPrimaryKey("/prop/long");
  ASSERT_GT(long_pred, 0);
  ID lat_val = ext->GetValueID("38");
  ASSERT_GT(lat_val, 0);

  IDSet lat_props;
  ext->FindLinksWithIDAtLinkage(kPredicate, lat_pred, &lat_props);
  EXPECT_EQ(1, lat_props.size());
  IDSet long_props;
  ext->FindLinksWithIDAtLinkage(kPredicate, long_pred, &long_props);
  EXPECT_EQ(0, long_props.size());

  IDSet latitudes = ext->ProjectLinkageToSet(lat_props, kObject);
  EXPECT_EQ(lat_val, *latitudes.begin());
}

TEST_F(TupleStoreTest, TestMutableCompoundCardinalityOverflow) {
  // Test handling of 'extra' spokes in mutable compounds.
  unique_ptr<const TupleSequence> tuples(
      bootstrap_trio_example_source_->FetchExample(
          "ex_compound_card_overflow_m_case01"));
  unique_ptr<TupleStore> db = TupleStore::NewTransient();

  // Write() should fail the whole thing.
  ID next_id = db->next_id();
  bool ok = db->Write(*tuples, nullptr);
  ASSERT_FALSE(ok);
  ASSERT_EQ(next_id, db->next_id());

  // WriteExtension() should produce the subset of all valid spokes.
  unique_ptr<TupleStore> ext(TupleStore::NewTransientExtending(db.get()));
  ok = ext->WriteExtension(*tuples, nullptr);
  ASSERT_FALSE(ok);
  ASSERT_NE(next_id, ext->next_id());

  // TODO(bendrees):
  // Test for version rendezvous as soon as Write() handles it correctly.
  // For now the mutable case is handled just like the immutable case.
  ID film_pred = ext->FindNodeByPrimaryKey("/prop/film");
  ASSERT_GT(film_pred, 0);
  ID actor_pred = ext->FindNodeByPrimaryKey("/prop/actor");
  ASSERT_GT(actor_pred, 0);
  ID character_pred = ext->FindNodeByPrimaryKey("/prop/character");
  ASSERT_GT(character_pred, 0);
  ID woody_harrelson_node = ext->FindNodeByPrimaryKey("woody_harrelson");
  ASSERT_GT(woody_harrelson_node, 0);

  IDSet film_props;
  ext->FindLinksWithIDAtLinkage(kPredicate, film_pred, &film_props);
  EXPECT_EQ(0, film_props.size());
  IDSet actor_props;
  ext->FindLinksWithIDAtLinkage(kPredicate, actor_pred, &actor_props);
  EXPECT_EQ(1, actor_props.size());
  IDSet character_props;
  ext->FindLinksWithIDAtLinkage(kPredicate, character_pred, &character_props);
  EXPECT_EQ(0, character_props.size());

  IDSet actors = ext->ProjectLinkageToSet(actor_props, kObject);
  EXPECT_EQ(woody_harrelson_node, *actors.begin());
}

TEST_F(TupleStoreTest, TestImmutableProperties) {
  // Test immutable properties.
  unique_ptr<const TupleSequence> tuples(
      bootstrap_trio_example_source_->FetchExample("ex_lat_long"));
  CHECK_NOTNULL(tuples);

  unique_ptr<TupleStore> db = TupleStore::NewTransient();
  bool write_ok = db->Write(*tuples, nullptr);
  ASSERT_TRUE(write_ok);

  ID location_id = db->FindNodeByPrimaryKey("location");
  ASSERT_GT(location_id, 0);

  IDSet all_location;
  db->FindLinksWithIDAtLinkage(kPredicate, location_id, &all_location);
  IDSet live_location = db->RemoveDeadTuples(all_location);

  ASSERT_EQ(live_location.size(), 4);
  ASSERT_EQ(all_location.size(), 4);
  ASSERT_TRUE(Includes(all_location, live_location));

  IDSet locations = db->ProjectLinkageToSet(live_location, kObject);
  ASSERT_EQ(locations.size(), 2);

  ID sf4 = db->FindNodeByPrimaryKey("sf4");
  ASSERT_GT(sf4, 0);

  ID sf_coord_id = db->FindNodeByPrimaryKey("sf-coord");
  ASSERT_GT(sf_coord_id, 0);
  ASSERT_TRUE(ContainsKey(locations, sf_coord_id));
}

TEST_F(TupleStoreTest, TestWriteCompound) {
  // Test writing of compound properties.
  //
  // A bit unrealistic as a "real" subgraph would be immutable.
  // Mutability tests more of the compound write code.
  unique_ptr<const TupleSequence> tuples(
      bootstrap_trio_example_source_->FetchExample("ex_compound"));
  CHECK_NOTNULL(tuples);

  unique_ptr<TupleStore> db = TupleStore::NewTransient();
  bool write_ok = db->Write(*tuples, nullptr);
  ASSERT_TRUE(write_ok);

  ID hub_id = db->FindNodeByPrimaryKey("hub_id");
  ID t_process = db->FindNodeByPrimaryKey("t_process");
  ID t_source = db->FindNodeByPrimaryKey("t_source");
  ASSERT_GT(hub_id, 0);
  ASSERT_GT(t_process, 0);
  ASSERT_GT(t_source, 0);

  IDSet all_process_links;
  db->FindLinksWithIDAtLinkage(kPredicate, t_process, &all_process_links);
  IDSet process_links = db->RemoveDeadTuples(all_process_links);
  IDSet process_hubs = db->ProjectLinkageToSet(process_links, kSubject);

  IDSet all_source_links;
  db->FindLinksWithIDAtLinkage(kPredicate, t_source, &all_source_links);
  IDSet source_links = db->RemoveDeadTuples(all_source_links);
  IDSet source_hubs = db->ProjectLinkageToSet(source_links, kSubject);

  IDSet all_hub_id_links;
  db->FindLinksWithIDAtLinkage(kPredicate, hub_id, &all_hub_id_links);
  IDSet hub_id_links = db->RemoveDeadTuples(all_hub_id_links);
  IDSet hub_ids = db->ProjectLinkageToSet(hub_id_links, kSubject);

  EXPECT_EQ(process_hubs.size(), 2);
  EXPECT_EQ(source_hubs.size(), 2);
  EXPECT_EQ(hub_ids.size(), 2);
}

#if 0
// This test is not valid as the IDs will differ with the new
// physical format.  Ideally we should roundtrip through the new format.
TEST_F(TupleStoreTest, TestBootstrapRewrite) {
  // Test that writing A into A yields A, where A is the bootstrap.
  unique_ptr<TupleSequence> bootstrap(new TupleSequence);
  ASSERT_TRUE(bootstrap->InitFromProtoStr(bc::AsProto()));
  CHECK(bootstrap->Valid())
      << ": Bootstrap input is not a valid TupleSequence.";
  unique_ptr<TupleStore> db = TupleStore::NewTransient();
  bool restore_ok = db->Restore(*bootstrap);
  ASSERT_TRUE(restore_ok);
  bool write_ok = db->Write(*bootstrap, nullptr);
  ASSERT_TRUE(write_ok);

  string input;
  bootstrap->ToProtoString(&input, false);
  string output;
  ToProtoString(*db, &output, false);
  ASSERT_EQ(input, output);
}
#endif

TEST_F(TupleStoreTest, TestWriteSimple) {
  // Test writing simple properties.
  unique_ptr<const TupleSequence> tuples(
      bootstrap_db_example_source_->FetchExample("ex_card1"));
  CHECK_NOTNULL(tuples);

  unique_ptr<TupleStore> db = TupleStore::NewTransient();
  bool write_ok = db->Write(*tuples, nullptr);
  ASSERT_TRUE(write_ok);

  ID p1_id = db->FindNodeByPrimaryKey("p1");
  ASSERT_GT(p1_id, 0);

  IDSet all_p1;
  db->FindLinksWithIDAtLinkage(kPredicate, p1_id, &all_p1);
  IDSet live_p1 = db->RemoveDeadTuples(all_p1);
  ASSERT_EQ(live_p1.size(), 1);
  ASSERT_EQ(all_p1.size(), 5);

  ASSERT_TRUE(Includes(all_p1, live_p1));
}

TEST_F(TupleStoreTest, TestDelete) {
  unique_ptr<const TupleSequence> add_link_tuples(
      bootstrap_trio_example_source_->FetchExample("ex_add_link"));
  CHECK_NOTNULL(add_link_tuples);

  unique_ptr<const TupleSequence> dead_tuples(
      bootstrap_trio_example_source_->FetchExample("ex_dead"));
  CHECK_NOTNULL(dead_tuples);

  unique_ptr<TupleStore> db = TupleStore::NewTransient();
  IDToIDMap id_map;
  bool write_ok = db->Write(*add_link_tuples, &id_map);
  ASSERT_TRUE(write_ok);

  ID p1_id = db->FindNodeByPrimaryKey("p1");
  ASSERT_GT(p1_id, 0);

  IDSet all_p1;
  db->FindLinksWithIDAtLinkage(kPredicate, p1_id, &all_p1);
  IDSet live_p1 = db->RemoveDeadTuples(all_p1);

  ASSERT_EQ(live_p1.size(), 1);
  ASSERT_EQ(all_p1.size(), 1);
  ASSERT_TRUE(Includes(all_p1, live_p1));

  write_ok = db->Write(*dead_tuples, nullptr);
  ASSERT_TRUE(write_ok);

  live_p1.clear();
  all_p1.clear();
  db->FindLinksWithIDAtLinkage(kPredicate, p1_id, &all_p1);
  live_p1 = db->RemoveDeadTuples(all_p1);
  ASSERT_EQ(live_p1.size(), 0);
  ASSERT_EQ(all_p1.size(), 4);
}

TEST_F(TupleStoreTest, TestWriteDead) {
  // Test that a link can be written with an initial state of 'dead'.
  unique_ptr<const TupleSequence> dead_tuples(
      bootstrap_trio_example_source_->FetchExample("ex_dead"));
  CHECK_NOTNULL(dead_tuples);

  unique_ptr<TupleStore> db = TupleStore::NewTransient();
  bool write_ok = db->Write(*dead_tuples, nullptr);
  ASSERT_TRUE(write_ok);

  ID p1_id = db->FindNodeByPrimaryKey("p1");
  ASSERT_GT(p1_id, 0);

  IDSet all_p1;
  db->FindLinksWithIDAtLinkage(kPredicate, p1_id, &all_p1);
  IDSet live_p1 = db->RemoveDeadTuples(all_p1);
  ASSERT_EQ(live_p1.size(), 0);
  ASSERT_EQ(all_p1.size(), 3);

  ID foo_id = db->FindNodeByPrimaryKey("foo");
  ASSERT_GT(foo_id, 0);
  ID bar_id = db->FindNodeByPrimaryKey("bar");
  ASSERT_GT(bar_id, 0);
  ID link_id = db->FindLinkBySPO(foo_id, p1_id, bar_id);
  ASSERT_GT(link_id, 0);

  ASSERT_TRUE(db->GetTuple(link_id).dead());
}

TEST_F(TupleStoreTest, TestDeleteAndRecreate) {
  // Test that a deleted link can be recreated.
  unique_ptr<const TupleSequence> add_tuples(
      bootstrap_db_example_source_->FetchExample("ex_add_link_with_when"));
  CHECK_NOTNULL(add_tuples);

  unique_ptr<const TupleSequence> delete_tuples(
      bootstrap_trio_example_source_->FetchExample("ex_dead"));
  CHECK_NOTNULL(delete_tuples);

  unique_ptr<const TupleSequence> add_tuples_again(
      blythe::ChangeWhen(*add_tuples, 0));
  CHECK_NOTNULL(add_tuples_again);

  unique_ptr<const TupleSequence> delete_tuples_again(
      blythe::ChangeWhen(*delete_tuples, 0));
  CHECK_NOTNULL(delete_tuples_again);

  unique_ptr<TupleStore> db = TupleStore::NewTransient();

  // Initial add.
  bool write_ok = db->Write(*add_tuples, nullptr);
  ASSERT_TRUE(write_ok);

  ID foo_id = db->FindNodeByPrimaryKey("foo");
  ASSERT_GT(foo_id, 0);
  ID p1_id = db->FindNodeByPrimaryKey("p1");
  ASSERT_GT(p1_id, 0);
  ID bar_id = db->FindNodeByPrimaryKey("bar");
  ASSERT_GT(bar_id, 0);

  // Link exists.
  ID link_id = db->FindLinkBySPO(foo_id, p1_id, bar_id);
  ASSERT_GT(link_id, 0);
  ASSERT_FALSE(db->GetTuple(link_id).dead());

  // Initial delete.
  write_ok = db->Write(*delete_tuples, nullptr);

  // Link no longer exists.
  link_id = db->FindLinkBySPO(foo_id, p1_id, bar_id);
  ASSERT_GT(link_id, 0);
  ASSERT_TRUE(db->GetTuple(link_id).dead());

  // Running "initial add" again will not recreate the link.
  write_ok = db->Write(*add_tuples, nullptr);
  ASSERT_TRUE(write_ok);
  link_id = db->FindLinkBySPO(foo_id, p1_id, bar_id);
  ASSERT_GT(link_id, 0);
  ASSERT_TRUE(db->GetTuple(link_id).dead());  // ***

  // Running a new add with a new timestamp will recreate the link.
  write_ok = db->Write(*add_tuples_again, nullptr);
  ASSERT_TRUE(write_ok);
  link_id = db->FindLinkBySPO(foo_id, p1_id, bar_id);
  ASSERT_GT(link_id, 0);
  ASSERT_FALSE(db->GetTuple(link_id).dead());

  // Running "initial delete" again will not delete the link.
  write_ok = db->Write(*delete_tuples, nullptr);
  link_id = db->FindLinkBySPO(foo_id, p1_id, bar_id);
  ASSERT_GT(link_id, 0);
  ASSERT_FALSE(db->GetTuple(link_id).dead());

  // Running a new delete with a new timestamp will re-delete the link.
  write_ok = db->Write(*delete_tuples_again, nullptr);
  link_id = db->FindLinkBySPO(foo_id, p1_id, bar_id);
  ASSERT_GT(link_id, 0);
  ASSERT_TRUE(db->GetTuple(link_id).dead());
}

TEST_F(TupleStoreTest, TestWriteUnreferencedNodesAndValues) {
  TupleSequence source_tuples(20000);
  source_tuples.AppendNode();
  source_tuples.AppendValue("SNH1ZvoFklEfFmG6HbjS");
  source_tuples.AppendNode();
  source_tuples.AppendValue("CUPlZR72HK1KPga7k7Hh");
  source_tuples.AppendNode();
  source_tuples.AppendValue("oBJmhF27PoUMFGMELxyr");

  // TODO(bendrees):
  // If the only link is a primary key link, that also counts as "unreferenced".

  unique_ptr<TupleStore> db = TupleStore::NewTransient();

  size_t old_size = db->size();
  bool write_ok = db->Write(source_tuples, nullptr);
  ASSERT_TRUE(write_ok);
  size_t new_size = db->size();
  ASSERT_EQ(old_size, new_size);
}

TEST_F(TupleStoreTest, TestRestoreWithMatchingOverlap) {
  unique_ptr<TupleStore> db = TupleStore::NewTransient();
  unique_ptr<const TupleSequence> tuples(CreateRestorableTupleSequence(512));
  AppendNode(db.get());
  EXPECT_TRUE(db->Restore(*tuples));
}

TEST_F(TupleStoreTest, TestTransitiveReplacing) {
  // Test the transitivity of replaced_by links.
  unique_ptr<const TupleSequence> tuples(
      bootstrap_trio_example_source_->FetchExample("ex_replace"));
  CHECK_NOTNULL(tuples);

  unique_ptr<TupleStore> db = TupleStore::NewTransient();
  bool write_ok = db->Write(*tuples, nullptr);
  ASSERT_TRUE(write_ok);

  ID t_prop = db->FindNodeByPrimaryKey("t_prop");
  ID t_a = db->FindNodeByPrimaryKey("t_A");
  ID t_b = db->FindNodeByPrimaryKey("t_B");
  ID t_c = db->FindNodeByPrimaryKey("t_C");
  ID t_0 = db->GetValueID("0");
  ID t_1 = db->GetValueID("1");
  ID t_2 = db->GetValueID("2");

  // "t_C" should be the only subject of a live "t_prop" link.
  IDSet correct_subjects;
  IDSet current_subjects;
  correct_subjects.insert(t_c);
  db->EntitiesWith(kPredicate, t_prop, kSubject, 0, &current_subjects);
  ASSERT_EQ(current_subjects.size(), 1);
  ASSERT_EQ(current_subjects, correct_subjects);

  // The objects of the "t_prop" links should be "0", "1", and "2".
  IDSet correct_objects;
  IDSet current_objects;
  correct_objects.insert(t_0);
  correct_objects.insert(t_1);
  correct_objects.insert(t_2);
  db->EntitiesWith(kPredicate, t_prop, kObject, 0, &current_objects);
  ASSERT_EQ(current_objects.size(), 3);
  ASSERT_EQ(current_objects, correct_objects);

  // "t_A" and "t_B" should be the subjects of live "replaced_by" links.
  IDSet correct_replaced;
  IDSet current_replaced;
  correct_replaced.insert(t_a);
  correct_replaced.insert(t_b);
  db->EntitiesWith(kPredicate, db->BootstrapConstants().b_p_replaced_by,
                   kSubject, 0, &current_replaced);
  ASSERT_EQ(current_replaced.size(), 2);
  ASSERT_EQ(current_replaced, correct_replaced);

  // Test transitivity again with the replaced_by links
  // already in the TupleStore, not in the argument to Write().
  TupleSequence more_tuples(20000);
  ID t_a_node = more_tuples.AppendNode();
  ID t_a_value = more_tuples.AppendValue("t_A");
  // ID t_a_pk_link =
  more_tuples.AppendLink(t_a_node, bc::b_p_primary_key, t_a_value);
  ID t_b_node = more_tuples.AppendNode();
  ID t_b_value = more_tuples.AppendValue("t_B");
  // ID t_b_pk_link =
  more_tuples.AppendLink(t_b_node, bc::b_p_primary_key, t_b_value);
  ID t_3_value = more_tuples.AppendValue("3");
  // ID t_prop_link =
  more_tuples.AppendLink(t_a_node, t_prop, t_3_value);
  // ID t_prop_link =
  more_tuples.AppendLink(t_b_node, t_prop, t_3_value);
  write_ok = db->Write(more_tuples, nullptr);
  ASSERT_TRUE(write_ok);

  ID t_3 = db->GetValueID("3");

  // "t_C" should still be the only subject of a live "t_prop" link.
  current_subjects.clear();
  db->EntitiesWith(kPredicate, t_prop, kSubject, 0, &current_subjects);
  ASSERT_EQ(current_subjects.size(), 1);
  ASSERT_EQ(current_subjects, correct_subjects);

  // The objects of the "t_prop" links should now be "0", "1", "2", and "3".
  current_objects.clear();
  correct_objects.insert(t_3);
  db->EntitiesWith(kPredicate, t_prop, kObject, 0, &current_objects);
  ASSERT_EQ(current_objects.size(), 4);
  ASSERT_EQ(current_objects, correct_objects);
}

TEST_F(TupleStoreTest, TestWriteReplacedBySubjectAlreadyReplaced) {
  // Test writing a replaced_by link where the subject is already replaced.
  unique_ptr<const TupleSequence> tuples(
      bootstrap_trio_example_source_->FetchExample("ex_replace"));
  CHECK_NOTNULL(tuples);

  unique_ptr<TupleStore> db = TupleStore::NewTransient();
  bool write_ok = db->Write(*tuples, nullptr);
  ASSERT_TRUE(write_ok);

  // At this point the replaced_by mapping is as follows:
  //
  // A -> C
  // B -> C
  // C

  // Assert that A is replaced by D.  Expect success and result mapping:
  //
  // A -> D
  // B -> D
  // C -> D
  // D

  ID t_a = db->FindNodeByPrimaryKey("t_A");
  ID t_b = db->FindNodeByPrimaryKey("t_B");
  ID t_c = db->FindNodeByPrimaryKey("t_C");

  TupleSequence more_tuples(20000);
  ID t_a_node = more_tuples.AppendNode();
  ID t_a_value = more_tuples.AppendValue("t_A");
  // ID t_a_pk_link =
  more_tuples.AppendLink(t_a_node, bc::b_p_primary_key, t_a_value);
  ID t_d_node = more_tuples.AppendNode();
  ID t_d_value = more_tuples.AppendValue("t_D");
  // ID t_d_pk_link =
  more_tuples.AppendLink(t_d_node, bc::b_p_primary_key, t_d_value);
  ID t_rb_link = more_tuples.AppendLink(
      t_a_node, db->BootstrapConstants().b_p_replaced_by, t_d_node);

  IDToIDMap id_map;
  write_ok = db->Write(more_tuples, &id_map);
  ASSERT_TRUE(write_ok);
  ASSERT_TRUE(ContainsKey(id_map, t_rb_link));
  ASSERT_NE(id_map[t_rb_link], 0);
  ID t_d = db->FindNodeByPrimaryKey("t_D");

  // "t_A", "t_B" and "t_C" should be the subjects of live "replaced_by" links.
  IDSet correct_replaced;
  IDSet current_replaced;
  correct_replaced.insert(t_a);
  correct_replaced.insert(t_b);
  correct_replaced.insert(t_c);
  db->EntitiesWith(kPredicate, db->BootstrapConstants().b_p_replaced_by,
                   kSubject, 0, &current_replaced);
  ASSERT_EQ(current_replaced.size(), 3);
  ASSERT_EQ(current_replaced, correct_replaced);

  // The objects of all "replaced_by" links should now be "t_D".
  IDSet correct_objects;
  IDSet current_objects;
  correct_objects.insert(t_d);
  db->EntitiesWith(kPredicate, db->BootstrapConstants().b_p_replaced_by,
                   kObject, 0, &current_objects);
  ASSERT_EQ(current_objects.size(), 1);
  ASSERT_EQ(current_objects, correct_objects);
}

TEST_F(TupleStoreTest, TestWriteReplacedByObjectAlreadyReplaced) {
  // Test writing a replaced_by link where the oject is already replaced.
  unique_ptr<const TupleSequence> tuples(
      bootstrap_trio_example_source_->FetchExample("ex_replace"));
  CHECK_NOTNULL(tuples);

  unique_ptr<TupleStore> db = TupleStore::NewTransient();
  bool write_ok = db->Write(*tuples, nullptr);
  ASSERT_TRUE(write_ok);

  // At this point the replaced_by mapping is as follows:
  //
  // A -> C
  // B -> C
  // C

  // Assert that D is replaced by A.  Expect success and result mapping:
  //
  // A -> C
  // B -> C
  // C
  // D -> C

  ID t_a = db->FindNodeByPrimaryKey("t_A");
  ID t_b = db->FindNodeByPrimaryKey("t_B");
  ID t_c = db->FindNodeByPrimaryKey("t_C");

  TupleSequence more_tuples(20000);
  ID t_a_node = more_tuples.AppendNode();
  ID t_a_value = more_tuples.AppendValue("t_A");
  // ID t_a_pk_link =
  more_tuples.AppendLink(t_a_node, bc::b_p_primary_key, t_a_value);
  ID t_d_node = more_tuples.AppendNode();
  ID t_d_value = more_tuples.AppendValue("t_D");
  // ID t_d_pk_link =
  more_tuples.AppendLink(t_d_node, bc::b_p_primary_key, t_d_value);
  ID t_rb_link = more_tuples.AppendLink(
      t_d_node, db->BootstrapConstants().b_p_replaced_by, t_a_node);

  IDToIDMap id_map;
  write_ok = db->Write(more_tuples, &id_map);
  ASSERT_TRUE(write_ok);
  ASSERT_TRUE(ContainsKey(id_map, t_rb_link));
  ASSERT_NE(id_map[t_rb_link], 0);
  ID t_d = db->FindNodeByPrimaryKey("t_D");

  // "t_A", "t_B" and "t_D" should be the subjects of live "replaced_by" links.
  IDSet correct_replaced;
  IDSet current_replaced;
  correct_replaced.insert(t_a);
  correct_replaced.insert(t_b);
  correct_replaced.insert(t_d);
  db->EntitiesWith(kPredicate, db->BootstrapConstants().b_p_replaced_by,
                   kSubject, 0, &current_replaced);
  ASSERT_EQ(current_replaced.size(), 3);
  ASSERT_EQ(current_replaced, correct_replaced);

  // The objects of all "replaced_by" links should now be "t_C".
  IDSet correct_objects;
  IDSet current_objects;
  correct_objects.insert(t_c);
  db->EntitiesWith(kPredicate, db->BootstrapConstants().b_p_replaced_by,
                   kObject, 0, &current_objects);
  ASSERT_EQ(current_objects.size(), 1);
  ASSERT_EQ(current_objects, correct_objects);
}

// For many tests we need a TupleSequence containing 1) a node
// with 2) a primary key and 3) at least one arbitrary property
// attached, so that it will all actually be written rather than
// dropped.
unique_ptr<TupleSequence> TupleStoreTest::MakeNodeAndPropertyWithKeys(
    string* node_pk, string* property_pk) {
  unique_ptr<TupleSequence> tuples(
      bootstrap_trio_example_source_->FetchExample("ex_add_link"));
  CHECK_NOTNULL(tuples);
  *node_pk = "foo";
  *property_pk = "p1";
  return tuples;
}

TEST_F(TupleStoreTest, TestAddDuplicatePrimaryKeyToPrefix) {
  // Test that adding a duplicate primary key to a prefix Node succeeds.

  unique_ptr<TupleStore> db = TupleStore::NewTransient();

  string node_pk;
  string property_pk;
  unique_ptr<const TupleSequence> tuples(
      MakeNodeAndPropertyWithKeys(&node_pk, &property_pk));

  IDToIDMap id_map;
  bool write_ok = db->Write(*tuples, &id_map);
  ASSERT_TRUE(write_ok);

  ID node_id = db->FindNodeByPrimaryKey(node_pk);
  ASSERT_NE(node_id, 0);

  TupleSequence more_tuples(20000);
  ID t20000 = more_tuples.AppendValue(node_pk);
  // The main point is that we use a prefix id for subject here.
  // ID t2001 =
  more_tuples.AppendLink(0, 0, node_id, bc::b_p_primary_key, t20000);

  id_map.clear();
  write_ok = db->Write(more_tuples, &id_map);
  ASSERT_TRUE(write_ok);
  ID other_node_id = db->FindNodeByPrimaryKey(node_pk);
  ASSERT_EQ(node_id, other_node_id);
}

TEST_F(TupleStoreTest, TestAddConflictingPrimaryKeyToPrefix) {
  // Test that adding a conflicting primary key to a prefix Node just succeeds.

  unique_ptr<TupleStore> db = TupleStore::NewTransient();

  string node_pk;
  string property_pk;
  unique_ptr<const TupleSequence> tuples(
      MakeNodeAndPropertyWithKeys(&node_pk, &property_pk));

  IDToIDMap id_map;
  bool write_ok = db->Write(*tuples, &id_map);
  ASSERT_TRUE(write_ok);

  ID node_id = db->FindNodeByPrimaryKey(node_pk);
  ASSERT_NE(node_id, 0);

  string other_node_pk = "other_key";
  ASSERT_NE(other_node_pk, node_pk);
  TupleSequence more_tuples(20000);
  ID t20000 = more_tuples.AppendValue(other_node_pk);
  // ID t20001 =
  more_tuples.AppendLink(0, 0, node_id, bc::b_p_primary_key, t20000);

  id_map.clear();
  write_ok = db->Write(more_tuples, &id_map);
  ASSERT_TRUE(write_ok);
  ID other_node_id = db->FindNodeByPrimaryKey(other_node_pk);
  ASSERT_NE(other_node_id, 0);

  // other_node_id should be the subject of a live "replaced_by" link.
  IDSet correct_subjects;
  IDSet current_subjects;
  correct_subjects.insert(other_node_id);
  db->EntitiesWith(kPredicate, db->BootstrapConstants().b_p_replaced_by,
                   kSubject, 0, &current_subjects);
  ASSERT_EQ(current_subjects.size(), 1);
  ASSERT_EQ(current_subjects, correct_subjects);

  // node_id should be the object of a live "replaced_by" link.
  IDSet correct_objects;
  IDSet current_objects;
  correct_objects.insert(node_id);
  db->EntitiesWith(kPredicate, db->BootstrapConstants().b_p_replaced_by,
                   kObject, 0, &current_objects);
  ASSERT_EQ(current_objects.size(), 1);
  ASSERT_EQ(current_objects, correct_objects);
}

TEST_F(TupleStoreTest, TestWriteConflictingPrimaryKeys) {
  // Test that having conflicting primarys in the input just succeeds.

  unique_ptr<TupleStore> db = TupleStore::NewTransient();

  TupleSequence tuples(1000);
  string a_key = "a_key";
  ID t1000 = tuples.AppendNode();
  ID t1001 = tuples.AppendValue(a_key);
  ID t1002 = tuples.AppendValue("gloss 1");
  // ID t1003 =
  tuples.AppendLink(0, 0, t1000, bc::b_p_primary_key, t1001);

  // ID t1004 =
  tuples.AppendLink(0, 0, t1000, db->BootstrapConstants().b_p_gloss, t1002);

  string another_key = "another_key";
  ID t1005 = tuples.AppendValue(another_key);
  ID t1006 = tuples.AppendValue("gloss 2");
  // ID t1007 =
  tuples.AppendLink(0, 0, t1000, bc::b_p_primary_key, t1005);

  // ID t1008 =
  tuples.AppendLink(0, 0, t1000, db->BootstrapConstants().b_p_gloss, t1006);

  IDToIDMap id_map;
  bool write_ok = db->Write(tuples, &id_map);
  ASSERT_TRUE(write_ok);

  ID node_id = db->FindNodeByPrimaryKey(a_key);
  ASSERT_NE(node_id, 0);

  ID other_node_id = db->FindNodeByPrimaryKey(another_key);
  ASSERT_NE(other_node_id, 0);

  // other_node_id should be the subject of a live "replaced_by" link.
  IDSet correct_subjects;
  IDSet current_subjects;
  correct_subjects.insert(other_node_id);
  db->EntitiesWith(kPredicate, db->BootstrapConstants().b_p_replaced_by,
                   kSubject, 0, &current_subjects);
  ASSERT_EQ(current_subjects.size(), 1);
  ASSERT_EQ(current_subjects, correct_subjects);

  // node_id should be the object of a live "replaced_by" link.
  IDSet correct_objects;
  IDSet current_objects;
  correct_objects.insert(node_id);
  db->EntitiesWith(kPredicate, db->BootstrapConstants().b_p_replaced_by,
                   kObject, 0, &current_objects);
  ASSERT_EQ(current_objects.size(), 1);
  ASSERT_EQ(current_objects, correct_objects);

  // node_id, but not another_node_id, should
  // be the subject of several /b/p/gloss links.
  current_subjects.clear();
  db->EntitiesWith(kPredicate, db->BootstrapConstants().b_p_gloss, kSubject, 0,
                   &current_subjects);
  ASSERT_TRUE(ContainsKey(current_subjects, node_id));
  ASSERT_FALSE(ContainsKey(current_subjects, other_node_id));
}

TEST_F(TupleStoreTest, TestWriteReplacedByError) {
  // Test that having conflicting primary keys in the input fails in some cases.

  unique_ptr<TupleStore> db = TupleStore::NewTransient();

  // Use "p1" from bootstrap.ExAddLink() as an
  // arbitrary example of a simple property.
  unique_ptr<const TupleSequence> add_link_tuples(
      bootstrap_db_example_source_->FetchExample("ex_add_link"));
  CHECK_NOTNULL(add_link_tuples);
  bool write_ok = db->Write(*add_link_tuples, nullptr);
  ASSERT_TRUE(write_ok);
  ID a_pred = db->FindNodeByPrimaryKey("p1");

  TupleSequence tuples(20000);
  ID a_node = tuples.AppendNode();
  ID other_node = tuples.AppendNode();

  tuples.AppendLink(0, 0, a_node, a_pred, other_node);

  ID other_key = tuples.AppendValue("other_key");

  // This should fail because a_pred is already in use as a predicate.
  // It therefore cannot become the object of a generated replaced_by link.
  ID bad_link = tuples.AppendLink(0, 0, a_pred, bc::b_p_primary_key, other_key);

  IDToIDMap id_map;
  write_ok = db->Write(tuples, &id_map);
  ASSERT_FALSE(write_ok);
  ASSERT_TRUE(ContainsKey(id_map, bad_link));
  ASSERT_EQ(id_map[bad_link], 0);
}

TEST_F(TupleStoreTest, TestAddPrimaryKeyFromPrefix) {
  // Test that use of an appropriate prefix id as a primary key succeeds.

  unique_ptr<TupleStore> db = TupleStore::NewTransient();

  // Use a value known to be in the bootstrap, but not used as a primary key.
  ID one_value_id = db->GetValueID("1");
  ASSERT_NE(one_value_id, 0);
  ASSERT_EQ(db->FindNodeByPrimaryKey("1"), 0);

  TupleSequence tuples(20000);
  ID t20000 = tuples.AppendNode();
  // ID t1001 =
  tuples.AppendLink(0, 0, t20000, bc::b_p_primary_key, one_value_id);

  AddTriggerProperty(t20000, &tuples);

  // Also test that aliases of this form are resolved properly.
  ID t20000_alias = tuples.AppendNode();
  tuples.AppendLink(0, 0, t20000_alias, bc::b_p_primary_key, one_value_id);
  AddTriggerProperty(t20000_alias, &tuples);

  IDToIDMap id_map;
  bool write_ok = db->Write(tuples, &id_map);
  ASSERT_TRUE(write_ok);

  ID node_id = db->FindNodeByPrimaryKey("1");
  ASSERT_NE(node_id, 0);
  ASSERT_EQ(id_map[t20000], node_id);
}

#if 0
TEST_F(TupleStoreTest, TestAddUndefinedPrimaryKeyFromPrefix) {
  // Test that use of an undefined prefix id as a primary key fails.

  unique_ptr<TupleStore> db = TupleStore::NewTransient();

  TupleSequence tuples(20000);
  ID t20000 = tuples.AppendNode();
  ID t20001 = tuples.AppendLink(0, 0, t20000, bc::b_p_primary_key, 999);

  IDToIDMap id_map;
  bool write_ok = db->Write(tuples, &id_map);
  ASSERT_FALSE(write_ok);

  // Neither the node nor the link should have been written.
  ASSERT_TRUE(ContainsKey(id_map, t20000));
  ASSERT_EQ(id_map[t20000], 0);
  ASSERT_TRUE(ContainsKey(id_map, t20001));
  ASSERT_EQ(id_map[t1001], 0);
}
#endif

TEST_F(TupleStoreTest, TestAddNonValuePrimaryKey) {
  // Test that use of a non-Value id as a primary key fails.

  unique_ptr<TupleStore> db = TupleStore::NewTransient();

  TupleSequence tuples(20000);
  ID t20000 = tuples.AppendNode();
  ID t1001 = tuples.AppendNode();
  ID t1002 = tuples.AppendLink(0, 0, t20000, bc::b_p_primary_key, t1001);

  AddTriggerProperty(t20000, &tuples);

  IDToIDMap id_map;
  bool write_ok = db->Write(tuples, &id_map);
  ASSERT_FALSE(write_ok);

  ASSERT_TRUE(ContainsKey(id_map, t20000));
  ASSERT_NE(id_map[t20000], 0);
  ASSERT_TRUE(ContainsKey(id_map, t1001));
  ASSERT_EQ(id_map[t1001], 0);
  ASSERT_TRUE(ContainsKey(id_map, t1002));
  ASSERT_EQ(id_map[t1002], 0);
}

TEST_F(TupleStoreTest, TestAddNonValuePrimaryKeyFromPrefix) {
  // Test that use of a non-Value prefix id as a primary key fails.

  unique_ptr<TupleStore> db = TupleStore::NewTransient();

  // Use bc::b_true, a Node (not Value) from the bootstrap.
  TupleSequence tuples(20000);
  ID t20000 = tuples.AppendNode();
  ID t1001 = tuples.AppendLink(0, 0, t20000, bc::b_p_primary_key,
                               db->BootstrapConstants().b_true);

  AddTriggerProperty(t20000, &tuples);

  IDToIDMap id_map;
  bool write_ok = db->Write(tuples, &id_map);
  ASSERT_FALSE(write_ok);

  ASSERT_TRUE(ContainsKey(id_map, t20000));
  ASSERT_NE(id_map[t20000], 0);
  ASSERT_TRUE(ContainsKey(id_map, t1001));
  ASSERT_EQ(id_map[t1001], 0);
}

TEST_F(TupleStoreTest, TestFollowLinkForward) {
  // Create foo -> bar -> baz.
  unique_ptr<TupleStore> db = TupleStore::NewTransient();
  ID prop = db->BootstrapConstants().b_p_gloss;
  ID foo = AppendNode(db.get());
  ID bar = AppendNode(db.get());
  ID baz = AppendNode(db.get());

  AppendLink(db.get(), 0, 0, foo, prop, bar, LinkState::kLive);
  AppendLink(db.get(), 0, 0, bar, prop, baz, LinkState::kLive);

  IDSet source;
  source.insert(bar);
  IDSet results = db->FollowLinkForward(prop, source);

  // Ensure that the only result is baz.
  EXPECT_EQ(1, results.size());
  bool baz_found = results.find(baz) != results.end();
  EXPECT_TRUE(baz_found);
}

TEST_F(TupleStoreTest, TestFollowLinkBackward) {
  // Create foo -> bar -> baz.
  unique_ptr<TupleStore> db = TupleStore::NewTransient();
  ID prop = db->BootstrapConstants().bc::b_p_gloss;
  ID foo = AppendNode(db.get());
  ID bar = AppendNode(db.get());
  ID baz = AppendNode(db.get());

  AppendLink(db.get(), 0, 0, foo, prop, bar, LinkState::kLive);
  AppendLink(db.get(), 0, 0, bar, prop, baz, LinkState::kLive);

  IDSet source;
  source.insert(bar);
  IDSet results = db->FollowLinkBackward(prop, source);

  // Ensure that the only result is foo.
  EXPECT_EQ(1, results.size());
  bool foo_found = results.find(foo) != results.end();
  EXPECT_TRUE(foo_found);
}

TEST_F(TupleStoreTest, TestSizeWithModifiedBase) {
  unique_ptr<TupleStore> db = TupleStore::NewTransient();
  AppendLink(db.get(), 0, 0, AppendNode(db.get()),
             db->BootstrapConstants().bc::b_p_gloss, AppendNode(db.get()),
             LinkState::kLive);
  unique_ptr<TupleStore> extension =
      TupleStore::NewTransientExtending(db.get());
  size_t start_size = extension->size();
  AppendLink(db.get(), 0, 0, AppendNode(db.get()),
             db->BootstrapConstants().bc::b_p_gloss, AppendNode(db.get()),
             LinkState::kLive);
  EXPECT_EQ(start_size, extension->size());
}

TEST_F(TupleStoreTest, TestOverwriteNone) {
  // Test none.
  unique_ptr<const TupleSequence> tuples(
      bootstrap_trio_example_source_->FetchExample("ex_none"));
  CHECK_NOTNULL(tuples);

  unique_ptr<TupleStore> db = TupleStore::NewTransient();
  bool write_ok = db->Write(*tuples, nullptr);
  ASSERT_TRUE(write_ok);

  // none + its killer = 2 links.
  IDSet all_none_links;
  db->FindLinksWithIDAtLinkage(
      kPredicate, db->BootstrapConstants().bc::b_p_none, &all_none_links);
  // none is killed by long.
  IDSet live_none_links = db->RemoveDeadTuples(all_none_links);
  ASSERT_EQ(0, live_none_links.size());
  ASSERT_EQ(2, all_none_links.size());

  ID long_pred = db->FindNodeByPrimaryKey("long");
  ASSERT_GT(long_pred, 0);

  IDSet all_long_links;
  db->FindLinksWithIDAtLinkage(kPredicate, long_pred, &all_long_links);
  // long is live.
  IDSet live_long_links = db->RemoveDeadTuples(all_long_links);
  ASSERT_EQ(1, live_long_links.size());
  ASSERT_EQ(1, all_long_links.size());

  // Write again to verify that none deletes the
  // live_long_links before they are re-asserted.
  write_ok = db->Write(*tuples, nullptr);
  ASSERT_TRUE(write_ok);

  // 2 * (none + its killer) = 4 links.
  all_none_links.clear();
  db->FindLinksWithIDAtLinkage(
      kPredicate, db->BootstrapConstants().bc::b_p_none, &all_none_links);
  // none is killed by long.
  live_none_links = db->RemoveDeadTuples(all_none_links);
  ASSERT_EQ(0, live_none_links.size());
  ASSERT_EQ(4, all_none_links.size());

  // long + its killer + long = 3 links.
  all_long_links.clear();
  db->FindLinksWithIDAtLinkage(kPredicate, long_pred, &all_long_links);
  // long is live.
  live_long_links = db->RemoveDeadTuples(all_long_links);
  ASSERT_EQ(1, live_long_links.size());
  ASSERT_EQ(3, all_long_links.size());
}

TEST_F(TupleStoreTest, TestWriteWithPrimaryKeyAlias) {
  // Test that writing primary key links by using a foreign
  // alias of /b/p/primary_key as the predicate works correctly.

  unique_ptr<TupleStore> db = TupleStore::NewTransient();

  // Use "p1" from bootstrap.ExAddLink() as an
  // arbitrary example of a simple property.
  unique_ptr<const TupleSequence> add_link_tuples(
      bootstrap_db_example_source_->FetchExample("ex_add_link"));
  CHECK_NOTNULL(add_link_tuples);

  bool write_ok = db->Write(*add_link_tuples, nullptr);
  ASSERT_TRUE(write_ok);
  ID predicate = db->FindNodeByPrimaryKey("p1");

  TupleSequence tuples(20000);

  ID alias_node_1 = tuples.AppendNode();
  ID alias_value_1 = tuples.AppendValue(bc::b_p_primary_key__pk);
  tuples.AppendLink(0, 0, alias_node_1, bc::b_p_primary_key, alias_value_1);

  ID alias_node_2 = tuples.AppendNode();
  ID alias_value_2 = tuples.AppendValue(bc::b_p_primary_key__pk);
  tuples.AppendLink(0, 0, alias_node_2, alias_node_1, alias_value_2);

  ID alias_node_3 = tuples.AppendNode();
  ID alias_value_3 = tuples.AppendValue(bc::b_p_primary_key__pk);
  tuples.AppendLink(0, 0, alias_node_3, alias_node_2, alias_value_3);

  ID a_node = tuples.AppendNode();
  ID a_pk_value = tuples.AppendValue("a_pk");
  tuples.AppendLink(0, 0, a_node, alias_node_1, a_pk_value);

  ID b_node = tuples.AppendNode();
  ID b_pk_value = tuples.AppendValue("b_pk");
  tuples.AppendLink(0, 0, b_node, alias_node_2, b_pk_value);

  ID c_node = tuples.AppendNode();
  ID c_pk_value = tuples.AppendValue("c_pk");
  tuples.AppendLink(0, 0, c_node, alias_node_3, c_pk_value);

  tuples.AppendLink(0, 0, a_node, predicate, b_node);
  tuples.AppendLink(0, 0, b_node, predicate, c_node);
  tuples.AppendLink(0, 0, c_node, predicate, a_node);

  write_ok = db->Write(tuples, nullptr);
  ASSERT_TRUE(write_ok);

  ID a_id = db->FindNodeByPrimaryKey("a_pk");
  ASSERT_GT(a_id, 0);
  ID b_id = db->FindNodeByPrimaryKey("b_pk");
  ASSERT_GT(b_id, 0);
  ID c_id = db->FindNodeByPrimaryKey("c_pk");
  ASSERT_GT(c_id, 0);
}

TEST_F(TupleStoreTest, TestDeletePrimaryKey) {
  // Test that an attempt to delete a primary key link fails.

  unique_ptr<TupleStore> db = TupleStore::NewTransient();

  // Use "p1" from bootstrap.ExAddLink() as an
  // arbitrary example of a simple property.
  unique_ptr<const TupleSequence> add_link_tuples(
      bootstrap_db_example_source_->FetchExample("ex_add_link"));
  CHECK_NOTNULL(add_link_tuples);

  bool write_ok = db->Write(*add_link_tuples, nullptr);
  ASSERT_TRUE(write_ok);
  ID foo = db->FindNodeByPrimaryKey("foo");
  ID foo_pk_value = db->FindPrimaryKeyByNodeID(foo);

  TupleSequence tuples(20000);
  ID killer = tuples.AppendRecord(Record::Link(tuples.next_id(), 0, 0, 0, foo,
                                               bc::b_p_primary_key,
                                               foo_pk_value, LinkState::kDead));

  IDToIDMap id_map;
  write_ok = db->Write(tuples, &id_map);
  ASSERT_FALSE(write_ok);

  foo = db->FindNodeByPrimaryKey("foo");
  ASSERT_GT(foo, 0);
  ASSERT_TRUE(ContainsKey(id_map, killer));
  ASSERT_EQ(id_map[killer], 0);
}

TEST_F(TupleStoreTest, TestRestoreFromSparse) {
  TupleSequence tuples;
  tuples.AppendNode();
  tuples.AppendRecord(
      Record::Link(tuples.next_id() + 19, 0, 0, 0, 1, 1, 1, LinkState::kLive));
  unique_ptr<TupleStore> db = TupleStore::NewTransient();
  EXPECT_FALSE(db->Restore(tuples));
}

TEST_F(TupleStoreTest, GetNoValuesInBulk) {
  unique_ptr<TupleStore> db = TupleStore::NewTransient();
  CHECK_NOTNULL(db.get());
  vector<ID> ids;
  vector<string> values;
  db->GetValues(ids, &values);
  EXPECT_EQ(0, values.size());
}

TEST_F(TupleStoreTest, GetOneValueInBulk) {
  unique_ptr<TupleStore> db = TupleStore::NewTransient();
  CHECK_NOTNULL(db.get());
  vector<ID> ids{AppendValue(db.get(), "foo")};
  vector<string> values;
  db->GetValues(ids, &values);
  ASSERT_EQ(1, values.size());
  EXPECT_EQ("foo", values[0]);
}

TEST_F(TupleStoreTest, GetNonValuesInBulk) {
  unique_ptr<TupleStore> db = TupleStore::NewTransient();
  CHECK_NOTNULL(db.get());
  vector<ID> ids{AppendNode(db.get()), AppendValue(db.get(), "foo")};
  vector<string> values;
  db->GetValues(ids, &values);
  ASSERT_EQ(2, values.size());
  EXPECT_EQ("", values[0]);
  EXPECT_EQ("foo", values[1]);
}

TEST_F(TupleStoreTest, GetValuesInBulkAcrossTupleStoreBoundary) {
  unique_ptr<TupleStore> db = TupleStore::NewTransient();
  CHECK_NOTNULL(db.get());
  vector<ID> ids{AppendValue(db.get(), "foo")};
  unique_ptr<TupleStore> ext = TupleStore::NewTransientExtending(db.get());
  CHECK_NOTNULL(ext.get());
  ids.push_back(AppendValue(db.get(), "bar"));
  vector<string> values;
  db->GetValues(ids, &values);
  ASSERT_EQ(2, values.size());
  EXPECT_EQ("foo", values[0]);
  EXPECT_EQ("bar", values[1]);
}

TEST_F(TupleStoreTest, GetPrimaryKeysFromNodeIDs) {
  unique_ptr<TupleStore> db = TupleStore::NewTransient();
  unique_ptr<const TupleSequence> base_tuples(
      bootstrap_trio_example_source_->FetchExample("ex_add_link"));
  ASSERT_TRUE(CHECK_NOTNULL(db)->Write(*CHECK_NOTNULL(base_tuples), nullptr));

  unique_ptr<TupleStore> ext = TupleStore::NewTransientExtending(db.get());
  unique_ptr<const TupleSequence> ext_tuples(
      bootstrap_trio_example_source_->FetchExample("ex_add_link_foo_baz"));
  ASSERT_TRUE(CHECK_NOTNULL(ext)->Write(*CHECK_NOTNULL(ext_tuples), nullptr));

  const vector<ID> ids{ext->FindNodeByPrimaryKey("foo"), ext->next_id(),
                       ext->FindNodeByPrimaryKey("baz"),
                       ext->FindNodeByPrimaryKey("bar")};
  vector<ID> primary_key_nodes;
  ext->FindPrimaryKeysByNodeIDs(ids, &primary_key_nodes);
  ASSERT_EQ(ids.size(), primary_key_nodes.size());
  vector<string> primary_keys;
  ext->GetValues(primary_key_nodes, &primary_keys);
  ASSERT_EQ(ids.size(), primary_keys.size());
  EXPECT_EQ("foo", primary_keys[0]);  // From the base.
  EXPECT_EQ("", primary_keys[1]);     // Doesn't exist.
  EXPECT_EQ("baz", primary_keys[2]);  // From the extension.
  EXPECT_EQ("bar", primary_keys[3]);  // Out of order request.
}

string WoodyRestoreDataFile() {
  return file::JoinPath(
      FLAGS_test_srcdir,
      "google3/testing/testdata/ex_woody_allen_data.db.trio");
}

TEST_F(TupleStoreTest, TestRestoreFromTrio) {
  unique_ptr<TupleStore> db = TupleStore::NewTransient();
  EXPECT_TRUE(db->Restore(WoodyRestoreDataFile()));
}

static void BM_TupleStoreRestoreFromTuples(int iters) {
  CHECK_GT(iters, 0);
  unique_ptr<const TupleSequence> woody_tuples =
      TupleSequence::CreateFromTrio(WoodyRestoreDataFile());
  unique_ptr<TupleStore> ts;
  for (int i = 0; i < iters; ++i) {
    ts = TupleStore::NewTransient();
    CHECK(ts->Restore(*woody_tuples));
  }
  // We don't actually care about bytes processed, but this provides a nice
  // way to see tuples/second, so we pass it the number of tuples instead.
  SetBenchmarkBytesProcessed(iters * ts->next_id());
}
BENCHMARK(BM_TupleStoreRestoreFromTuples);

static void BM_TupleStoreRestoreFromTrio(int iters) {
  CHECK_GT(iters, 0);
  unique_ptr<TupleStore> ts;
  for (int i = 0; i < iters; ++i) {
    ts = TupleStore::NewTransient();
    CHECK(ts->Restore(WoodyRestoreDataFile()));
  }
  // We don't actually care about bytes processed, but this provides a nice
  // way to see tuples/second, so we pass it the number of tuples instead.
  SetBenchmarkBytesProcessed(iters * ts->next_id());
}
BENCHMARK(BM_TupleStoreRestoreFromTrio);
