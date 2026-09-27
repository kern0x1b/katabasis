#include "tsd_scan.h"

// farmdec is the same aarch64 decoder rellume itself uses to lift (deps/rellume/subprojects/
// farmdec); librellume.dylib already exports fad_decode and friends (`nm -gU
// librellume.dylib | grep fad_`), so this scan calls the real decoder xlate links against
// rather than hand-rolling a second, unaudited one for two instruction shapes that between them
// have a couple dozen sub-encodings (LDR/STR alone: register, unscaled, pre/post-index,
// literal...). Not FARMDEC_INTERNAL: farmdec.h gives real stdint types and puts everything in
// namespace farmdec, which is all a caller needs.
#include <farmdec.h>

#include <llvm/BinaryFormat/MachO.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <sstream>

namespace xlate {

namespace {

std::string Hex(uint64_t value) {
  std::ostringstream os;
  os << "0x" << std::hex << value;
  return os.str();
}

bool IsAllowedOffset(uint64_t offset) {
  return std::any_of(std::begin(kAllowedTSDOffsets), std::end(kAllowedTSDOffsets),
                      [&](auto &entry) { return entry.offset == offset; });
}

const char *WhyAllowed(uint64_t offset) {
  for (auto &entry : kAllowedTSDOffsets) {
    if (entry.offset == offset) return entry.why;
  }
  return "";
}

// Ops whose rt/rt2 (the union farmdec's Inst.rd/rt/rt2 slot) names a SIMD/FP register: writing
// it is not a GPR clobber, and it is never the GPR our TSD tracking cares about.
bool IsVectorDestOp(farmdec::Op op) {
  switch (op) {
    case farmdec::A64_LD1_MULT: case farmdec::A64_ST1_MULT:
    case farmdec::A64_LD2_MULT: case farmdec::A64_ST2_MULT:
    case farmdec::A64_LD3_MULT: case farmdec::A64_ST3_MULT:
    case farmdec::A64_LD4_MULT: case farmdec::A64_ST4_MULT:
    case farmdec::A64_LD1_SINGLE: case farmdec::A64_ST1_SINGLE:
    case farmdec::A64_LD2_SINGLE: case farmdec::A64_ST2_SINGLE:
    case farmdec::A64_LD3_SINGLE: case farmdec::A64_ST3_SINGLE:
    case farmdec::A64_LD4_SINGLE: case farmdec::A64_ST4_SINGLE:
    case farmdec::A64_LD1R: case farmdec::A64_LD2R:
    case farmdec::A64_LD3R: case farmdec::A64_LD4R:
    case farmdec::A64_LDNP_FP: case farmdec::A64_STNP_FP:
    case farmdec::A64_LDP_FP: case farmdec::A64_STP_FP:
    case farmdec::A64_LDR_FP: case farmdec::A64_STR_FP:
      return true;
    default:
      return false;
  }
}

// Ops whose rt (and, for pairs, rt2) is a SOURCE value being written to memory or tested, not a
// destination: the GPR it names is read here, never clobbered by the instruction.
bool IsStoreLikeOp(farmdec::Op op) {
  switch (op) {
    case farmdec::A64_ST1_MULT: case farmdec::A64_ST2_MULT:
    case farmdec::A64_ST3_MULT: case farmdec::A64_ST4_MULT:
    case farmdec::A64_ST1_SINGLE: case farmdec::A64_ST2_SINGLE:
    case farmdec::A64_ST3_SINGLE: case farmdec::A64_ST4_SINGLE:
    case farmdec::A64_STNP: case farmdec::A64_STP:
    case farmdec::A64_STR: case farmdec::A64_STXR: case farmdec::A64_STXP:
      return true;
    default:
      return false;
  }
}

bool IsTestBranchOp(farmdec::Op op) {
  switch (op) {
    case farmdec::A64_CBZ: case farmdec::A64_CBNZ:
    case farmdec::A64_TBZ: case farmdec::A64_TBNZ:
      return true;
    default:
      return false;
  }
}

// True if `inst` definitely overwrites GPR `r` with something other than the raw TPIDRRO_EL0
// value (so tracking must stop). Deliberately conservative in the safe direction: an op this
// does not recognize as a plain single-GPR-destination write returns false (tracking continues)
// rather than true -- losing track of a register that still validly holds the TSD pointer would
// let a later real access through it go unchecked (a false negative, the dangerous direction);
// wrongly leaving a clobbered register "tracked" only risks flagging an unrelated instruction
// that reused the register for something else (a false positive: loud, safe, and the whole point
// of a scanner whose job is to fail loudly on anything it cannot certify).
bool ClobbersGPR(const farmdec::Inst &inst, farmdec::Reg r) {
  if (IsVectorDestOp(inst.op) || IsStoreLikeOp(inst.op) || IsTestBranchOp(inst.op)) {
    return false;
  }
  switch (inst.op) {
    case farmdec::A64_LDP: case farmdec::A64_LDNP: case farmdec::A64_LDXP:
      return inst.rt == r || inst.rt2 == r;
    default:
      return inst.rd == r;
  }
}

// True if `inst` reads GPR `r` in any role at all (base, index, source value stored, value
// tested by a conditional branch, ...). Used only to decide whether to FLAG an instruction as an
// unrecognized use of a live TPIDRRO_EL0 register, so being loose here is the safe direction too:
// the second union slot (rm for ALU ops and register-offset addressing, rt2/rs for pairs and
// atomics) is checked uniformly as "rm" -- misreading which exact role it plays never turns a
// real use into a non-use, it can only flag something this scanner does not need to flag, which
// a human then confirms and this file's classification is extended to cover.
bool UsesReg(const farmdec::Inst &inst, farmdec::Reg r) {
  if (inst.rn == r || inst.rm == r) {
    return true;
  }
  if ((IsStoreLikeOp(inst.op) || IsTestBranchOp(inst.op)) && inst.rt == r) {
    return true;
  }
  return false;
}

struct FunctionRange {
  uint64_t start = 0;
  uint64_t end = 0;  // exclusive
};

// A human-readable name for the function starting at `vmaddr`: its exported symbol if one names
// exactly that address, else "sub_<hex>" -- the same fallback xlate itself gives a lifted
// function with no export (xlate.cpp: `lifted->setName("sub_" + Hex(address))`).
std::string FunctionName(const Image &image, uint64_t vmaddr) {
  for (auto &[name, addr] : image.exports()) {
    if (addr == vmaddr) {
      return name;
    }
  }
  return "sub_" + Hex(vmaddr).substr(2);
}

bool InDataInCode(const std::vector<DataInCodeRange> &ranges, uint64_t vmaddr) {
  return std::any_of(ranges.begin(), ranges.end(), [&](auto &range) {
    return vmaddr >= range.vmaddr && vmaddr < range.vmaddr + range.length;
  });
}

}  // namespace

bool ScanTSDOffsets(const Image &image, std::vector<std::string> &violations) {
  for (auto &section : image.sections()) {
    if (!(section.flags & (llvm::MachO::S_ATTR_PURE_INSTRUCTIONS | llvm::MachO::S_ATTR_SOME_INSTRUCTIONS))) {
      continue;
    }
    if (section.size < 4) {
      continue;
    }
    std::vector<uint8_t> bytes(section.size);
    if (!image.ReadBytes(image.host(section.addr), bytes.data(), bytes.size())) {
      continue;  // a section with no file backing (e.g. zerofill) has nothing to decode
    }
    uint32_t words = section.size / 4;
    std::vector<uint32_t> code(words);
    memcpy(code.data(), bytes.data(), words * 4);
    std::vector<farmdec::Inst> decoded(words);
    fad_decode(code.data(), words, decoded.data());

    // Function boundaries within this section, for both resetting the per-function dataflow
    // state (a chain must not leak from one function into the next just because they are
    // contiguous in the same section) and for attributing a violation to a name.
    std::vector<uint64_t> starts;
    for (auto addr : image.functions()) {
      if (addr >= section.addr && addr < section.addr + section.size) {
        starts.push_back(addr);
      }
    }
    std::sort(starts.begin(), starts.end());
    std::vector<FunctionRange> ranges;
    for (size_t i = 0; i < starts.size(); i++) {
      uint64_t end = (i + 1 < starts.size()) ? starts[i + 1] : section.addr + section.size;
      ranges.push_back({starts[i], end});
    }
    // Code in this section before its first known function start (rare -- padding, or a section
    // LC_FUNCTION_STARTS does not cover) is scanned too, attributed to the section itself.
    if (ranges.empty() || ranges.front().start != section.addr) {
      uint64_t end = ranges.empty() ? section.addr + section.size : ranges.front().start;
      ranges.insert(ranges.begin(), {section.addr, end});
    }

    for (auto &range : ranges) {
      std::array<bool, 32> tracked{};
      std::array<uint64_t, 32> mrs_at{};  // address of the mrs that set tracked[r], for messages
      for (uint64_t addr = range.start; addr < range.end; addr += 4) {
        if (InDataInCode(image.data_in_code(), addr)) {
          continue;
        }
        auto &inst = decoded[(addr - section.addr) / 4];
        if (inst.op == farmdec::A64_ERROR || inst.op == farmdec::A64_UNKNOWN) {
          continue;  // not a real instruction this scanner can reason about either way
        }
        if (inst.op == farmdec::A64_MRS) {
          bool is_tpidrro = (inst.imm == 0xde83);
          if (inst.rt != farmdec::ZERO_REG) {
            tracked[inst.rt] = is_tpidrro;
            if (is_tpidrro) mrs_at[inst.rt] = addr;
          }
          continue;
        }
        for (farmdec::Reg r = 0; r < 31; r++) {
          if (!tracked[r]) continue;
          bool fixed_offset_access =
              (inst.op == farmdec::A64_LDR || inst.op == farmdec::A64_STR) && inst.rn == r &&
              fad_get_addrmode(inst.flags) == farmdec::AM_OFF_IMM;
          if (fixed_offset_access) {
            auto offset = static_cast<uint64_t>(inst.offset);
            if (!IsAllowedOffset(offset)) {
              std::ostringstream line;
              line << image.path() << ": " << FunctionName(image, range.start) << ": " << Hex(addr)
                   << ": load/store off TPIDRRO_EL0 (from mrs at " << Hex(mrs_at[r]) << ") at offset "
                   << Hex(offset) << ", not on the audited allow-list";
              violations.push_back(line.str());
            }
          } else if (UsesReg(inst, r)) {
            std::ostringstream line;
            line << image.path() << ": " << FunctionName(image, range.start) << ": " << Hex(addr)
                 << ": register holding TPIDRRO_EL0 (from mrs at " << Hex(mrs_at[r])
                 << ") used in a form this scanner does not model (opcode " << static_cast<int>(inst.op)
                 << "); extend xlate/src/tsd_scan.cpp to classify it, or confirm this is not a TSD access";
            violations.push_back(line.str());
          }
        }
        if (inst.op == farmdec::A64_BL || inst.op == farmdec::A64_BLR) {
          // AAPCS64 caller-saved registers (X0-X17) and LR may not survive a call; X19-X28 are
          // callee-saved and do (which is presumably why real Swift code keeps the TSD base in
          // x24 -- it lives across calls without a fresh mrs).
          for (farmdec::Reg r = 0; r <= 17; r++) tracked[r] = false;
          tracked[30] = false;
        }
        for (farmdec::Reg r = 0; r < 31; r++) {
          if (tracked[r] && ClobbersGPR(inst, r)) tracked[r] = false;
        }
      }
    }
  }
  return violations.empty();
}

}  // namespace xlate
