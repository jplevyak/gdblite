#include <stddef.h>
#include <unordered_map>
using std::unordered_map;
#define hash_map unordered_map
#include <memory>
#include <string>
using ::std::string;
#include <utility>
#include <vector>
using ::std::vector;

#include "base/integral_types.h"
#include "base/macros.h"
#include "base/port.h"
#include "common/common_types.h"
#include "common/record.h"
#include "core/bootstrap.h"
#include "core/doc.h"
#include "core/metaschema.h"
#include "core/mql.h"
#include "core/query.h"
#include "core/system.h"
#include "core/tupleindex.h"
#include "pgi/pgi.h"
#include "strings/stringpiece.h"

class Cord;

class RecordReader;

template <typename X>
struct GoodFastHash;

class CompoundProperty;
class ObjectGraph;
class SimpleProperty;
class TupleSequence;

typedef hash_map<ID, IDSet> IDToSetMap;
typedef hash_map<ID, string> IDToValueMap;
typedef hash_map<ID, StringSet> IDToValueSetMap;
typedef vector<pair<Linkage, ID> > Path;
typedef hash_map<UUID, ID> SPOKeyToIDMap;

class TupleArrayPb;

struct SpokeDescriptor {
  SpokeDescriptor(ID link_id, ID predicate, ID non_hub_id) {
    this->link_id = link_id;
    this->predicate = predicate;
    this->non_hub_id = non_hub_id;
  }
  ID link_id;
  ID predicate;
  ID non_hub_id;
};

typedef pair<const CompoundProperty*, bool> CompoundPropWithSource;
typedef pair<CompoundPropWithSource, vector<SpokeDescriptor> >
    CompoundDescriptor;

struct CardCheckState;
struct WriteContext;

class TupleStore {
 public:
  virtual ~TupleStore() {}

  // Create/open fully-initialized instances that the caller owns.
  static std::unique_ptr<TupleStore> NewTransientEmpty();
  static std::unique_ptr<TupleStore> NewTransientWithBootstrap();
  static std::unique_ptr<TupleStore> NewTransientWithSystem();
  static std::unique_ptr<TupleStore> NewTransientExtending(
      const TupleStore* base);
  static std::unique_ptr<TupleStore> CreateEmpty(StringPiece path,
                                                 int64 tuple_limit);
  static std::unique_ptr<TupleStore> CreateWithBootstrap(StringPiece path,
                                                         int64 tuple_limit);
  static std::unique_ptr<TupleStore> CreateWithSystem(StringPiece path,
                                                      int64 tuple_limit);
  static std::unique_ptr<TupleStore> Open(StringPiece path, bool writer);

  // TODO: delete these legacy methods.
  static std::unique_ptr<TupleStore> NewTransient() {
    return NewTransientWithBootstrap();
  }
  static std::unique_ptr<TupleStore> Create(StringPiece path,
                                            int64 tuple_limit) {
    return CreateWithSystem(path, tuple_limit);
  }

  bool Restore(StringPiece db_file_path);
  bool Restore(RecordReader* tuple_reader);
  bool Restore(const TupleSequence& source);
  bool Restore(const TupleStore& source);

  bool WriteExtension(const TupleSequence& source,
                      IDToIDMap* id_map) MUST_USE_RESULT;
  // TODO: currently the above allows the source to include local
  // IDs (other than 1 for primary key).  In general, this should be disallowed,
  // and a new function (below) should be used for the special case of sliding
  // an extension or rewriting and existing extension which has been written
  // and had the external references resolved (i.e the normal way via
  // primary keys and compounds).
  bool WriteExtensionWithLocalReferences(const TupleSequence& source,
                                         IDToIDMap* id_map) MUST_USE_RESULT;

  // Writes all or nothing, and returns true only if there were no errors.
  // Once done, 'id_map' will map input IDs -> written IDs (or 0 for errors).
  bool Write(const TupleSequence& source, IDToIDMap* id_map) MUST_USE_RESULT;

  // Basic functions

  inline ID start_id_ext() const { return tuples_->start_id(); }
  inline ID next_id() const { return tuples_->next_id(); }

  inline size_t size() const { return next_id() - kDBStartID; }
  inline bool empty() const { return size() == 0; }

  inline size_t size_ext() const { return next_id() - start_id_ext(); }
  inline bool empty_ext() const { return size_ext() == 0; }

  // PGI Stats

  PgiStats GetPgiStats() const;
  PgiStats GetPgiStatsExt() const { return pgi_->GetPgiStats(); }

  // Append

  // AppendNode(), AppendValue() and AppendLink() are intended for use
  // by unit tests.  They exercise the interning/indexing logic in
  // AppendRecord() but bypass the MGP constraints in the higher-level Write().
  ID AppendNode();
  ID AppendValue(StringPiece value);
  ID AppendLink(ID author, ID subgraph, ID subject, ID predicate, ID object,
                LinkState state);

  ID AppendTombstone(ID doomed_id);

  // Callers that use AppendX and need indexing should use CommitAndIndex.
  void CommitAndIndex();

  // All AppendXXX() methods and Write() call through to this.
  ID AppendRecord(const Record& tuple);

  ID PtsAppendRecord(const Record& tuple);
  void AppendRecords(const vector<Record>& tuples);

  // Query Indexes.

  // Get the ID for a given value.
  ID GetValueID(StringPiece val) const;

  // Find live (relative to as_of virtual time) Tuples (links) having any
  // member of <ids> in linkage slot <i> and also having the specified
  // predicate.  Return the set of link ids and the set of node ids referenced
  // by the reciprocal linkage slot.
  void QueryProperty(const IDSet& ids, Linkage i, ID predicate, ID as_of,
                     IDSet* out_link_ids, IDSet* out_node_ids) const;

  // Return the links ids with the predicate 'predicate' and
  // subject/predicates 'from', as determined by the linkage 'start'.
  IDSet IntersectionLinks(const IDSet& from, Linkage start, ID predicate) const;

  // Find live (relative to as_of virtual time) links with <in_id> in linkage
  // slot <in>, return the entities in the <out> linkage slot.
  void EntitiesWith(Linkage in, ID in_id, Linkage out, ID as_of,
                    IDSet* out_ids) const;
  ID EntityWith(Linkage in, ID in_id, Linkage out, ID as_of) const;

  IDSet LinksTo(Linkage i, const IDSet& ids) const;

  // Given IDs for subjects and the ID of a predicate, return all objects.
  IDSet FollowLinkForward(ID property, const IDSet& ids) const;

  // Given IDs for objects and the ID of a predicate, return all subjects.
  IDSet FollowLinkBackward(ID property, const IDSet& ids) const;

  // Query TupleSequence.
  // Set values[i] to be the value that corresponds to value_id[i]; this clears
  // the output vector of any other data first.
  void GetValues(const vector<StringID>& value_ids,
                 vector<string>* values) const;

  // Do not use in performance-sensitive code: prefer bulk operations.
  inline bool HasTuple(ID id) const {
    return id >= kDBStartID && id < next_id();
  }
  Record GetTuple(ID id) const;
  ID NextTupleID(const Record& tuple) const;
  string GetValue(const Record& tuple) const;
  string GetValue(ID id) const { return GetValue(GetTuple(id)); }

  // Find Value ids for strings (does not look in base_).
  void FindExtValue(const vector<string>& strings,
                    vector<ID>* out_value_ids) const;

  void FindValues(const vector<string>& strings,
                  vector<ID>* out_value_ids) const;
  ID FindValue(const string& s) const {
    vector<ID> ids;
    FindValues(vector<string>{s}, &ids);
    if (ids.size()) return ids[0];
    return 0;
  }

  ID FindLinkBySPO(ID subject, ID predicate, ID object) const;

  void FindIDsBySPO(ID subject, ID predicate, ID object, IDSet* out_ids) const {
    FindIDsBySPO(SPOKey(subject, predicate, object), out_ids);
  }
  void FindIDsBySPO(UUID spo_key, IDSet* out_ids) const;

  void FindNodeIDsByPrimaryKeys(const vector<ID>& value_ids,
                                vector<ID>* out_node_ids) const;
  ID FindNodeByPrimaryKey(StringPiece primary_key) const;
  ID FindNodeByPrimaryKeyID(ID id) const;

  void FindPrimaryKeysByNodeIDs(const vector<ID>& node_ids,
                                vector<ID>* pk_ids) const;
  ID FindPrimaryKeyByNodeID(ID id) const;

  // Query Indexes.

  // Find ids of Tuples having <id> in linkage slot <i>.
  void FindLinksWithIDAtLinkage(Linkage i, ID id, IDSet* out_ids) const;
  IDSet FindLinksWithIDAtLinkage(Linkage i, ID id) const;

  // Find ids of Tuples having any member of <ids> in linkage slot <i>.
  void FindLinksWithIDAtLinkage(Linkage i, const IDSet& ids,
                                IDSet* out_ids) const;
  void FindLinksWithIDAtLinkage(Linkage i, const IDSet& ids, IDSet* out_ids,
                                IDToSetMap* out_map) const;
  IDSet FindLinksWithIDAtLinkage(Linkage i, const IDSet& ids) const;
  int64 EstimateSizeByLinkage(Linkage i, const IDSet& ids) const;

  // For each Record with id in <ids>, return value of linkage slot <i>.
  void ProjectLinkageToSet(const IDSet& ids, Linkage i, IDSet* out_ids) const;
  IDSet ProjectLinkageToSet(const IDSet& ids, Linkage i) const;

  // For each Record with id in <ids>, return value of linkage slot <i>.
  void ProjectLinkageToMap(const IDSet& ids, Linkage i,
                           IDToSetMap* out_map) const;
  void ProjectLinkageToMap(const IDSet& ids, Linkage i, IDSet* out_ids,
                           IDToSetMap* out_map) const;

  const SimpleProperty* FindSimplePropertyByID(ID id) const;
  const CompoundProperty* FindCompoundPropertyByID(ID id) const;

  const SimpleProperty* LoadSimplePropertyByID(ID id) const;
  const CompoundProperty* LoadCompoundPropertyByID(ID id) const;

  // No-op if as_of == 0, otherwise return IDs that are <= as_of.
  IDSet TruncateByAsOf(const IDSet& ids, ID as_of) const;

  // Remove dead tuples from any set. This assumes both the killer and the
  // killed tuples are in the input set of IDs.
  // Note that as_of filtering must be done prior to this, though not
  // necessarily paired.
  IDSet RemoveDeadTuples(const IDSet& ids) const;

  // This constructs a copy of the entire database; use only for testing!
  unique_ptr<TupleSequence> CopyTuples() const;

  // Used for inter-database replication.
  void GetTupleArrayPbInRange(ID start_id, ID end_id,
                              TupleArrayPb* tuple_array) const {
    return PtsGetTupleArrayPbInRange(start_id, end_id, tuple_array);
  }

  // TODO: replace these with pgi async APIs.
  void ForEachRecord(ForEachRecordCallback* callback) const;
  void ForEachRecordInRange(ID start_id, ID end_id,
                            ForEachRecordCallback* callback) const;
  void ForEachRecordInSet(const IDSet& ids, ForEachRecordCallback* callback,
                          bool in_order) const;

  void GetTuplesInRange(ID start_id, ID end_id,
                        vector<Record>* out_tuples) const;

  bool WriteToRecordIOFile(StringPiece path) const MUST_USE_RESULT;

  // Useful for logging:
  string ToDebugString(bool normalize) const;
  string ToDebugString(const ID id, bool normalize) const;
  string ToDebugString(const Record& tuple, bool normalize) const;
  string ToDebugString(const IDSet& ids) const;
  string ToDebugString(const vector<ID>& ids) const;
  string ToDebugString(const vector<IDPair>& local_spokes,
                       bool normalize) const;

  PhysicalGraphInterface* pgi() { return pgi_.get(); }
  const PhysicalGraphInterface* pgi() const { return pgi_.get(); }

  void FindHubByCSPO(const vector<UUID>& cspo_keys,
                     vector<ID>* out_hub_ids) const;

  // Load a db file into a new TupleStore.
  static std::unique_ptr<TupleStore> Load(StringPiece load_database,
                                          StringPiece graph_path);

  // Open a snapshot into a new TupleStore. Overwrite any current instance.
  // Writing a snapshot is done with PhysicalGraphInterface::WriteSnapshot().
  static std::unique_ptr<TupleStore> OpenFromSnapshot(
      StringPiece graph_path, StringPiece restore_snapshot);

  const b_bootstrap_constants& BootstrapConstants() const {
    return bootstrap_constants_;
  }
  const b_doc_constants& DocConstants() const { return doc_constants_; }
  const b_mql_constants& MqlConstants() const { return mql_constants_; }
  const b_query_constants& QueryConstants() const { return query_constants_; }
  const b_system_constants& SystemConstants() const {
    return system_constants_;
  }
  struct RestoreContext;

 private:
  friend class GraQLImpl;  // TODO: remove this when we agree on the API.
  friend class Indexes;
  friend class BlytheStoreTest;
  friend class BlytheStoreRewriteTest;
  friend class TupleStoreConstantsTest;
  friend class TupleStoreForTest;
  friend class TupleStoreRewriteTest;
  friend class TupleStoreTest;
  
  TupleStore(PhysicalGraphInterface* pgi, const TupleStore* base);
  TupleStore(std::unique_ptr<PhysicalGraphInterface> pgi,
             const TupleStore* base);

  static const ID kBadSequence;

  bool RestoreFromProtoString(const string& proto_string);
  bool InitModules();

  template <typename TupleSource>
  bool RestoreTupleSource(const TupleSource& source, ID start_id);

  IDSet FindSimplePropertyDefinition(ID id, ID* out_parent_compound_id) const;
  IDSet FindSimplePropertyDefinitions(const IDSet& simple_ids) const;
  IDSet FindCompoundPropertyDefinition(ID id, IDSet* simple_ids) const;

  bool HasTuple(const Record& tuple) const;
  bool IsIdentifyingProperty(ID predicate) const;

  // Return the first new tuple's id, or kBadSequence if the source is invalid.
  template <typename TupleSource>
  ID SkipToFirstNewTuple(const TupleSource& source, ID start_id,
                         RestoreContext* ctx) const;
  // Return false if the tuple_reader is corrupt or contains invalid data.
  bool SkipToFirstNewTuple(RecordReader* tuple_reader,
                           RestoreContext* ctx) const;

  // Write() callbacks
  void GatherForeignNodesAndValuesCb(WriteContext* wc, ObjectGraph* accumulator,
                                     const Record& tuple);
  void InspectPrefixTuplesCb(WriteContext* wc, const Record& tuple);
  void GatherCompoundSpokesWithHubsCb(WriteContext* wc, const Record& tuple);
  void GatherLocalSPOKeysCb(WriteContext* wc, const Record& tuple);
  void FindReplacedByMappingCb(IDToIDMap* out_map, const Record& tuple);
  void WriteTupleCb(WriteContext* wc, const Record& tuple);

  void FindLatestLiveSubgraphsCb(WriteContext* wc, vector<ID>* local_id,
                                 vector<UUID>* local_spo_key,
                                 vector<LinkState>* local_state,
                                 vector<ID>* latest_live_subgraph,
                                 const Record& tuple);

  // Indexing callbacks

  // For RemoveDeadTuples.
  void RemoveDeadTuplesCb(SPOKeyToIDMap* representative_tuples,
                          bool* has_killers, const Record& tuple) const;

  bool MakeLocalCSPOKey(WriteContext* wc, ID foreign_hub_id, UUID* out_cspo_key,
                        IDSet* out_non_hub_ids) const;

  ID FindInternedLink(WriteContext* wc, ID subject, ID predicate, ID object,
                      ID author, ID subgraph, LinkTime when, LinkState state,
                      ID* live_link_to_replace, ID* adjusted_subgraph,
                      LinkState* adjusted_state);

  void FindLinkBySPO(const vector<UUID>& spo_keys,
                     vector<ID>* out_link_ids) const;

  bool IsValidSubgraph(const WriteContext& wc, ID subject, ID predicate,
                       ID object, ID subgraph) const;
  bool IsValidSubgraphUnion(const WriteContext& wc, ID hub_id) const;
  IDSet GetSubgraphNodes(ID hub_id) const;
  ID WriteSubgraphUnion(WriteContext* wc, ID author, const IDSet& subgraphs);
  ID AddSubgraphUnion(WriteContext* wc, ID author, ID existing_subgraph_union,
                      ID new_subgraph_union);

  void FindSupersetHubs(const CompoundProperty& cp,
                        const vector<IDPair>& local_spokes,
                        IDSet* out_hub_ids) const;
  void FindSubsetHubs(const CompoundProperty& cp,
                      const vector<IDPair>& local_spokes,
                      hash_map<ID, int>* out_hub_ids) const;
  ID MergeSubsetHubs(WriteContext* wc,
                     const hash_map<ID, int>& local_hub_to_spoke_count);

  void GatherForeignNodesAndValues(WriteContext* wc);
  void MapForeignToLocalValues(WriteContext* wc);
  void MapForeignToLocalNodes(WriteContext* wc);
  void GatherCompoundSpokesWithHubs(WriteContext* wc);
  void GatherLocalSPOKeys(WriteContext* wc);
  void MapForeignToLocalCompounds(WriteContext* wc);
  void SplitLocalAndPendingNodes(const WriteContext& wc, const IDSet& node_ids,
                                 IDSet* local_nodes, IDSet* foreign_nodes);
  void FindReplacedByMapping(const IDSet& node_ids, IDToIDMap* replaced_by_map);
  bool FindReplacedByErrors(const IDSet& nodes_to_write,
                            const IDSet& values_to_write, ID foreign_subject,
                            ID foreign_object, ID local_subject,
                            ID local_object);
  void ValidateLocalSubgraphs(WriteContext* wc, const IDSet& local_subgraphs);
  void FindLatestLiveSubgraphs(WriteContext* wc, IDSet* local_spo_subgraphs);
  void FindInputSubsetHubs(const hash_map<UUID, ID>& cspo_to_hub_map,
                           const vector<IDPair>& normalized_spokes,
                           hash_map<ID, int>* out_hub_ids) const;

  void NormalizeCompoundSpokes(const WriteContext& wc, bool is_foreign_cp,
                               const vector<SpokeDescriptor>& foreign_spokes,
                               vector<IDPair>* normalized_spokes,
                               IDSet* identifying_spoke_ids,
                               IDSet* extra_spoke_ids) const;

  ID FindExtCompound(WriteContext* wc, ID foreign_hub_id);
  void WriteSimpleProperties(WriteContext* wc);
  void WriteNode(WriteContext* wc, ID foreign_id, ID pk_value_id);
  void WriteValue(WriteContext* wc, ID foreign_id);
  void WriteLink(WriteContext* wc, const Record& foreign_tuple);
  void WriteLinkReplacing(WriteContext* wc, const Record& tuple, ID foreign_id,
                          bool propagate_replace, ID recursive_trigger_link_id);
  void MoveLinks(WriteContext* wc, Linkage linkage, ID src_node_id,
                 ID dst_node_id, ID local_author, ID local_subgraph,
                 LinkTime when, ID recursive_trigger_link_id);

  bool FindCardinalityViolatorCb(CardCheckState* state, ID id) const;
  ID FindCardinalityViolator(ID linked_node_id, ID predicate,
                             Linkage subject_or_object, int cardinality,
                             CardCheckState* out_state) const;

  void PartitionIDs(const IDSet& ids, IDSet* out_low_ids,
                    IDSet* out_high_ids) const;

  // Copy non-zero elements of a into b.
  // Asserts that the destination elements in b are zero.
  // This is for merging the results of bulk lookups from base and extension.
  void MergeIDVectors(const vector<ID>& a, vector<ID>* b) const;

  void ToProto(TupleArrayPb* tuple_array_pb) const;
  void ToProtoCb(TupleArrayPb* tuple_array_pb, const Record& tuple) const;
  void ToProtoString(string* output, bool text_format) const;

  void ToDebugStringCb(Cord* output, bool normalize, const Record& tuple) const;

  // For Restore.
  void AppendRecord(const Record& tuple, RestoreContext* ctx);
  void CommitAndNotify(RestoreContext* ctx);
  inline bool IsPacked() const { return true; }

  void PtsForEachTuple(ForEachRecordCallback* callback) const {
    PtsForEachTupleInRange(start_id_ext(), next_id(), callback);
  }
  void PtsForEachTupleInRange(ID start_id, ID end_id,
                              ForEachRecordCallback* callback) const;
  void PtsForEachTupleInVector(const vector<ID>& ids,
                               ForEachRecordCallback* callback) const;
  void PtsForEachTupleInSet(const IDSet& ids, ForEachRecordCallback* callback,
                            bool unused_in_order) const {
    vector<ID> tuples(ids.begin(), ids.end());
    PtsForEachTupleInVector(tuples, callback);
  }

  inline bool HasTupleExt(ID id) const {
    return id >= start_id_ext() && id < next_id();
  }

  Record PtsGetTuple(ID id) const;

  // Used for inter-database replication.
  void PtsGetTupleArrayPbInRange(ID start_id, ID end_id,
                                 TupleArrayPb* tuple_array) const;

  // Get the strings possessed by the Tuples with the given ids.
  void PtsGetValues(const vector<ID>& value_ids, vector<string>* values) const;

  // These methods provide for rare circumstances when tuple-at-a-time
  // access is required. They wrap a single ID in a container and
  // call the PGI bulk APIs. They should not be called from any
  // performance-sensitive context.
  string PtsGetValue(const Record& tuple) const;
  string PtsGetValue(ID id) const { return PtsGetValue(PtsGetTuple(id)); }

  // Adds a group of strings to the underlying string table of the pgi
  // For Tuples (links) identified by <ids>, return
  // the set of linkage slot <i> values in <out_ids>.
  void PtsProjectLinkageToSet(const IDSet& ids, Linkage i,
                              IDSet* out_ids) const;

  // For Tuples (links) identified by <ids>, return a map
  // with link id keys and set(linkage slot <i>) values in <out_map>.
  // Each value set contains exactly one element.
  void PtsProjectLinkageToMap(const IDSet& ids, Linkage i,
                              IDToSetMap* out_map) const;

  void PtsCommit();

  void PtsGetTuplesInRange(ID start_id, ID end_id,
                           vector<Record>* tuples) const;
  string PtsToDebugString() const;

  unique_ptr<PhysicalGraphInterface> pgi_;
  const TupleStore* base_;

  PgiRecordLog* tuples_;
  Indexes indexes_;
  ID first_unindexed_tuple_;

  // Cache used by Write() and compound index NewTuples().
  mutable MetaSchema meta_schema_;

  b_bootstrap_constants bootstrap_constants_;
  b_doc_constants doc_constants_;
  b_mql_constants mql_constants_;
  b_query_constants query_constants_;
  b_system_constants system_constants_;

  DISALLOW_COPY_AND_ASSIGN(TupleStore);
};

class TupleStoreForTest : public TupleStore {
 public:
  using TupleStore::AppendNode;
  using TupleStore::AppendValue;
  using TupleStore::AppendLink;
};
