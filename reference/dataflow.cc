#include "dataflow/dataflow.h"
#include "dataflow/dataflow-inl.h"

#include <deque>
using ::std::deque;
#include <iterator>

#include "gflags/gflags.h"

DEFINE_string(dataflow_profile_filename, "",
              "Store profile data for data flow runs.");

namespace dataflow {

// No operation should exceed this timeout.
// TODO(: detect when we are not running on the mock remote and set this
// lower.
// TODO: investigate why it takes so long for normal build on Remote.
const int BaseFlow::kDefaultTimeoutMsec = 10000;
const int BaseFlow::kNotifyLimit = 100;  // Limit on depth of Notify() stack.
BaseStream* const BaseStream::kFindRootInProgress =
    reinterpret_cast<BaseStream*>(-1);

void BaseFlow::InitializeRun() {
  deque<BaseStream*> todo(root_operations_.begin(), root_operations_.end());
  while (!todo.empty()) {
    BaseStream* op = todo.back();
    todo.pop_back();
    operations_.insert(op);
    op->flow_ = this;
    set_difference(op->attached_.begin(), op->attached_.end(),
                   operations_.begin(), operations_.end(),
                   inserter(todo, todo.begin()));
  }
  for (auto op : operations_) op->Initialize();
}
}  // namespace dataflow
