#include "objc.h"

#include <llvm/Support/Format.h>
#include <llvm/Support/JSON.h>

namespace xlate {

namespace {

class Reader {
 public:
  explicit Reader(const Image &image) : image_(image) {}

  uint32_t U32(uint64_t host) const {
    uint8_t bytes[4] = {};
    image_.ReadBytes(host, bytes, 4);
    return bytes[0] | bytes[1] << 8 | bytes[2] << 16 | static_cast<uint32_t>(bytes[3]) << 24;
  }

  uint64_t U64(uint64_t host) const {
    return U32(host) | static_cast<uint64_t>(U32(host + 4)) << 32;
  }

  Pointer Read(uint64_t host) const {
    Pointer pointer;
    uint64_t vmaddr = host - image_.slide();
    if (auto bind = image_.binds().find(vmaddr); bind != image_.binds().end()) {
      pointer.kind = Pointer::Import;
      pointer.symbol = bind->second.symbol;
      return pointer;
    }
    if (auto rebase = image_.rebases().find(vmaddr); rebase != image_.rebases().end()) {
      pointer.kind = Pointer::Local;
      pointer.host = image_.host(rebase->second & 0x00FFFFFFFFFFFFFFull);
      return pointer;
    }
    uint64_t raw = U64(host);
    pointer.kind = raw ? Pointer::Raw : Pointer::Null;
    pointer.host = raw;
    return pointer;
  }

  uint64_t Local(uint64_t host, std::vector<std::string> &errors, const char *what) const {
    auto pointer = Read(host);
    if (pointer.kind == Pointer::Null) {
      return 0;
    }
    if (pointer.kind != Pointer::Local) {
      errors.push_back(std::string(what) + " is not a pointer into the image");
      return 0;
    }
    return pointer.host;
  }

  std::string String(uint64_t host) const {
    std::string text;
    for (uint8_t c = 0; host && image_.ReadBytes(host, &c, 1) && c; ++host) {
      text.push_back(static_cast<char>(c));
    }
    return text;
  }

  const Section *SectionNamed(const char *name) const {
    for (auto &section : image_.sections()) {
      if (section.sectname == name && section.segname.rfind("__DATA", 0) == 0) {
        return &section;
      }
    }
    return nullptr;
  }

  const Image &image() const { return image_; }

 private:
  const Image &image_;
};

std::vector<Method> ReadMethods(const Reader &reader, uint64_t list, std::vector<std::string> &errors) {
  std::vector<Method> methods;
  if (!list) {
    return methods;
  }
  uint32_t header = reader.U32(list);
  uint32_t count = reader.U32(list + 4);
  bool relative = header & 0x80000000u;
  uint32_t entsize = header & 0x0000FFFCu;
  for (uint32_t i = 0; i < count; ++i) {
    uint64_t entry = list + 8 + static_cast<uint64_t>(i) * entsize;
    Method method;
    if (relative) {
      int32_t name = static_cast<int32_t>(reader.U32(entry));
      int32_t types = static_cast<int32_t>(reader.U32(entry + 4));
      int32_t imp = static_cast<int32_t>(reader.U32(entry + 8));
      auto selref = reader.Read(entry + name);
      if (selref.kind != Pointer::Local) {
        errors.push_back("relative method name does not point to a selector reference");
        continue;
      }
      method.name_address = selref.host;
      method.types_address = entry + 4 + types;
      method.imp = entry + 8 + imp;
    } else {
      if (entsize != 24) {
        errors.push_back("unexpected method entry size");
        return methods;
      }
      method.name_address = reader.Local(entry, errors, "method name");
      method.types_address = reader.Local(entry + 8, errors, "method types");
      method.imp = reader.Local(entry + 16, errors, "method implementation");
    }
    method.selector = reader.String(method.name_address);
    method.types = reader.String(method.types_address);
    methods.push_back(method);
  }
  return methods;
}

std::vector<Property> ReadProperties(const Reader &reader, uint64_t list, std::vector<std::string> &errors) {
  std::vector<Property> properties;
  if (!list) {
    return properties;
  }
  uint32_t entsize = reader.U32(list);
  uint32_t count = reader.U32(list + 4);
  for (uint32_t i = 0; i < count; ++i) {
    uint64_t entry = list + 8 + static_cast<uint64_t>(i) * entsize;
    Property property;
    property.name_address = reader.Local(entry, errors, "property name");
    property.attributes_address = reader.Local(entry + 8, errors, "property attributes");
    property.name = reader.String(property.name_address);
    property.attributes = reader.String(property.attributes_address);
    properties.push_back(property);
  }
  return properties;
}

std::vector<uint64_t> ReadProtocolList(const Reader &reader, uint64_t list, std::vector<std::string> &errors) {
  std::vector<uint64_t> protocols;
  if (!list) {
    return protocols;
  }
  uint64_t count = reader.U64(list);
  for (uint64_t i = 0; i < count; ++i) {
    if (uint64_t protocol = reader.Local(list + 8 + i * 8, errors, "protocol list entry")) {
      protocols.push_back(protocol);
    }
  }
  return protocols;
}

ClassData ReadData(const Reader &reader, uint64_t ro, std::vector<std::string> &errors) {
  ClassData data;
  data.flags = reader.U32(ro);
  data.instance_start = reader.U32(ro + 4);
  data.instance_size = reader.U32(ro + 8);
  data.name_address = reader.Local(ro + 24, errors, "class name");
  data.name = reader.String(data.name_address);
  data.methods = ReadMethods(reader, reader.Local(ro + 32, errors, "method list"), errors);
  data.protocols = ReadProtocolList(reader, reader.Local(ro + 40, errors, "class protocols"), errors);
  if (uint64_t ivars = reader.Local(ro + 48, errors, "ivar list")) {
    uint32_t entsize = reader.U32(ivars);
    uint32_t count = reader.U32(ivars + 4);
    for (uint32_t i = 0; i < count; ++i) {
      uint64_t entry = ivars + 8 + static_cast<uint64_t>(i) * entsize;
      Ivar ivar;
      ivar.offset_address = reader.Local(entry, errors, "ivar offset");
      if (ivar.offset_address) {
        ivar.offset_value = reader.U32(ivar.offset_address);
      }
      ivar.name_address = reader.Local(entry + 8, errors, "ivar name");
      ivar.type_address = reader.Local(entry + 16, errors, "ivar type");
      ivar.alignment = reader.U32(entry + 24);
      ivar.size = reader.U32(entry + 28);
      ivar.name = reader.String(ivar.name_address);
      ivar.type = reader.String(ivar.type_address);
      data.ivars.push_back(ivar);
    }
  }
  data.properties = ReadProperties(reader, reader.Local(ro + 64, errors, "property list"), errors);
  return data;
}

}  // namespace

ObjCImage AnalyzeObjC(const Image &image) {
  ObjCImage objc;
  Reader reader(image);
  auto &errors = objc.errors;
  if (auto list = reader.SectionNamed("__objc_classlist")) {
    for (uint64_t offset = 0; offset < list->size; offset += 8) {
      ObjCClass cls;
      cls.address = reader.Local(image.host(list->addr + offset), errors, "class list entry");
      if (!cls.address) {
        continue;
      }
      cls.metaclass = reader.Local(cls.address, errors, "class isa");
      cls.superclass = reader.Read(cls.address + 8);
      uint64_t data = reader.Local(cls.address + 32, errors, "class data") & ~7ull;
      cls.data = ReadData(reader, data, errors);
      cls.meta_isa = reader.Read(cls.metaclass);
      cls.meta_superclass = reader.Read(cls.metaclass + 8);
      uint64_t meta_data = reader.Local(cls.metaclass + 32, errors, "metaclass data") & ~7ull;
      cls.meta_data = ReadData(reader, meta_data, errors);
      objc.classes.push_back(cls);
    }
  }
  if (auto list = reader.SectionNamed("__objc_catlist")) {
    for (uint64_t offset = 0; offset < list->size; offset += 8) {
      uint64_t address = reader.Local(image.host(list->addr + offset), errors, "category list entry");
      if (!address) {
        continue;
      }
      ObjCCategory category;
      category.name_address = reader.Local(address, errors, "category name");
      category.name = reader.String(category.name_address);
      category.cls = reader.Read(address + 8);
      category.instance_methods = ReadMethods(reader, reader.Local(address + 16, errors, "category methods"), errors);
      category.class_methods = ReadMethods(reader, reader.Local(address + 24, errors, "category class methods"), errors);
      category.protocols = ReadProtocolList(reader, reader.Local(address + 32, errors, "category protocols"), errors);
      category.properties = ReadProperties(reader, reader.Local(address + 40, errors, "category properties"), errors);
      objc.categories.push_back(category);
    }
  }
  if (auto list = reader.SectionNamed("__cfstring")) {
    for (uint64_t offset = 0; offset + 32 <= list->size; offset += 32) {
      uint64_t address = image.host(list->addr + offset);
      auto isa = reader.Read(address);
      if (isa.kind != Pointer::Import || isa.symbol != "___CFConstantStringClassReference") {
        errors.push_back("constant string without the CFString class");
        continue;
      }
      ConstantString string;
      string.address = address;
      string.flags = reader.U32(address + 8);
      string.string = reader.Local(address + 16, errors, "constant string contents");
      string.length = reader.U64(address + 24);
      objc.strings.push_back(string);
    }
  }
  if (auto list = reader.SectionNamed("__objc_selrefs")) {
    for (uint64_t offset = 0; offset < list->size; offset += 8) {
      SelectorReference selref;
      selref.slot = image.host(list->addr + offset);
      selref.string = reader.Local(selref.slot, errors, "selector reference");
      selref.selector = reader.String(selref.string);
      objc.selectors.push_back(selref);
    }
  }
  if (auto list = reader.SectionNamed("__objc_protolist")) {
    for (uint64_t offset = 0; offset < list->size; offset += 8) {
      ObjCProtocol protocol;
      protocol.address = reader.Local(image.host(list->addr + offset), errors, "protocol list entry");
      if (!protocol.address) {
        continue;
      }
      uint64_t address = protocol.address;
      protocol.name_address = reader.Local(address + 8, errors, "protocol name");
      protocol.name = reader.String(protocol.name_address);
      protocol.protocols = ReadProtocolList(reader, reader.Local(address + 16, errors, "adopted protocols"), errors);
      protocol.instance_methods = ReadMethods(reader, reader.Local(address + 24, errors, "protocol methods"), errors);
      protocol.class_methods = ReadMethods(reader, reader.Local(address + 32, errors, "protocol class methods"), errors);
      protocol.optional_instance_methods = ReadMethods(reader, reader.Local(address + 40, errors, "optional methods"), errors);
      protocol.optional_class_methods = ReadMethods(reader, reader.Local(address + 48, errors, "optional class methods"), errors);
      protocol.properties = ReadProperties(reader, reader.Local(address + 56, errors, "protocol properties"), errors);
      protocol.size = reader.U32(address + 64);
      protocol.flags = reader.U32(address + 68);
      bool duplicate = false;
      for (auto &existing : objc.protocols) {
        duplicate |= existing.address == protocol.address;
      }
      if (!duplicate) {
        objc.protocols.push_back(protocol);
      }
    }
  }
  return objc;
}

namespace {

void WritePointer(llvm::json::OStream &json, const char *key, const Pointer &pointer) {
  json.attributeObject(key, [&] {
    switch (pointer.kind) {
      case Pointer::Null:
        json.attribute("kind", "null");
        break;
      case Pointer::Local:
        json.attribute("kind", "local");
        json.attribute("address", static_cast<int64_t>(pointer.host));
        break;
      case Pointer::Import:
        json.attribute("kind", "import");
        json.attribute("symbol", pointer.symbol);
        break;
      case Pointer::Raw:
        json.attribute("kind", "raw");
        json.attribute("address", static_cast<int64_t>(pointer.host));
        break;
    }
  });
}

void WriteMethods(llvm::json::OStream &json, const char *key, const std::vector<Method> &methods) {
  json.attributeArray(key, [&] {
    for (auto &method : methods) {
      json.object([&] {
        json.attribute("selector", method.selector);
        json.attribute("types", method.types);
        json.attribute("name_address", static_cast<int64_t>(method.name_address));
        json.attribute("imp", static_cast<int64_t>(method.imp));
      });
    }
  });
}

void WriteProperties(llvm::json::OStream &json, const std::vector<Property> &properties) {
  json.attributeArray("properties", [&] {
    for (auto &property : properties) {
      json.object([&] {
        json.attribute("name", property.name);
        json.attribute("attributes", property.attributes);
        json.attribute("name_address", static_cast<int64_t>(property.name_address));
        json.attribute("attributes_address", static_cast<int64_t>(property.attributes_address));
      });
    }
  });
}

void WriteAddresses(llvm::json::OStream &json, const char *key, const std::vector<uint64_t> &addresses) {
  json.attributeArray(key, [&] {
    for (auto address : addresses) {
      json.value(static_cast<int64_t>(address));
    }
  });
}

void WriteData(llvm::json::OStream &json, const char *key, const ClassData &data) {
  json.attributeObject(key, [&] {
    json.attribute("flags", static_cast<int64_t>(data.flags));
    json.attribute("instance_start", static_cast<int64_t>(data.instance_start));
    json.attribute("instance_size", static_cast<int64_t>(data.instance_size));
    json.attribute("name", data.name);
    json.attribute("name_address", static_cast<int64_t>(data.name_address));
    WriteMethods(json, "methods", data.methods);
    json.attributeArray("ivars", [&] {
      for (auto &ivar : data.ivars) {
        json.object([&] {
          json.attribute("name", ivar.name);
          json.attribute("type", ivar.type);
          json.attribute("offset_address", static_cast<int64_t>(ivar.offset_address));
          json.attribute("offset_value", static_cast<int64_t>(ivar.offset_value));
          json.attribute("name_address", static_cast<int64_t>(ivar.name_address));
          json.attribute("type_address", static_cast<int64_t>(ivar.type_address));
          json.attribute("alignment", static_cast<int64_t>(ivar.alignment));
          json.attribute("size", static_cast<int64_t>(ivar.size));
        });
      }
    });
    WriteProperties(json, data.properties);
    WriteAddresses(json, "protocols", data.protocols);
  });
}

}  // namespace

void WriteManifest(llvm::raw_ostream &os, const std::vector<const Image *> &images, const std::vector<ObjCImage> &objc) {
  llvm::json::OStream json(os, 1);
  json.object([&] {
    json.attributeArray("images", [&] {
      for (size_t index = 0; index < images.size(); ++index) {
        auto &image = *images[index];
        auto &info = objc[index];
        json.object([&] {
          json.attribute("path", image.path());
          json.attribute("index", static_cast<int64_t>(index));
          json.attributeArray("errors", [&] {
            for (auto &error : info.errors) {
              json.value(error);
            }
          });
          json.attributeArray("classes", [&] {
            for (auto &cls : info.classes) {
              json.object([&] {
                json.attribute("address", static_cast<int64_t>(cls.address));
                json.attribute("metaclass", static_cast<int64_t>(cls.metaclass));
                WritePointer(json, "superclass", cls.superclass);
                WritePointer(json, "meta_isa", cls.meta_isa);
                WritePointer(json, "meta_superclass", cls.meta_superclass);
                WriteData(json, "data", cls.data);
                WriteData(json, "meta_data", cls.meta_data);
              });
            }
          });
          json.attributeArray("categories", [&] {
            for (auto &category : info.categories) {
              json.object([&] {
                json.attribute("name", category.name);
                json.attribute("name_address", static_cast<int64_t>(category.name_address));
                WritePointer(json, "class", category.cls);
                WriteMethods(json, "instance_methods", category.instance_methods);
                WriteMethods(json, "class_methods", category.class_methods);
                WriteProperties(json, category.properties);
                WriteAddresses(json, "protocols", category.protocols);
              });
            }
          });
          json.attributeArray("protocols", [&] {
            for (auto &protocol : info.protocols) {
              json.object([&] {
                json.attribute("address", static_cast<int64_t>(protocol.address));
                json.attribute("name", protocol.name);
                json.attribute("name_address", static_cast<int64_t>(protocol.name_address));
                json.attribute("flags", static_cast<int64_t>(protocol.flags));
                WriteAddresses(json, "protocols", protocol.protocols);
                WriteMethods(json, "instance_methods", protocol.instance_methods);
                WriteMethods(json, "class_methods", protocol.class_methods);
                WriteMethods(json, "optional_instance_methods", protocol.optional_instance_methods);
                WriteMethods(json, "optional_class_methods", protocol.optional_class_methods);
                WriteProperties(json, protocol.properties);
              });
            }
          });
          json.attributeArray("selectors", [&] {
            for (auto &selref : info.selectors) {
              json.object([&] {
                json.attribute("slot", static_cast<int64_t>(selref.slot));
                json.attribute("selector", selref.selector);
              });
            }
          });
          json.attribute("constant_strings", static_cast<int64_t>(info.strings.size()));
        });
      }
    });
  });
}

}  // namespace xlate
