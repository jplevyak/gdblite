#undef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64

#include "util/dynamic_table.h"

#include <algorithm>
#include <array>
#include <functional>
#include <map>
using ::std::map;
#include <memory>
#include <string>
using ::std::string;
#include <utility>

#include "base/atomicops.h"
#include "gflags/gflags.h"
#include "base/integral_types.h"
#include "base/logging.h"
#include "util/coder.h"
#include "util/mmap_backing.h"
#include "strings/strcat.h"
#include "strings/stringpiece.h"
#include "util/endian/endian.h"
#include "util/random/random_base.h"

using std::function;
using std::map;
using std::max;
using std::min;
using std::unique_ptr;

DEFINE_bool(dt_map_private, false,
            "Map the Dynamic Table MAP_PRIVATE so that local changes are not "
            "written back to the backing files (i.e. for testing).");

const int DynamicTable::kMinimumTickIntervalInSeconds;
const int DynamicTable::kMaxValues;
const size_t DynamicTable::kMaxSnapshotChunk;

class DynamicTableImpl : public DynamicTable {
  static const int kPadding = 10;  // For reads past the end of the bucket.
  // Generated using efficient64 from https://github.com/wizykowski/miller-rabin
  static const uint64 kHashPrime = 13709122941870337439ull;

 public:
  ~DynamicTableImpl();

  static DynamicTable* Open(StringPiece prefix, bool writer,
                            uint64 create_size);
  static DynamicTable* OpenFromSnapshot(
      StringPiece prefix, const function<StringPiece()>& chunk_source);
  static void Delete(StringPiece prefix);
  static bool Exists(StringPiece prefix);

  // Note that the key and values must be non-zero.
  virtual void Insert(const vector<InsertOp>& ops);
  virtual void Lookup(vector<LookupOp>* ops);
  virtual void PurgeCache();
  virtual void Tick();
  virtual bool IsValid();

  virtual void Snapshot(const function<void(StringPiece)>& chunk_handler);

  virtual void InsertCorrupt(const vector<InsertOp>& ops, RandomBase* rnd,
                             float rate);

  void SetFlags();
  bool Initialize(StringPiece prefix, bool writer, uint64 create_size);
  bool InitializeFromSnapshot(StringPiece prefix,
                              const function<StringPiece()>& chunk_source);

 private:
  struct Metadata {
    uint64 base_buckets;
    uint64 file_size;
    uint64 last_overflow_bucket;
    uint64 last_expanded_bucket;
  };
  typedef uint32 BucketHeader;
  enum RecordType {
    RT_ILLEGAL,  // Zero is illegal.
    RT_END,
    RT_INSERT,
    RT_REPLACE,
    RT_DELETE,
    RT_INSERT_PREVIOUS,
    RT_REPLACE_PREVIOUS,
    RT_DELETE_PREVIOUS,
  };
  typedef map<pair<uint64, uint64>, pair<bool, vector<uint64>>> Records;

  static const int kMaxFiles = 128;
  // 1 << 7 = 128 buckets or 32KB + 8KB (for prealloc) at level 0.
  static const int kBucketsPerLevelLog2 = 7;
  static const uint32 kBucketHeaderNoOverflow = 0;
  static const uint32 kBucketHeaderExtended = kuint32max;
  static const int kRecordTypeMask = 0x7;
  static const int kBucketSizeInBytes = 128;
  static const int kBucketSizeInWords = kBucketSizeInBytes / 8;
  static const int kRecordBitShift = 3;
  static const int kRecord128BitKeyShift = 4;
  static const int kRecordValuesShift = 5;
  static const int kTicksTillDelete = 3;
  static const int kBucketsPerExpandStep = 16;
  static const int kMaxValues = 8;
  // Must be a power of 2.
  static const int kInsertionsPerExpandStep = kBucketsPerExpandStep;
  static constexpr double kOverflowHighwater = 0.01;
  static constexpr double kOverflowPreallocate = 0.02;

  bool UpdateMetadata();
  bool UpdateFiles(bool force_expand, uint64 create_buckets = 0);

  // File access functions.
  int MaxFiles() { return metadata_size_ / sizeof(Metadata); }
  void SetLevel() {
    for (int i = 0; i < MaxFiles(); i++)
      if (metadata_[i].base_buckets &&
          metadata_[i].base_buckets > metadata_[i].last_expanded_bucket) {
        base::subtle::Release_Store(&level_, i);
        return;
      }

    base::subtle::Release_Store(&level_, 0);
  }
  int GetLastFile() {
    if (level_ < MaxFiles() - 1 && metadata_[level_ + 1].base_buckets)
      return level_ + 1;
    return level_;
  }

  // Data access functions.
  // TODO(jplev): consider adding non-expanding or slower expanding levels
  // when the table is large.
  uint64 BaseBuckets(int level) {
    return (1LL << (level + kBucketsPerLevelLog2));
  }
  uint64 InitialBuckets(int level) {
    return metadata_[level].base_buckets * 1.0 + kOverflowPreallocate;
  }
  size_t LevelDataSize(int level) {
    return RoundUpToPageSize(InitialBuckets(level) * kBucketSizeInBytes);
  }
  uint8* Data(int level) {
    return reinterpret_cast<uint8*>(file_mmap_[level]);
  }
  uint8* Bucket(int level, int64 bucket) {
    return Data(level) + bucket * kBucketSizeInBytes;
  }

  // Header predicates.
  bool ExtraBit(uint8 rh) { return (rh >> kRecordBitShift) & 1; }
  bool Key128Bits(uint8 rh) { return (rh >> kRecord128BitKeyShift) & 1; }
  int NumberOfRecordValues(uint8 rh) { return (rh >> kRecordValuesShift) + 1; }
  bool IsReplace(uint8 rh) {
    uint8 type = rh & kRecordTypeMask;
    return type == RT_REPLACE || type == RT_REPLACE_PREVIOUS;
  }
  bool IsDelete(uint8 rh) {
    uint8 type = rh & kRecordTypeMask;
    return type == RT_DELETE || type == RT_DELETE_PREVIOUS;
  }
  bool IsPrevious(uint8 rh) {
    uint8 type = rh & kRecordTypeMask;
    return type == RT_INSERT_PREVIOUS || type == RT_REPLACE_PREVIOUS ||
           type == RT_DELETE_PREVIOUS;
  }
  bool IsIllegal(uint8 rh) { return (rh & kRecordTypeMask) == RT_ILLEGAL; }
  bool IsEnd(uint8 rh) { return (rh & kRecordTypeMask) == RT_END; }

  // Varint utility functions.
  uint8* ParseVarint(uint8* p, uint64* v) {
    return const_cast<uint8*>(reinterpret_cast<const uint8*>(
        ParseVariableInteger64(reinterpret_cast<const char*>(p), v)));
  }
  uint8* SkipVarint(uint8* p) {
    return const_cast<uint8*>(reinterpret_cast<const uint8*>(
        SkipVariableInteger64(reinterpret_cast<const char*>(p))));
  }
  uint8* EncodeVarint(uint8* p, uint64 val) {
    return reinterpret_cast<uint8*>(
        MakeNonZeroVariableInteger64(reinterpret_cast<char*>(p), val));
  }

  // True if the Word containing the byte directly before p is all zeros.
  // Used to see if an RT_END should be expected at p.  Note: if p points to
  // the start of a Word, no RT_END is necessary but the previous Word will be
  // non-zero so the result will be false: no harm no foul.
  bool WordAllZeros(uint8* p) {
    return !*reinterpret_cast<uint64*>((reinterpret_cast<uintptr_t>(p) - 1) &
                                       ~7ull);
  }
  template <typename T>
  T AlignForward8(T p) {
    return reinterpret_cast<T>((reinterpret_cast<uintptr_t>(p) + 7) & ~7ull);
  }
  template <typename T>
  T AlignBackward8(T p) {
    return reinterpret_cast<T>(reinterpret_cast<uintptr_t>(p) & ~7ull);
  }
  template <typename T>
  uint8* FindEnd(T* w) {
    int j = 0;
    uint64* words = reinterpret_cast<uint64*>(w);
    for (; j < kBucketSizeInWords; j++)
      if (words[j] == 0) break;
    return reinterpret_cast<uint8*>(words + j);
  }

  // Core insertion and expansion functions.
  void InsertInternal(int level, uint64 key, uint64 key2, bool replace,
                      bool bit, int nvalues, const uint64* values,
                      RandomBase* rnd, float rate);
  bool TryInsert(uint8* bucket, uint8* p, bool previous_matches, uint64 key,
                 uint64 key2, bool replace, bool bit, int nvalues,
                 const uint64* values, RandomBase* rnd, float rate);
  void LookupInternal(int level, uint64 key, uint64 key2, bool cache, bool* bit,
                      int* n, uint64* values);
  void GetBucket(int level, int64 bucket, uint8* buffer);
  void ClearCorruption(uint8* bucket, uint8* position);
  uint64 Overflow(int level, uint64 bucket);
  uint32 GetBucketHeader(int level, int64 bucket);
  void SetBucketHeader(int level, int64 bucket, uint32 bucket_header);
  bool IsExpanding() { return metadata_[level_].last_expanded_bucket != 0; }
  void CheckExpansion();
  void StartExpanding();
  void ExpandStep();
  void ExpandFile(int level);
  void CollectRecords(int64 bucket, Records* records);
  void CopyRecord(int64 base_bucket, const Records::value_type& value);
  void NextLevel();
  void DeleteFiles();

  // Member variables.
  MMapFlags mmap_flags_;
  string prefix_;
  bool writer_ = false;
  int metadata_fd_ = 0;
  Metadata* metadata_ = nullptr;
  size_t metadata_size_ = 0;
  int file_fd_[kMaxFiles] = {};
  void* file_mmap_[kMaxFiles] = {};
  size_t file_mmap_size_[kMaxFiles] = {};
  int ticks_since_expansion_complete_ = -1;
  AtomicWord level_ = 0;
  AtomicWord lookups_ = 0;
  AtomicWord insertions_ = 0;
};

DynamicTableImpl::~DynamicTableImpl() {
  for (int i = 0; i < level_; i++) {
    if (file_mmap_[i]) {
      ::munmap(file_mmap_[i], file_mmap_size_[i]);
      ::close(file_fd_[i]);
    }
  }
}

void DynamicTableImpl::Insert(const vector<InsertOp>& ops) {
  CHECK(writer_);
  for (auto& op : ops) {
    InsertInternal(level_, op.key, op.key2, op.replace, op.bit, op.n, op.values,
                   nullptr, 0.0);
    CheckExpansion();
  }
}

void DynamicTableImpl::InsertCorrupt(const vector<InsertOp>& ops,
                                     RandomBase* rnd, float rate) {
  CHECK(writer_);
  for (auto& op : ops) {
    InsertInternal(level_, op.key, op.key2, op.replace, op.bit, op.n, op.values,
                   rnd, rate);
    CheckExpansion();
  }
}

void DynamicTableImpl::CheckExpansion() {
  auto inserts = base::subtle::NoBarrier_AtomicIncrement(&insertions_, 1);
  if (IsExpanding()) {
    if ((inserts & (kInsertionsPerExpandStep - 1)) == 0) ExpandStep();
  } else if (metadata_[level_].last_overflow_bucket >
             metadata_[level_].base_buckets * kOverflowHighwater) {
    StartExpanding();
  }
}

// Write END and clear to end of the bucket.
void DynamicTableImpl::ClearCorruption(uint8* bucket, uint8* p) {
  uint8 buffer[kBucketSizeInBytes + kPadding];  // Padding to detect overflow.
  int alignment_offset = reinterpret_cast<uintptr_t>(p) % 8;
  uint8* start = buffer + alignment_offset;
  uint8* b = start;
  int bytes_in_bucket = p - bucket;
  uint8* bend = start + kBucketSizeInBytes - bytes_in_bucket;
  if (start != buffer)  // Copy partial Word in so we don't change it.
    memcpy(buffer, p - alignment_offset, alignment_offset);
  if (b >= bend) return;
  *b++ = RT_END;
  if (b >= bend) return;
  memset(b, 0, bend - b);
  AtomicWord* to_start = AlignBackward8(reinterpret_cast<AtomicWord*>(p));
  AtomicWord* from_start = AlignBackward8(reinterpret_cast<AtomicWord*>(start));
  AtomicWord* from_end = AlignForward8(reinterpret_cast<AtomicWord*>(b));
  for (; from_start < from_end;)
    base::subtle::NoBarrier_Store(to_start++, *from_start++);
}

void DynamicTableImpl::InsertInternal(int level, uint64 key, uint64 key2,
                                      bool replace, bool bit, int nvalues,
                                      const uint64* values, RandomBase* rnd,
                                      float rate) {
LRestart:
  int bucket_shift = 64 - (level + kBucketsPerLevelLog2);
  // Use the high order bits so expanded buckets have locality.
  uint64 b = (key * kHashPrime) >> bucket_shift;
  uint64 tag = 0;   // key of the Record stored in the bucket.
  uint64 tag2 = 0;  // key2 of the Record stored in the bucket.
  for (;;) {  // Chaining loop.
    uint8* bucket = Bucket(level, b);
    uint32 bucket_header = *reinterpret_cast<uint32*>(bucket);
    if (bucket_header == kBucketHeaderExtended) {
      level++;
      goto LRestart;
    }
    uint8* p = bucket + sizeof(BucketHeader);
    uint8* end = FindEnd(bucket);
    uint8* found = nullptr;
    bool corrupt = false;
    bool previous = false;

    // Find Insertion point.
    while (p < end) {
      uint8* tmp_p = p;  // This entry could be incomplete, use a tmp.
      uint8 rh = *p;     // Record Header.
      if (IsEnd(rh)) {
        corrupt = false;
        break;
      }
      corrupt = true;  // Be conservative.
      if (IsIllegal(rh)) break;
      int n = NumberOfRecordValues(rh);
      tmp_p++;
      if (tmp_p > end) break;

      // Parse optional high order 64-bits of 128 bit tag.
      uint64 tmp_tag2 = 0;
      previous = IsPrevious(rh);
      if (Key128Bits(rh) && !previous) {
        tmp_tag2 = LittleEndian::Load64(tmp_p);
        tmp_p += sizeof(uint64);
        if (tmp_p > end) break;
      }

      // Parse tag.
      uint64 tmp_tag = 0;
      if (!IsPrevious(rh)) {
        tmp_p = ParseVarint(tmp_p, &tmp_tag);
        if (tmp_p > end) break;
        tag = tmp_tag;
        tag2 = tmp_tag2;
      }

      // Check tag.
      if (tag == key && tag2 == key2 &&
          (n == nvalues || (nvalues == 0 && IsDelete(rh)))) {
        if (n == 0 && IsDelete(rh)) {
          found = p;
          p = tmp_p;
          continue;
        }
        // If the values match, set found.
        bool match = true;
        auto tmp_values = values;
        for (; n > 0; n--) {
          uint64 tmp_v;
          tmp_p = ParseVarint(tmp_p, &tmp_v);
          if (tmp_v != *tmp_values++) match = false;
          if (tmp_p > end) goto LDone;
        }
        if (WordAllZeros(tmp_p)) break;  // The last word must not be all zeros.
        if (match) found = p;
      } else {
        for (; n > 0; n--) {        // No match, skip this record.
          if (!*tmp_p) goto LDone;  // A zero is illegal, indicating corruption.
          tmp_p = SkipVarint(tmp_p);
          if (tmp_p > end) goto LDone;
        }
        if (WordAllZeros(tmp_p)) break;  // The last word must not be all zeros.
      }
      corrupt = false;
      p = tmp_p;  // Entry was complete.
    }

  LDone:
    if (!found && bucket_header) {  // Follow the overflow chain.
      b = metadata_[level].base_buckets + bucket_header - 1;
      continue;
    }
    if (corrupt) ClearCorruption(bucket, p);
    // Set previous if we found one and it had previous set or if the previous
    // tags match our key.
    bool set_previous = found ? previous : key == tag && key2 == tag2;
    if (TryInsert(bucket, found ? found : p, set_previous, key,
                  key2, replace, bit, nvalues, values, rnd, rate))
      return;
    b = Overflow(level, b);  // Create a new overflow bucket and use it.
  }
}

// Try to insert into bucket at insertion point p.  Returns true if insertion
// was successful.  Note: no key can have more than 8 values.
bool DynamicTableImpl::TryInsert(uint8* bucket, uint8* p, bool previous_matches,
                                 uint64 key, uint64 key2, bool replace,
                                 bool bit, int n, const uint64* values,
                                 RandomBase* rnd, float rate) {
  uint8 buffer[kBucketSizeInBytes + kPadding];  // Padding to detect overflow.
  int alignment_offset = reinterpret_cast<uintptr_t>(p) % 8;
  uint8* start = buffer + alignment_offset;
  uint8* b = start;
  int bytes_in_bucket = p - bucket;
  uint8* bend = start + kBucketSizeInBytes - bytes_in_bucket;
  if (start != buffer)  // Copy partial Word in so we don't change it.
    memcpy(buffer, p - alignment_offset, alignment_offset);
  uint8 header = 0;
  if (n <= 0) {  // n == zero means delete the record.
    CHECK_EQ(n, 0);
    header = replace ? RT_DELETE_PREVIOUS : RT_DELETE;
  } else {
    header = replace ? (previous_matches ? RT_REPLACE_PREVIOUS : RT_REPLACE)
                     : (previous_matches ? RT_INSERT_PREVIOUS : RT_INSERT);
  }
  if (bit) header |= 1 << kRecordBitShift;
  if (key2) header |= 1 << kRecord128BitKeyShift;
  CHECK_LE(n, 8);  // Don't corrupt memory.
  header |= (n - 1) << kRecordValuesShift;
  *b++ = header;
  if (!previous_matches) {
    if (key2) {
      LittleEndian::Store64(b, key2);
      b += sizeof(key2);
      if (b > bend) return false;
    }
    b = EncodeVarint(b, key);
  }
  if (b > bend) return false;
  for (; n > 0; n--) {
    DCHECK_NE(*values, 0) << "Can't insert zero into dynamic table.";
    b = EncodeVarint(b, *values++);
    if (b > bend) return false;
  }
  if (b != bend) *b++ = RT_END;
  uint8* e = AlignForward8(b);
  if (b != e) {  // Copy padding from underlying buffer for idempotence.
    if (p + (b - start) + (e - b) > bucket + kBucketSizeInBytes) return false;
    memcpy(b, p + (b - start), e - b);
  }
  AtomicWord* to_start = AlignBackward8(reinterpret_cast<AtomicWord*>(p));
  AtomicWord* from_start = AlignBackward8(reinterpret_cast<AtomicWord*>(start));
  AtomicWord* from_end = AlignForward8(reinterpret_cast<AtomicWord*>(b));
  if (rnd) {  // For testing of corruption by an interrupted write.
    for (; from_start < from_end;) {
      if (rnd->RandFloat() < rate) return true;
      base::subtle::NoBarrier_Store(to_start++, *from_start++);
    }
  } else {
    for (; from_start < from_end;)
      base::subtle::NoBarrier_Store(to_start++, *from_start++);
  }
  return true;  // Success.
}

// Return a new bucket appended to the chain ending at 'bucket'.
uint64 DynamicTableImpl::Overflow(int level, uint64 bucket) {
  uint64 new_bucket =
      metadata_[level].base_buckets + metadata_[level].last_overflow_bucket;
  if ((new_bucket + 1) * kBucketSizeInBytes > file_mmap_size_[level])
    ExpandFile(level);
  // There is only one writer, so no need for atomic increment.
  metadata_[level].last_overflow_bucket++;
  uint32 overflow_bucket = metadata_[level].last_overflow_bucket;
  SetBucketHeader(level, bucket, overflow_bucket);
  return new_bucket;
}

// Note: this could be avoided on the writer if it was willing to lock out
// simultaneous readers.
void DynamicTableImpl::GetBucket(int level, int64 bucket, uint8* buffer) {
  AtomicWord* from = reinterpret_cast<AtomicWord*>(Bucket(level, bucket));
  AtomicWord* to = reinterpret_cast<AtomicWord*>(buffer);
  *to++ = base::subtle::Acquire_Load(from++);
  for (int i = 1; i < kBucketSizeInWords; i++)
    *to++ = base::subtle::NoBarrier_Load(from++);
}

uint32 DynamicTableImpl::GetBucketHeader(int level, int64 bucket) {
  AtomicWord* b = reinterpret_cast<AtomicWord*>(Bucket(level, bucket));
  uint32 header = base::subtle::NoBarrier_Load(b);  // Grab lower 32 bits.
  return header;
}

void DynamicTableImpl::SetBucketHeader(int level, int64 bucket,
                                       uint32 bucket_header) {
  AtomicWord* b = reinterpret_cast<AtomicWord*>(Bucket(level, bucket));
  uint64 header = base::subtle::NoBarrier_Load(b);
  header = (header & ~0xFFFFFFull) | bucket_header;
  base::subtle::Release_Store(b, header);
}

void DynamicTableImpl::StartExpanding() {
  CHECK(UpdateFiles(true));
  ExpandStep();
}

void DynamicTableImpl::ExpandStep() {
  for (int i = 0; i < kBucketsPerExpandStep; i++) {
    int64 b = metadata_[level_].last_expanded_bucket;
    if (b < metadata_[level_].base_buckets) {
      if (GetBucketHeader(level_, b) == kBucketHeaderExtended) continue;
      Records records;
      CollectRecords(b, &records);
      for (auto& p : records) CopyRecord(b, p);
      // Set this first, and check above to detect if (continued below)
      SetBucketHeader(level_, b, kBucketHeaderExtended);
      // (continued from above) crash here.
      metadata_[level_].last_expanded_bucket = b + 1;
    }
    if (metadata_[level_].last_expanded_bucket >=
        metadata_[level_].base_buckets)
      return NextLevel();
  }
}

void DynamicTableImpl::NextLevel() {
  base::subtle::Release_Store(&level_, level_ + 1);
  ticks_since_expansion_complete_ = 0;
  return;
}

// Collect all the Records in this bucket: merging inserted values and handling
// DELETE and REPLACE.
void DynamicTableImpl::CollectRecords(int64 b, Records* records) {
  uint64 tag = 0;
  uint64 tag2 = 0;
  for (;;) {
    uint8* bucket = Bucket(level_, b);
    uint8* p = bucket + sizeof(BucketHeader);
    uint8* end = FindEnd(bucket);

    while (p < end) {
      uint8 rh = *p;  // Record Header.
      if (IsEnd(rh) || IsIllegal(rh)) break;
      p++;
      if (p > end) break;

      if (Key128Bits(rh)) {
        if (!IsPrevious(rh)) {
          tag2 = LittleEndian::Load64(p);
          p += sizeof(uint64);
          if (p > end) break;
        }
      } else {
        tag2 = 0;
      }

      if (!IsPrevious(rh)) {
        p = ParseVarint(p, &tag);
        if (p > end) break;
      }
      if (IsDelete(rh)) {
        records->erase(make_pair(tag, tag2));
      } else {
        auto& r = (*records)[make_pair(tag, tag2)];
        if (IsReplace(rh)) r.second.clear();
        int nvalues = NumberOfRecordValues(rh);
        std::array<uint64, 8> values;
        for (int i = 0; i < nvalues; i++) {
          if (!*p) goto LDone;
          p = ParseVarint(p, &values[i]);
          if (p > end) goto LDone;
        }
        // Complete Record: insert the values.
        r.first = ExtraBit(rh);
        r.second.insert(r.second.end(), values.begin(),
                        values.begin() + nvalues);
      }
    }

  LDone:
    uint32 bucket_header = *reinterpret_cast<uint32*>(bucket);
    if (bucket_header) {  // Chain.
      auto next = metadata_[level_].base_buckets + bucket_header - 1;
      CHECK_GT(next, b);
      b = next;
      continue;
    }
    break;
  }
}

void DynamicTableImpl::CopyRecord(int64 base_bucket,
                                  const Records::value_type& value) {
  if (value.second.second.size() > 0) {
    InsertInternal(level_ + 1, value.first.first, value.first.second, false,
                   value.second.first, value.second.second.size(),
                   &value.second.second[0], nullptr, 0.0);
  }
}

void DynamicTableImpl::ExpandFile(int level) {
  // If we get here, we are probably inserting too fast and need some headroom.
  size_t new_size =
      RoundUpToPageSize(file_mmap_size_[level] + file_mmap_size_[level] * 1.1);
  if (new_size == file_mmap_size_[level]) new_size += getpagesize();
  if (file_fd_[level] >= 0) {
    CHECK_EQ(::ftruncate(file_fd_[level], new_size), 0);
  }
  file_mmap_[level] = ::mremap(file_mmap_[level], file_mmap_size_[level],
                               new_size, MREMAP_MAYMOVE);
  CHECK_NE(file_mmap_[level], MAP_FAILED);
  metadata_[level].file_size = file_mmap_size_[level] = new_size;
}

void DynamicTableImpl::Lookup(vector<LookupOp>* ops) {
  for (auto& op : *ops) {
    int level = base::subtle::Acquire_Load(&level_);
    LookupInternal(level, op.key, op.key2, op.cache, &op.bit, &op.n,
                   op.values);
    base::subtle::NoBarrier_AtomicIncrement(&lookups_, 1);
  }
}

void DynamicTableImpl::LookupInternal(int level, uint64 key, uint64 key2,
                                      bool cache, bool* bit, int* n,
                                      uint64* values) {
LRestart:
  int bucket_shift = 64 - (level + kBucketsPerLevelLog2);
  // Use the high order bits so expanded buckets have locality.
  uint64 b = (key * kHashPrime) >> bucket_shift;
  uint64* v = values;
  uint8 buffer[kBucketSizeInBytes + kPadding];
  bool last_bit = false;
  uint64 tag = 0;
  uint64 tag2 = 0;
  for (;;) {  // Chaining loop.
    GetBucket(level, b, buffer);
    uint32 bucket_header = *reinterpret_cast<uint32*>(buffer);
    if (bucket_header == kBucketHeaderExtended) {
      level++;
      goto LRestart;
    }
    uint8* p = buffer + sizeof(BucketHeader);
    uint8* end = FindEnd(buffer);
    uint64* tmp_v = v;  // This entry could be incomplete, use a tmp.

    while (p < end) {
      // Parse Bucket Header.
      uint8 rh = *p;  // Record Header.
      if (IsEnd(rh) || IsIllegal(rh)) break;
      p++;
      if (p > end) break;

      if (Key128Bits(rh)) {
        if (!IsPrevious(rh)) {
          tag2 = LittleEndian::Load64(p);
          p += sizeof(uint64);
          if (p > end) break;
        }
      } else {
        tag2 = 0;
      }

      // Parse tag.
      if (!IsPrevious(rh)) {
        p = ParseVarint(p, &tag);
        if (p > end) break;
      }
      int nvalues = NumberOfRecordValues(rh);
      if (tag != key || tag2 != key2) {  // Not a match.
        for (; nvalues > 0; nvalues--) {
          if (!*p) break;  // A zero indicates corruption.
          p = SkipVarint(p);
          if (p > end) break;
        }
        continue;  // Continue searching in the bucket.
      }
      // Match found.
      if (IsDelete(rh) || IsReplace(rh)) tmp_v = v = values;
      if (!IsDelete(rh)) {
        // Fill values.
        last_bit = ExtraBit(rh);
        for (; nvalues > 0; nvalues--) {
          p = ParseVarint(p, tmp_v++);
          if (p > end) break;
        }
        v = tmp_v;  // Entry was complete.
      }
    }

    if (bucket_header) {  // Chain.
      b = metadata_[level].base_buckets + bucket_header - 1;
      continue;
    }
    *n = v - values;
    *bit = *n ? last_bit : false;  // Only valid if we found an entry.
    return;
  }
}

void DynamicTableImpl::PurgeCache() {}

void DynamicTableImpl::Tick() {
  CHECK(UpdateMetadata());
  if (writer_) {
    ExpandStep();
    if (ticks_since_expansion_complete_ >= 0) {
      if (ticks_since_expansion_complete_ > kTicksTillDelete) {
        DeleteFiles();
        ticks_since_expansion_complete_ = -1;
      } else {
        ticks_since_expansion_complete_++;
      }
    }
  }
}

bool DynamicTableImpl::IsValid() {
  return UpdateMetadata();
}

void DynamicTableImpl::Snapshot(
    const function<void(StringPiece)>& chunk_handler) {
  int64 size = metadata_size_;
  chunk_handler(StringPiece(reinterpret_cast<char*>(&size), sizeof(size)));
  chunk_handler(
      StringPiece(reinterpret_cast<char*>(metadata_), metadata_size_));
  int start = level_ ? level_ - 1 : level_;
  for (int i = start; i <= level_ + 1; i++) {
    if (file_mmap_[i]) {
      char *p = static_cast<char*>(file_mmap_[i]);
      size_t s = file_mmap_size_[i];
      while (s) {
        size_t ss = min(s, kMaxSnapshotChunk);
        chunk_handler(StringPiece(p, ss));
        s -= ss;
        p += ss;
      }
    }
  }
}

void DynamicTableImpl::DeleteFiles() {
  int start = level_ ? level_ - 1 : level_;
  for (int i = start; i <= level_ - 1; i++) {
    if (file_mmap_[i])
      MUnMap(prefix_ + StrCat(i), writer_, file_mmap_size_[i], &file_fd_[i],
             &file_mmap_[i]);
  }
}

bool DynamicTableImpl::UpdateFiles(bool force_expand, uint64 create_buckets) {
  SetLevel();
  int start = level_ ? level_ - 1 : level_;
  int end = force_expand ? level_ + 1 : GetLastFile();
  for (int i = start; i <= end; i++) {
    if (!file_fd_[i]) {
      if (!metadata_[i].base_buckets) {
        if (!i)
          metadata_[i].base_buckets = max(create_buckets, BaseBuckets(i));
        else
          metadata_[i].base_buckets = metadata_[i - 1].base_buckets * 2;
      }
      if (!metadata_[i].file_size)
        metadata_[i].file_size = LevelDataSize(i);
      CHECK(MMap(mmap_flags_, prefix_ + StrCat(i), metadata_[i].file_size,
                 &file_fd_[i], &file_mmap_size_[i], &file_mmap_[i]));
      if (writer_)  // Store the file size in the metadata for Snapshots
                    // if we are the writer.
        metadata_[i].file_size = file_mmap_size_[i];
      ticks_since_expansion_complete_ = 0;
    }
  }
  return file_mmap_[level_] != nullptr;
}

bool DynamicTableImpl::UpdateMetadata() {
  struct stat st;
  if (fstat(metadata_fd_, &st) < 0) return false;
  if (st.st_size != metadata_size_ &&
      metadata_ !=
          ::mremap(metadata_, metadata_size_, st.st_size, mmap_flags_.flags))
    return false;
  metadata_size_ = st.st_size;
  return UpdateFiles(false);
}

void DynamicTableImpl::SetFlags() {
  SetMMapFlags(prefix_.empty(), FLAGS_dt_map_private, writer_, writer_,
               &mmap_flags_);
}

bool DynamicTableImpl::Initialize(StringPiece prefix, bool writer,
                                  uint64 create_size) {
  prefix_ = prefix.ToString();
  writer_ = writer;
  SetFlags();
  if (!MMap(mmap_flags_, prefix_ + "metadata", getpagesize(), &metadata_fd_,
            &metadata_size_, reinterpret_cast<void**>(&metadata_)))
    return false;
  if (!UpdateFiles(false, create_size / kBucketSizeInBytes)) return false;
  return true;
}

DynamicTable* DynamicTable::Open(StringPiece prefix, bool writer,
                                 uint64 create_size) {
  return DynamicTableImpl::Open(prefix, writer, create_size);
}

DynamicTable* DynamicTableImpl::Open(StringPiece prefix, bool writer,
                                     uint64 create_size) {
  unique_ptr<DynamicTableImpl> dt(new DynamicTableImpl());
  if (!dt->Initialize(prefix, writer, create_size)) return nullptr;
  return dt.release();
}

DynamicTable* DynamicTable::OpenFromSnapshot(
    StringPiece prefix, const function<StringPiece()>& chunk_source) {
  return DynamicTableImpl::OpenFromSnapshot(prefix, chunk_source);
}

DynamicTable* DynamicTableImpl::OpenFromSnapshot(
    StringPiece prefix, const function<StringPiece()>& chunk_source) {
  unique_ptr<DynamicTableImpl> dt(new DynamicTableImpl());
  if (!dt->InitializeFromSnapshot(prefix, chunk_source)) return nullptr;
  return dt.release();
}

bool DynamicTableImpl::InitializeFromSnapshot(
    StringPiece prefix, const function<StringPiece()>& chunk_source) {
  prefix_ = prefix.ToString();
  writer_ = true;
  SetFlags();
  auto piece = chunk_source();
  metadata_size_ = *(reinterpret_cast<const int64*>(piece.data()));
  if (!MMap(mmap_flags_, prefix_ + "metadata", metadata_size_, &metadata_fd_,
            &metadata_size_, reinterpret_cast<void**>(&metadata_)))
    return false;
  piece = chunk_source();
  if (piece.size() != metadata_size_) return false;
  memcpy(metadata_, piece.data(), metadata_size_);
  if (!UpdateFiles(false)) return false;
  int start = level_ ? level_ - 1 : level_;
  for (int i = start; i <= level_ + 1; i++) {
    if (file_mmap_[i]) {
      char *p = static_cast<char*>(file_mmap_[i]);
      size_t s = metadata_[i].file_size;
      while (s) {
        size_t ss = min(s, kMaxSnapshotChunk);
        piece = chunk_source();
        if (piece.size() != ss) return false;
        memcpy(p, piece.data(), ss);
        s -= ss;
        p += ss;
      }
    }
  }
  return true;
}

void DynamicTable::RestoreSnapshot(
    StringPiece prefix, const function<StringPiece()>& chunk_source) {
  delete OpenFromSnapshot(prefix, chunk_source);
}

void DynamicTable::Delete(StringPiece prefix) {
  DynamicTableImpl::Delete(prefix);
}

bool DynamicTable::Exists(StringPiece prefix) {
  return DynamicTableImpl::Exists(prefix);
}

void DynamicTableImpl::Delete(StringPiece prefix) {
  ::unlink((prefix.ToString() + "metadata").c_str());
  for (int i = 0; i < kMaxFiles; i++)
    ::unlink((prefix.ToString() + StrCat(i)).c_str());
}

bool DynamicTableImpl::Exists(StringPiece prefix) {
  int fd = ::open((prefix.ToString() + "metadata").c_str(), O_RDONLY);
  ::close(fd);
  return fd > 0;
