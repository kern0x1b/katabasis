#pragma once

#include "macho.h"

#include <llvm/Support/raw_ostream.h>

#include <string>
#include <vector>

namespace xlate {

struct Pointer {
  enum Kind { Null, Local, Import, Raw } kind = Null;
  uint64_t host = 0;
  std::string symbol;
};

struct Method {
  std::string selector;
  std::string types;
  uint64_t name_address = 0;
  uint64_t types_address = 0;
  uint64_t imp = 0;
};

struct Ivar {
  std::string name;
  std::string type;
  uint64_t offset_address = 0;
  uint32_t offset_value = 0;
  uint64_t name_address = 0;
  uint64_t type_address = 0;
  uint32_t alignment = 0;
  uint32_t size = 0;
};

struct Property {
  std::string name;
  std::string attributes;
  uint64_t name_address = 0;
  uint64_t attributes_address = 0;
};

struct ObjCProtocol {
  uint64_t address = 0;
  std::string name;
  uint64_t name_address = 0;
  std::vector<uint64_t> protocols;
  std::vector<Method> instance_methods;
  std::vector<Method> class_methods;
  std::vector<Method> optional_instance_methods;
  std::vector<Method> optional_class_methods;
  std::vector<Property> properties;
  uint32_t flags = 0;
  uint32_t size = 0;
};

struct ClassData {
  uint32_t flags = 0;
  uint32_t instance_start = 0;
  uint32_t instance_size = 0;
  std::string name;
  uint64_t name_address = 0;
  std::vector<Method> methods;
  std::vector<Ivar> ivars;
  std::vector<Property> properties;
  std::vector<uint64_t> protocols;
};

struct ObjCClass {
  uint64_t address = 0;
  Pointer superclass;
  ClassData data;
  uint64_t metaclass = 0;
  Pointer meta_isa;
  Pointer meta_superclass;
  ClassData meta_data;
};

struct ObjCCategory {
  std::string name;
  uint64_t name_address = 0;
  Pointer cls;
  std::vector<Method> instance_methods;
  std::vector<Method> class_methods;
  std::vector<Property> properties;
  std::vector<uint64_t> protocols;
};

struct ConstantString {
  uint64_t address = 0;
  uint32_t flags = 0;
  uint64_t string = 0;
  uint64_t length = 0;
};

struct SelectorReference {
  uint64_t slot = 0;
  uint64_t string = 0;
  std::string selector;
};

struct ObjCImage {
  std::vector<ObjCClass> classes;
  std::vector<ObjCCategory> categories;
  std::vector<ObjCProtocol> protocols;
  std::vector<ConstantString> strings;
  std::vector<SelectorReference> selectors;
  std::vector<std::string> errors;
};

ObjCImage AnalyzeObjC(const Image &image);
void WriteManifest(llvm::raw_ostream &os, const std::vector<const Image *> &images, const std::vector<ObjCImage> &objc);

}  // namespace xlate
