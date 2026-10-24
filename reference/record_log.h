// A log of tuples and values.

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <string>
using ::std::string;

#include "base/integral_types.h"
#include "base/logging.h"
#include "util/coder.h"
#include "strings/stringpiece.h"

namespace blythe {

class RecordLog {
 public:
  class Tuple;
  class Value;

  static const uint64 kVersion = 0;  // Default.
  enum CompressionType {
    COMPRESSION_NONE = 0,
    COMPRESSION_V1 = 1,
    COMPRESSION_LAST = COMPRESSION_V1
  };
  static const uint64 kWrittenVersion = 1;
  static const int kPaddingRequired = 7;    // Log must have this much headroom.
  static const int kHeader = 1;             // First byte is the version.
  static const uint32 kMagic = 0xe3e0ba2f;  // Random.

  // This header is stored with persistent version of the RecordLog.  Its
  // physical layout is part of the description of a RecordLog: little endian in
  // the given order.  Static_asserts guarantee this in the .cc file.
  struct PersistentHeader {
    uint32 magic;
    uint8 version;      // RecordLog Version.
    uint8 compression;  // Compression Type/Version, 0 is uncompressed.
    uint8 written;      // Write Type/Version, 0 = unwritten, 1+ = restorable.
    uint8 reserved;     // Reserved for future use: should be zero.
    uint64 start_id;    // The first ID; 1 for a restorable root database.
    char body[0];
  };
  PersistentHeader& GetHeader() {
    return *reinterpret_cast<PersistentHeader*>(this);
  }
  const PersistentHeader& GetHeader() const {
    return *reinterpret_cast<const PersistentHeader*>(this);
  }
  char* body() { return &GetHeader().body[0]; }
  const char* body() const { return &GetHeader().body[0]; }

  struct Entry {
    const uint8* uchars() const { return reinterpret_cast<const uint8*>(this); }
    const char* chars() const { return reinterpret_cast<const char*>(this); }
    char* chars() { return reinterpret_cast<char*>(this); }
    // The high order bit determines if an Entry is a Record (1) or Value (0).
    bool IsTuple() const { return *uchars() >> 7; }
    bool IsValue() const { return !IsTuple(); }
    const Tuple* AsTuple() const {
      return IsTuple() ? reinterpret_cast<const Tuple*>(this) : nullptr;
    }
    const Value* AsValue() const {
      return IsValue() ? reinterpret_cast<const Value*>(this) : nullptr;
    }
    const Tuple* ToTuple() const {
      return reinterpret_cast<const Tuple*>(this);
    }
    const Value* ToValue() const {
      return reinterpret_cast<const Value*>(this);
    }
    size_t bytes() const {
      if (IsValue())
        return ToValue()->bytes();
      else
        return ToTuple()->bytes();
    }
    size_t safe_bytes(int64 limit) const {
      if (IsValue())
        return ToValue()->safe_bytes(limit);
      else
        return ToTuple()->safe_bytes(limit);
    }
  };

  struct Value : Entry {
    StringPiece ToStringPiece() const {
      uint8 tag;
      uint64 n;
      // The size is a varaible integer where the high order bit was used to
      // indicate that this is a value.
      auto p = ParseTaggedVariableInteger(chars(), 1, &tag, &n);
      return StringPiece(p, n);
    }
    char* c_str() const {
      uint8 tag;
      uint64 n;
      return const_cast<char*>(
          ParseTaggedVariableInteger(chars(), 1, &tag, &n));
    }
    size_t size() const { return ToStringPiece().size(); }
    string ToString() const { return ToStringPiece().ToString(); }
    size_t bytes() const {
      uint8 tag;
      uint64 n;
      auto p = ParseTaggedVariableInteger(chars(), 1, &tag, &n);
      return p - chars() + n;
    }
    size_t safe_bytes(int64 limit) const {
      uint8 tag;
      uint64 n;
      auto p =
          ParseTaggedVariableIntegerSafe(chars(), chars() + limit, 1, &tag, &n);
      if (!p || p + n > chars() + limit) return 0;
      return p - chars() + n;
    }
  };

  struct Tuple : Entry {
    size_t size() const {
      uint8 tag;
      uint64 n;
      ParseTaggedVariableInteger(chars(), 1, &tag, &n);
      DCHECK_NE(n, 0);
      return n;
    }
    char* first() const {
      return const_cast<char*>(SkipTaggedVariableInteger(chars(), 1));
    }
    bool extra_bit() const { return ExtraBitFromGroupVarInt5(first()); }
    uint64 field0() const { return ParseGroupVarInt5Field0(first()); }
    uint64 field1() const { return ParseGroupVarInt5Field1(first()); }
    uint64 field2() const { return ParseGroupVarInt5Field2(first()); }
    uint64 field3() const { return ParseGroupVarInt5Field3(first()); }
    uint64 field4() const { return ParseGroupVarInt5Field4(first()); }
    uint64 id0(uint64 base) const {
      return DecodeField(base, ParseGroupVarInt5Field0(first()));
    }
    uint64 id1(uint64 base) const {
      return DecodeField(base, ParseGroupVarInt5Field1(first()));
    }
    uint64 id2(uint64 base) const {
      return DecodeField(base, ParseGroupVarInt5Field2(first()));
    }
    uint64 id3(uint64 base) const {
      return DecodeField(base, ParseGroupVarInt5Field3(first()));
    }
    uint64 id4(uint64 base) const {
      return DecodeField(base, ParseGroupVarInt5Field4(first()));
    }
    static uint64 DecodeField(uint64 base, uint64 value) {
      int64 offset = (value >> 1) ^ -(value & 1);
      return offset < 0 ? (base + offset) : offset;
    }
    size_t bytes() const {
      const char* p = chars(), *x = p;
      uint8 tag;
      uint64 n;
      x = ParseTaggedVariableInteger(x, 1, &tag, &n);
      for (; n >= 5; n -= 5) x = SkipGroupVarInt5(x);
      switch (n) {
        case 0:
          break;
        case 1:
          x = SkipGroupVarInt2Safe1(x);
          break;
        case 2:
          x = SkipGroupVarInt2(x);
          break;
        case 3:
          x = SkipGroupVarInt5Safe3(x);
          break;
        case 4:
          x = SkipGroupVarInt5Safe4(x);
          break;
      }
      return x - p;
    }
    // Returns 0 if the complete object is missing.
    size_t safe_bytes(size_t limit) const {
      const char* p = chars(), *x = p;
      uint8 tag;
      uint64 n;
      x = ParseTaggedVariableIntegerSafe(x, x + limit, 1, &tag, &n);
      if (!x) return 0;
      // GroupVarInt5 uses 2 prefix bytes and GroupVarInt2 uses 1.
      for (; n >= 5; n -= 5) {
        if (x + 2 >= p + limit) return 0;
        x = SkipGroupVarInt5(x);
      }
      switch (n) {
        case 0:
          break;
        case 1:
          if (x + 1 >= p + limit) return 0;
          x = SkipGroupVarInt2Safe1(x);
          break;
        case 2:
          if (x + 1 >= p + limit) return 0;
          x = SkipGroupVarInt2(x);
          break;
        case 3:
          if (x + 2 >= p + limit) return 0;
          x = SkipGroupVarInt5Safe3(x);
          break;
        case 4:
          if (x + 2 >= p + limit) return 0;
          x = SkipGroupVarInt5Safe4(x);
          break;
      }
      if (x > p + limit) return 0;
      return x - p;
    }
    // Within the limit conservatively estimate the size of the tuples in bytes.
    size_t bytes_estimate(size_t limit) const {
      const char* p = chars(), *x = p, *e = p + limit;
      uint8 tag;
      uint64 n;
      x = ParseTaggedVariableInteger(x, 1, &tag, &n);
      for (; n >= 5; n -= 5) {
        if (x + 2 > e) return limit + n * 9;
        x = SkipGroupVarInt5(x);
      }
      if (x + 2 > e) return limit + n * 9;
      switch (n) {
        case 0:
          break;
        case 1:
          x = SkipGroupVarInt2Safe1(x);
          break;
        case 2:
          x = SkipGroupVarInt2(x);
          break;
        case 3:
          x = SkipGroupVarInt5Safe3(x);
          break;
        case 4:
          x = SkipGroupVarInt5Safe4(x);
          break;
      }
      return x - p;
    }
    const char* Decode(uint64 id, int n, bool* extra_bit, uint64* ids) const {
      auto p = const_cast<char*>(reinterpret_cast<const char*>(this));
      const char* x = p;
      x = const_cast<char*>(SkipTaggedVariableInteger(x, 1));
      bool unused;
      for (; n >= 5; n -= 5, ids += 5) {
        x = DecodeGroupVarInt5(x, id, extra_bit, ids);
        extra_bit = &unused;
      }
      switch (n) {
        case 0:
          break;
        case 1:
          x = DecodeGroupVarInt2Safe1(x, id, extra_bit, ids);
          break;
        case 2:
          x = DecodeGroupVarInt2(x, id, extra_bit, ids);
          break;
        case 3:
          x = DecodeGroupVarInt5Safe3(x, id, extra_bit, ids);
          break;
        case 4:
          x = DecodeGroupVarInt5Safe4(x, id, extra_bit, ids);
          break;
      }
      return x;
    }
  };

  static void MakeDefaultHeader(PersistentHeader* header, uint64 next_id) {
    header->magic = kMagic;
    header->version = kVersion;
    header->compression = 0;
    header->written = 0;
    header->start_id = next_id;
  }
  bool IsValid() {
    return GetHeader().magic == kMagic && GetHeader().version == kVersion;
  }
  static const RecordLog* Adopt(const void* p) {
    return reinterpret_cast<const RecordLog*>(p);
  }
  static RecordLog* Adopt(void* p) { return reinterpret_cast<RecordLog*>(p); }
  static RecordLog* Make(void* p, uint64 next_id = 1) {
    MakeDefaultHeader(reinterpret_cast<struct PersistentHeader*>(p), next_id);
    return reinterpret_cast<RecordLog*>(p);
  }
  uint64 NextID(size_t size) const {
    return start_id() + size - sizeof(PersistentHeader);
  }
  static size_t Size(uint64 start_id, uint64 next_id) {
    return next_id - start_id + sizeof(PersistentHeader);
  }
  size_t Size(uint64 next_id) const {
    return reinterpret_cast<const char*>(GetEntry(next_id)) -
           reinterpret_cast<const char*>(this);
  }
  size_t BodySize(uint64 next_id) {
    return Size(next_id) - sizeof(PersistentHeader);
  }
  void Free() const { ::free(const_cast<RecordLog*>(this)); }
  StringPiece AsStringPiece(uint64 next_id) const {
    return StringPiece(const_cast<char*>(reinterpret_cast<const char*>(this)),
                       Size(next_id));
  }
  uint64 start_id() const { return GetHeader().start_id; }
  Entry* GetEntry(uint64 id) {
    return reinterpret_cast<Entry*>(body() + id - start_id());
  }
  const Entry* GetEntry(uint64 id) const {
    return reinterpret_cast<const Entry*>(body() + id - start_id());
  }
  bool IsValue(uint64 id) const { return GetEntry(id)->IsValue(); }
  bool IsTuple(uint64 id) const { return GetEntry(id)->IsTuple(); }
  Value* GetValue(uint64 id) { return reinterpret_cast<Value*>(GetEntry(id)); }
  const Value* GetValue(uint64 id) const {
    return reinterpret_cast<const Value*>(GetEntry(id));
  }
  uint64 ValueSize(uint64 id) { return GetValue(id)->size(); }
  StringPiece GetBlob(uint64 id) { return GetValue(id)->ToStringPiece(); }
  Tuple* GetTuple(uint64 id) {
    return reinterpret_cast<Tuple*>(GetEntry(id));
  }
  const Tuple* GetTuple(uint64 id) const {
    return reinterpret_cast<const Tuple*>(GetEntry(id));
  }
  uint64 TupleSize(uint64 id) const { return GetTuple(id)->size(); }
  uint64 DecodeTupleWithOffset(uint64 id, uint64 offset, int n, bool* extra_bit,
                               uint64* ids) const {
    auto t = GetTuple(id);
    auto p = reinterpret_cast<const char*>(t);
    return id + (GetTuple(id)->Decode(offset, n, extra_bit, ids) - p);
  }
  uint64 DecodeTuple(uint64 id, int n, bool* extra_bit, uint64* ids) const {
    return DecodeTupleWithOffset(id, id, n, extra_bit, ids);
  }
  uint64 MakeNode(uint64 id) {
    auto p = reinterpret_cast<char*>(GetEntry(id));
    *p = 0;
    return id + 1;
  }
  uint64 MakeValue(uint64 id, StringPiece s) {
    auto p = reinterpret_cast<char*>(GetTuple(id));
    size_t size = s.size();
    auto x = MakeTaggedVariableInteger(p, 0, 1, size);
    memcpy(x, s.begin(), size);
    return id + ((x + size) - p);
  }
  static size_t EncodeValue(char* p, StringPiece s) {
    size_t size = s.size();
    auto x = MakeTaggedVariableInteger(p, 0, 1, size);
    memcpy(x, s.begin(), size);
    return (x - p) + size;
  }
  // A 48 bit TaggedVariableInteger can hold values up to ~300TB using 7 bytes.
  static uint64 MaxMakeValueSize(StringPiece s) {
    size_t size = s.size();
    return TaggedVariableIntegerSize(1, size) + size;
  }
  static uint64 EncodeTupleWithOffset(char* p, uint64 id, uint64 offset,
                                      bool extra_bit, int n, uint64* ids) {
    auto x = p;
    x = MakeTaggedVariableInteger(x, 1, 1, n);
    for (; n >= 5; n -= 5, ids += 5) {
      x = EncodeGroupVarInt5(x, offset, extra_bit, ids);
      extra_bit = false;
    }
    switch (n) {
      case 0:
        break;
      case 1:
        x = EncodeGroupVarInt2Safe1(x, offset, extra_bit, ids);
        break;
      case 2:
        x = EncodeGroupVarInt2(x, offset, extra_bit, ids);
        break;
      case 3:
        x = EncodeGroupVarInt5Safe3(x, offset, extra_bit, ids);
        break;
      case 4:
        x = EncodeGroupVarInt5Safe4(x, offset, extra_bit, ids);
        break;
    }
    return id + (x - p);
  }
  uint64 MakeTupleWithOffset(uint64 id, uint64 offset, bool extra_bit, int n,
                             uint64* ids) {
    auto p = reinterpret_cast<char*>(GetTuple(id));
    return EncodeTupleWithOffset(p, id, offset, extra_bit, n, ids);
  }
  uint64 MakeTuple(uint64 id, bool extra_bit, int n, uint64* ids) {
    return MakeTupleWithOffset(id, id, extra_bit, n, ids);
  }
  // A 48 bit TaggedVariableInteger for tuples up to ~300T linkages in 7 bytes.
  static uint64 MaxMakeTupleSize(int n) { return 7 + 9 * n; }

  // RecordLog(s) are allocated with ::malloc, override delete so unique_ptr
  // works.
  static void operator delete(void* ptr, std::size_t size) { ::free(ptr); }
};
