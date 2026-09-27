#pragma once

#include "macho.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace xlate {

// What names a library. dyld resolves @rpath/, @executable_path/ and @loader_path/ by search and a system path by the
// file the system has; xlate has neither a search path nor a system, so it names a library by its file: the dylib's
// own name, or X.framework/X. translate.sh matches the bundle's frameworks the same way.
std::string LibraryKey(const std::string &install_name);

// What the guest images define, looked up the way a two-level bind does it: a bind that names a library takes the
// export of the guest image that library is (and what that image re-exports), never that of another image that
// happens to define the same name, so a host framework's class is not taken by a guest image that also defines it.
// A bind that names no library (flat namespace, weak coalescing) takes the first image that defines the name.
// A bridge image stands for the host's libraries, so every bind that no library image answers may take its export.
class GuestExports {
 public:
  void Add(const Image &image, bool bridge);
  std::optional<uint64_t> Find(const std::string &library, const std::string &symbol) const;

 private:
  std::optional<uint64_t> Find(const std::string &library, const std::string &symbol, unsigned depth) const;

  std::vector<const Image *> images_;
  std::vector<const Image *> bridges_;
  std::map<std::string, const Image *> by_key_;
  mutable std::set<std::string> reported_;
};

}  // namespace xlate
