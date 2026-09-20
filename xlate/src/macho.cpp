#include "macho.h"

#include <llvm/BinaryFormat/MachO.h>
#include <llvm/Object/MachOUniversal.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/LEB128.h>

#include <algorithm>
#include <cstring>

namespace xlate {

using namespace llvm;
using namespace llvm::object;

std::unique_ptr<Image> Image::Load(const std::string &path, std::string &error) {
  auto binary = createBinary(path);
  if (!binary) {
    error = path + ": " + toString(binary.takeError());
    return nullptr;
  }
  auto image = std::unique_ptr<Image>(new Image);
  image->path_ = path;
  auto [bin, buffer] = binary->takeBinary();
  if (auto *universal = dyn_cast<MachOUniversalBinary>(bin.get())) {
    auto slice = universal->getMachOObjectForArch("arm64");
    if (!slice) {
      error = path + ": no arm64 slice";
      return nullptr;
    }
    auto data = slice->get()->getMemoryBufferRef();
    auto copy = MemoryBuffer::getMemBufferCopy(data.getBuffer(), path);
    auto reparsed = createBinary(copy->getMemBufferRef());
    if (!reparsed) {
      error = path + ": " + toString(reparsed.takeError());
      return nullptr;
    }
    image->owning_ = OwningBinary<Binary>(std::move(*reparsed), std::move(copy));
  } else {
    image->owning_ = OwningBinary<Binary>(std::move(bin), std::move(buffer));
  }
  image->object_ = dyn_cast<MachOObjectFile>(image->owning_.getBinary());
  if (!image->object_ || !image->object_->is64Bit() ||
      image->object_->getHeader64().cputype != MachO::CPU_TYPE_ARM64) {
    error = path + ": not an arm64 Mach-O";
    return nullptr;
  }
  if (!image->Parse(error)) {
    return nullptr;
  }
  return image;
}

uint64_t Image::preferred_base() const {
  uint64_t base = UINT64_MAX;
  for (auto &segment : segments_) {
    if (segment.name != "__PAGEZERO" && segment.vmsize) {
      base = std::min(base, segment.vmaddr);
    }
  }
  return base;
}

uint64_t Image::image_end() const {
  uint64_t end = 0;
  for (auto &segment : segments_) {
    if (segment.name != "__PAGEZERO" && segment.name != "__LINKEDIT") {
      end = std::max(end, segment.vmaddr + segment.vmsize);
    }
  }
  return end;
}

const Segment *Image::SegmentForVM(uint64_t vmaddr) const {
  for (auto &segment : segments_) {
    if (segment.name != "__PAGEZERO" && vmaddr >= segment.vmaddr &&
        vmaddr < segment.vmaddr + segment.vmsize) {
      return &segment;
    }
  }
  return nullptr;
}

bool Image::ReadBytes(uint64_t host_addr, uint8_t *out, size_t size) const {
  uint64_t vmaddr = host_addr - slide_;
  auto segment = SegmentForVM(vmaddr);
  if (!segment || vmaddr + size > segment->vmaddr + segment->vmsize) {
    return false;
  }
  auto data = object_->getData();
  for (size_t i = 0; i < size; ++i) {
    uint64_t offset = vmaddr + i - segment->vmaddr;
    out[i] = offset < segment->filesize
                 ? static_cast<uint8_t>(data[segment->fileoff + offset])
                 : 0;
  }
  return true;
}

const Section *Image::SectionAt(uint64_t host_addr) const {
  uint64_t vmaddr = host_addr - slide_;
  for (auto &section : sections_) {
    if (vmaddr >= section.addr && vmaddr < section.addr + section.size) {
      return &section;
    }
  }
  return nullptr;
}

bool Image::IsExecutable(uint64_t host_addr) const {
  auto section = SectionAt(host_addr);
  return section && (section->flags & (MachO::S_ATTR_PURE_INSTRUCTIONS |
                                       MachO::S_ATTR_SOME_INSTRUCTIONS));
}

bool Image::Parse(std::string &error) {
  for (auto &lc : object_->load_commands()) {
    switch (lc.C.cmd) {
      case MachO::LC_SEGMENT_64: {
        auto command = object_->getSegment64LoadCommand(lc);
        Segment segment;
        segment.name = std::string(command.segname, strnlen(command.segname, 16));
        segment.vmaddr = command.vmaddr;
        segment.vmsize = command.vmsize;
        segment.fileoff = command.fileoff;
        segment.filesize = command.filesize;
        segment.initprot = command.initprot;
        segments_.push_back(segment);
        for (unsigned i = 0; i < command.nsects; ++i) {
          auto raw = object_->getSection64(lc, i);
          Section section;
          section.segname = std::string(raw.segname, strnlen(raw.segname, 16));
          section.sectname = std::string(raw.sectname, strnlen(raw.sectname, 16));
          section.addr = raw.addr;
          section.size = raw.size;
          section.offset = raw.offset;
          section.flags = raw.flags;
          section.reserved1 = raw.reserved1;
          section.reserved2 = raw.reserved2;
          sections_.push_back(section);
        }
        break;
      }
      case MachO::LC_ID_DYLIB: {
        auto command = object_->getDylibIDLoadCommand(lc);
        install_name_ = std::string(lc.Ptr + command.dylib.name);
        break;
      }
      case MachO::LC_MAIN: {
        auto command = object_->getEntryPointCommand(lc);
        entry_ = command.entryoff;
        break;
      }
      case MachO::LC_FUNCTION_STARTS:
        ParseFunctionStarts(lc);
        break;
      case MachO::LC_DATA_IN_CODE:
        ParseDataInCode(lc);
        break;
      default:
        break;
    }
  }
  uint64_t text_base = 0;
  for (auto &segment : segments_) {
    if (segment.name == "__TEXT") {
      text_base = segment.vmaddr;
    }
  }
  if (entry_) {
    entry_ = *entry_ + text_base;
  }
  for (auto start : raw_function_starts_) {
    functions_.insert(start + text_base);
  }
  if (entry_) {
    functions_.insert(*entry_);
  }
  for (auto &section : sections_) {
    if (section.sectname != "__objc_stubs") {
      continue;
    }
    auto data = object_->getData();
    for (uint64_t offset = 0; offset + 4 <= section.size; offset += 4) {
      uint32_t word = 0;
      memcpy(&word, data.data() + section.offset + offset, 4);
      if ((word & 0x9f00001f) == 0x90000001) {
        functions_.insert(section.addr + offset);
      }
    }
  }
  if (!ParseStubs(error) || !ParseFixups(error)) {
    return false;
  }
  ParseExports();
  return true;
}

// Data-in-code entries mark byte ranges inside __text that are data, not instructions --
// almost always compiler jump tables (DICE_KIND_JUMP_TABLE*). Those are already handled: the
// lifter stops at the indirect branch that precedes a table (no static fallthrough into it) and
// JumpTables recovers the targets, so the table bytes are never decoded as code. Earlier the
// mere presence of any data-in-code aborted the whole translation; instead accept it and only
// warn about raw-data islands (DICE_KIND_DATA), which a fallthrough path could still misdecode.
bool Image::ParseDataInCode(const MachOObjectFile::LoadCommandInfo &lc) {
  auto command = object_->getLinkeditDataLoadCommand(lc);
  auto data = object_->getData();
  const auto *ptr = reinterpret_cast<const uint8_t *>(data.data() + command.dataoff);
  unsigned count = command.datasize / 8, raw = 0;
  for (unsigned i = 0; i < count; i++) {
    uint16_t kind;
    memcpy(&kind, ptr + i * 8 + 6, sizeof kind);
    if (kind == MachO::DICE_KIND_DATA) {
      ++raw;
    }
  }
  if (raw) {
    warnings_.push_back(path_ + ": LC_DATA_IN_CODE has " + std::to_string(raw) + " of " +
                        std::to_string(count) + " entries that are raw data (not jump tables); "
                        "a fallthrough path into one could misdecode");
  }
  return true;
}

bool Image::ParseFunctionStarts(const MachOObjectFile::LoadCommandInfo &lc) {
  auto command = object_->getLinkeditDataLoadCommand(lc);
  auto data = object_->getData();
  const auto *ptr = reinterpret_cast<const uint8_t *>(data.data() + command.dataoff);
  const auto *end = ptr + command.datasize;
  uint64_t address = 0;
  while (ptr < end) {
    unsigned length = 0;
    uint64_t delta = decodeULEB128(ptr, &length, end);
    ptr += length;
    if (!delta) {
      break;
    }
    address += delta;
    raw_function_starts_.push_back(address);
  }
  return true;
}

bool Image::ParseStubs(std::string &error) {
  auto dysymtab = object_->getDysymtabLoadCommand();
  std::vector<std::string> symbol_names;
  for (auto &symbol : object_->symbols()) {
    auto name = symbol.getName();
    symbol_names.push_back(name ? name->str() : std::string());
  }
  for (auto &section : sections_) {
    auto type = section.flags & MachO::SECTION_TYPE;
    if (type != MachO::S_SYMBOL_STUBS) {
      continue;
    }
    if (!section.reserved2) {
      error = path_ + ": stub section with zero entry size";
      return false;
    }
    uint64_t count = section.size / section.reserved2;
    for (uint64_t i = 0; i < count; ++i) {
      uint32_t index = object_->getIndirectSymbolTableEntry(dysymtab, section.reserved1 + i);
      if (index & (MachO::INDIRECT_SYMBOL_LOCAL | MachO::INDIRECT_SYMBOL_ABS)) {
        continue;
      }
      if (index >= symbol_names.size()) {
        error = path_ + ": indirect symbol out of range";
        return false;
      }
      stubs_[section.addr + i * section.reserved2] = symbol_names[index];
    }
  }
  return true;
}

bool Image::ParseFixups(std::string &error) {
  auto data = object_->getData();
  auto read64 = [&](uint64_t vmaddr) -> std::optional<uint64_t> {
    auto segment = SegmentForVM(vmaddr);
    if (!segment || vmaddr + 8 > segment->vmaddr + segment->filesize) {
      return std::nullopt;
    }
    uint64_t value = 0;
    memcpy(&value, data.data() + segment->fileoff + (vmaddr - segment->vmaddr), 8);
    return value;
  };
  Error err = Error::success();
  for (auto &entry : object_->rebaseTable(err)) {
    auto value = read64(entry.address());
    if (!value) {
      error = path_ + ": rebase outside file data";
      consumeError(std::move(err));
      return false;
    }
    rebases_[entry.address()] = *value;
  }
  if (err) {
    error = path_ + ": " + toString(std::move(err));
    return false;
  }
  auto add_binds = [&](auto range) -> bool {
    for (auto &entry : range) {
      Bind bind;
      bind.symbol = entry.symbolName().str();
      bind.addend = entry.addend();
      bind.weak_import = entry.flags() & MachO::BIND_SYMBOL_FLAGS_WEAK_IMPORT;
      binds_[entry.address()] = bind;
    }
    if (err) {
      error = path_ + ": " + toString(std::move(err));
      return false;
    }
    return true;
  };
  if (!add_binds(object_->bindTable(err)) || !add_binds(object_->lazyBindTable(err)) ||
      !add_binds(object_->weakBindTable(err))) {
    return false;
  }
  for (auto &entry : object_->fixupTable(err)) {
    if (entry.isRebase()) {
      rebases_[entry.address()] = entry.pointerValue();
    } else {
      Bind bind;
      bind.symbol = entry.symbolName().str();
      bind.addend = entry.addend();
      bind.weak_import = entry.flags() & MachO::BIND_SYMBOL_FLAGS_WEAK_IMPORT;
      binds_[entry.address()] = bind;
    }
  }
  if (err) {
    error = path_ + ": " + toString(std::move(err));
    return false;
  }
  return true;
}

void Image::ParseExports() {
  for (auto &symbol : object_->symbols()) {
    auto raw = object_->getSymbol64TableEntry(symbol.getRawDataRefImpl());
    if ((raw.n_type & MachO::N_STAB) || (raw.n_type & MachO::N_TYPE) != MachO::N_SECT) {
      continue;
    }
    auto name = symbol.getName();
    if (!name || name->empty()) {
      continue;
    }
    if (raw.n_type & MachO::N_EXT) {
      exports_[name->str()] = raw.n_value;
    }
  }
}

}  // namespace xlate
