#include <map>
using ::std::map;
#include <memory>
#include <set>
using ::std::set;
#include <string>
using ::std::string;
#include <tuple>
#include <utility>
#include <vector>
using ::std::vector;

#include "base/callback.h"
#include "base/logging.h"
#include "base/macros.h"
#include "codegen/schema.h"
#include "common/common_types.h"
#include "common/object_graph.h"
#include "common/record.h"
#include "core/bootstrap.h"
#include "core/metaschema.h"
#include "core/tupleindex.h"
#include "core/tuplesequence.h"
#include "core/tuplestore.h"
#include "util/set.h"
#include "strings/numbers.h"
#include "util/btree/btree_set.h"
#include "util/gtl/map_util.h"

using std::unique_ptr;

typedef b_bootstrap_constants bc;

class MetaSchemaScanner {
 public:
  explicit MetaSchemaScanner(const TupleStore& ts) : ts_(ts) {}
  virtual ~MetaSchemaScanner() {}
  virtual ID ForeignToLocal(ID id) ABSTRACT;
  virtual string GetValue(ID value_id) ABSTRACT;
  void HandleTuple(MetaSchema* schema, ObjectGraph* accumulator,
                   const Record& tuple) {
    if (tuple.IsLink()) {
      ID foreign_subject = tuple.subject();
      ID local_predicate = ForeignToLocal(tuple.predicate());
      ID foreign_object = tuple.object();
      if (local_predicate == ts_.BootstrapConstants().b_p_subject_meta) {
        accumulator->CreateLink(foreign_subject, local_predicate,
                                foreign_object);
      } else if (local_predicate == ts_.BootstrapConstants().b_p_object_meta) {
        accumulator->CreateLink(foreign_subject, local_predicate,
                                foreign_object);
      } else if (local_predicate == ts_.BootstrapConstants().b_p_cardinality) {
        const string value_string = GetValue(foreign_object);
        int cardinality = -1;
        if (safe_strto32(value_string, &cardinality) && (cardinality >= 0)) {
          if (accumulator->GetValue(value_string) == 0) {
            accumulator->CreateValue(foreign_object, value_string);
          }
          accumulator->CreateLink(foreign_subject, local_predicate,
                                  foreign_object);
        }
      } else if (local_predicate == ts_.BootstrapConstants().b_p_mutable) {
        ID local_object = ForeignToLocal(tuple.object());
        accumulator->CreateLink(foreign_subject, local_predicate, local_object);
      }
    }
  }
  unique_ptr<ObjectGraph> ObjectGraphWithPredicates() {
    unique_ptr<ObjectGraph> graph(new ObjectGraph);
    graph->CreateNode(ts_.BootstrapConstants().b_p_cardinality,
                      bc::b_p_cardinality__pk);
    graph->CreateNode(ts_.BootstrapConstants().b_p_mutable,
                      bc::b_p_mutable__pk);
    graph->CreateNode(ts_.BootstrapConstants().b_p_object_meta,
                      bc::b_p_object_meta__pk);
    graph->CreateNode(bc::b_p_primary_key, bc::b_p_primary_key__pk);
    graph->CreateNode(ts_.BootstrapConstants().b_p_subject_meta,
                      bc::b_p_subject_meta__pk);
    graph->CreateNode(ts_.BootstrapConstants().b_true, bc::b_true__pk);
    return graph;
  }

 protected:
  const TupleStore& ts_;
};

class LocalMetaSchemaScanner : public MetaSchemaScanner {
 public:
  explicit LocalMetaSchemaScanner(const TupleStore& ts)
      : MetaSchemaScanner(ts) {}
  void ScanInto(const IDSet& source_ids, MetaSchema* schema) {
    unique_ptr<ObjectGraph> accumulator(ObjectGraphWithPredicates());
    unique_ptr<ForEachRecordCallback> callback(NewPermanentCallback(
        this, &MetaSchemaScanner::HandleTuple, schema, accumulator.get()));
    ts_.ForEachRecordInSet(source_ids, callback.get(), true);
    schema->CreateDomainInstances(accumulator.get());
  }
  virtual ID ForeignToLocal(ID id) { return id; }
  virtual string GetValue(ID value_id) {
    CHECK_LT(value_id, ts_.next_id());
    return ts_.GetValue(value_id);
  }
};

class ForeignMetaSchemaScanner : public MetaSchemaScanner {
 public:
  ForeignMetaSchemaScanner(const TupleSequence& source,
                           const IDToIDMap& foreign_to_local_id_map,
                           const TupleStore& ts)
      : MetaSchemaScanner(ts),
        source_(source),
        foreign_to_local_id_map_(foreign_to_local_id_map) {}
  virtual ID ForeignToLocal(ID id) {
    return FindWithDefault(foreign_to_local_id_map_, id, 0);
  }
  virtual string GetValue(ID value_id) {
    if (value_id >= source_.start_id()) {
      return source_.GetValue(value_id);
    } else {
      return ts_.GetValue(value_id);
    }
  }

 private:
  const TupleSequence& source_;
  const IDToIDMap& foreign_to_local_id_map_;
};

struct WriteContext {
  WriteContext(const TupleStore& ts, ID bootstrap_end, ID database_next_id,
               const TupleSequence* source, IDToIDMap* id_map)
      : ts_(ts) {
    next_local_id = database_next_id;
    foreign.source = source;
    foreign.start_id = source->start_id();
    results.error_count = 0;
    results.id_map = id_map;
  }
  ~WriteContext() {}
  void Initialize() {
    foreign.metaschema_scanner.reset(
        new ForeignMetaSchemaScanner(*foreign.source, *results.id_map, ts_));
    // Add subgraph used for synthetic links.
    valid_subgraph_unions.insert(ts_.BootstrapConstants().b_mgp_subgraph_union);
    unique_ptr<TupleSequence> tuples(new TupleSequence);
    CHECK(tuples->InitFromProtoStr(bc::AsProto()));
    for (ID id = tuples->start_id(); id < tuples->next_id(); ++id) {
      const Record& tuple = tuples->GetTuple(id);
      if (tuple.IsLink() && (tuple.predicate() == bc::b_p_primary_key)) {
        const ID subject = tuple.subject();
        if ((subject == bc::b_p_primary_key) ||
            (subject == ts_.BootstrapConstants().b_p_subject_meta) ||
            (subject == ts_.BootstrapConstants().b_p_object_meta) ||
            (subject == ts_.BootstrapConstants().b_p_cardinality) ||
            (subject == ts_.BootstrapConstants().b_p_mutable) ||
            (subject == ts_.BootstrapConstants().b_true) ||
            (subject == ts_.BootstrapConstants().b_false)) {
          InsertOrDie(&bootstrap.pk_value_to_node, tuple.object(), subject);
        }
      }
    }
    InsertOrDie(&bootstrap.pk_string_to_node, bc::b_p_primary_key__pk,
                bc::b_p_primary_key);
    InsertOrDie(&bootstrap.pk_string_to_node, bc::b_p_subject_meta__pk,
                ts_.BootstrapConstants().b_p_subject_meta);
    InsertOrDie(&bootstrap.pk_string_to_node, bc::b_p_object_meta__pk,
                ts_.BootstrapConstants().b_p_object_meta);
    InsertOrDie(&bootstrap.pk_string_to_node, bc::b_p_cardinality__pk,
                ts_.BootstrapConstants().b_p_cardinality);
    InsertOrDie(&bootstrap.pk_string_to_node, bc::b_p_mutable__pk,
                ts_.BootstrapConstants().b_p_mutable);
    InsertOrDie(&bootstrap.pk_string_to_node, bc::b_true__pk,
                ts_.BootstrapConstants().b_true);
    InsertOrDie(&bootstrap.pk_string_to_node, bc::b_false__pk,
                ts_.BootstrapConstants().b_false);
  }

  void AddIDMapEntry(ID foreign_id, ID local_id) {
    CHECK_NE(foreign_id, 0);
    if (ContainsKey(*results.id_map, foreign_id)) {
      CHECK((*results.id_map)[foreign_id] == local_id)
          << "Overrode foreign to local mapping: " << foreign_id << "->"
          << (*results.id_map)[foreign_id] << " overridden to: " << foreign_id
          << "->" << local_id;
    } else {
      (*results.id_map)[foreign_id] = local_id;
    }
  }

  void AddIDMapError(ID foreign_id) {
    AddIDMapEntry(foreign_id, 0);
    ++results.error_count;
  }

  bool HasIDMapEntry(ID foreign_id) {
    return ContainsKey(*results.id_map, foreign_id);
  }

  ID GetIDMapEntry(ID foreign_id) {
    return FindOrDie(*results.id_map, foreign_id);
  }

  ID MapIDForeignToLocal(ID foreign_id) const {
    ID local_id = FindWithDefault(*results.id_map, foreign_id, 0);
    if (local_id) {
      return local_id;
    }
    if ((foreign_id < foreign.start_id) && (foreign_id < next_local_id)) {
      return foreign_id;  // Constant reference to local id space.
    }
    if (foreign_id <= ts_.BootstrapConstants().b_p_primary_key) {
      return foreign_id;  // Constant reference to bootstrap.
    }
    return 0;
  }

  void AddAliasMapEntry(ID foreign_id, ID first_foreign_id, bool clobber) {
    CHECK_NE(foreign_id, 0);
    const ID prior_mapping = FindWithDefault(alias_map, foreign_id, 0);
    if (prior_mapping) {
      CHECK((prior_mapping == first_foreign_id) ||
            (clobber && (prior_mapping == foreign_id)))
          << "prior_mapping: " << prior_mapping
          << " first_foreign_id: " << first_foreign_id
          << " foreign_id: " << foreign_id;
    }
    alias_map[foreign_id] = first_foreign_id;
  }

  ID MapIDAlias(ID foreign_id, ID default_id) const {
    return FindWithDefault(alias_map, foreign_id, default_id);
  }

  ID AddValueOrAlias(const string& str, ID new_value_id) {
    CHECK_NE(new_value_id, 0);
    const ID value_id =
        LookupOrInsert(&foreign.string_to_value_id, str, new_value_id);
    AddAliasMapEntry(new_value_id, value_id, false);
    return value_id;
  }

  ID AddNodeOrAlias(ID pk_value_id, ID new_node_id) {
    CHECK_NE(pk_value_id, 0);
    CHECK_NE(new_node_id, 0);
    const ID node_id = LookupOrInsert(&foreign.pk_value_id_to_node_id,
                                      pk_value_id, new_node_id);
    AddAliasMapEntry(new_node_id, node_id, false);
    return node_id;
  }

  ID AddCompoundOrAlias(UUID cspo_key, ID a_hub_id) {
    const ID first_hub_id =
        FindWithDefault(foreign.normalized_cspo_to_hub_id, cspo_key, 0);
    if (first_hub_id) {
      AddAliasMapEntry(a_hub_id, first_hub_id, true);
      return first_hub_id;
    } else {
      AddAliasMapEntry(a_hub_id, a_hub_id, true);
      foreign.normalized_cspo_to_hub_id[cspo_key] = a_hub_id;
      return a_hub_id;
    }
  }

  const TupleStore& ts_;

  struct {
    // Lookup tables for alias detection in GatherForeignNodesAndValuesCb().
    IDToIDMap pk_value_to_node;
    hash_map<string, ID> pk_string_to_node;
  } bootstrap;
  // Last id (of entire TupleStore stack) at commencement of Write().
  ID next_local_id;
  // Stuff accumulated in the first pass over the input
  // by GatherForeignNodesAndValues().
  struct {
    const TupleSequence* source;
    unique_ptr<ForeignMetaSchemaScanner> metaschema_scanner;
    // Parallel vectors
    vector<ID> value_ids;
    vector<string> value_strings;
    // Same contents as value_ids
    // arranged for faster membership testing.
    IDSet value_ids_set;
    // Parallel vectors
    vector<ID> pk_link_ids;
    vector<ID> pk_subjects;
    vector<ID> pk_objects;
    IDSet subgraphs;
    map<ID, CompoundDescriptor> compounds;
    IDToSetMap hub_to_identifying_spokes;
    // String to foreign id of Value first occurrence.
    hash_map<string, ID> string_to_value_id;
    // Primary key Value id to foreign id of Node first occurrence.
    IDToIDMap pk_value_id_to_node_id;
    // Normalized cspo key to foreign id of compound hub first instance.
    hash_map<UUID, ID> normalized_cspo_to_hub_id;
    // Minimum id in the input TupleSequence.
    ID start_id;
  } foreign;
  struct {
    // All valid prefix ids from input.
    IDSet ids;
    // Each prefix tuple is inspected and its id is added to
    // either node_ids, value_ids, or link_ids.
    IDSet node_ids;
    IDSet value_ids;
    IDSet link_ids;
  } prefix;
  // Stuff discovered during the first probe of base.
  // Pending nodes and values after initial rounds of foreign
  // to local mapping (by primary key and various interning checks)
  // have been completed.  These are written upon first reference,
  // but are never removed from these collections.
  struct {
    // All foreign Nodes get an entry in this map at first.
    // Foreign Node id -> foreign primary key Value id (or 0).
    IDToIDMap nodes;
    // Foreign Values which have not been mapped to local
    // Values after the initial pass get an entry in this map.
    hash_map<ID, string> value_to_string;
    // Deferred primary key links, to be written iff subject
    // is referenced by any other kind of link.
    // Foreign Node id -> [foreign primary key Link].
    hash_map<ID, vector<ID> > pk_node_to_links;
    // Foreign schema - must be interpreted before they are
    // written in order to deal with compound property interning.
    MetaSchema schema;
  } pending;
  // Stuff accumulated during the second pass over the input
  // by GatherCompoundSpokesWithHubs() and the second probe of base.
  struct {
    // Compound subsets that exist locally.
    IDToSetMap foreign_hub_to_local_subsets;
    hash_map<ID, int> local_hub_to_spoke_count;
    // Compound subsets that exist in the input only.
    IDToSetMap foreign_hub_to_foreign_subsets;
    hash_map<ID, int> foreign_hub_to_spoke_count;
  } compound;
  struct {
    // local_spo_keys gets SPO keys for all input links where S, P and O
    // already exist in the database.  For batch loading 'latest live
    // subgraph'.
    vector<UUID> local_spo_keys;
    // Built by FindLatestLiveSubgraphs().  Used by FindInternedLink().
    hash_map<UUID, ID> local_link_id_by_spo_key;
    hash_map<ID, LinkState> dead_by_local_link_id;
    IDToIDMap subgraph_by_local_link_id;
  } simple;
  // For all /b/p/replaced_by links relevant to the input, this maps
  // from the <loser> local Node id that has been replaced to the <winner>
  // local Node id that has replaced it.
  IDToIDMap replaced_by_map;
  // All foreign -> foreign duplicates, includes self-mapping.
  // Initialized in first pass and updated in second (compound) pass.
  IDToIDMap alias_map;
  hash_map<UUID, ID> cspo_to_hub_id;
  IDSet valid_subgraph_unions;
  IDToSetMap subgraph_union_to_nodes;
  struct {
    int error_count;
    IDToIDMap* id_map;
  } results;
};

ID TupleStore::MergeSubsetHubs(
    WriteContext* wc, const hash_map<ID, int>& local_hub_to_spoke_count) {
  CHECK(!local_hub_to_spoke_count.empty());
  ID winning_hub_id = 0;
  ID winning_num_spokes = 0;
  for (auto it : local_hub_to_spoke_count) {
    ID hub_id = it.first;
    int spoke_count = it.second;
    if (spoke_count > winning_num_spokes) {
      winning_hub_id = hub_id;
      winning_num_spokes = spoke_count;
    }
  }
  for (auto it : local_hub_to_spoke_count) {
    ID hub_id = it.first;
    if (hub_id != winning_hub_id) {
      Record rb_link = Record::Link(
          next_id(), 0, 0, BootstrapConstants().b_mgp_subgraph_union, hub_id,
          BootstrapConstants().b_p_replaced_by, winning_hub_id,
          LinkState::kLive);
      WriteLinkReplacing(wc, rb_link, 0, true, 0);
    }
  }
  return winning_hub_id;
}

void TupleStore::SplitLocalAndPendingNodes(const WriteContext& wc,
                                           const IDSet& node_ids,
                                           IDSet* local_nodes,
                                           IDSet* foreign_nodes) {
  // Each node is in one of these categories:
  // 1) Foreign id is already a local id.
  // 2) Foreign id can now be mapped to local id (was resolved by primary key).
  // 3) Foreign id is in wc->pending.nodes.
  // 4) Foreign id is not a valid foreign node reference.
  for (auto id : node_ids) {
    if ((id < wc.foreign.start_id) && (id < wc.next_local_id)) {
      local_nodes->insert(id);
    } else {
      ID local_id = FindWithDefault(*wc.results.id_map, id, 0);
      if (local_id) {
        local_nodes->insert(local_id);
      } else {
        if (ContainsKey(wc.pending.nodes, id)) {
          foreign_nodes->insert(id);
        }  // else undefined
      }
    }
  }
}

// WriteExtension() is permissive/optimistic.  It continues iterating over
// <source> after individual Record write failures (for example: a link write
// failure due to an undefined predicate).  Individual Record write failures
// are indicated in the optional out param <out_id_map> by a mapping of the
// <source> Record id to 0.  Returns true if <source> is written completely
// without errors.  Otherwise returns false.
bool TupleStore::WriteExtension(const TupleSequence& source,
                                IDToIDMap* out_id_map) {
  CHECK(empty_ext());
  if (!source.Valid()) {
    LOG(ERROR) << "!source.Valid()";
    return false;
  }
  IDToIDMap local_id_map;
  IDToIDMap* const id_map = out_id_map ? out_id_map : &local_id_map;
  WriteContext wc(*this, BootstrapConstants().b_p_primary_key + 1, next_id(),
                  &source, id_map);
  wc.Initialize();
  // Pass 1 over the input.
  GatherForeignNodesAndValues(&wc);
  MapForeignToLocalValues(&wc);
  MapForeignToLocalNodes(&wc);
  // Pass 2 over the input.
  GatherCompoundSpokesWithHubs(&wc);
  MapForeignToLocalCompounds(&wc);
  // Pass 3 over the input.
  GatherLocalSPOKeys(&wc);
  // Subgraphs referred to by foreign links.
  IDSet local_subgraphs;
  IDSet foreign_subgraphs;
  SplitLocalAndPendingNodes(wc, wc.foreign.subgraphs, &local_subgraphs,
                            &foreign_subgraphs);
  // Latest live subgraphs referred to by local links.
  IDSet latest_live_subgraphs;
  FindLatestLiveSubgraphs(&wc, &latest_live_subgraphs);
  // Load all referenced local subgraph unions into cache.
  IDSet all_local_subgraphs = Union(local_subgraphs, latest_live_subgraphs);
  ValidateLocalSubgraphs(&wc, all_local_subgraphs);
  // Pass 3 over the input = main loop.
  WriteSimpleProperties(&wc);
  // Mark unused tuples with a zero in
  // out_id_map, but do not count them as errors.
  if (out_id_map) {
    for (auto kv : wc.pending.nodes) {
      ID id = kv.first;
      if (!wc.HasIDMapEntry(id)) {
        wc.AddIDMapEntry(id, 0);
      }
    }
    for (auto kv : wc.pending.value_to_string) {
      ID id = kv.first;
      if (!wc.HasIDMapEntry(id)) {
        wc.AddIDMapEntry(id, 0);
      }
    }
    for (auto kv : wc.pending.pk_node_to_links) {
      const vector<ID>& pk_link_ids = kv.second;
      for (auto pk_link_id : pk_link_ids) {
        if (!wc.HasIDMapEntry(pk_link_id)) {
          wc.AddIDMapEntry(pk_link_id, 0);
        }
      }
    }
  }
  return wc.results.error_count == 0;
}

void TupleStore::GatherForeignNodesAndValuesCb(WriteContext* wc,
                                               ObjectGraph* accumulator,
                                               const Record& tuple) {
  // This callback (the first pass over Write() input) does several things:
  //
  // - Aggregates Value strings and primary-key/Node relationships
  //   so that the target database can be probed for matches in bulk.
  //
  // - Aggregates schema tuples so that compound properties can be
  //   interpreted before any new schema are written to the database.
  //
  // - Identifies duplicate Values and Nodes.  The first occurrence of each
  //   can subsequently be used in place of any aliases to generate normalized
  //   CSPO keys.  This in turn allows us to identify duplicate compound
  //   property instances before any tuples are actually written.
  wc->foreign.metaschema_scanner->HandleTuple(&wc->pending.schema, accumulator,
                                              tuple);
  if (tuple.IsNode()) {
    wc->pending.nodes[tuple.id()] = 0;
  } else if (tuple.IsValue()) {
    string str = tuple.ToString();
    wc->foreign.value_ids.push_back(tuple.id());
    wc->foreign.value_strings.push_back(str);
    wc->foreign.value_ids_set.insert(tuple.id());
    wc->AddValueOrAlias(str, tuple.id());
  } else if (tuple.IsLink()) {
    if ((!tuple.subject()) || (!tuple.predicate()) || (!tuple.object())) {
      LOG(ERROR) << "Link subject, predicate or object is zero: "
                 << tuple.ToDebugString();
      wc->AddIDMapError(tuple.id());
      return;
    }
    for (auto l : kAllLinkages) {
      ID linked_id = tuple.linkage(l);
      if ((linked_id >= wc->next_local_id) &&
          (linked_id < wc->foreign.start_id)) {
        LOG(ERROR) << "Link refers to an undefined entity (" << linked_id
                   << "):\n" << tuple.ToDebugString();
        wc->AddIDMapError(tuple.id());
        return;
      } else if ((linked_id > 0) && (linked_id < wc->next_local_id)) {
        // Determine whether this a Node, Value or Link in
        // InspectPrefixTuples() and then test in WriteLink().
        wc->prefix.ids.insert(linked_id);
      }
    }
    if (tuple.subgraph()) {
      wc->foreign.subgraphs.insert(tuple.subgraph());
    }
    if (wc->MapIDForeignToLocal(tuple.predicate()) == bc::b_p_primary_key) {
      const ID local_object_id =
          tuple.object() < wc->foreign.start_id ? tuple.object() : 0;
      if (local_object_id) {
        // Find bootstrap property aliased by way of prefix Value id.
        const ID local_node_id =
            FindWithDefault(wc->bootstrap.pk_value_to_node, local_object_id, 0);
        if (local_node_id) {
          wc->AddIDMapEntry(tuple.subject(), local_node_id);
        }
      } else {
        // Find bootstrap property aliased by way of duplicate Value string.
        const Record object_tuple =
            wc->foreign.source->GetTuple(tuple.object());
        if (object_tuple.IsValue()) {
          const string str = wc->foreign.source->GetValue(tuple.object());
          const ID local_node_id =
              FindWithDefault(wc->bootstrap.pk_string_to_node, str, 0);
          if (local_node_id) {
            wc->AddIDMapEntry(tuple.subject(), local_node_id);
          }
        } else {
          LOG(ERROR) << "Primary key Link object is not a Value:\n"
                     << tuple.ToDebugString();
          wc->AddIDMapError(tuple.id());
          return;
        }
      }
      wc->foreign.pk_link_ids.push_back(tuple.id());
      wc->foreign.pk_subjects.push_back(tuple.subject());
      wc->foreign.pk_objects.push_back(tuple.object());
      if (ContainsKey(wc->pending.nodes, tuple.subject())) {
        bool object_is_local_value =
            local_object_id && GetTuple(local_object_id).IsValue();
        bool object_is_pending_value =
            ContainsKey(wc->foreign.value_ids_set, tuple.object());
        if (object_is_local_value || object_is_pending_value) {
          if (wc->pending.nodes[tuple.subject()] == 0) {
            wc->pending.nodes[tuple.subject()] = tuple.object();
          }  // else this is an additional pk link for the same subject.
        }    // else object is non-existent or not a Value.
      }      // else subject is in the prefix, non-existent or not a Node.
      const ID pk_value_id =
          local_object_id ? local_object_id : wc->MapIDAlias(tuple.object(), 0);
      CHECK_NE(pk_value_id, 0);
      wc->AddNodeOrAlias(pk_value_id, tuple.subject());
    }
  } else {
    LOG(FATAL) << "Invalid Record kind.";
  }
}

void TupleStore::InspectPrefixTuplesCb(WriteContext* wc, const Record& tuple) {
  if (tuple.IsNode()) {
    wc->prefix.node_ids.insert(tuple.id());
  } else if (tuple.IsValue()) {
    wc->prefix.value_ids.insert(tuple.id());
  } else {
    CHECK(tuple.IsLink());
    wc->prefix.link_ids.insert(tuple.id());
  }
}

void TupleStore::GatherForeignNodesAndValues(WriteContext* wc) {
  // This is the first pass over the input to Write().
  // It populates the following WriteContext fields, which unless otherwise
  // specified are append-only, immutable after this function returns, and
  // contain only foreign ids:
  //
  //   pending.schema:
  //     All schema defined in the input.  May correspond in part or as
  //     a whole with schema already in the database.  The brand new parts
  //     must be understood before they are written in order to deal with
  //     compound properties.  pending.schema is populated with the help of
  //     foreign.metaschema_scanner.
  //
  //   pending.nodes:
  //     All input Nodes, stored as a map from Node id to primary key Value id.
  //     Each entry's primary key Value id starts out as 0 and is updated when
  //     the first relevant primary key link is encountered, possibly never.
  //
  //   foreign.value_ids,
  //   foreign.value_strings:
  //     All input Value id/string pairs, stored as parallel vectors.  Used
  //     later by MapForeignToLocalValues() to find and map to interned Values.
  //
  //   foreign.value_ids_set:
  //     Same contents as foreign.value_ids arranged for faster
  //     membership testing.  Used for Link error checking.
  //
  //   foreign.pk_link_ids,
  //   foreign.pk_subjects,
  //   foreign.pk_objects:
  //     All input primary key Links, stored as parallel vectors.  Used
  //     later by MapForeignToLocalNodes() to find and map to local Nodes.
  //
  //   foreign.subgraphs:
  //     All non-zero input Link subgraph field values.  Used later by
  //     ValidateLocalSubgraphs() to batch load/validate prefix subgraph
  //     unions.
  //
  //   foreign.string_to_value_id:
  //     Distinct input Values, stored as a map of string to first
  //     occurrence Value id.  Used to build alias_map.
  //
  //   foreign.pk_value_id_to_node_id:
  //     Distinct input Nodes, stored as a map of first occurrence
  //     primary key Value id to first occurrence Node id.  Used to build
  //     alias_map.
  //
  //   alias_map:
  //     All input Nodes and Values, stored as a map from Node or Value id
  //     to the first occurrence of that Node or Value in the input.  Value
  //     equivalence is determined by the Value string.  Node equivalence is
  //     determined by primary key Value string.  This map may be updated
  //     in subsequent phases of Write() when other Node identification
  //     mechanisms come into play (see MapForeignToLocalCompounds()).
  //
  //   results.id_map:
  //     The main foreign-to-local map from foreign input tuple ids to local
  //     result tuple ids.  In this phase of Write(), the only entries added
  //     are ones corresponding to error conditions (entry value = 0).  This
  //     map is updated in several other phases of Write().
  //
  //   prefix.ids:
  //     All input Link references to prefix tuples.  Used to determine whether
  //     each is a Node, Value or Link in InspectPrefixTuplesCb().
  //
  //   prefix.node_ids,
  //   prefix.value_ids:
  //   prefix.link_ids:
  //     Results of InspectPrefixTuplesCb().
  unique_ptr<ObjectGraph> accumulator(
      wc->foreign.metaschema_scanner->ObjectGraphWithPredicates());
  unique_ptr<ForEachRecordCallback> callback(NewPermanentCallback(
      this, &TupleStore::GatherForeignNodesAndValuesCb, wc, accumulator.get()));
  wc->foreign.source->ForEachRecord(callback.get());
  wc->pending.schema.CreateDomainInstances(accumulator.get());
  if (!wc->prefix.ids.empty()) {
    callback.reset(
        NewPermanentCallback(this, &TupleStore::InspectPrefixTuplesCb, wc));
    base_->ForEachRecordInSet(wc->prefix.ids, callback.get(), false);
  }
}

void TupleStore::MapForeignToLocalValues(WriteContext* wc) {
  // Find input Values that already exist in the TupleStore and add
  // corresponding entries to results.id_map, the foreign-to-local id map.
  // All remaining (new) Values get entries in the pending.value_to_string
  // map, which is immutable after this function returns.  The new Values
  // will be written later only if referenced.
  //
  // Populates the following WriteContext fields:
  //   pending.value_to_string:
  //
  // Extends the following WriteContext fields:
  //   results.id_map:
  vector<ID> local_value_ids;
  if (wc->foreign.value_strings.size() > 0) {
    base_->FindValues(wc->foreign.value_strings, &local_value_ids);
  }
  // Add foreign/local mapping for Values.
  for (int i = 0; i < local_value_ids.size(); ++i) {
    const string& val = wc->foreign.value_strings[i];
    const ID f_id = wc->foreign.value_ids[i];
    const ID l_id = local_value_ids[i];
    if (l_id != 0) {
      wc->AddIDMapEntry(f_id, l_id);
    } else {
      wc->pending.value_to_string[f_id] = val;
    }
  }
}

void TupleStore::MapForeignToLocalNodes(WriteContext* wc) {
  // Find input Nodes that already exist in the TupleStore and add
  // corresponding entries to results.id_map, the foreign-to-local id map.
  // All remaining (new) Nodes get entries in the pending.pk_node_to_links
  // map, which is immutable after this function returns.  The new Nodes
  // will be written later only if referenced.  Also populates replaced_by_map
  // for local Nodes that have already been replaced in the database.
  // replaced_by_map is extended by WriteLink() as additional replaced_by
  // links in the input (and synthetic replaced_by links) are written.
  //
  // Populates the following WriteContext fields:
  //   pending.pk_node_to_links
  //   replaced_by_map
  //
  // Extends the following WriteContext fields:
  //   results.id_map
  // Translate primary key Value ids foreign-to-local.
  vector<ID> local_pk_value_ids;
  for (int i = 0; i < wc->foreign.pk_objects.size(); ++i) {
    local_pk_value_ids.push_back(
        FindWithDefault(*wc->results.id_map, wc->foreign.pk_objects[i], 0));
  }
  // Get the local Node ids for local primary key Values.
  vector<ID> local_pk_node_ids;
  base_->FindNodeIDsByPrimaryKeys(local_pk_value_ids, &local_pk_node_ids);
  // Add foreign/local mapping for Nodes.
  IDSet local_node_ids;
  for (int i = 0; i < local_pk_node_ids.size(); ++i) {
    const ID l_pkn_id = local_pk_node_ids[i];
    const ID f_pkl_id = wc->foreign.pk_link_ids[i];
    const ID f_pkn_id = wc->foreign.pk_subjects[i];
    if (l_pkn_id) {
      wc->AddIDMapEntry(f_pkn_id, l_pkn_id);
      local_node_ids.insert(l_pkn_id);
    } else {
      wc->pending.pk_node_to_links[f_pkn_id].push_back(f_pkl_id);
    }
  }
  FindReplacedByMapping(local_node_ids, &wc->replaced_by_map);
}

// TODO:
// There are three cases in total, could be handled separately:
// For local subgraphs from foreign links:
// 1) Local hub is identified by spokes - we already have the spokes.
// 2) Local hub is referenced as prefix id - must get the spokes, verify valid.
// For latest live subgraphs from SPO interns:
// 3) They're known valid because they're already in the db - cache the spokes.
void TupleStore::ValidateLocalSubgraphs(WriteContext* wc,
                                        const IDSet& local_subgraph_unions) {
  //   cspo_to_hub_id:
  //   valid_subgraph_unions:
  //   subgraph_union_to_nodes:
  // TODO:
  // Improve performance by making bulk/parallel.
  for (ID subgraph_union : local_subgraph_unions) {
    if (subgraph_union == BootstrapConstants().b_mgp_subgraph_union) {
      continue;
    }
    IDSet subgraph_nodes = GetSubgraphNodes(subgraph_union);
    if (!subgraph_nodes.empty()) {
      vector<IDPair> spokes;
      for (ID subgraph : subgraph_nodes) {
        spokes.push_back(
            IDPair(BootstrapConstants().b_p_subgraph_union, subgraph));
      }
      auto cspo_key = CSPOKey(spokes);
      CHECK(!ContainsKey(wc->cspo_to_hub_id, cspo_key));
      CHECK(!ContainsKey(wc->valid_subgraph_unions, subgraph_union));
      CHECK(!ContainsKey(wc->subgraph_union_to_nodes, subgraph_union));
      wc->cspo_to_hub_id[cspo_key] = subgraph_union;
      wc->valid_subgraph_unions.insert(subgraph_union);
      wc->subgraph_union_to_nodes[subgraph_union] = subgraph_nodes;
    }
  }
}

void TupleStore::FindLatestLiveSubgraphsCb(WriteContext* wc,
                                           vector<ID>* local_id,
                                           vector<UUID>* local_spo_key,
                                           vector<LinkState>* local_dead,
                                           vector<ID>* latest_live_subgraph,
                                           const Record& tuple) {
  local_id->push_back(tuple.id());
  local_spo_key->push_back(SPOKey(tuple));
  local_dead->push_back(tuple.state());
  if (tuple.dead()) {
    // Placeholder - to be looked up in a subsequent pass.
    latest_live_subgraph->push_back(0);
  } else {
    latest_live_subgraph->push_back(tuple.subgraph());
  }
}

void TupleStore::FindLatestLiveSubgraphs(WriteContext* wc,
                                         IDSet* latest_live_subgraphs) {
  // Populates the following WriteContext fields:
  //   simple.local_link_id_by_spo_key
  //   simple.dead_by_local_link_id
  //   simple.subgraph_by_local_link_id
  vector<ID> local_link_ids;
  FindLinkBySPO(wc->simple.local_spo_keys, &local_link_ids);
  // Remove duplicates and 0s.
  IDSet local_link_ids_set;
  for (auto id : local_link_ids) {
    if (id) {
      local_link_ids_set.insert(id);
    }
  }
  // Find local dead and latest live subgraph if live.
  vector<ID> local_id;
  vector<UUID> local_spo_key;
  vector<LinkState> local_state;
  vector<ID> latest_live_subgraph;
  unique_ptr<ForEachRecordCallback> callback(NewPermanentCallback(
      this, &TupleStore::FindLatestLiveSubgraphsCb, wc, &local_id,
      &local_spo_key, &local_state, &latest_live_subgraph));
  ForEachRecordInSet(local_link_ids_set, callback.get(), false);
  // Find latest live subgraph if dead.
  // This involves stepping backward through link version chains.
  // TODO: Perform WHEN testing here as well.
  for (int i = 0; i < local_link_ids_set.size(); ++i) {
    ID local_link_id = local_id[i];
    auto spo_key = local_spo_key[i];
    auto state = local_state[i];
    ID subgraph = latest_live_subgraph[i];
    if (state == LinkState::kDead) {
      CHECK_EQ(subgraph, 0);
      IDSet ids;
      FindIDsBySPO(spo_key, &ids);
      for (auto it = ids.rbegin(); it != ids.rend(); ++it) {
        if (it != ids.rbegin()) {
          // The second is the one.
          ID interned_id = *it;
          const Record interned_tuple = GetTuple(interned_id);
          CHECK(!interned_tuple.dead());
          subgraph = interned_tuple.subgraph();
          latest_live_subgraph[i] = subgraph;
          break;
        }
      }
    }
    if (subgraph) {
      latest_live_subgraphs->insert(subgraph);
    }
    CHECK(!ContainsKey(wc->simple.local_link_id_by_spo_key, spo_key));
    wc->simple.local_link_id_by_spo_key[spo_key] = local_link_id;
    CHECK(!ContainsKey(wc->simple.dead_by_local_link_id, local_link_id));
    wc->simple.dead_by_local_link_id[local_link_id] = state;
    CHECK(!ContainsKey(wc->simple.subgraph_by_local_link_id, local_link_id));
    wc->simple.subgraph_by_local_link_id[local_link_id] = subgraph;
  }
}

void TupleStore::GatherCompoundSpokesWithHubsCb(WriteContext* wc,
                                                const Record& foreign_tuple) {
  const Record& tuple = foreign_tuple;
  if (!tuple.IsLink()) {
    return;
  }
  ID foreign_subject = tuple.subject();
  ID foreign_predicate = tuple.predicate();
  ID local_predicate = wc->MapIDForeignToLocal(foreign_predicate);
  ID foreign_object = tuple.object();
  ID local_object = wc->MapIDForeignToLocal(foreign_object);
  bool is_none = (local_predicate == BootstrapConstants().b_p_none);
  if (is_none) {
    foreign_predicate = foreign_object;
    local_predicate = local_object;
  }
  // Choose from local or foreign (pending) schema.
  const SimpleProperty* sp = nullptr;
  if (local_predicate != 0) {
    sp = FindSimplePropertyByID(local_predicate);
  } else {
    CHECK_GT(foreign_predicate, 0);
    sp = wc->pending.schema.FindSimplePropertyByID(foreign_predicate);
  }
  if (sp == nullptr) {
    return;
  }
  const CompoundProperty* cp = FindCompoundParent(*sp);
  if (cp == nullptr) {
    return;
  }
  // Is subject or object the hub?
  Linkage hub_linkage = GetHubLinkage(*cp, sp->tuple_id());
  ID hub_id = tuple.linkage(hub_linkage);
  ID non_hub_id = tuple.linkage(InverseLinkage(hub_linkage));
  if (is_none) {
    // The hub id is always in the subject field of a /b/p/none link.
    hub_id = foreign_subject;
    non_hub_id = 0;
  }
  if (!ContainsKey(wc->foreign.compounds, hub_id)) {
    wc->foreign.compounds[hub_id] =
        CompoundDescriptor(CompoundPropWithSource(cp, local_predicate == 0),
                           vector<SpokeDescriptor>());
  }
  wc->foreign.compounds[hub_id].second.push_back(
      SpokeDescriptor(tuple.id(), foreign_predicate, non_hub_id));
}

void TupleStore::GatherCompoundSpokesWithHubs(WriteContext* wc) {
  // This is the second pass over the input to Write().
  // It groups compound spoke ids (which may not be contiguous in the input)
  // with their hub ids in WriteContext field foreign.compounds, which is
  // immutable after this function returns.
  //
  // Populates the following WriteContext fields:
  //
  //   foreign.compounds:
  unique_ptr<ForEachRecordCallback> callback(NewPermanentCallback(
      this, &TupleStore::GatherCompoundSpokesWithHubsCb, wc));
  wc->foreign.source->ForEachRecord(callback.get());
}

void TupleStore::GatherLocalSPOKeysCb(WriteContext* wc,
                                      const Record& foreign_tuple) {
  const Record& tuple = foreign_tuple;
  if (!tuple.IsLink()) {
    return;
  }
  ID foreign_subject = tuple.subject();
  ID local_subject = wc->MapIDForeignToLocal(foreign_subject);
  ID foreign_predicate = tuple.predicate();
  ID local_predicate = wc->MapIDForeignToLocal(foreign_predicate);
  ID foreign_object = tuple.object();
  ID local_object = wc->MapIDForeignToLocal(foreign_object);
  if (local_subject && local_predicate && local_object) {
    wc->simple.local_spo_keys.push_back(
        SPOKey(local_subject, local_predicate, local_object));
  }
}

void TupleStore::GatherLocalSPOKeys(WriteContext* wc) {
  // This is the third pass over the input to Write().
  // It aggregates SPO keys for all input simple properties with S, P,
  // and O known to already exist in the database.  These simple properties
  // may also already exist in the database - they are looked up as a batch
  // in FindLatestLiveSubgraphs().
  //
  // Populates the following WriteContext fields:
  //
  //   simple.local_spo_keys:
  unique_ptr<ForEachRecordCallback> callback(
      NewPermanentCallback(this, &TupleStore::GatherLocalSPOKeysCb, wc));
  wc->foreign.source->ForEachRecord(callback.get());
}

bool TupleStore::MakeLocalCSPOKey(WriteContext* wc, ID foreign_hub_id,
                                  UUID* out_cspo_key,
                                  IDSet* out_non_hub_ids) const {
  // If all the spoke predicates and non_hub_ids exist locally,
  // return the local cspo key and the corresponding set of local
  // non_hub_ids in out params and return true, otherwise return false.
  // A return value of true does not necessarily mean that all the
  // spokes have been written.
  const CompoundDescriptor descriptor =
      FindOrDie(wc->foreign.compounds, foreign_hub_id);
  const IDSet& identifying_spoke_ids =
      FindOrDie(wc->foreign.hub_to_identifying_spokes, foreign_hub_id);
  const vector<SpokeDescriptor>& foreign_spokes = descriptor.second;
  vector<IDPair> local_spokes;
  for (auto foreign_spoke : foreign_spokes) {
    ID foreign_link_id = foreign_spoke.link_id;
    if (ContainsKey(identifying_spoke_ids, foreign_link_id)) {
      ID local_predicate = wc->MapIDForeignToLocal(foreign_spoke.predicate);
      ID local_non_hub_id = wc->MapIDForeignToLocal(foreign_spoke.non_hub_id);
      if (local_predicate && local_non_hub_id) {
        local_spokes.push_back(IDPair(local_predicate, local_non_hub_id));
      }
    }
  }
  if (local_spokes.size() != identifying_spoke_ids.size()) {
    return false;
  }
  *out_cspo_key = CSPOKey(local_spokes);
  if (out_non_hub_ids) {
    for (auto local_spoke : local_spokes) {
      out_non_hub_ids->insert(local_spoke.second);
    }
  }
  return true;
}

void TupleStore::WriteTupleCb(WriteContext* wc, const Record& foreign_tuple) {
  const ID foreign_id = foreign_tuple.id();
  if (wc->HasIDMapEntry(foreign_id)) {
    // Record has been mapped to an existing Record - do not write.
    return;
  }
  if (foreign_tuple.IsNode()) {
    // Record will be written if and when referenced.
    CHECK(ContainsKey(wc->pending.nodes, foreign_id));
    return;
  } else if (foreign_tuple.IsValue()) {
    // Record will be written if and when referenced.
    CHECK(ContainsKey(wc->pending.value_to_string, foreign_id));
    return;
  } else {
    CHECK(foreign_tuple.IsLink());
    WriteLink(wc, foreign_tuple);
  }
}

void TupleStore::NormalizeCompoundSpokes(
    const WriteContext& wc, bool is_foreign_cp,
    const vector<SpokeDescriptor>& foreign_spokes,
    vector<IDPair>* normalized_spokes, IDSet* identifying_spoke_ids,
    IDSet* extra_spoke_ids) const {
  // Normalized means:
  // - aliases resolved
  // - /b/p/none links filtered out
  // - extra (cardinality overflow) links filtered to the side
  // - if local schema exists, use local predicate
  set<std::tuple<ID, ID, ID> > distinct_spokes;
  // Counts of input spokes per predicate.
  HubCardinalityMap input_cardinality_map;
  bool cardinality_overflow = false;
  for (auto spoke : foreign_spokes) {
    // Use default if predicate is a prefix id.
    ID predicate = wc.MapIDAlias(spoke.predicate, spoke.predicate);
    // Use default if non_hub_id is a prefix id or it lacks a primary key.
    ID non_hub_id = wc.MapIDAlias(spoke.non_hub_id, spoke.non_hub_id);
    if (!non_hub_id) {
      // spoke is a /b/p/none link.
      continue;
    }
    // If cp is local, use local predicates in normalized spokes.
    if (!is_foreign_cp) {
      predicate = wc.MapIDForeignToLocal(predicate);
      CHECK_NE(predicate, 0);
    }
    const Record link_tuple = wc.foreign.source->GetTuple(spoke.link_id);
    std::tuple<ID, ID, ID> key(predicate, non_hub_id, link_tuple.subgraph());
    if (ContainsKey(distinct_spokes, key)) {
      // spoke is a duplicate.
      // Do not include it in the compound key.
      // Do not treat it as a compound cardinality violator.
      continue;
    } else {
      distinct_spokes.insert(key);
    }
    const SimpleProperty* sp =
        is_foreign_cp ? wc.pending.schema.FindSimplePropertyByID(predicate)
                      : FindSimplePropertyByID(predicate);
    const CompoundProperty* cp = FindCompoundParent(*sp);
    int current_cardinality =
        FindWithDefault(input_cardinality_map, predicate, 0);
    unique_ptr<HubCardinalityMap> hub_cardinality_map = HubCardinalities(*cp);
    int max_cardinality = FindOrDie(*hub_cardinality_map, predicate);
    if (max_cardinality && (current_cardinality == max_cardinality)) {
      // If any spoke overflows, all remaining spokes are considered extras.
      cardinality_overflow = true;
    }
    if (!cardinality_overflow) {
      identifying_spoke_ids->insert(spoke.link_id);
      normalized_spokes->push_back(IDPair(predicate, non_hub_id));
      input_cardinality_map[predicate] += 1;
    } else {
      extra_spoke_ids->insert(spoke.link_id);
    }
  }
}

void TupleStore::MapForeignToLocalCompounds(WriteContext* wc) {
  // Populates the following WriteContext fields:
  //   foreign.hub_to_identifying_spokes
  //   foreign.normalized_cspo_to_hub_id
  //   compound.foreign_hub_to_local_subsets
  //   compound.foreign_hub_to_foreign_subsets
  //   compound.local_hub_to_spoke_count
  //   compound.foreign_hub_to_spoke_count
  //
  // Extends the following WriteContext fields:
  //   alias_map
  //   results.id_map
  vector<ID> foreign_hub_ids;
  vector<UUID> local_cspo_keys;
  map<ID, vector<IDPair> > foreign_hub_to_local_spokes;
  hash_map<ID, vector<IDPair> > foreign_hub_to_normalized_spokes;
  // Check for compound cardinality overflow.
  // Identifies compound aliases.
  // Finds spoke subsets that may already exist locally.
  for (auto kv : wc->foreign.compounds) {
    const ID hub_id = kv.first;
    const CompoundDescriptor descriptor = kv.second;
    const CompoundProperty* cp = descriptor.first.first;
    const bool is_foreign_cp = descriptor.first.second;
    const vector<SpokeDescriptor>& foreign_spokes = descriptor.second;
    vector<IDPair> normalized_spokes;
    IDSet extra_spoke_ids;
    NormalizeCompoundSpokes(
        *wc, is_foreign_cp, foreign_spokes, &normalized_spokes,
        &wc->foreign.hub_to_identifying_spokes[hub_id], &extra_spoke_ids);
    // TODO:
    // Handle cardinality overflow differently for mutable compounds as
    // described in http://go/blythe-compound-property-cardinality-overflow.
    for (ID extra_spoke_id : extra_spoke_ids) {
      LOG(ERROR) << "Discarding extra compound spoke: " << extra_spoke_id;
      wc->AddIDMapError(extra_spoke_id);
    }
    UUID cspo_key = CSPOKey(normalized_spokes);
    const ID first_hub_id = wc->AddCompoundOrAlias(cspo_key, hub_id);
    if (first_hub_id == hub_id) {
      vector<IDPair> local_spokes;
      for (auto spoke : normalized_spokes) {
        if (!is_foreign_cp) {
          ID local_non_hub_id = wc->MapIDForeignToLocal(spoke.second);
          if (local_non_hub_id) {
            local_spokes.push_back(IDPair(spoke.first, local_non_hub_id));
          }
        }
      }
      if (local_spokes.size() == normalized_spokes.size()) {
        // To be used for exact matching in base.
        foreign_hub_ids.push_back(hub_id);
        local_cspo_keys.push_back(CSPOKey(local_spokes));
      }
      if (cp->is_mutable()) {
        if (!local_spokes.empty()) {
          // To be used for subset/superset matching in base.
          foreign_hub_to_local_spokes[hub_id] = local_spokes;
        }
        // To be used for subset/superset matching in input.
        foreign_hub_to_normalized_spokes[hub_id] = normalized_spokes;
      }
    }
  }
  // Find exact matches in base.
  vector<ID> local_hub_ids;
  base_->FindHubByCSPO(local_cspo_keys, &local_hub_ids);
  CHECK_EQ(foreign_hub_ids.size(), local_hub_ids.size());
  for (int i = 0; i < local_hub_ids.size(); ++i) {
    const ID local_hub_id = local_hub_ids[i];
    if (local_hub_id) {
      const ID foreign_hub_id = foreign_hub_ids[i];
      wc->AddIDMapEntry(foreign_hub_id, local_hub_id);
    }
  }
  // Find supersets and subsets in base.
  for (auto it : foreign_hub_to_local_spokes) {
    ID foreign_hub_id = it.first;
    if (!wc->HasIDMapEntry(foreign_hub_id)) {
      const vector<IDPair>& local_spokes = it.second;
      const CompoundDescriptor& descriptor =
          FindOrDie(wc->foreign.compounds, foreign_hub_id);
      const CompoundProperty* cp = descriptor.first.first;
      const bool is_foreign_cp = descriptor.first.second;
      CHECK_EQ(is_foreign_cp, false);
      IDSet superset_hub_ids;
      base_->FindSupersetHubs(*cp, local_spokes, &superset_hub_ids);
      if (!superset_hub_ids.empty()) {
        // TODO:
        // Never gets here - needs a test.
        const ID local_hub_id = *superset_hub_ids.rbegin();
        wc->AddIDMapEntry(foreign_hub_id, local_hub_id);
        continue;
      }
      hash_map<ID, int> hub_id_to_spoke_count;
      base_->FindSubsetHubs(*cp, local_spokes, &hub_id_to_spoke_count);
      if (!hub_id_to_spoke_count.empty()) {
        // TODO:
        // Never gets here - needs a test.
        CHECK(!ContainsKey(wc->compound.foreign_hub_to_local_subsets,
                           foreign_hub_id));
        InsertKeysFromMap(
            hub_id_to_spoke_count,
            &wc->compound.foreign_hub_to_local_subsets[foreign_hub_id]);
        for (auto kv : hub_id_to_spoke_count) {
          CHECK(!ContainsKey(wc->compound.local_hub_to_spoke_count, kv.first));
          wc->compound.local_hub_to_spoke_count.insert(kv);
        }
      }
    }
  }
  // Find supersets and subsets in input.
  hash_map<IDPair, IDSet> spoke_to_hub_map;
  hash_map<UUID, ID> cspo_to_hub_map;
  for (auto kv : wc->foreign.compounds) {
    ID foreign_hub_id = kv.first;
    if (ContainsKey(foreign_hub_to_normalized_spokes, foreign_hub_id)) {
      const vector<IDPair>& normalized_spokes =
          FindOrDie(foreign_hub_to_normalized_spokes, foreign_hub_id);
      // Find supersets.
      IDSet running_intersection;
      for (auto it = normalized_spokes.begin(); it != normalized_spokes.end();
           ++it) {
        auto spoke = *it;
        IDSet node_ids = FindWithDefault(spoke_to_hub_map, spoke, IDSet());
        if (it == normalized_spokes.begin()) {
          running_intersection = node_ids;
        } else {
          Intersection(node_ids, &running_intersection);
        }
      }
      if (!running_intersection.empty()) {
        ID superset_hub_id = *running_intersection.rbegin();
        wc->AddAliasMapEntry(foreign_hub_id, superset_hub_id, true);
        continue;
      }
      // Find subsets.
      hash_map<ID, int> hub_id_to_spoke_count;
      FindInputSubsetHubs(cspo_to_hub_map, normalized_spokes,
                          &hub_id_to_spoke_count);
      if (!hub_id_to_spoke_count.empty()) {
        CHECK(!ContainsKey(wc->compound.foreign_hub_to_foreign_subsets,
                           foreign_hub_id));
        InsertKeysFromMap(
            hub_id_to_spoke_count,
            &wc->compound.foreign_hub_to_foreign_subsets[foreign_hub_id]);
        for (auto kv : hub_id_to_spoke_count) {
          CHECK(
              !ContainsKey(wc->compound.foreign_hub_to_spoke_count, kv.first));
          wc->compound.foreign_hub_to_spoke_count.insert(kv);
        }
      }
      for (auto spoke : normalized_spokes) {
        spoke_to_hub_map[spoke].insert(foreign_hub_id);
      }
      UUID cspo_key = CSPOKey(normalized_spokes);
      CHECK(!ContainsKey(cspo_to_hub_map, cspo_key));
      cspo_to_hub_map[cspo_key] = foreign_hub_id;
    }
  }
}

void TupleStore::WriteSimpleProperties(WriteContext* wc) {
  unique_ptr<ForEachRecordCallback> callback(
      NewPermanentCallback(this, &TupleStore::WriteTupleCb, wc));
  wc->foreign.source->ForEachRecord(callback.get());
}

void TupleStore::WriteNode(WriteContext* wc, ID foreign_id, ID pk_value_id) {
  CHECK(!wc->HasIDMapEntry(foreign_id));
  // Test for duplicate Node in Write() input.
  if (pk_value_id != 0) {
    if (wc->HasIDMapEntry(pk_value_id)) {
      ID local_pk_value_id = wc->GetIDMapEntry(pk_value_id);
      PgiIDSet s = indexes_.FindIDsByIntersection(
          kPredicateObjectIndex, bc::b_p_primary_key, local_pk_value_id);
      ID interned_id = s.Max();
      if (interned_id != 0) {
        wc->AddIDMapEntry(foreign_id, interned_id);
        return;
      }
    }
  }
  ID local_id = AppendNode();
  wc->AddIDMapEntry(foreign_id, local_id);
  ID aliased_id = wc->MapIDAlias(foreign_id, 0);
  if (aliased_id && (aliased_id != foreign_id)) {
    if (!wc->HasIDMapEntry(aliased_id)) {
      // An alias Node was written before the first/aliased Node.
      wc->AddIDMapEntry(aliased_id, local_id);
    }
  }
}

void TupleStore::WriteValue(WriteContext* wc, ID foreign_id) {
  string str = FindOrDie(wc->pending.value_to_string, foreign_id);
  ID local_id = AppendValue(str);
  wc->AddIDMapEntry(foreign_id, local_id);
  ID aliased_id = wc->MapIDAlias(foreign_id, 0);
  if (aliased_id && (aliased_id != foreign_id)) {
    if (!wc->HasIDMapEntry(aliased_id)) {
      // An alias Value was written before the first/aliased Value.
      wc->AddIDMapEntry(aliased_id, local_id);
    }
  }
}

ID TupleStore::FindExtCompound(WriteContext* wc, ID foreign_hub_id) {
  UUID cspo_key;
  if (MakeLocalCSPOKey(wc, foreign_hub_id, &cspo_key, nullptr)) {
    return FindWithDefault(wc->cspo_to_hub_id, cspo_key, 0);
  }
  return 0;
}

void TupleStore::WriteLink(WriteContext* wc, const Record& foreign_tuple) {
  const ID foreign_id = foreign_tuple.id();
  CHECK_NE(foreign_id, 0);
  if (BootstrapConstants().b_p_subgraph_union ==
      wc->MapIDForeignToLocal(foreign_tuple.predicate())) {
    if (!wc->HasIDMapEntry(foreign_tuple.subject())) {
      ID interned_id = FindExtCompound(wc, foreign_tuple.subject());
      if (interned_id) {
        // A subgraph union created on the fly has duplicated pending input.
        wc->AddIDMapEntry(foreign_tuple.subject(), interned_id);
      }
    }
  }
  // Nodes and values are written upon first reference, but
  // only after all potential error conditions have been checked.
  IDSet nodes_to_write;
  IDSet values_to_write;
  for (auto l : kAllLinkages) {
    ID linked_id = foreign_tuple.linkage(l);
    if (linked_id != 0) {
      if (wc->HasIDMapEntry(linked_id)) {
        // Linkage target has already been written.
        CHECK_NE(wc->GetIDMapEntry(linked_id), 0);
        continue;
      }
      ID aliased_id = wc->MapIDAlias(linked_id, 0);
      if ((aliased_id != 0) && (aliased_id != linked_id)) {
        if (wc->HasIDMapEntry(aliased_id)) {
          ID local_id = wc->GetIDMapEntry(aliased_id);
          CHECK_NE(local_id, 0);
          wc->AddIDMapEntry(linked_id, local_id);
          continue;
        }
      }
      if (ContainsKey(wc->pending.nodes, linked_id)) {
        nodes_to_write.insert(linked_id);
      } else if (ContainsKey(wc->pending.value_to_string, linked_id)) {
        values_to_write.insert(linked_id);
      } else if (!ContainsKey(wc->prefix.node_ids, linked_id) &&
                 !ContainsKey(wc->prefix.value_ids, linked_id)) {
        LOG(ERROR) << "Link field references a prefix tuple (" << linked_id
                   << ") that is not a Node or Value:\n"
                   << foreign_tuple.ToDebugString();
        wc->AddIDMapError(foreign_id);
        return;
      }
    }
  }
  ID foreign_subject = foreign_tuple.subject();
  ID foreign_predicate = foreign_tuple.predicate();
  ID foreign_object = foreign_tuple.object();
  ID foreign_author = foreign_tuple.author();
  ID foreign_subgraph = foreign_tuple.subgraph();
  ID local_subject = wc->MapIDForeignToLocal(foreign_subject);
  ID local_predicate = wc->MapIDForeignToLocal(foreign_predicate);
  ID local_object = wc->MapIDForeignToLocal(foreign_object);
  ID local_author = wc->MapIDForeignToLocal(foreign_author);
  ID local_subgraph = wc->MapIDForeignToLocal(foreign_subgraph);
  if (local_predicate == bc::b_p_primary_key) {
    if (foreign_subgraph) {
      LOG(ERROR) << "Primary key links cannot specify subgraph: "
                 << foreign_tuple.ToDebugString();
      wc->AddIDMapError(foreign_id);
      return;
    }
    CHECK_EQ(local_subgraph, 0);
    if (!local_subject) {
      // Deferred until Node is referenced in some other way.
      CHECK(ContainsKey(wc->pending.pk_node_to_links, foreign_subject));
      return;
    }
    CHECK_NE(local_subject, 0);
    bool object_is_value =
        ContainsKey(values_to_write, foreign_object) ||
        ContainsKey(wc->prefix.value_ids, foreign_object) ||
        (local_object &&
         ContainsKey(wc->foreign.value_ids_set, foreign_object));
    if (!object_is_value) {
      LOG(ERROR) << "Primary keys must be values:\n"
                 << foreign_tuple.ToDebugString();
      wc->AddIDMapError(foreign_id);
      return;
    }
    // If subject is already written and it has a different primary key,
    // write special replaced_by structure that's like an alternate key.
    if (!ContainsKey(nodes_to_write, foreign_subject)) {
      ID primary_key = FindPrimaryKeyByNodeID(local_subject);
      if (primary_key != 0) {
        if (foreign_tuple.dead()) {
          LOG(ERROR) << "Primary keys may not be deleted:\n"
                     << foreign_tuple.ToDebugString();
          wc->AddIDMapError(foreign_id);
          return;
        } else if (primary_key != local_object) {
          // Think of local_subject as node1.
          // Fabricate a new node, node2.
          // Modify this pk link so that the new pk attaches to node2.
          // Add link: node2 replaced_by node1.
          // Check for errors that would be caused by the generated replaced_by.
          IDSet empty_set;
          const IDSet& rb_nodes_to_write = empty_set;
          const IDSet& rb_values_to_write = empty_set;
          ID rb_foreign_subject = 0;
          ID rb_foreign_object = 0;
          ID rb_local_subject = next_id();
          ID rb_local_object = local_subject;
          if (FindReplacedByErrors(rb_nodes_to_write, rb_values_to_write,
                                   rb_foreign_subject, rb_foreign_object,
                                   rb_local_subject, rb_local_object)) {
            wc->AddIDMapError(foreign_id);
            return;
          }
          ID fabricated_node = AppendNode();
          if (ContainsKey(values_to_write, foreign_object)) {
            WriteValue(wc, foreign_object);
            local_object =
                FindWithDefault(*wc->results.id_map, foreign_object, 0);
          }
          CHECK_NE(local_object, 0);
          Record pk_link =
              Record::Link(next_id(), 0, 0, 0, fabricated_node,
                           bc::b_p_primary_key, local_object, LinkState::kLive);
          AppendRecord(pk_link);
          wc->AddIDMapEntry(foreign_id, pk_link.id());
          Record rb_link = Record::Link(
              next_id(), 0, 0, BootstrapConstants().b_mgp_subgraph_union,
              fabricated_node, BootstrapConstants().b_p_replaced_by,
              local_subject, LinkState::kLive);
          AppendRecord(rb_link);
          return;
        }
      }
    }
  } else if (local_predicate == BootstrapConstants().b_p_replaced_by) {
    if (FindReplacedByErrors(nodes_to_write, values_to_write, foreign_subject,
                             foreign_object, local_subject, local_object)) {
      wc->AddIDMapError(foreign_id);
      return;
    }
  } else if (local_predicate == BootstrapConstants().b_p_cardinality) {
    string value_string;
    if (local_object) {
      value_string = GetValue(local_object);
    } else {
      value_string = wc->foreign.source->GetValue(foreign_object);
    }
    int cardinality = -1;
    if (!safe_strto32(value_string, &cardinality) || (cardinality < 0)) {
      LOG(ERROR) << "The object of a /b/p/cardinality link must be "
                 << "a non-negative integer (was '" << value_string << "').\n"
                 << foreign_tuple.ToDebugString();
      wc->AddIDMapError(foreign_id);
      return;
    }
  } else if (local_predicate == 0) {
    LOG(ERROR) << "Error mapping predicate foreign to local:\n"
               << foreign_tuple.ToDebugString();
    wc->AddIDMapError(foreign_id);
    return;
  }
  const SimpleProperty* sp = FindSimplePropertyByID(local_predicate);
  if (sp == nullptr) {
    LOG(ERROR) << "Error loading metaproperty for predicate: "
               << foreign_tuple.ToDebugString();
    wc->AddIDMapError(foreign_id);
    return;
  }
  // Merge compound subsets if necessary.
  const CompoundProperty* cp = FindCompoundParent(*sp);
  if (cp) {
    Linkage hub_linkage = GetHubLinkage(*cp, local_predicate);
    const ID local_hub =
        (hub_linkage == kSubject) ? local_subject : local_object;
    if (!local_hub) {
      // This is the first spoke.
      const ID foreign_hub =
          (hub_linkage == kSubject) ? foreign_subject : foreign_object;
      hash_map<ID, int> local_hub_to_spoke_count;
      // Get base subset sizes.
      if (ContainsKey(wc->compound.foreign_hub_to_local_subsets, foreign_hub)) {
        const IDSet& local_subset_hubs =
            FindOrDie(wc->compound.foreign_hub_to_local_subsets, foreign_hub);
        for (ID local_subset_hub : local_subset_hubs) {
          CHECK(!ContainsKey(local_hub_to_spoke_count, local_subset_hub));
          local_hub_to_spoke_count[local_subset_hub] = FindOrDie(
              wc->compound.local_hub_to_spoke_count, local_subset_hub);
        }
      }
      // Get extension subset sizes.
      if (ContainsKey(wc->compound.foreign_hub_to_foreign_subsets,
                      foreign_hub)) {
        const IDSet& foreign_subset_hubs =
            FindOrDie(wc->compound.foreign_hub_to_foreign_subsets, foreign_hub);
        for (ID foreign_subset_hub : foreign_subset_hubs) {
          ID local_subset_hub = wc->MapIDForeignToLocal(foreign_subset_hub);
          CHECK_GT(local_subset_hub, 0);
          CHECK(!ContainsKey(local_hub_to_spoke_count, local_subset_hub));
          local_hub_to_spoke_count[local_subset_hub] = FindOrDie(
              wc->compound.foreign_hub_to_spoke_count, foreign_subset_hub);
        }
      }
      if (!local_hub_to_spoke_count.empty()) {
        ID merged_hub_id = MergeSubsetHubs(wc, local_hub_to_spoke_count);
        wc->AddIDMapEntry(foreign_hub, merged_hub_id);
      }
    }
  }
  // Write referenced Values and Nodes.
  for (ID id : values_to_write) {
    if (!wc->HasIDMapEntry(id)) {
      WriteValue(wc, id);
    }
  }
  for (ID id : nodes_to_write) {
    if (!wc->HasIDMapEntry(id)) {
      IDToIDMap::iterator pn_it = wc->pending.nodes.find(id);
      CHECK(pn_it != wc->pending.nodes.end());
      ID pk_value_id = pn_it->second;
      WriteNode(wc, id, pk_value_id);
      // Write deferred primary key links.
      if (ContainsKey(wc->pending.pk_node_to_links, id)) {
        for (auto pk_link_id : FindOrDie(wc->pending.pk_node_to_links, id)) {
          if (pk_link_id < foreign_tuple.id()) {
            if (!wc->HasIDMapEntry(pk_link_id)) {
              Record link = wc->foreign.source->GetTuple(pk_link_id);
              CHECK_EQ(link.subject(), id);
              CHECK_NE(link.id(), foreign_tuple.id());
              WriteTupleCb(wc, link);
            }
          }
        }
      }
    }
  }
  // Check once before mapping through replaced_by.
  if (local_subject && local_predicate && local_object) {
    ID interned_id =
        FindInternedLink(wc, local_subject, local_predicate, local_object,
                         local_author, local_subgraph, foreign_tuple.when(),
                         foreign_tuple.state(), nullptr, nullptr, nullptr);
    if (interned_id) {
      wc->AddIDMapEntry(foreign_id, interned_id);
      return;
    }
  }
  WriteLinkReplacing(wc, foreign_tuple, foreign_tuple.id(), true, 0);
}

ID TupleStore::FindInternedLink(WriteContext* wc, ID subject, ID predicate,
                                ID object, ID author, ID subgraph,
                                LinkTime when, LinkState state,
                                ID* live_link_to_replace, ID* adjusted_subgraph,
                                LinkState* adjusted_state) {
  if (live_link_to_replace) *live_link_to_replace = 0;
  if (adjusted_state) *adjusted_state = state;
  if (adjusted_subgraph) *adjusted_subgraph = subgraph;
  bool make_subgraph_changes = adjusted_state && adjusted_subgraph;
  auto spo_key = SPOKey(subject, predicate, object);
  ID interned_id = indexes_.FindLinkBySPO(spo_key);
  ID interned_subgraph = 0;
  auto interned_state = LinkState::kLive;
  if (interned_id) {
    // Latest version of SPO exists in extension.
    const Record& interned_tuple = PtsGetTuple(interned_id);
    interned_subgraph = interned_tuple.subgraph();
    interned_state = interned_tuple.state();
  } else {
    interned_id =
        FindWithDefault(wc->simple.local_link_id_by_spo_key, spo_key, 0);
    if (interned_id) {
      // Latest version of SPO exists in base.
      interned_subgraph =
          FindOrDie(wc->simple.subgraph_by_local_link_id, interned_id);
      interned_state = FindOrDie(wc->simple.dead_by_local_link_id, interned_id);
    }
  }
  if (!interned_id) {
    return 0;
  }
  if (interned_state == state) {
    if (state == LinkState::kLive) {
      // live + live
      if (live_link_to_replace) {
        *live_link_to_replace = interned_id;
      }
      if (!subgraph || (subgraph == interned_subgraph)) {
        // subgraph unchanged
        return interned_id;
      } else {
        // subgraph expands
        if (make_subgraph_changes) {
          if (interned_subgraph) {
            *adjusted_subgraph =
                AddSubgraphUnion(wc, author, interned_subgraph, subgraph);
          }
        }
        return 0;
      }
    } else {
      // dead + dead
      return interned_id;
    }
  } else {
    // TODO:
    // Move these round trips into an earlier phase if possible.
    if (when) {
      IDSet ids;
      FindIDsBySPO(subject, predicate, object, &ids);
      for (auto it = ids.rbegin(); it != ids.rend(); ++it) {
        ID interned_id = *it;
        const Record interned_tuple = GetTuple(interned_id);
        if ((when == interned_tuple.when()) &&
            (state == interned_tuple.state()) &&
            (subgraph == interned_tuple.subgraph())) {
          return interned_id;
        }
      }
    }
    if (state == LinkState::kLive) {
      // dead + live
      if (make_subgraph_changes) {
        if (interned_subgraph) {
          *adjusted_subgraph =
              AddSubgraphUnion(wc, author, interned_subgraph, subgraph);
        }
      }
      return 0;
    } else {
      // live + dead
      if (live_link_to_replace) {
        *live_link_to_replace = interned_id;
      }
      if (make_subgraph_changes) {
        if (subgraph) {
          if (interned_subgraph) {
            const IDSet& live_subgraphs =
                FindOrDie(wc->subgraph_union_to_nodes, interned_subgraph);
            const IDSet& dead_subgraphs =
                FindOrDie(wc->subgraph_union_to_nodes, subgraph);
            IDSet remaining_subgraphs =
                Difference(live_subgraphs, dead_subgraphs);
            if (!remaining_subgraphs.empty()) {
              *adjusted_state = LinkState::kLive;
              *adjusted_subgraph =
                  WriteSubgraphUnion(wc, author, remaining_subgraphs);
            }
          }
        }
        if (*adjusted_state == LinkState::kDead) {
          // Killer subgraph must match for dead removal.
          *adjusted_subgraph = interned_subgraph;
        }
      }
      return 0;
    }
  }
}

// If parameter 'foreign_id' is non-zero, then parameter 'tuple' is a
// foreign tuple requiring foreign-to-local and replaced-by mapping of its
// linkage values.  Otherwise parameter 'tuple' is a synthetic tuple with
// ready-to-write linkage values.
//
// Parameters 'propagate_replace' and 'recursive_trigger_link_id' control
// recursion when handling /b/p/replaced_by links.
void TupleStore::WriteLinkReplacing(WriteContext* wc, const Record& tuple,
                                    ID foreign_id, bool propagate_replace,
                                    ID recursive_trigger_link_id) {
  ID local_subject =
      foreign_id ? wc->MapIDForeignToLocal(tuple.subject()) : tuple.subject();
  ID local_predicate = foreign_id ? wc->MapIDForeignToLocal(tuple.predicate())
                                  : tuple.predicate();
  ID local_object =
      foreign_id ? wc->MapIDForeignToLocal(tuple.object()) : tuple.object();
  ID local_author =
      foreign_id ? wc->MapIDForeignToLocal(tuple.author()) : tuple.author();
  ID local_subgraph =
      foreign_id ? wc->MapIDForeignToLocal(tuple.subgraph()) : tuple.subgraph();
  auto local_state = tuple.state();
  CHECK_NE(local_subject, 0);
  CHECK_NE(local_predicate, 0);
  CHECK_NE(local_object, 0);
  CHECK((local_predicate != bc::b_p_primary_key) || (local_subgraph == 0));
  CHECK(propagate_replace || !foreign_id);
  if (foreign_id) {
    local_subject =
        FindWithDefault(wc->replaced_by_map, local_subject, local_subject);
    local_object =
        FindWithDefault(wc->replaced_by_map, local_object, local_object);
  }
  if (!ContainsKey(wc->valid_subgraph_unions, local_subgraph)) {
    if (!IsValidSubgraph(*wc, local_subject, local_predicate, local_object,
                         local_subgraph)) {
      LOG(ERROR) << "Invalid subgraph:"
                 << "\nwc->foreign.start_id: " << wc->foreign.start_id
                 << "\nwc->next_local_id: " << wc->next_local_id
                 << "\nlocal_subject: " << local_subject
                 << "\nlocal_predicate: " << local_predicate
                 << "\nlocal_object: " << local_object
                 << "\nlocal_subgraph: " << local_subgraph
                 << "\ntuple: " << tuple.ToDebugString();
      if (foreign_id) {
        wc->AddIDMapError(foreign_id);
      }
      return;
    }
  }
  // Check again after mapping through replaced_by.
  ID live_link_to_replace = 0;
  ID interned_id =
      FindInternedLink(wc, local_subject, local_predicate, local_object,
                       local_author, local_subgraph, tuple.when(), local_state,
                       &live_link_to_replace, &local_subgraph, &local_state);
  if (interned_id) {
    if (foreign_id) {
      wc->AddIDMapEntry(foreign_id, interned_id);
    }
    return;
  }
  // Enforce MGP constraints.
  CHECK_GT(local_predicate, 0);
  const SimpleProperty* sp = FindSimplePropertyByID(local_predicate);
  // This should not happen as either 1) WriteLink() has already performed
  // this lookup successfully once in the case of user-supplied input, and
  // we're reading from cache, or 2) we're in a recursive call dealing with
  // links already in the database.
  CHECK_NOTNULL(sp);
  if (local_state == LinkState::kLive) {
    // IDs of links to be killed.
    IDSet doomed_ids;
    // Changing subgraph of live link.
    if (live_link_to_replace) {
      doomed_ids.insert(live_link_to_replace);
    }
    ID sub_cv = 0;
    const ID obj_cv =
        FindCardinalityViolator(local_object, local_predicate, kObject,
                                object_cardinality(*sp), nullptr);
    // Find live none link, if any, that the new link will replace.
    ID none_id = FindLinkBySPO(local_subject, BootstrapConstants().b_p_none,
                               local_predicate);
    if (none_id && !GetTuple(none_id).dead()) {
      doomed_ids.insert(none_id);
    } else {
      sub_cv = FindCardinalityViolator(local_subject, local_predicate, kSubject,
                                       subject_cardinality(*sp), nullptr);
    }
    // If we are moving this Link from a replaced_by merge loser
    // to the merge winner (!propagate_replace), cardinality constraints
    // should not cause the winner's Links to be deleted.
    if (sub_cv) {
      if (!propagate_replace) {
        return;
      }
      doomed_ids.insert(sub_cv);
    }
    if (obj_cv) {
      if (!propagate_replace) {
        return;
      }
      doomed_ids.insert(obj_cv);
    }
    if (local_predicate == BootstrapConstants().b_p_none) {
      IDSet ids;
      ids.insert(local_subject);
      IDSet link_ids;
      QueryProperty(ids, kSubject, local_object, 0, &link_ids, nullptr);
      if (!link_ids.empty()) {
        doomed_ids.insert(link_ids.begin(), link_ids.end());
      }
    } else if (local_predicate == BootstrapConstants().b_p_replaced_by) {
      if (propagate_replace) {
        MoveLinks(wc, kSubject, local_subject, local_object, local_author,
                  local_subgraph, tuple.when(), recursive_trigger_link_id);
        MoveLinks(wc, kObject, local_subject, local_object, local_author,
                  local_subgraph, tuple.when(), recursive_trigger_link_id);
      }
      wc->replaced_by_map[local_subject] = local_object;
    }
    // Add tombstones for doomed links.
    IDSet::const_reverse_iterator doomed_it = doomed_ids.rbegin();
    while (doomed_it != doomed_ids.rend()) {
      ID doomed_id = *doomed_it;
      CHECK(AppendTombstone(doomed_id));
      ++doomed_it;
    }
  }
  Record new_link(Record::Link(next_id(), tuple.when(), local_author,
                               local_subgraph, local_subject, local_predicate,
                               local_object, local_state));
  CHECK(new_link.IsValidLink());
  ID id = AppendRecord(new_link);
  CHECK_EQ(id, new_link.id());
  if (foreign_id) {
    wc->AddIDMapEntry(foreign_id, id);
    ID foreign_hub_id = 0;
    const ID foreign_subject = tuple.subject();
    const ID foreign_object = tuple.object();
    if (ContainsKey(wc->foreign.compounds, foreign_subject)) {
      foreign_hub_id = foreign_subject;
    } else if (ContainsKey(wc->foreign.compounds, foreign_object)) {
      foreign_hub_id = foreign_object;
    }
    if (foreign_hub_id) {
      const IDSet& identifying_spoke_ids =
          FindOrDie(wc->foreign.hub_to_identifying_spokes, foreign_hub_id);
      if (ContainsKey(identifying_spoke_ids, foreign_id)) {
        // The link we just wrote is part of a new compound.
        UUID cspo_key;
        IDSet non_hub_ids;
        if (MakeLocalCSPOKey(wc, foreign_hub_id, &cspo_key, &non_hub_ids)) {
          if (!ContainsKey(wc->cspo_to_hub_id, cspo_key)) {
            wc->cspo_to_hub_id[cspo_key] =
                wc->MapIDForeignToLocal(foreign_hub_id);
            if (local_predicate == BootstrapConstants().b_p_subgraph_union) {
              CHECK(!ContainsKey(wc->subgraph_union_to_nodes, local_subject));
              wc->subgraph_union_to_nodes[local_subject] = non_hub_ids;
              wc->valid_subgraph_unions.insert(local_subject);
            }
          }
        }
      }
    }
  }
  CommitAndIndex();
}

void TupleStore::MoveLinks(WriteContext* wc, Linkage linkage, ID src_node_id,
                           ID dst_node_id, ID local_author, ID local_subgraph,
                           LinkTime when, ID recursive_trigger_link_id) {
  // Move Links from src Node to dst Node.
  IDSet ids;
  FindLinksWithIDAtLinkage(linkage, src_node_id, &ids);
  ids = RemoveDeadTuples(ids);
  for (ID id : ids) {
    const Record& old_link = GetTuple(id);
    if (old_link.predicate() == bc::b_p_primary_key) {
      continue;
    }
    // Any Link that identifies a dependent Node is handled by
    // replacing the dependent Node rather than moving the Link,
    // if possible (if a corresponding dependent Node exists).
    if (IsIdentifyingProperty(old_link.predicate())) {
      if (old_link.id() != recursive_trigger_link_id) {
        IDSet ids;
        ids.insert(dst_node_id);
        IDSet node_ids;
        QueryProperty(ids, linkage, old_link.predicate(), 0, nullptr,
                      &node_ids);
        if (!node_ids.empty()) {
          CHECK_EQ(node_ids.size(), 1);
          const ID new_dependent_node_id = *node_ids.begin();
          CHECK(AppendTombstone(old_link.id()));
          Record rb_link = Record::Link(
              next_id(), 0, 0, BootstrapConstants().b_mgp_subgraph_union,
              old_link.linkage(InverseLinkage(linkage)),
              BootstrapConstants().b_p_replaced_by, new_dependent_node_id,
              LinkState::kLive);
          WriteLinkReplacing(wc, rb_link, 0, true, old_link.id());
          continue;
        }
      }
    }
    const ID local_subject =
        linkage == kSubject ? dst_node_id : old_link.subject();
    const ID local_object =
        linkage == kObject ? dst_node_id : old_link.object();
    CHECK(AppendTombstone(old_link.id()));
    Record new_link(Record::Link(next_id(), when, local_author, local_subgraph,
                                 local_subject, old_link.predicate(),
                                 local_object, LinkState::kLive));
    WriteLinkReplacing(wc, new_link, 0, false, 0);
  }
}

bool TupleStore::IsValidSubgraph(const WriteContext& wc, ID subject,
                                 ID predicate, ID object, ID subgraph) const {
  if (IsValidSubgraphUnion(wc, subgraph)) {
    return true;
  }
  // A link that creates a subgraph union, in whole or in part,
  // may use that subgraph union as its own subgraph (i.e.,
  // in both the subject and subgraph linkages).  This is permitted
  // so that a subgraph can contain its own identifier.
  if ((subject == subgraph) &&
      (predicate == BootstrapConstants().b_p_subgraph_union) &&
      (object != subject) && (!IsValidSubgraphUnion(wc, object))) {
    return true;
  }
  return false;
}

bool TupleStore::IsValidSubgraphUnion(const WriteContext& wc, ID hub_id) const {
  return !hub_id || ContainsKey(wc.valid_subgraph_unions, hub_id);
}

ID TupleStore::WriteSubgraphUnion(WriteContext* wc, ID author,
                                  const IDSet& subgraphs) {
  // Write and intern a subgraph union.
  vector<IDPair> local_spokes;
  for (ID id : subgraphs) {
    local_spokes.push_back(IDPair(BootstrapConstants().b_p_subgraph_union, id));
  }
  // TODO:
  // We need to look at base also, since this compound may not
  // have appeared as such in the input, thus may not have been
  // front-loaded.  Add test.  Fix behavior.
  UUID cspo_key = CSPOKey(local_spokes);
  ID interned_hub = FindWithDefault(wc->cspo_to_hub_id, cspo_key, 0);
  if (interned_hub) {
    return interned_hub;
  }
  ID fabricated_hub_id = AppendNode();
  for (ID id : subgraphs) {
    Record fabricated_spoke = Record(Record::Link(
        next_id(), 0, author, BootstrapConstants().b_mgp_subgraph_union,
        fabricated_hub_id, BootstrapConstants().b_p_subgraph_union, id,
        LinkState::kLive));
    // ID spoke_id =
    AppendRecord(fabricated_spoke);
  }
  CommitAndIndex();
  wc->cspo_to_hub_id[cspo_key] = fabricated_hub_id;
  wc->valid_subgraph_unions.insert(fabricated_hub_id);
  wc->subgraph_union_to_nodes[fabricated_hub_id] = subgraphs;
  return fabricated_hub_id;
}

ID TupleStore::AddSubgraphUnion(WriteContext* wc, ID author,
                                ID existing_subgraph_union,
                                ID new_subgraph_union) {
  if (!new_subgraph_union) {
    return existing_subgraph_union;
  }
  // Create the union of two subgraph unions.
  const IDSet& existing_subgraphs =
      FindOrDie(wc->subgraph_union_to_nodes, existing_subgraph_union);
  const IDSet& new_subgraphs =
      FindOrDie(wc->subgraph_union_to_nodes, new_subgraph_union);
  // Unions with mgp and axiom subgraph are not allowed.
  for (ID existing_subgraph : existing_subgraphs) {
    if ((existing_subgraph == BootstrapConstants().b_mgp_subgraph) ||
        (existing_subgraph == BootstrapConstants().b_bootstrap__module)) {
      return existing_subgraph_union;
    }
  }
  for (ID new_subgraph : new_subgraphs) {
    if ((new_subgraph == BootstrapConstants().b_mgp_subgraph) ||
        (new_subgraph == BootstrapConstants().b_bootstrap__module)) {
      return existing_subgraph_union;
    }
  }
  IDSet subgraphs = Union(existing_subgraphs, new_subgraphs);
  return WriteSubgraphUnion(wc, author, subgraphs);
}

const SimpleProperty* TupleStore::LoadSimplePropertyByID(ID id) const {
  CHECK_GT(id, 0);
  IDSet simple_ids;
  IDSet source_ids;
  simple_ids.insert(id);
  ID parent_compound = 0;
  source_ids = FindSimplePropertyDefinition(id, &parent_compound);
  if (parent_compound) {
    source_ids = FindCompoundPropertyDefinition(parent_compound, &simple_ids);
  }
  LocalMetaSchemaScanner scanner(*this);
  scanner.ScanInto(source_ids, &meta_schema_);
  const SimpleProperty* sp = meta_schema_.FindSimplePropertyByID(id);
  if (!sp) {
    LOG(ERROR) << "LoadSimplePropertyByID(" << id
               << ") failed, source tuple ids: " << ToDebugString(source_ids);
    if (id > 0) {
      const Record tuple = GetTuple(id);
      if (tuple.IsValue()) {
        LOG(ERROR) << "A predicate (MetaProperty root) must "
                   << "be a Node, but tuple " << id << " is a Value.";
        LOG(ERROR) << "predicate: " << ToDebugString(id, false);
      }
    }
  }
  return sp;
}

const CompoundProperty* TupleStore::LoadCompoundPropertyByID(ID id) const {
  IDSet simple_ids;
  IDSet source_ids;
  source_ids = FindCompoundPropertyDefinition(id, &simple_ids);
  LocalMetaSchemaScanner scanner(*this);
  scanner.ScanInto(source_ids, &meta_schema_);
  const CompoundProperty* cp = meta_schema_.FindCompoundPropertyByID(id);
  if (!cp) {
    LOG(ERROR) << "LoadCompoundPropertyByID(" << id
               << ") failed, source tuple ids: " << ToDebugString(source_ids);
  }
  return cp;
}

// TODO: we can do 1 pass and with less footprint when the TupleLog
// supports reverse traversal. Currently it only supports traversal in tupleid
// order.
IDSet TupleStore::RemoveDeadTuples(const IDSet& ids) const {
  SPOKeyToIDMap representative_tuples;
  bool has_killers = false;
  unique_ptr<ForEachRecordCallback> callback(
      NewPermanentCallback(this, &TupleStore::RemoveDeadTuplesCb,
                           &representative_tuples, &has_killers));
  ForEachRecordInSet(ids, callback.get(), true);
  if (!has_killers) {
    return ids;
  }
  IDSet results;
  for (auto i : representative_tuples) {
    if (i.second) {
      results.insert(i.second);
    }
  }
  return results;
}

void TupleStore::RemoveDeadTuplesCb(SPOKeyToIDMap* repr_tuples,
                                    bool* has_killers,
                                    const Record& tuple) const {
  CHECK(tuple.IsLink());
  InsertOrUpdate(repr_tuples, SPOKey(tuple), tuple.dead() ? 0 : tuple.id());
  if (tuple.dead()) {
    *has_killers = true;
  }
}
