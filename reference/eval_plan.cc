#include "graql/eval_plan.h"

#include <stddef.h>
#include <unordered_map>
using std::unordered_map;
#define hash_map unordered_map
#include <memory>
#include <string>
using ::std::string;
#include <utility>

#include "gflags/gflags.h"
#include "base/logging.h"
#include "codegen/graql_asg_dynamic.h"
#include "common/common_types.h"
#include "core/metaschema.h"
#include "core/tupleindex.h"
#include "core/tuplestore.h"
#include "dataflow/dataflow.h"
#include "dataflow/dataflow-inl.h"
#include "dataflow/intersect2.h"
#include "dataflow/set_engine.h"
#include "dataflow/set_engine_data_interface.h"
#include "graql/graql_asg.h"
#include "pgi/pgi_idset.h"
#include "util/macros.h"
#include "strings/join.h"
#include "strings/strcat.h"
#include "strings/strutil.h"
#include "strings/substitute.h"
#include "util/btree/btree.h"
#include "util/gtl/map_util.h"
#include "util/refcount/reffed_ptr.h"
#include "util/task/codes.pb.h"

namespace util {
class Status;
}  // namespace util

using std::unique_ptr;
using util::Status;
using util::error::INTERNAL;
using util::error::INVALID_ARGUMENT;

// Used for unit tests and STL PGI. 128 is the sweet spot for experiments
// on schema data set.
DEFINE_int64(estimation_size_limit, 128,
             "Index set size limit for cost estimation.");

#define DEBUG_OP_IDS(level, ids, info) \
  if (debug_level_ >= level) {         \
    DebugOp(ids, new IDSet, info);     \
  }

#define DEBUG_OP_TUPLES(level, tuples, info)   \
  if (debug_level_ >= level) {                 \
    DebugOp(tuples, new TupleContainer, info); \
  }

using dataflow::FilterZeroOp;
using dataflow::IDSetOp;
using dataflow::Set;
using dataflow::IDToSetIndex;
using dataflow::IDToSetStream;
using dataflow::IDStream;
using dataflow::TupleStream;
using dataflow::LinkageIndexOp;
using dataflow::IntersectionStream;
using dataflow::LinkageOp;
using dataflow::LinkageIndexStream;
using dataflow::TuplesOp;
using dataflow::StoreStream;
using dataflow::DI;
using dataflow::Stream;
using dataflow::TransformStream;

template <>
void EvalPlanner::OnDoneStoreResults(const LinkCon* const& input,
                                     bool new_results,
                                     EvaluatedConstraint* e_c) {
  if (!e_c->constraint()->AsNodeCon()) {
    LOG(ERROR) << "Internal error duing eval plannning on "
               << sq_->GetQuery()->ToDebugString(*e_c->constraint());
  }
  if (!new_results) return;
  InsertIfNotPresent(&new_results_, e_c);
}

template <>
void EvalPlanner::OnDoneStoreResults(const vector<const NodeCon*>& inputs,
                                     bool new_results,
                                     EvaluatedConstraint* e_c) {
  if (!e_c->constraint()->AsLinkCon()) {
    LOG(ERROR) << "Internal error duing eval plannning on \'"
               << sq_->GetQuery()->ToDebugString(*e_c->constraint());
  }
  if (!new_results) return;
  InsertIfNotPresent(&new_results_, e_c);
}

EvalPlanner::EvalPlanner(EvaluatedSubquery* sq, uint32 debug_level)
    : sq_(sq), debug_level_(debug_level) {}

vector<WorkPlan> EvalPlanner::ComputeWorkPlans(bool estimate_costs) {
  vector<WorkPlan> work_plans;
  for (auto lc : sq_->GetLinkCons()) {
    auto e_lc = sq_->GetEvaluatedConstraint(lc);
    if (e_lc->IsStarted()) {
      continue;
    }
    vector<LinkageInfo> ready_linkages;
    vector<LinkageInfo> todo_linkages;
    GetLinkageInfo(lc, &ready_linkages, &todo_linkages);
    if (!ready_linkages.empty()) {
      work_plans.push_back(
          ComputeWorkPlan(ready_linkages, todo_linkages, e_lc, estimate_costs));
    }
  }
  return work_plans;
}

namespace {
// An adapter for usage in StoreStream
class TupleContainer : public vector<Record> {
 public:
  template <typename InputIterator>
  void insert(InputIterator b, InputIterator e) {
    vector<Record>::insert(end(), b, e);
  }
};

template <typename I>
bool HasIncompleteResults(const EvaluatedQuery& e_q, const I& inputs);
template <>
bool HasIncompleteResults(const EvaluatedQuery& e_q,
                          const vector<const NodeCon*>& inputs) {
  return false;
}
template <>
bool HasIncompleteResults(const EvaluatedQuery& e_q, const LinkCon* const& lc) {
  return e_q.GetEvaluatedConstraint(lc)->HasIncompleteResults();
}

string ToDebugString(const EvaluatedConstraint& e_c) {
  return e_c.IsStarted() ? blythe::ToDebugString(e_c.ResultIDs()) : "universe";
}
template <typename I>
string ToDebugString(const EvaluatedQuery& e_q, const I& inputs, bool verbose);
template <>
string ToDebugString(const EvaluatedQuery& e_q,
                     const vector<const NodeCon*>& inputs, bool verbose) {
  vector<string> names;
  int count = 0;
  for (auto i : inputs) {
    auto e_c = e_q.GetEvaluatedConstraint(i);
    if (verbose) {
      names.push_back(strings::Substitute("        $0 $1",
                                          e_q.ToDebugString(*i->AsGeneric()),
                                          ToDebugString(*e_c)));
    } else {
      names.push_back(strings::Substitute(
          "        $0 result size=$1", e_q.ToDebugString(*i->AsGeneric()),
          (e_c->IsStarted() ? SimpleItoa(e_c->ResultIDs().size())
                            : "universe")));
    }
    if (++count > 10) break;
  }
  return strings::Join(names, "\n");
}
string ToDebugString(const EvaluatedQuery& e_q, const LinkCon* const& lc,
                     bool verbose) {
  auto e_lc = e_q.GetEvaluatedConstraint(lc);
  if (verbose)
    return strings::Substitute("        $0 $1",
                               e_q.ToDebugString(*lc->AsGeneric()),
                               ToDebugString(*e_lc));
  return strings::Substitute(
      "        $0 result size=$1", e_q.ToDebugString(*lc->AsGeneric()),
      (e_lc->IsStarted() ? SimpleItoa(e_lc->ResultIDs().size()) : "universe"));
}
string ToDebugString(const IDSet& ids) { return blythe::ToDebugString(ids); }
string ToDebugString(const TupleContainer& tuples) {
  vector<string> names;
  int count = 0;
  names.push_back("size=" + SimpleItoa(tuples.size()));
  for (auto tuple : tuples) {
    names.push_back(strings::Substitute("$0[$1 $2 $3]", tuple.id(),
                                        tuple.subject(), tuple.predicate(),
                                        tuple.object()));
    if (++count > 2000) break;
  }
  return strings::Join(names, " ");
}

// Store results in temporary storage and set results after it is done.
// TODO(haichunc): consider using intersection op for better readability and
// better flow.
// E: type of the element/data.
// C: container type
// I: type of the consumed inputs, either NodeCon or LinkCon.
template <typename E, typename C, typename I>
class StoreIntoRuntimeStream : public StoreStream<E, C, DI> {
 public:
  typedef StoreStream<E, C, DI> BASE;
  typedef Stream<E, DI> BaseOp;
  StoreIntoRuntimeStream(const I& inputs, BaseOp* op, C* s,
                         EvaluatedConstraint* e_c, EvaluatedQuery* e_q,
                         EvalPlanner* planner, uint32 debug_level)
      : BASE(op, s, true),
        inputs_(inputs),
        new_results_(s),
        evaluated_constraint_(e_c),
        evaluated_query_(e_q),
        planner_(planner),
        debug_level_(debug_level) {}

 private:
  // TODO(haichunc): stop the flow earlier, say at MaterializeOp.
  void DoneStoreResults() {
    if (evaluated_constraint_->HasIncompleteResults() ||
        HasIncompleteResults(*evaluated_query_, inputs_)) {
      LOG_LEVEL(3, StrCat("Found incomplete sets on ",
                          evaluated_query_->ToDebugString(
                              *evaluated_constraint_->constraint())));
      return;
    }
    LOG_LEVEL(
        0, StrCat("SetResult on ", evaluated_query_->ToDebugString(
                                       *evaluated_constraint_->constraint())));
    LOG_LEVEL(0,
              StrCat("    inputs=\n", ToDebugString(*evaluated_query_, inputs_,
                                                    debug_level_ >= 1)));
    LOG_LEVEL(1, StrCat("    before= ",
                        ToDebugString(evaluated_constraint_->ResultIDs())));
    LOG_LEVEL(1, StrCat("    computed= ", ToDebugString(*new_results_)));
    bool is_new_result = evaluated_constraint_->SetResult(*new_results_);
    LOG_LEVEL(1, StrCat("    after= ",
                        (is_new_result
                             ? ToDebugString(evaluated_constraint_->ResultIDs())
                             : "before")));
    if (is_new_result && evaluated_constraint_->runtime()->ResultEmpty()) {
      LOG(WARNING) << "Result of " << evaluated_query_->ToDebugString(
                                          *evaluated_constraint_->constraint())
                   << " becomes zero!";
    }
    planner_->OnDoneStoreResults(inputs_, is_new_result, evaluated_constraint_);
  }
  const I inputs_;
  unique_ptr<C> new_results_;
  EvaluatedConstraint* evaluated_constraint_;
  EvaluatedQuery* evaluated_query_;
  EvalPlanner* planner_;
  const uint32 debug_level_;
};

template <typename E, typename C, typename I>
inline StoreIntoRuntimeStream<E, C, I>* StoreIntoRuntimeOp(
    const I& inputs, Stream<E, DI>* op, C* s, EvaluatedConstraint* e_c,
    EvaluatedQuery* e_q, EvalPlanner* planner, uint32 debug_level) {
  return new StoreIntoRuntimeStream<E, C, I>(inputs, op, s, e_c, e_q, planner,
                                             debug_level);
}

class AsOfAndDeadFilter {
 public:
  explicit AsOfAndDeadFilter(ID as_of) : as_of_(as_of), has_dead_(false) {}
  // "true" means filter.
  inline bool operator()(const Record& tuple) {
    if (as_of_ && tuple.id() > as_of_) return true;
    auto key = SPOKey(tuple);
    auto id_ptr = FindOrNull(killers_, key);
    if (tuple.dead()) {
      if (!id_ptr || tuple.id() > *id_ptr) {
        InsertOrUpdate(&killers_, key, tuple.id());
      }
      has_dead_ = true;
      return true;
    }
    if (id_ptr && tuple.id() < *id_ptr) return true;
    return false;
  }

  const SPOKeyToIDMap& GetKillers() const { return killers_; }
  bool HasDead() const { return has_dead_; }

 private:
  ID as_of_;
  bool has_dead_;
  SPOKeyToIDMap killers_;
};

// Filter out tuples that are later than "as_of" or "dead".
class ApplyAsOfAndRemoveDeadStream
    : public TransformStream<Record, Record, DI> {
 public:
  typedef TransformStream<Record, Record, DI> Base;
  ApplyAsOfAndRemoveDeadStream(TupleStream* op, EvaluatedConstraint* e_c,
                               EvaluatedQuery* e_q, uint32 debug_level)
      : Base(op),
        evaluated_constraint_(CHECK_NOTNULL(e_c)),
        evaluated_query_(CHECK_NOTNULL(e_q)),
        filter_(e_q->as_of()),
        debug_level_(debug_level) {}

  void Transform(in_iterator* pin, in_iterator in_end, out_iterator* pout,
                 out_iterator out_end) {
    auto in = *pin;
    auto out = *pout;
    for (; in != in_end && out != out_end; ++in) {
      if (filter_(*in)) continue;
      *out++ = *in;
    }
    *pin = in;
    *pout = out;
  }

  ~ApplyAsOfAndRemoveDeadStream() {
    if (filter_.HasDead()) {
      LOG_LEVEL(0, StrCat("Remove dead links on ",
                          evaluated_query_->ToDebugString(
                              *evaluated_constraint_->constraint())));
      evaluated_constraint_->RemoveDead(filter_.GetKillers());
    }
  }

 private:
  EvaluatedConstraint* evaluated_constraint_;
  EvaluatedQuery* evaluated_query_;
  AsOfAndDeadFilter filter_;
  const uint32 debug_level_;
};

inline ApplyAsOfAndRemoveDeadStream* ApplyAsOfAndRemoveDeadOp(
    TupleStream* op, EvaluatedConstraint* e_c, EvaluatedQuery* e_q,
    uint32 debug_level) {
  return new ApplyAsOfAndRemoveDeadStream(op, e_c, e_q, debug_level);
}

typedef pair<ID, Set<ID>> IDIDSetPair;
// When "estimate_costs" is true, filter out index sets that are large.
class FilterLargeSetsStream
    : public TransformStream<IDIDSetPair, IDIDSetPair, DI> {
 public:
  typedef TransformStream<IDIDSetPair, IDIDSetPair, DI> Base;
  FilterLargeSetsStream(IDToSetStream* op, hash_map<ID, PgiIDSet>* index_sets,
                        LinkRuntime* lrt, bool estimate_costs,
                        size_t cutoff_size)
      : Base(CHECK_NOTNULL(op)),
        index_sets_(index_sets),
        lrt_(CHECK_NOTNULL(lrt)),
        estimate_costs_(estimate_costs),
        cutoff_size_(cutoff_size) {}

  void Transform(in_iterator* pin, in_iterator in_end, out_iterator* pout,
                 out_iterator out_end) {
    auto in = *pin;
    auto out = *pout;
    for (; in != in_end && out != out_end; ++in) {
      auto& p = *in;
      index_sets_->insert(p);
      if (estimate_costs_ &&
          (!p.second.IsImmediate() || p.second.size() > cutoff_size_)) {
        lrt_->SetHasIncompleteResults(true);
        continue;
      }
      *out++ = p;
    }
    *pin = in;
    *pout = out;
  }

 private:
  hash_map<ID, PgiIDSet>* index_sets_;
  LinkRuntime* lrt_;
  const bool estimate_costs_;
  size_t cutoff_size_;
};

IDToSetStream* LinkageIndexOpWithFilter(IDStream* ids, Linkage linkage,
                                        EvaluatedConstraint* e_c,
                                        bool estimate_costs) {
  auto lrt = static_cast<LinkRuntime*>(e_c->runtime());
  return new FilterLargeSetsStream(LinkageIndexOp(ids, linkage),
                                   &lrt->IndexSets(linkage), lrt,
                                   estimate_costs, FLAGS_estimation_size_limit);
}
}  // anonymous namespace

IDStream* EvalPlanner::UnionIntersect(ID single_id, LinkageInfo* single_fanout,
                                      LinkageInfo* other_fanout,
                                      EvaluatedConstraint* e_c,
                                      bool estimate_costs) {
  IDToSetStream* singleton = LinkageIndexOpWithFilter(
      IDSetOp(single_id), single_fanout->linkage, e_c, estimate_costs);
  DEBUG_OP_IDS(3, JoinOp(SecondOp(singleton)),
               strings::Substitute("UX $0 index",
                                   GetLinkageName(single_fanout->linkage)));
  ElementRuntime* rt = other_fanout->evaluated->runtime();
  const IDSet ids = rt->ResultIDs();
  IDToSetStream* small_sets = LinkageIndexOpWithFilter(
      IDSetOp(ids), other_fanout->linkage, e_c, estimate_costs);
  DEBUG_OP_IDS(3, JoinOp(SecondOp(small_sets)),
               strings::Substitute("UX with $0 index",
                                   GetLinkageName(other_fanout->linkage)));
  IDStream* link_ids = UnionIntersectOp(singleton, small_sets);
  DEBUG_OP_IDS(3, link_ids, "Output link ids");
  return link_ids;
}

inline IDStream* EvalPlanner::IntersectOp(const vector<IDToSetStream*>& indexes,
                                          const vector<Linkage>& linkages) {
  IDStream* ids = MaterializeOp(JoinOp(SecondOp(indexes[0])));
  DEBUG_OP_IDS(3, ids, strings::Substitute("Intersect $0 index",
                                           GetLinkageName(linkages[0])));
  for (int i = 1; i < indexes.size(); ++i) {
    IDStream* ids2 = MaterializeOp(JoinOp(SecondOp(indexes[i])));
    DEBUG_OP_IDS(3, ids2, strings::Substitute("Intersecting with $0 index",
                                              GetLinkageName(linkages[i])));
    ids = Intersect2Op(ids, ids2);
  }
  DEBUG_OP_IDS(3, ids, "Output link ids");
  return ids;
}

IDStream* EvalPlanner::Intersect(const vector<LinkageInfo>& linkage_infos,
                                 EvaluatedConstraint* e_c,
                                 bool estimate_costs) {
  vector<IDToSetStream*> linkage_indexes;
  vector<Linkage> linkages;
  for (const auto& item : linkage_infos) {
    // TODO(haichunc): optimize it to use local maps of LinkRuntime when
    // available.
    linkage_indexes.push_back(
        LinkageIndexOpWithFilter(IDSetOp(item.evaluated->ResultIDs()),
                                 item.linkage, e_c, estimate_costs));
    linkages.push_back(item.linkage);
  }
  return IntersectOp(linkage_indexes, linkages);
}

IDStream* EvalPlanner::ComputeLinksFromLinkages(
    const vector<LinkageInfo>& ready_linkages, EvaluatedConstraint* e_c,
    bool estimate_costs) {
  if (ready_linkages.size() == 2) {
    // Let's first try to use our secondary indices (SP and PO)
    const LinkageInfo* seen_predicate = nullptr;
    const LinkageInfo* other_linkage = nullptr;
    for (const auto& item : ready_linkages) {
      if (item.linkage == kPredicate) {
        seen_predicate = &item;
      } else {
        other_linkage = &item;
      }
    }
    // ASG is valid so if we have a predicate link the other
    // link will not also be a predicate link.
    if (seen_predicate) {
      // We can use a secondary index!
      return JoinOp(SecondOp(LinkageIntersectionIndexOp(
          IDSetOp(seen_predicate->evaluated->ResultIDs()),
          IDSetOp(other_linkage->evaluated->ResultIDs()), kPredicate,
          other_linkage->linkage)));
    }

    const LinkageInfo* single_fanout = nullptr;
    ID single_id;
    for (const auto& item : ready_linkages) {
      const IDSet ids = item.evaluated->ResultIDs();
      if (ids.size() == 1 && (!single_fanout || kPredicate == item.linkage)) {
        single_fanout = &item;
        single_id = *ids.begin();
      }
    }
    if (single_fanout) {
      const LinkageInfo* other_fanout =
          (&ready_linkages[0] == single_fanout ? &ready_linkages[1]
                                               : &ready_linkages[0]);
      return UnionIntersect(single_id, const_cast<LinkageInfo*>(single_fanout),
                            const_cast<LinkageInfo*>(other_fanout), e_c,
                            estimate_costs);
    }
  }
  return Intersect(ready_linkages, e_c, estimate_costs);
}

vector<const NodeCon*> EvalPlanner::GetNodes(
    const vector<LinkageInfo>& linkages) {
  vector<const NodeCon*> nodes;
  for (const auto& item : linkages) {
    nodes.push_back(&item.node);
  }
  return nodes;
}

void EvalPlanner::ComputeLinkagesFromLinks(
    const LinkCon* lc, TupleStream* links,
    const vector<LinkageInfo>& todo_linkages, WorkPlan* plan) {
  for (const auto& item : todo_linkages) {
    EvaluatedConstraint* e_nc =
        const_cast<EvaluatedConstraint*>(item.evaluated);
    if (e_nc->IsDone()) {
      continue;
    }
    plan->push_back(make_pair(
        e_nc, StoreIntoRuntimeOp(lc, LinkageOp(links, item.linkage), new IDSet,
                                 e_nc, sq_->GetQuery(), this, debug_level_)));
  }
}

WorkPlan EvalPlanner::ComputeWorkPlan(const vector<LinkageInfo>& ready_linkages,
                                      const vector<LinkageInfo>& todo_linkages,
                                      EvaluatedConstraint* e_lc,
                                      bool estimate_costs) {
  IDStream* link_ids =
      ComputeLinksFromLinkages(ready_linkages, e_lc, estimate_costs);
  TupleStream* links = ApplyAsOfAndRemoveDeadOp(TuplesOp(link_ids), e_lc,
                                                sq_->GetQuery(), debug_level_);
  DEBUG_OP_IDS(3, link_ids, "Input to tuplesop");
  DEBUG_OP_TUPLES(3, links, "Output of tuplesop");
  WorkPlan plan;
  plan.push_back(make_pair(
      e_lc,
      StoreIntoRuntimeOp(GetNodes(ready_linkages), links, new TupleContainer,
                         e_lc, sq_->GetQuery(), this, debug_level_)));
  // Enforce arc-consistency for this LinkCon. There is a special case
  // where the link con has a single candidate on a linkage (usually
  // con_link_pred) and another linkage has new reduced result. In this case
  // rejection can be computed while performing union intersection. With proper
  // cost models, this case should be the majority case.
  auto lc = e_lc->constraint()->AsLinkCon();
  ComputeLinkagesFromLinks(lc, links, todo_linkages, &plan);
  return plan;
}

void EvalPlanner::GetLinkageInfo(const LinkCon* lc,
                                 vector<LinkageInfo>* ready_linkages,
                                 vector<LinkageInfo>* todo_linkages) {
  for (auto linkage : kAllLinkages) {
    auto nc = GetLinkage(*lc, linkage);
    if (nc) {
      EvaluatedConstraint* e_c = sq_->GetEvaluatedConstraint(nc);
      // Temporarily created property by functions should not propagate.
      if (e_c->IsStarted() && !e_c->runtime()->IsTemporaryProp()) {
        ready_linkages->emplace_back(*nc, linkage, e_c);
      } else if (!e_c->IsDone()) {
        todo_linkages->emplace_back(*nc, linkage, e_c);
      }
    }
  }
}
