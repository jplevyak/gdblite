#include "dataflow/set_engine.h"

namespace dataflow {

IntersectionStream::IntersectionStream(const vector<IDStream*> inputs) {}

IntersectionStream::IntersectionStream(
    const vector<IDToSetIndexStream*> inputs) {}

SetEngineFlow* NewFlow(const TupleStore& store) {
  return new SetEngineFlow(store);
}
}  // namespace dataflow
