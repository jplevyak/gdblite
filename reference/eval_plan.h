#include <set>
using ::std::set;
#include <vector>
using ::std::vector;

#include "base/integral_types.h"
#include "base/logging.h"
#include "common/record.h"
#include "dataflow/set_engine.h"
#include "dataflow/set_engine_data_interface.h"
#include "graql/eval_runtime.h"
#include "graql/graql_asg.h"

class EvalPlanner {
 public:
  EvalPlanner(EvaluatedSubquery* sq, uint32 debug_level);
  // Based on current state of evaluated subquery, compute a query plan and
  // return corresponding work plans to be done. The returned work plans are
  // flow execution alternatives. Caller must take care of race conditions if
  // he/she wants to execute >1 plans in parallel.
  // Caller takes ownership of work plans.
  vector<WorkPlan> ComputeWorkPlans(bool estimate_costs);
  // Get notified when an evaluated constraint has new results. This can be
  // the place for postprocessing.
  // inputs: the ASG objects that are inputs to compute the results.
  template <typename I>
  void OnDoneStoreResults(const I& inputs, bool new_results,
                          EvaluatedConstraint* evaluated_constraint);

  // Newly computed results, used for efficiency for rejection propagation.
  void ClearNewResults() { new_results_.clear(); }
  const set<EvaluatedConstraint*>& NewResults() const { return new_results_; }

 private:
  struct LinkageInfo {
    LinkageInfo(const NodeCon& n, Linkage l,
                EvaluatedConstraint* evaluated_constraint)
        : node(n), linkage(l), evaluated(CHECK_NOTNULL(evaluated_constraint)) {}

    LinkageInfo(LinkageInfo&& arg)
        : node(std::move(arg.node)), linkage(arg.linkage),
          evaluated(arg.evaluated) {}

    LinkageInfo& operator=(LinkageInfo&& other) {
      if (this != &other) {
        new (this) LinkageInfo(other.node, other.linkage, other.evaluated);
      }
      return *this;
    }

    LinkageInfo(const LinkageInfo&) = delete;
    LinkageInfo& operator=(const LinkageInfo& other) = delete;

    const NodeCon& node;
    const Linkage linkage;
    EvaluatedConstraint* evaluated;
  };
  // Given a link constraint, compute a work plan that: computes new link
  // results, performs property hop to its linkages if needed, and make the
  // linkages arc-consistent. A work plan is consisted of a set of related
  // flow operations.
  WorkPlan ComputeWorkPlan(const vector<LinkageInfo>& ready_linkages,
                           const vector<LinkageInfo>& todo_linkages,
                           EvaluatedConstraint* e_lc, bool estimate_costs);
  void GetLinkageInfo(const LinkCon* lc, vector<LinkageInfo>* ready_linkages,
                      vector<LinkageInfo>* todo_linkages);
  // Perform "union intersection": first intersect each of other_fanouts with
  // single_fanout, then union all intersect results.
  dataflow::IDStream* UnionIntersect(dataflow::ID single_id,
                                     LinkageInfo* single_fanout,
                                     LinkageInfo* other_fanouts,
                                     EvaluatedConstraint* e_c,
                                     bool estimate_costs);
  // N-way intersect of linkage index sets, with "N == 1" being a trivial case.
  // TODO(haichunc): 1, optimize N-way intersection by considering costs; 2, use
  // different strategy for joining the sets, or never run MaterializeOp unless
  // it can fit in local memory.
  dataflow::IDStream* Intersect(const vector<LinkageInfo>& linkages,
                                EvaluatedConstraint* e_c, bool estimate_costs);
  dataflow::IDStream* IntersectOp(
      const vector<dataflow::IDToSetStream*>& indexes,
      const vector<Linkage>& linkages);
  // Produce link results from known linkages.
  dataflow::IDStream* ComputeLinksFromLinkages(
      const vector<LinkageInfo>& ready_linkages, EvaluatedConstraint* e_c,
      bool estimate_costs);
  // Enforce arc-consistency from link con to linkages.
  void ComputeLinkagesFromLinks(const LinkCon* lc, dataflow::TupleStream* links,
                                const vector<LinkageInfo>& todo_linkages,
                                WorkPlan* plan);

  vector<const NodeCon*> GetNodes(const vector<LinkageInfo>& linkages);

  EvaluatedSubquery* sq_;
  const uint32 debug_level_;
  // Constraints with new results thus candidates for rejection propagation.
  set<EvaluatedConstraint*> new_results_;
};
