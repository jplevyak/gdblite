#include "record_log.h"

#include <algorithm>
#include <memory>
#include <unordered_map>
using ::std::unordered_map;
#include <vector>
using ::std::vector;


static const double kMaxCompression = 1.2;
static const int kHeadroom = 7;
static const int kInitialHeadroom = 200;
static const double kResizeMultiplier = 1.2;
// The WHEN field (5th) is delta encoded.
// TODO(jplev): move this to tuple.h.
static const int kWhen = 4;


// This function is not called, but must compile to ensure that the static
// assertions hold.  These assert the layout of the RecordLog::Heaer which must
// follow the textual order.
void RecordLogStaticAsserts() {
  static_assert(offsetof(RecordLog::PersistentHeader, magic) == 0,
                "RecordLog::PersistentHeader::magic must be at offset 0");
  static_assert(offsetof(RecordLog::PersistentHeader, version) == 4,
                "RecordLog::PersistentHeader::version must be at offset 4");
  static_assert(offsetof(RecordLog::PersistentHeader, compression) == 5,
                "RecordLog::PersistentHeader::compression must be at offset 5");
  static_assert(offsetof(RecordLog::PersistentHeader, written) == 6,
                "RecordLog::PersistentHeader::written must be at offset 6");
  static_assert(offsetof(RecordLog::PersistentHeader, reserved) == 7,
                "RecordLog::PersistentHeader::reserved must be at offset 7");
  static_assert(offsetof(RecordLog::PersistentHeader, start_id) == 8,
                "RecordLog::PersistentHeader::start_id must be at offset 8");
  static_assert(offsetof(RecordLog::PersistentHeader, body) == 16,
                "RecordLog::PersistentHeader::version must be at offset 16");
}
