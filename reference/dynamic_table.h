#include <functional>
#include <vector>
using ::std::vector;

#include "base/integral_types.h"
#include "base/macros.h"
#include "strings/stringpiece.h"

class RandomBase;

// Bring your own locks to prevent simultaneous
// updates. Specifically, only a single Insert()/Tick()/PurgeCache() may be in
// progress at any time but any number of Lookup()s along with the single
// Insert()/Tick()/PurgeCache() are safe.
class DynamicTable {
 public:
  virtual ~DynamicTable() {}

  static const int kMinimumTickIntervalInSeconds = 10;
  static const int kMaxValues = 8;
  static const size_t kMaxSnapshotChunk = 1 << 20;

  // If the prefix is empty, the resulting table will be ephemeral.
  static DynamicTable* Open(StringPiece prefix, bool writer,
                            uint64 create_size = 0);
  static DynamicTable* OpenFromSnapshot(
      StringPiece prefix, const std::function<StringPiece()>& chunk_source);
  static void RestoreSnapshot(StringPiece prefix,
                              const std::function<StringPiece()>& chunk_source);
  static void Delete(StringPiece prefix);
  static bool Exists(StringPiece prefix);

  struct InsertOp {
    uint64 key;    // Must be non-zero.
    uint64 key2;   // Zero for 64-bit keys, non-zero otherwise.
    bool replace;  // Replace any previous entry.
    bool bit;      // Extra bit, e.g. that the data is in an external file.
    int n;         // # of values if positive, if zero, the entry is deleted.
    const uint64* values;  // Must be non-zero.
    InsertOp() {}
    InsertOp(uint64 k, bool r, bool abit, int an, const uint64* avalues)
        : key(k), key2(0), replace(r), bit(abit), n(an), values(avalues) {}
    InsertOp(uint64 k, uint64 k2, bool r, bool abit, int an,
             const uint64* avalues)
        : key(k), key2(k2), replace(r), bit(abit), n(an), values(avalues) {}
  };

  struct LookupOp {
    uint64 key;   // Must be non-zero.
    uint64 key2;  // Zero for 64-bit keys, non-zero otherwise.
    bool cache;   // A hint that the data should be cached (write after read).
    bool bit;     // bit == false if nothing is found.
    int n;        // # of values found or n == 0 if nothing is found.
    uint64* values;  // Must be non-zero.
    LookupOp() {}
    LookupOp(uint64 k, bool acache, uint64* avalues)
        : key(k), key2(0), cache(acache), bit(false), n(0), values(avalues) {}
    LookupOp(uint64 k, uint64 k2, bool acache, uint64* avalues)
        : key(k), key2(k2), cache(acache), bit(false), n(0), values(avalues) {}
  };

  virtual void Lookup(std::vector<LookupOp>* ops) ABSTRACT;
  virtual void Insert(const std::vector<InsertOp>& ops) ABSTRACT;
  virtual void PurgeCache() ABSTRACT;
  virtual void Tick() ABSTRACT;  // Call periodically for maintainance tasks.
  virtual bool IsValid() ABSTRACT;

  // Calls the provided function with chunks of data representing the snapshot.
  // Chunk boundardaries must be respected by the result of chunk_source.
  virtual void Snapshot(
      const std::function<void(StringPiece)>& chunk_sink) ABSTRACT;
  // For testing: insert with Word corrution at a rate of 'rate'.
  virtual void InsertCorrupt(const std::vector<InsertOp>& ops, RandomBase* rnd,
                             float rate) ABSTRACT;
};
