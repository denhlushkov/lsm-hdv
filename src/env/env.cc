#include <memory>

#include "lsm/env.h"

namespace lsm {

Status WriteStringToFile(Env* env, std::string_view data,
                         const std::string& fname, bool sync) {
  std::unique_ptr<WritableFile> file;
  Status s = env->NewWritableFile(fname, &file);
  if (!s.ok()) {
    return s;
  }
  s = file->Append(data);
  if (s.ok() && sync) {
    s = file->Sync();
  }
  if (s.ok()) {
    s = file->Close();
  }
  if (!s.ok()) {
    env->RemoveFile(fname);
  }
  return s;
}

Status ReadFileToString(Env* env, const std::string& fname, std::string* data) {
  data->clear();
  std::unique_ptr<SequentialFile> file;
  Status s = env->NewSequentialFile(fname, &file);
  if (!s.ok()) {
    return s;
  }
  constexpr size_t kBufferSize = 8192;
  auto space = std::make_unique<char[]>(kBufferSize);
  while (true) {
    std::string_view fragment;
    s = file->Read(kBufferSize, &fragment, space.get());
    if (!s.ok() || fragment.empty()) {
      break;
    }
    data->append(fragment.data(), fragment.size());
  }
  return s;
}

}  // namespace lsm
