#include "lsm/iterator.h"

namespace lsm {

namespace {

class EmptyIterator final : public Iterator {
 public:
  explicit EmptyIterator(Status s) : status_(std::move(s)) {}
  bool Valid() const override { return false; }
  void SeekToFirst() override {}
  void Seek(std::string_view) override {}
  void Next() override {}
  std::string_view key() const override { return {}; }
  std::string_view value() const override { return {}; }
  Status status() const override { return status_; }

 private:
  Status status_;
};

}  // namespace

std::unique_ptr<Iterator> NewEmptyIterator() {
  return std::make_unique<EmptyIterator>(Status::OK());
}

std::unique_ptr<Iterator> NewErrorIterator(const Status& status) {
  return std::make_unique<EmptyIterator>(status);
}

}  // namespace lsm
