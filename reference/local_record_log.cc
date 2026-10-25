#undef _GNU_SOURCE
#define _GNU_SOURCE
#undef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64

#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <memory>
#include <string>
using ::std::string;
#include <vector>
using ::std::vector;

#include "gflags/gflags.h"
#include "base/integral_types.h"
#include "base/logging.h"
#include "file/base/file.h"
#include "file/base/fileutils.h"
#include "file/base/helpers.h"
#include "common/errors.h"
#include "common/record.h"
#include "pgi/local_pgi.h"
#include "pgi/pgi.h"
#include "pgi/pgi_callbacks.h"
#include "pgi/pgi_resource_manager.h"
#include "util/mmap_backing.h"
#include "util/record_log.h"
#include "util/tagged_ptr.h"
#include "strings/strcat.h"
#include "util/random/secure_random.h"
#include "util/task/codes.pb.h"
#include "util/task/status.h"

DEFINE_bool(
    rlog_map_private, false,
    "Map the RecordLog MAP_PRIVATE so that local changes are not written "
    "back to the backing files (i.e. for testing).");

using std::unique_ptr;
using util::Status;

namespace blythe {
namespace {
const int kMaxFiles = 256;
const int kBaseSize = 1 << 16;  // 64K
const int kDefaultLastID = kBaseSize - sizeof(RecordLog::PersistentHeader);
const int kExpandToBaseSizeLimit = 1 << 15;
const int kExpandBy20PercentLimit = 1 << 22;  // 4MB
const int64 kMinCreateSize = 1 << 16;         // 64K.

string MetadataPath(StringPiece prefix) {
  return prefix.ToString() + ".metadata";
}
string LogPath(StringPiece prefix, int i) { return StrCat(prefix, ".", i); }
}  // namespace

void LocalRecordLog::SetFlags(bool create) {
  SetMMapFlags(AnonymousMMaps(), FLAGS_rlog_map_private, create, writer_,
               &mmap_flags_);
}

RecordLog* LocalRecordLog::last_log() const {
  return files_[files_.size() - 1].log;
}

LocalRecordLog::~LocalRecordLog() {
  if (metadata_fd_ >= 0) ::close(metadata_fd_);
  if (mmapped_metadata_)
    ::munmap(mmapped_metadata_, RoundUpToPageSize(sizeof(PgiMetadata)));
  for (auto f : files_) {
    ::close(f.fd);
    ::munmap(f.log, f.size);
  }
}

void LocalRecordLog::UpdateStartID(ID start_id) {
  metadata_.start_id = start_id;
  metadata_.next_id = start_id;
  if (!metadata_.pre_next_id)  // Do not overwrite commit intent.
    metadata_.pre_next_id = start_id;
  WriteMetadata(metadata_, nullptr);
  RecordLog::MakeDefaultHeader(&files_[0].log->GetHeader(), start_id);
  last_id_ = start() + files_[0].size - sizeof(RecordLog::PersistentHeader);
}

Status LocalRecordLog::Initialize(StringPiece prefix, bool writer, ID start_id,
                                  int64 create_size) {
  prefix_ = prefix.ToString();
  writer_ = writer;
  SetFlags(create_size != kDoNotCreateSize);
  create_size = max(create_size, kMinCreateSize);
  if (!MMap(mmap_flags_, MetadataPath(prefix_),
            RoundUpToPageSize(sizeof(PgiMetadata)), &metadata_fd_,
            &metadata_size_, reinterpret_cast<void**>(&mmapped_metadata_)))
    return Status(::util::error::INTERNAL,
                  "unable to open record log metadata file");
  ReadMetadata(nullptr);
  if (!LoadFiles(start_id + create_size))
    return Status(::util::error::INTERNAL, "unable to open rlog data files");
  if (!metadata_.uuid.uuid0 && !metadata_.uuid.uuid1) {
    std::unique_ptr<SecureRandom> rng(
        new ARC4Random(SecureRandom::SEED_FROM_URANDOM));
    metadata_.uuid.uuid0 = rng->Rand64();
    metadata_.uuid.uuid1 = rng->Rand64();
    UpdateStartID(start_id);
  }
  SetFlags(writer_);  // create == writer so we can create additional files.
  return Status::OK;
}

// Open existing files, and if last_id_ < new_last_id open an additional file
// to cover the new space.
bool LocalRecordLog::LoadFiles(uint64 new_last_id) {
  auto create_size =
      RoundUpToPageSize(new_last_id + sizeof(RecordLog::PersistentHeader));
  if (AnonymousMMaps()) {
    files_.resize(1);
    auto& f = files_[0];
    f.size = create_size;
    if (!MMap(mmap_flags_, LogPath(prefix_, 0), f.size, &f.fd, &f.size,
              reinterpret_cast<void**>(&f.log)))
      return false;
    last_id_ = start() + f.size - sizeof(RecordLog::PersistentHeader);
  } else {
    int i = 0;
    for (; i < kMaxFiles; i++) {
      string fn = LogPath(prefix_, i);
      int fd =
          ::open(fn.c_str(), mmap_flags_.open_flags & ~O_CREAT, kOpenPerms);
      if (fd < 0) break;
      files_.resize(files_.size() + 1);
      auto& f = files_[i];
      f.size = !i ? create_size : 0;
      if (!MMap(mmap_flags_, fn, f.size, &f.fd, &f.size,
                reinterpret_cast<void**>(&f.log))) {
        files_.resize(files_.size() - 1);
        return false;
      }
      last_id_ = f.log->GetHeader().start_id + f.size -
                 sizeof(RecordLog::PersistentHeader);
    }
    if (!i || last_id_ < new_last_id)
      if (!Expand(new_last_id)) return false;
    auto& f = files_[files_.size() - 1];
    last_id_ = f.log->GetHeader().start_id + f.size -
               sizeof(RecordLog::PersistentHeader);
  }
  return true;
}

void LocalRecordLog::ReadMetadata(PgiCallback* done) {
  metadata_ = *mmapped_metadata_;
  if (done) done->Run(Status::OK);
}

void LocalRecordLog::WriteMetadata(const PgiMetadata& metadata,
                                   PgiCallback* done) {
  *mmapped_metadata_ = metadata_ = metadata;
  if (done) done->Run(Status::OK);
}

uint64 LocalRecordLog::ExpandSize(uint64 new_last_id) {
  if (new_last_id < kExpandToBaseSizeLimit)
    new_last_id += kBaseSize;
  else if (new_last_id > kExpandBy20PercentLimit)
    new_last_id *= 1.2;
  else
    new_last_id *= 2;
  return new_last_id;
}

bool LocalRecordLog::Expand(uint64 new_last_id) {
  bool new_file = !files_.size();  // If we have no files we need to create one.
  if (!new_file) {                 // Try to expand an existing file.
    int i = files_.size() - 1;
    auto& f = files_[i];
    size_t new_size = RoundUpToPageSize(f.size + new_last_id - last_id_ +
                                        RecordLog::kPaddingRequired);
    if (MMap(mmap_flags_, LogPath(prefix_, i), new_size, &f.fd, &f.size,
             reinterpret_cast<void**>(&f.log))) {
      last_id_ = f.log->GetHeader().start_id + f.size -
                 sizeof(RecordLog::PersistentHeader);
      return true;
    }
  }
  int n = files_.size();
  files_.resize(n + 1);
  auto& f = files_[n];
  f.size = RoundUpToPageSize(new_last_id - next() +
                             sizeof(RecordLog::PersistentHeader));
  if (MMap(mmap_flags_, LogPath(prefix_, n), f.size, &f.fd, &f.size,
           reinterpret_cast<void**>(&f.log))) {
    RecordLog::MakeDefaultHeader(&f.log->GetHeader(), next());
    last_id_ = f.log->GetHeader().start_id + f.size -
               sizeof(RecordLog::PersistentHeader);
    return true;
  }
  files_.resize(n);
  return false;
}

void LocalRecordLog::Append(PgiTransaction* t, PgiBufferPtr<Record> records,
                            PgiCallback* done) {
  if (next() < kDBStartID) UpdateStartID(records[0].id());
  for (auto& r : records) {
    if (r.id() != next()) {
      CHECK(done);
      done->Run(Status(::util::error::INTERNAL, "Bad IDs in Append."));
      return;
    }
    uint64 bytes = r->bytes();
    if (r.IsNode()) {
      *last_log()->GetEntry(next())->chars() = 0;
    } else {
      if (next() + bytes > last_id_) Expand(ExpandSize(next() + bytes));
      memcpy(last_log()->GetEntry(next())->chars(), r->chars(), bytes);
    }
    next() += bytes;
  }
  if (done) done->Run(Status::OK);
}

void LocalRecordLog::AppendRecord(const Record& r) {
  if (next() < kDBStartID) UpdateStartID(r.id());
  DCHECK(r.id() == next());
  uint64 bytes = r->bytes();
  if (r.IsNode()) {
    *last_log()->GetEntry(next())->chars() = 0;
  } else {
    if (next() + bytes > last_id_) Expand(ExpandSize(next() + bytes));
    memcpy(last_log()->GetEntry(next())->chars(), r->chars(), bytes);
  }
  next() += bytes;
}

void LocalRecordLog::Get(PgiBufferPtr<Record> values, PgiCallback* done,
                         size_t limit) const {
  int n = files_.size();
  for (auto& p : values) {
    if (p.id() < start_id() || p.id() >= next_id()) continue;
    int i = 0;
    for (; i < n - 1; i++)
      if (p.id() < files_[i + 1].log->start_id()) break;
    p.reset(MakeRawRecordLogRef(files_[i].log->GetEntry(p.id())));
  }
  if (done) done->Run(Status::OK);
}

void LocalRecordLog::GetRange(ID start_id, ID end_id,
                              vector<PgiBufferPtr<char>>* buffers,
                              vector<bool>* buffer_is_ephmeral,
                              PgiCallback* done) const {
  int n = files_.size();
  int i = 0;
  if (end_id > next_id()) {
    if (done) done->Run(Status(::util::error::INTERNAL, "end_id out of range"));
    return;
  }
  while (start_id < end_id) {
    for (; i < n - 1; i++)  // Find the file containing start_id.
      if (start_id < files_[i + 1].log->start_id()) break;
    ID next =
        i >= files_.size() - 1 ? next_id() : files_[i + 1].log->start_id();
    next = end_id < next ? end_id : next;
    buffers->resize(buffers->size() + 1);
    buffer_is_ephmeral->push_back(false);  // Local memory never moves.
    (*buffers)[buffers->size() - 1].set_alias(
        reinterpret_cast<char*>(files_[i].log->GetEntry(start_id)),
        reinterpret_cast<char*>(files_[i].log->GetEntry(next)));
    start_id = next;
  }
  if (done) done->Run(Status::OK);
}

void LocalRecordLog::WriteSnapshot(StringPiece snapshot_path) const {
  FileCloser file(
      File::Open(snapshot_path.ToString(), "w"));
  file->Write(&GetMetadata(), sizeof(GetMetadata()));
  int s = files_.size();
  ID next_id = GetMetadata().next_id;
  for (int i = 0; i < s; i++) {
    ID last_id_in_file = i == s - 1 ? next_id : files_[i + 1].log->start_id();
    if (!i)
      // Write the PersistenHeader of the first file.
      file->Write(files_[0].log, files_[0].log->Size(last_id_in_file));
    else
      file->Write(files_[i].log->body(),
                  files_[i].log->BodySize(last_id_in_file));
  }
}

Status OpenLocalRecordLogFromSnapshot(StringPiece prefix,
                                      StringPiece snapshot_path,
                                      unique_ptr<LocalRecordLog>* result) {
  unique_ptr<LocalRecordLog> log(new LocalRecordLog);
  FileCloser file(
      File::Open(snapshot_path.ToString(), "r"));
  if (!file.get())
    return Status(::util::error::INTERNAL, "unable to open snapshot");
  auto size = file->Size();
  auto record_log_size = size - sizeof(log->GetMetadata());
  auto data_size = record_log_size - sizeof(RecordLog::PersistentHeader);
  if (NotOK status{log->Initialize(prefix, true, kDBStartID, data_size)})
    return status;
  file->Read(log->mmapped_metadata_, sizeof(log->GetMetadata()));
  log->metadata_ = *log->mmapped_metadata_;
  CHECK(log->files_[0].size >= record_log_size);
  file->Read(log->files_[0].log, record_log_size);
  *result = std::move(log);
  return Status::OK;
}

// All persistent database files start with the pathname "prefix".
Status CreateLocalRecordLog(StringPiece prefix, uint64 create_size) {
  unique_ptr<LocalRecordLog> log(new LocalRecordLog);
  return log->Initialize(prefix, true, 0, create_size);
}

Status OpenLocalRecordLog(StringPiece prefix, bool writer,
                          std::unique_ptr<PgiRecordLog>* result) {
  unique_ptr<LocalRecordLog> log(new LocalRecordLog);
  auto status = log->Initialize(prefix, writer, 0, 0);
  if (!status.ok()) return status;
  (*result) = std::move(log);
  return Status::OK;
}

bool LocalRecordLog::Exists(StringPiece prefix) {
  int fd = ::open(MetadataPath(prefix).c_str(), O_RDONLY);
  ::close(fd);
  return fd >= 0;
}

bool LocalRecordLog::IsValid(StringPiece prefix) {
  // TODO(jplev): extend to include consistency checking.
  return Exists(prefix);
}

void DeleteLocalRecordLog(StringPiece prefix) {
  ::unlink(MetadataPath(prefix).c_str());
  for (int i = 0; i < kMaxFiles; i++) ::unlink(LogPath(prefix, i).c_str());
}
