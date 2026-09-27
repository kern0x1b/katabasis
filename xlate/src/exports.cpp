#include "exports.h"

#include <llvm/Support/raw_ostream.h>

namespace xlate {

namespace {

constexpr unsigned kMaxReexportDepth = 8;

bool IsSystemPath(const std::string &path) {
  return path.rfind("/System/", 0) == 0 || path.rfind("/usr/lib/", 0) == 0;
}

}  // namespace

std::string LibraryKey(const std::string &install_name) {
  auto framework = install_name.find(".framework/");
  if (framework != std::string::npos) {
    auto start = install_name.rfind('/', framework);
    return install_name.substr(start == std::string::npos ? 0 : start + 1);
  }
  auto slash = install_name.rfind('/');
  return slash == std::string::npos ? install_name : install_name.substr(slash + 1);
}

void GuestExports::Add(const Image &image, bool bridge) {
  for (auto &[name, vmaddr] : image.exports()) {
    // A class two guest images both define is told apart by the library a bind names; a bind with none gets the first.
    if (name.rfind("_OBJC_CLASS_$_", 0) != 0) {
      continue;
    }
    for (auto earlier : images_) {
      if (earlier->exports().count(name)) {
        llvm::errs() << "xlate: " << image.path() << ": " << name << " is also defined by " << earlier->path()
                     << "; a bind that names no library takes that one\n";
        break;
      }
    }
  }
  images_.push_back(&image);
  if (bridge) {
    bridges_.push_back(&image);
  }
  if (!image.install_name().empty()) {
    auto [existing, added] = by_key_.emplace(LibraryKey(image.install_name()), &image);
    if (!added) {
      llvm::errs() << "xlate: " << image.path() << ": " << image.install_name() << " is named like " << existing->second->path()
                   << "; a bind to that library takes that one\n";
    }
  }
}

std::optional<uint64_t> GuestExports::Find(const std::string &library, const std::string &symbol) const {
  return Find(library, symbol, 0);
}

std::optional<uint64_t> GuestExports::Find(const std::string &library, const std::string &symbol, unsigned depth) const {
  if (library.empty()) {
    for (auto image : images_) {
      if (auto found = image->exports().find(symbol); found != image->exports().end()) {
        return image->host(found->second);
      }
    }
    return std::nullopt;
  }
  if (auto found = by_key_.find(LibraryKey(library)); found != by_key_.end()) {
    auto &image = *found->second;
    if (auto exported = image.exports().find(symbol); exported != image.exports().end()) {
      return image.host(exported->second);
    }
    if (auto sent = image.reexports().find(symbol); sent != image.reexports().end() && depth < kMaxReexportDepth) {
      if (auto target = Find(sent->second.library, sent->second.symbol, depth + 1)) {
        return target;
      }
    }
  } else if (!IsSystemPath(library) && reported_.insert(library).second) {
    // Right for a framework the build does not lift, wrong for a library a guest image is under another spelling.
    llvm::errs() << "xlate: a bind names " << library << ", which is no guest image and no system library; "
                 << "its symbols are taken from the host\n";
  }
  for (auto bridge : bridges_) {
    if (auto exported = bridge->exports().find(symbol); exported != bridge->exports().end()) {
      return bridge->host(exported->second);
    }
  }
  return std::nullopt;
}

}  // namespace xlate
