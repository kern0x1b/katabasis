#include "macho.h"
#include "objc.h"

#include <llvm-c/Core.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Analysis/LazyValueInfo.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>
#include <rellume/rellume.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <set>

using namespace llvm;

static cl::list<std::string> Inputs(cl::Positional, cl::OneOrMore, cl::desc("<main image> [guest images...]"));
static cl::opt<std::string> Output("output", cl::desc("Output bitcode"));
static cl::opt<std::string> IROutput("ir", cl::desc("Optional textual IR output"));
static cl::opt<std::string> LayoutOutput("layout", cl::desc("Linker arguments placing guest segments"));
static cl::opt<std::string> StateHeader("state-header", cl::desc("C header with guest register offsets and guest export addresses"));
static cl::opt<std::string> PassthroughPath("passthrough", cl::desc("Host symbols guest data may bind directly"));
static cl::opt<std::string> ObjCManifest("objc-manifest", cl::desc("Write the Objective-C metadata of the guest images as JSON"));
static cl::opt<std::string> CoveragePath("coverage", cl::desc("Only lift every instruction and list unsupported ones"));
static cl::opt<std::string> HostTriple("host-triple", cl::init("armv7-apple-ios6.0.0"));
static cl::opt<std::string> HostLayout("host-datalayout", cl::init("e-m:o-p:32:32-Fi8-f64:32:64-v64:32:64-v128:32:128-a:0:32-n32-S32"));
static cl::opt<uint64_t> Base("base", cl::init(0x10000000));
static cl::opt<bool> Optimize("optimize", cl::init(true));

namespace {

using xlate::Image;

constexpr const char *kTrapPrefix = "_xl_trap_";

uint64_t AlignUp(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

std::string Hex(uint64_t value) {
  std::string text;
  raw_string_ostream os(text);
  os << format_hex_no_prefix(value, 8);
  return text;
}

std::string IRName(const std::string &symbol) {
  if (!symbol.empty() && symbol[0] == '_') {
    return symbol.substr(1);
  }
  return "\1" + symbol;
}

struct Program {
  std::vector<std::unique_ptr<Image>> images;
  std::map<std::string, uint64_t> exports;
  std::map<uint64_t, uint64_t> stub_to_guest;
  std::map<uint64_t, std::string> stub_to_host;
  std::set<std::string> passthrough;
  std::set<uint64_t> functions;

  const Image *ImageAt(uint64_t host) const {
    for (auto &image : images) {
      if (host >= image->host(image->preferred_base()) && host < image->host(image->image_end())) {
        return image.get();
      }
    }
    return nullptr;
  }
};

size_t ReadCode(size_t address, uint8_t *buffer, size_t size, void *user) {
  auto program = static_cast<Program *>(user);
  auto image = program->ImageAt(address);
  if (!image || !image->IsExecutable(address) || program->stub_to_guest.count(address) ||
      program->stub_to_host.count(address)) {
    return 0;
  }
  auto section = image->SectionAt(address);
  auto end = image->host(section->addr + section->size);
  size = std::min<size_t>(size, end - address);
  return image->ReadBytes(address, buffer, size) ? size : 0;
}

bool LoadPassthrough(Program &program) {
  if (PassthroughPath.empty()) {
    return true;
  }
  std::ifstream input(PassthroughPath);
  if (!input) {
    errs() << "xlate: cannot read " << PassthroughPath << "\n";
    return false;
  }
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line[0] != '#') {
      program.passthrough.insert(line);
    }
  }
  return true;
}

bool LayoutAndResolve(Program &program, bool strict) {
  uint64_t cursor = Base;
  for (auto &image : program.images) {
    auto base = image->preferred_base();
    image->set_slide(cursor - base);
    cursor = AlignUp(image->host(image->image_end()), 0x100000);
  }
  for (auto &image : program.images) {
    for (auto &[name, vmaddr] : image->exports()) {
      program.exports.emplace(name, image->host(vmaddr));
    }
    for (auto vmaddr : image->functions()) {
      program.functions.insert(image->host(vmaddr));
    }
  }
  bool ok = true;
  for (auto &image : program.images) {
    for (auto &[vmaddr, symbol] : image->stubs()) {
      auto host = image->host(vmaddr);
      if (auto target = program.exports.find(symbol); target != program.exports.end()) {
        program.stub_to_guest[host] = target->second;
      } else if (symbol.rfind(kTrapPrefix, 0) == 0) {
        program.stub_to_host[host] = "xl_h_" + symbol.substr(strlen(kTrapPrefix));
      } else if (strict) {
        errs() << "xlate: " << image->path() << ": call stub for unresolved import " << symbol << "\n";
        ok = false;
      }
    }
  }
  return ok;
}

class Lifter {
 public:
  Lifter(Program &program, Module &module) : program_(program), module_(module), context_(module.getContext()) {
    auto type = FunctionType::get(Type::getVoidTy(context_), {PointerType::get(context_, 0)}, false);
    call_ = Function::Create(type, GlobalValue::ExternalLinkage, "xl_call", module_);
    tail_ = Function::Create(type, GlobalValue::ExternalLinkage, "xl_tail", module_);
    config_ = ll_config_new();
    ll_config_set_architecture(config_, "aarch64");
    ll_config_set_call_ret_clobber_flags(config_, true);
    ll_config_set_call_func(config_, wrap(call_));
    ll_config_set_tail_func(config_, wrap(tail_));
  }

  ~Lifter() { ll_config_free(config_); }

  Function *Lift(uint64_t address) {
    auto func = ll_func_new(wrap(&module_), config_);
    int failed = ll_func_decode_cfg(func, address, ReadCode, &program_);
    Function *lifted = nullptr;
    if (!failed) {
      // ll_func_lift returns null when rellume cannot lift the function (e.g. an
      // instruction pattern it does not model, as in some hand-written crash-reporter
      // code). Guard the unwrap: a null here must become a skipped function (handled by
      // the caller as a failure), not cast<Function>(null). The function is then absent
      // from the lookup table and any call to it faults at runtime -- the same trap
      // treatment as an unbridged import -- so the rest of the app still builds and runs.
      LLVMValueRef raw = ll_func_lift(func);
      lifted = raw ? unwrap<Function>(raw) : nullptr;
    }
    ll_func_dispose(func);
    if (!lifted) {
      return nullptr;
    }
    lifted->setName("sub_" + Hex(address));
    lifted->setLinkage(GlobalValue::ExternalLinkage);
    lifted->setVisibility(GlobalValue::HiddenVisibility);
    functions[address] = lifted;
    return lifted;
  }

  Function *Handler(const std::string &name) {
    if (auto existing = module_.getFunction(name)) {
      return existing;
    }
    return Function::Create(call_->getFunctionType(), GlobalValue::ExternalLinkage, name, module_);
  }

  Function *Target(uint64_t pc) {
    if (auto guest = program_.stub_to_guest.find(pc); guest != program_.stub_to_guest.end()) {
      pc = guest->second;
    }
    if (auto host = program_.stub_to_host.find(pc); host != program_.stub_to_host.end()) {
      return Handler(host->second);
    }
    auto found = functions.find(pc);
    return found == functions.end() ? nullptr : found->second;
  }

  unsigned Devirtualize() {
    unsigned count = 0;
    std::vector<CallInst *> calls;
    for (auto dispatcher : {call_, tail_}) {
      for (auto user : dispatcher->users()) {
        if (auto call = dyn_cast<CallInst>(user)) {
          calls.push_back(call);
        }
      }
    }
    for (auto call : calls) {
      auto sptr = call->getArgOperand(0);
      std::optional<uint64_t> pc;
      for (auto it = call->getReverseIterator(); it != call->getParent()->rend(); ++it) {
        auto store = dyn_cast<StoreInst>(&*it);
        if (!store || store->getPointerOperand() != sptr) {
          continue;
        }
        if (auto constant = dyn_cast<ConstantInt>(store->getValueOperand())) {
          pc = constant->getZExtValue();
        }
        break;
      }
      if (!pc) {
        continue;
      }
      auto target = Target(*pc);
      if (!target) {
        continue;
      }
      call->setCalledFunction(target);
      ++count;
    }
    return count;
  }

  std::map<uint64_t, Function *> functions;

 private:
  Program &program_;
  Module &module_;
  LLVMContext &context_;
  Function *call_;
  Function *tail_;
  LLConfig *config_;
};


class JumpTables {
 public:
  explicit JumpTables(Program &program) : program_(program), starts_(program.functions) {}

  std::set<uint64_t> Targets(Function &lifted, uint64_t start) {
    std::set<uint64_t> targets;
    auto tail = lifted.getParent()->getFunction("xl_tail");
    if (!tail) {
      return targets;
    }
    bool indirect = false;
    for (auto user : tail->users()) {
      auto call = dyn_cast<CallInst>(user);
      indirect |= call && call->getFunction() == &lifted;
    }
    if (!indirect) {
      return targets;
    }
    Module scratch("jump-tables", lifted.getContext());
    scratch.setDataLayout(lifted.getParent()->getDataLayout());
    dl_ = &scratch.getDataLayout();
    ValueToValueMapTy map;
    auto clone = Function::Create(lifted.getFunctionType(), GlobalValue::ExternalLinkage, lifted.getName(), scratch);
    auto dest = clone->arg_begin();
    for (auto &argument : lifted.args()) {
      map[&argument] = &*dest++;
    }
    SmallVector<ReturnInst *, 4> returns;
    CloneFunctionInto(clone, &lifted, map, CloneFunctionChangeType::DifferentModule, returns);
    LoopAnalysisManager lam;
    FunctionAnalysisManager fam;
    CGSCCAnalysisManager cgam;
    ModuleAnalysisManager mam;
    PassBuilder builder;
    builder.registerModuleAnalyses(mam);
    builder.registerCGSCCAnalyses(cgam);
    builder.registerFunctionAnalyses(fam);
    builder.registerLoopAnalyses(lam);
    builder.crossRegisterProxies(lam, fam, cgam, mam);
    FunctionPassManager passes;
    passes.addPass(InstCombinePass());
    passes.run(*clone, fam);
    auto &lvi = fam.getResult<LazyValueAnalysis>(*clone);
    uint64_t end = NextFunction(start);
    for (auto &block : *clone) {
      for (auto &inst : block) {
        auto call = dyn_cast<CallInst>(&inst);
        if (!call || !call->getCalledFunction() || call->getCalledFunction()->getName() != "xl_tail") {
          continue;
        }
        StoreInst *store = nullptr;
        for (auto it = call->getReverseIterator(); it != block.rend(); ++it) {
          if (auto candidate = dyn_cast<StoreInst>(&*it); candidate && candidate->getPointerOperand() == clone->getArg(0)) {
            store = candidate;
            break;
          }
        }
        if (!store) {
          continue;
        }
        std::vector<Value *> values = {store->getValueOperand()};
        if (auto phi = dyn_cast<PHINode>(values[0])) {
          values.assign(phi->incoming_values().begin(), phi->incoming_values().end());
        }
        for (auto value : values) {
          Resolve(value, lvi, start, end, targets);
        }
      }
    }
    return targets;
  }

 private:
  uint64_t NextFunction(uint64_t start) {
    auto next = starts_.upper_bound(start);
    return next == starts_.end() ? start + 0x100000 : *next;
  }

  static Value *Strip(Value *value) {
    if (auto expr = dyn_cast<ConstantExpr>(value); expr && expr->getOpcode() == Instruction::IntToPtr) {
      return expr->getOperand(0);
    }
    if (auto cast = dyn_cast<CastInst>(value); cast && (isa<IntToPtrInst>(cast) || isa<PtrToIntInst>(cast))) {
      return cast->getOperand(0);
    }
    return value;
  }

  // Strip pointer<->int and integer width casts to reach the value the compiler actually
  // range-checks and uses as the table index (e.g. the `sub w8, w0, #min` result), so that
  // LVI bounds it and iteration runs over the real table domain, not a wider parent value.
  static Value *StripCasts(Value *value) {
    for (;;) {
      value = Strip(value);
      if (auto cast = dyn_cast<CastInst>(value); cast && (isa<ZExtInst>(cast) || isa<SExtInst>(cast) || isa<TruncInst>(cast))) {
        value = cast->getOperand(0);
        continue;
      }
      return value;
    }
  }

  bool FindLoad(Value *value, LoadInst *&load, int depth) {
    if (depth > 12) {
      return false;
    }
    if (auto found = dyn_cast<LoadInst>(value)) {
      if (load && load != found) {
        return false;
      }
      load = found;
      return true;
    }
    if (isa<ConstantInt>(value)) {
      return true;
    }
    if (auto op = dyn_cast<BinaryOperator>(value)) {
      return FindLoad(op->getOperand(0), load, depth + 1) && FindLoad(op->getOperand(1), load, depth + 1);
    }
    if (auto cast = dyn_cast<CastInst>(value)) {
      return FindLoad(cast->getOperand(0), load, depth + 1);
    }
    return false;
  }

  bool FindIndex(Value *value, Value *&index, int depth) {
    value = Strip(value);
    if (depth > 12) {
      return false;
    }
    if (isa<ConstantInt>(value)) {
      return true;
    }
    if (auto gep = dyn_cast<GEPOperator>(value)) {
      // A jump table access may be scaled: InstCombine turns `ldrh [x9, x8, lsl #1]`
      // into `getelementptr [2 x i8], ptr base, index` (element stride 2) and the
      // signed-word `ldrsw [x9, x8, lsl #2]` into a stride-4 element. Accept any GEP and
      // let Evaluate apply the per-element stride via the data layout. Latch the index onto
      // the GEP's own varying operand (past casts, but not past the `sub` that produces the
      // table index) — that is the value the range check bounds and the true iteration domain.
      if (!FindIndex(gep->getPointerOperand(), index, depth + 1)) {
        return false;
      }
      for (unsigned i = 1; i < gep->getNumOperands(); ++i) {
        auto operand = gep->getOperand(i);
        if (isa<ConstantInt>(operand)) {
          continue;
        }
        auto candidate = StripCasts(operand);
        if (index && index != candidate) {
          return false;
        }
        index = candidate;
      }
      return true;
    }
    if (auto op = dyn_cast<BinaryOperator>(value)) {
      return FindIndex(op->getOperand(0), index, depth + 1) && FindIndex(op->getOperand(1), index, depth + 1);
    }
    if (auto cast = dyn_cast<CastInst>(value); cast && (isa<ZExtInst>(cast) || isa<SExtInst>(cast) || isa<TruncInst>(cast))) {
      if (!isa<Instruction>(cast->getOperand(0)) || !index || index == cast->getOperand(0)) {
        if (!index && isa<Instruction>(cast->getOperand(0)) && !isa<BinaryOperator>(cast->getOperand(0)) &&
            !isa<CastInst>(cast->getOperand(0))) {
          index = cast->getOperand(0);
          return true;
        }
      }
      return FindIndex(cast->getOperand(0), index, depth + 1);
    }
    if (isa<Instruction>(value) || isa<Argument>(value)) {
      if (index && index != value) {
        return false;
      }
      index = value;
      return true;
    }
    return false;
  }

  std::optional<uint64_t> Evaluate(Value *value, Value *index, uint64_t index_value, LoadInst *load,
                                   std::optional<uint64_t> loaded, int depth) {
    value = Strip(value);
    if (depth > 16) {
      return std::nullopt;
    }
    if (value == index) {
      return index_value;
    }
    if (value == load) {
      return loaded;
    }
    if (auto constant = dyn_cast<ConstantInt>(value)) {
      return constant->getZExtValue();
    }
    if (auto gep = dyn_cast<GEPOperator>(value)) {
      auto base = Evaluate(gep->getPointerOperand(), index, index_value, load, loaded, depth + 1);
      if (!base) {
        return std::nullopt;
      }
      // Walk the indices honouring each level's stride: an array/pointer index scales by
      // the indexed element's allocation size, a struct index by the field offset. Without
      // this a scaled table (`[2 x i8]`/`[4 x i8]` elements) would be read at the wrong stride.
      uint64_t address = *base;
      auto gti = gep_type_begin(gep);
      for (unsigned i = 1; i < gep->getNumOperands(); ++i, ++gti) {
        auto idx = Evaluate(gep->getOperand(i), index, index_value, load, loaded, depth + 1);
        if (!idx) {
          return std::nullopt;
        }
        if (StructType *st = gti.getStructTypeOrNull()) {
          address += dl_->getStructLayout(st)->getElementOffset(*idx);
        } else {
          int64_t signed_index = SignExtend64(*idx, gep->getOperand(i)->getType()->getIntegerBitWidth());
          address += signed_index * static_cast<int64_t>(dl_->getTypeAllocSize(gti.getIndexedType()));
        }
      }
      return address;
    }
    if (auto cast = dyn_cast<CastInst>(value)) {
      auto operand = Evaluate(cast->getOperand(0), index, index_value, load, loaded, depth + 1);
      if (!operand) {
        return std::nullopt;
      }
      unsigned from = cast->getSrcTy()->getScalarSizeInBits();
      unsigned to = cast->getDestTy()->getScalarSizeInBits();
      uint64_t mask_from = from >= 64 ? ~0ull : (1ull << from) - 1;
      uint64_t mask_to = to >= 64 ? ~0ull : (1ull << to) - 1;
      if (isa<SExtInst>(cast)) {
        return static_cast<uint64_t>(SignExtend64(*operand & mask_from, from)) & mask_to;
      }
      return *operand & mask_from & mask_to;
    }
    if (auto op = dyn_cast<BinaryOperator>(value)) {
      auto a = Evaluate(op->getOperand(0), index, index_value, load, loaded, depth + 1);
      auto b = Evaluate(op->getOperand(1), index, index_value, load, loaded, depth + 1);
      if (!a || !b) {
        return std::nullopt;
      }
      unsigned bits = op->getType()->getIntegerBitWidth();
      uint64_t mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
      switch (op->getOpcode()) {
        case Instruction::Add: return (*a + *b) & mask;
        case Instruction::Sub: return (*a - *b) & mask;
        case Instruction::Mul: return (*a * *b) & mask;
        case Instruction::Shl: return (*a << *b) & mask;
        case Instruction::LShr: return (*a >> *b) & mask;
        case Instruction::AShr: return static_cast<uint64_t>(SignExtend64(*a, bits) >> *b) & mask;
        case Instruction::And: return *a & *b;
        case Instruction::Or: return *a | *b;
        case Instruction::Xor: return *a ^ *b;
        default: return std::nullopt;
      }
    }
    if (auto cmp = dyn_cast<ICmpInst>(value)) {
      auto a = Evaluate(cmp->getOperand(0), index, index_value, load, loaded, depth + 1);
      auto b = Evaluate(cmp->getOperand(1), index, index_value, load, loaded, depth + 1);
      if (!a || !b) {
        return std::nullopt;
      }
      unsigned bits = cmp->getOperand(0)->getType()->getIntegerBitWidth();
      int64_t sa = SignExtend64(*a, bits), sb = SignExtend64(*b, bits);
      uint64_t mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
      uint64_t ua = *a & mask, ub = *b & mask;
      switch (cmp->getPredicate()) {
        case CmpInst::ICMP_EQ: return ua == ub;
        case CmpInst::ICMP_NE: return ua != ub;
        case CmpInst::ICMP_UGT: return ua > ub;
        case CmpInst::ICMP_UGE: return ua >= ub;
        case CmpInst::ICMP_ULT: return ua < ub;
        case CmpInst::ICMP_ULE: return ua <= ub;
        case CmpInst::ICMP_SGT: return sa > sb;
        case CmpInst::ICMP_SGE: return sa >= sb;
        case CmpInst::ICMP_SLT: return sa < sb;
        case CmpInst::ICMP_SLE: return sa <= sb;
        default: return std::nullopt;
      }
    }
    if (auto sel = dyn_cast<SelectInst>(value)) {
      auto cond = Evaluate(sel->getCondition(), index, index_value, load, loaded, depth + 1);
      if (!cond) {
        return std::nullopt;
      }
      return Evaluate(*cond ? sel->getTrueValue() : sel->getFalseValue(), index, index_value, load, loaded, depth + 1);
    }
    return std::nullopt;
  }

  // The conditional branches that must be taken a given way to reach `bb`, walking up the
  // chain of single-predecessor blocks. For a switch this captures the range check that
  // gates the table-load block, which InstCombine may encode opaquely to LVI (e.g. as an
  // xor of two comparisons), letting us bound the table by evaluating the guard per index.
  std::vector<std::pair<Value *, bool>> Guards(BasicBlock *bb) {
    std::vector<std::pair<Value *, bool>> guards;
    for (int steps = 0; steps < 32 && bb; ++steps) {
      auto pred = bb->getSinglePredecessor();
      if (!pred) {
        break;
      }
      if (auto br = dyn_cast<BranchInst>(pred->getTerminator()); br && br->isConditional()) {
        if (br->getSuccessor(0) == bb) {
          guards.push_back({br->getCondition(), true});
        } else if (br->getSuccessor(1) == bb) {
          guards.push_back({br->getCondition(), false});
        }
      }
      bb = pred;
    }
    return guards;
  }

  void Resolve(Value *pc, LazyValueInfo &lvi, uint64_t start, uint64_t end, std::set<uint64_t> &targets) {
    if (isa<ConstantInt>(pc)) {
      return;
    }
    LoadInst *load = nullptr;
    if (!FindLoad(pc, load, 0) || !load) {
      return;
    }
    Value *index = nullptr;
    if (!FindIndex(load->getPointerOperand(), index, 0) || !index) {
      return;
    }
    uint64_t limit = 4096;
    if (index->getType()->isIntegerTy()) {
      auto range = lvi.getConstantRange(index, load, false);
      if (!range.isFullSet() && !range.getUnsignedMax().isMaxValue()) {
        limit = std::min<uint64_t>(limit, range.getUnsignedMax().getZExtValue() + 1);
      }
    }
    // Only guards expressed purely in terms of the index can bound the table; probe each once
    // with two index values to discard guards that depend on other (unknown) state.
    std::vector<std::pair<Value *, bool>> guards;
    for (auto &guard : Guards(load->getParent())) {
      if (Evaluate(guard.first, index, 0, load, 0, 0) && Evaluate(guard.first, index, 1, load, 0, 0)) {
        guards.push_back(guard);
      }
    }
    unsigned width = load->getType()->getIntegerBitWidth() / 8;
    if (!width || width > 8) {
      return;
    }
    bool accepted = false;
    for (uint64_t i = 0; i < limit; ++i) {
      // Stop once the index leaves the domain the guards admit — the table only covers the
      // range the range check lets through; reads past it are unrelated memory. The domain is
      // contiguous, so before entering it (a switch whose normalised index starts above 0) we
      // keep scanning within the cap, but once we have entered and then left it we are done.
      bool in_domain = true;
      for (auto &guard : guards) {
        auto value = Evaluate(guard.first, index, i, load, 0, 0);
        if (value && (*value != 0) != guard.second) {
          in_domain = false;
          break;
        }
      }
      if (!in_domain) {
        if (accepted) {
          break;
        }
        continue;
      }
      auto address = Evaluate(load->getPointerOperand(), index, i, nullptr, std::nullopt, 0);
      if (!address) {
        return;
      }
      auto image = program_.ImageAt(*address);
      uint8_t bytes[8] = {};
      if (!image || !image->ReadBytes(*address, bytes, width)) {
        return;
      }
      uint64_t raw = 0;
      memcpy(&raw, bytes, width);
      auto target = Evaluate(pc, index, i, load, raw, 0);
      if (!target || *target < start || *target >= end || (*target & 3)) {
        if (limit == 4096 && guards.empty()) {
          return;
        }
        continue;
      }
      targets.insert(*target);
      accepted = true;
    }
  }

  Program &program_;
  std::set<uint64_t> starts_;
  const DataLayout *dl_ = nullptr;

 public:
  uint64_t Owner(uint64_t address) const {
    auto it = starts_.upper_bound(address);
    return it == starts_.begin() ? address : *std::prev(it);
  }
};

struct GuestSegment {
  const Image *image;
  xlate::Segment segment;
  std::string host_name;
};

Constant *HostSymbol(Module &module, const std::string &symbol) {
  auto name = IRName(symbol);
  if (auto global = module.getNamedValue(name)) {
    return global;
  }
  return new GlobalVariable(module, Type::getInt8Ty(module.getContext()), false, GlobalValue::ExternalLinkage,
                            nullptr, name);
}

struct Override {
  std::vector<Constant *> fields;
  uint64_t size = 0;
  GlobalVariable *global = nullptr;
};

using Overrides = std::map<uint64_t, Override>;

Overrides ObjCOverrides(Module &module, const std::vector<xlate::ObjCImage> &objc) {
  auto &context = module.getContext();
  auto i32 = Type::getInt32Ty(context);
  auto i8 = Type::getInt8Ty(context);
  Overrides overrides;
  auto padding = [&](uint64_t size) { return ConstantAggregateZero::get(ArrayType::get(i8, size)); };
  auto symbol = [&](const std::string &name) {
    return ConstantExpr::getPtrToInt(HostSymbol(module, "_" + name), i32);
  };
  auto class_type = StructType::get(context, {i32, i32, i32, i32, i32, ArrayType::get(i8, 20)}, true);
  std::map<uint64_t, GlobalVariable *> classes;
  for (auto &image : objc) {
    for (auto &cls : image.classes) {
      for (auto address : {cls.address, cls.metaclass}) {
        auto global = new GlobalVariable(module, class_type, false, GlobalValue::ExternalLinkage, nullptr,
                                         "xl_guest_class_" + Hex(address));
        global->setVisibility(GlobalValue::HiddenVisibility);
        classes[address] = global;
      }
    }
  }
  auto pointer = [&](const xlate::Pointer &value) -> Constant * {
    switch (value.kind) {
      case xlate::Pointer::Local:
        if (auto found = classes.find(value.host); found != classes.end()) {
          return ConstantExpr::getPtrToInt(found->second, i32);
        }
        return ConstantInt::get(i32, value.host);
      case xlate::Pointer::Raw:
        return ConstantInt::get(i32, value.host);
      case xlate::Pointer::Import:
        return ConstantExpr::getPtrToInt(HostSymbol(module, value.symbol), i32);
      case xlate::Pointer::Null:
        break;
    }
    return ConstantInt::get(i32, 0);
  };
  for (size_t image = 0; image < objc.size(); ++image) {
    for (size_t index = 0; index < objc[image].classes.size(); ++index) {
      auto &cls = objc[image].classes[index];
      auto suffix = std::to_string(image) + "_" + std::to_string(index);
      xlate::Pointer metaclass;
      metaclass.kind = xlate::Pointer::Local;
      metaclass.host = cls.metaclass;
      Override object;
      object.fields = {pointer(metaclass), pointer(cls.superclass), symbol("_objc_empty_cache"),
                       symbol("_objc_empty_vtable"), symbol("xl_objc_ro_" + suffix), padding(20)};
      object.size = 40;
      object.global = classes[cls.address];
      overrides[cls.address] = object;
      Override meta;
      meta.fields = {pointer(cls.meta_isa), pointer(cls.meta_superclass), symbol("_objc_empty_cache"),
                     symbol("_objc_empty_vtable"), symbol("xl_objc_metaro_" + suffix), padding(20)};
      meta.size = 40;
      meta.global = classes[cls.metaclass];
      overrides[cls.metaclass] = meta;
    }
    for (auto &protocol : objc[image].protocols) {
      auto name = [&](const char *part) { return "xl_objc_proto_" + Hex(protocol.address) + "_" + part; };
      auto list = [&](bool present, const char *part) -> Constant * {
        return present ? symbol(name(part)) : ConstantInt::get(i32, 0);
      };
      // The override replaces the guest protocol_t in place, so it must consume exactly the
      // guest structure's bytes (its `size` field) — a fixed 96 over-claims smaller protocols
      // and clobbers whatever __data follows them (e.g. constant-string pointers used as keys).
      uint64_t occupied = protocol.size >= 44 ? protocol.size : 96;
      auto protocol_type = StructType::get(
          context, {i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, ArrayType::get(i8, occupied - 44)}, true);
      Override value;
      value.fields = {ConstantInt::get(i32, 0),
                      ConstantInt::get(i32, protocol.name_address),
                      list(!protocol.protocols.empty(), "protocols"),
                      list(!protocol.instance_methods.empty(), "instance"),
                      list(!protocol.class_methods.empty(), "class"),
                      list(!protocol.optional_instance_methods.empty(), "optional_instance"),
                      list(!protocol.optional_class_methods.empty(), "optional_class"),
                      list(!protocol.properties.empty(), "properties"),
                      ConstantInt::get(i32, 40),
                      ConstantInt::get(i32, protocol.flags),
                      ConstantInt::get(i32, 0),
                      padding(occupied - 44)};
      value.size = occupied;
      value.global = new GlobalVariable(module, protocol_type, false, GlobalValue::ExternalLinkage, nullptr,
                                        "xl_guest_protocol_" + Hex(protocol.address));
      value.global->setVisibility(GlobalValue::HiddenVisibility);
      overrides[protocol.address] = value;
    }
    for (auto &string : objc[image].strings) {
      Override value;
      value.fields = {symbol("__CFConstantStringClassReference"), ConstantInt::get(i32, string.flags),
                      ConstantInt::get(i32, string.string), ConstantInt::get(i32, string.length), padding(16)};
      value.size = 32;
      overrides[string.address] = value;
    }
  }
  return overrides;
}

bool EmitGuestData(Program &program, Module &module, std::vector<GuestSegment> &out, const Overrides &overrides) {
  auto &context = module.getContext();
  auto i32 = Type::getInt32Ty(context);
  auto i64 = Type::getInt64Ty(context);
  bool ok = true;
  unsigned image_index = 0;
  for (auto &image : program.images) {
    for (auto &segment : image->segments()) {
      if (segment.name == "__PAGEZERO" || segment.name == "__LINKEDIT" || !segment.vmsize) {
        continue;
      }
      std::string host_name = "__X" + std::to_string(image_index) +
                              segment.name.substr(2, std::min<size_t>(12, segment.name.size() - 2));
      std::vector<uint8_t> bytes(segment.vmsize);
      image->ReadBytes(image->host(segment.vmaddr), bytes.data(), bytes.size());
      std::map<uint64_t, Constant *> slots;
      for (auto &[vmaddr, target] : image->rebases()) {
        if (vmaddr < segment.vmaddr || vmaddr >= segment.vmaddr + segment.vmsize) {
          continue;
        }
        uint64_t value = target & 0x00FFFFFFFFFFFFFFull;
        if (value < image->preferred_base() || value >= image->image_end()) {
          errs() << "xlate: " << image->path() << ": rebase at 0x" << Hex(vmaddr) << " targets 0x" << Hex(value)
                 << " outside the image\n";
          ok = false;
          continue;
        }
        slots[vmaddr] = ConstantInt::get(i64, image->host(value));
      }
      for (auto &[vmaddr, bind] : image->binds()) {
        if (vmaddr < segment.vmaddr || vmaddr >= segment.vmaddr + segment.vmsize) {
          continue;
        }
        if (auto target = program.exports.find(bind.symbol); target != program.exports.end()) {
          slots[vmaddr] = ConstantInt::get(i64, target->second + bind.addend);
        } else if (program.passthrough.count(bind.symbol)) {
          auto host = HostSymbol(module, bind.symbol);
          if (bind.weak_import) {
            if (auto global = dyn_cast<GlobalVariable>(host)) {
              global->setLinkage(GlobalValue::ExternalWeakLinkage);
            }
          }
          Constant *address = ConstantExpr::getPtrToInt(host, i32);
          if (bind.addend) {
            address = ConstantExpr::getAdd(address, ConstantInt::get(i32, bind.addend));
          }
          slots[vmaddr] = ConstantStruct::getAnon(context, {address, ConstantInt::get(i32, 0)}, true);
        } else if (bind.symbol == "dyld_stub_binder" || bind.weak_import || bind.symbol.rfind(kTrapPrefix, 0) == 0) {
          slots[vmaddr] = ConstantInt::get(i64, 0);
        } else {
          errs() << "xlate: " << image->path() << ": data bind to unresolved symbol " << bind.symbol << "\n";
          ok = false;
        }
      }
      Overrides pieces;
      for (auto &[vmaddr, value] : slots) {
        Override piece;
        piece.fields = {value};
        piece.size = 8;
        pieces[vmaddr] = piece;
      }
      for (auto &[host, piece] : overrides) {
        uint64_t vmaddr = host - image->slide();
        if (vmaddr < segment.vmaddr || vmaddr >= segment.vmaddr + segment.vmsize) {
          continue;
        }
        pieces.erase(pieces.lower_bound(vmaddr), pieces.lower_bound(vmaddr + piece.size));
        pieces[vmaddr] = piece;
      }
      std::vector<Constant *> fields;
      std::vector<Type *> types;
      uint64_t cursor = segment.vmaddr;
      uint64_t chunk_start = segment.vmaddr;
      auto place = [&](GlobalVariable *global) {
        global->removeFromParent();
        module.insertGlobalVariable(global);
        global->setSection(host_name + ",__image");
        global->setAlignment(Align(1));
        appendToCompilerUsed(module, {global});
      };
      auto close_chunk = [&]() {
        if (fields.empty()) {
          return;
        }
        auto type = StructType::get(context, types, true);
        auto global = new GlobalVariable(module, type, false, GlobalValue::InternalLinkage,
                                         ConstantStruct::get(type, fields),
                                         "guest_" + std::to_string(image_index) + "_" + Hex(chunk_start));
        place(global);
        fields.clear();
        types.clear();
      };
      auto flush = [&](uint64_t until) {
        if (until > cursor) {
          auto begin = bytes.data() + (cursor - segment.vmaddr);
          auto array = ConstantDataArray::get(context, ArrayRef<uint8_t>(begin, until - cursor));
          fields.push_back(array);
          types.push_back(array->getType());
          cursor = until;
        }
      };
      for (auto &[vmaddr, piece] : pieces) {
        flush(vmaddr);
        if (piece.global) {
          close_chunk();
          piece.global->setInitializer(ConstantStruct::get(cast<StructType>(piece.global->getValueType()), piece.fields));
          place(piece.global);
          cursor = vmaddr + piece.size;
          chunk_start = cursor;
          continue;
        }
        for (auto value : piece.fields) {
          fields.push_back(value);
          types.push_back(value->getType());
        }
        cursor = vmaddr + piece.size;
      }
      flush(segment.vmaddr + segment.vmsize);
      close_chunk();
      out.push_back({image.get(), segment, host_name});
    }
    ++image_index;
  }
  return ok;
}

void EmitTables(Program &program, Lifter &lifter, Module &module) {
  auto &context = module.getContext();
  auto i32 = Type::getInt32Ty(context);
  auto ptr = PointerType::get(context, 0);
  auto entry_type = StructType::get(context, {i32, ptr});
  std::map<uint64_t, Function *> table(lifter.functions.begin(), lifter.functions.end());
  for (auto &[stub, target] : program.stub_to_guest) {
    if (auto function = lifter.Target(stub)) {
      table[stub] = function;
    }
  }
  for (auto &[stub, name] : program.stub_to_host) {
    table[stub] = lifter.Handler(name);
  }
  std::vector<Constant *> entries;
  for (auto &[address, function] : table) {
    entries.push_back(ConstantStruct::get(entry_type, {ConstantInt::get(i32, address), function}));
  }
  auto array_type = ArrayType::get(entry_type, entries.size());
  new GlobalVariable(module, array_type, true, GlobalValue::ExternalLinkage, ConstantArray::get(array_type, entries),
                     "xl_functions");
  new GlobalVariable(module, i32, true, GlobalValue::ExternalLinkage, ConstantInt::get(i32, entries.size()),
                     "xl_function_count");
  auto &main_image = program.images.front();
  uint64_t entry = main_image->entry() ? main_image->host(*main_image->entry()) : 0;
  new GlobalVariable(module, i32, true, GlobalValue::ExternalLinkage, ConstantInt::get(i32, entry),
                     "xl_entry_address");
  std::vector<Constant *> initializers;
  for (auto it = program.images.rbegin(); it != program.images.rend(); ++it) {
    auto &image = *it;
    for (auto &section : image->sections()) {
      if ((section.flags & MachO::SECTION_TYPE) != MachO::S_MOD_INIT_FUNC_POINTERS) {
        continue;
      }
      for (uint64_t offset = 0; offset + 8 <= section.size; offset += 8) {
        auto rebase = image->rebases().find(section.addr + offset);
        if (rebase != image->rebases().end()) {
          initializers.push_back(ConstantInt::get(i32, image->host(rebase->second & 0x00FFFFFFFFFFFFFFull)));
        }
      }
    }
  }
  auto initializer_type = ArrayType::get(i32, initializers.size());
  new GlobalVariable(module, initializer_type, true, GlobalValue::ExternalLinkage,
                     ConstantArray::get(initializer_type, initializers), "xl_initializers");
  new GlobalVariable(module, i32, true, GlobalValue::ExternalLinkage, ConstantInt::get(i32, initializers.size()),
                     "xl_initializer_count");
}

bool WriteLayout(const std::vector<GuestSegment> &segments) {
  if (LayoutOutput.empty()) {
    return true;
  }
  std::error_code ec;
  raw_fd_ostream os(LayoutOutput, ec);
  if (ec) {
    errs() << "xlate: " << LayoutOutput << ": " << ec.message() << "\n";
    return false;
  }
  for (auto &entry : segments) {
    os << "-Wl,-segaddr," << entry.host_name << ",0x" << Hex(entry.image->host(entry.segment.vmaddr)) << "\n";
    const char *prot = (entry.segment.initprot & 2) ? "rw" : "r";
    os << "-Wl,-segprot," << entry.host_name << "," << prot << "," << prot << "\n";
  }
  return true;
}

bool WriteStateHeader(const Program &program) {
  if (StateHeader.empty()) {
    return true;
  }
  std::error_code ec;
  raw_fd_ostream os(StateHeader, ec);
  if (ec) {
    errs() << "xlate: " << StateHeader << ": " << ec.message() << "\n";
    return false;
  }
  os << "#pragma once\n";
  uint64_t size = 0;
#define RELLUME_PUBLIC_REG(name, name_cap, reg_size, offset) \
  os << "#define XL_OFFSET_" #name_cap " " << (offset) << "u\n"; \
  size = std::max<uint64_t>(size, (offset) + (reg_size));
#include <cpustruct-aarch64.inc>
#undef RELLUME_PUBLIC_REG
  os << "#define XL_STATE_SIZE " << AlignUp(size, 16) << "u\n";
  for (size_t index = 1; index < program.images.size(); ++index) {
    for (auto &[name, vmaddr] : program.images[index]->exports()) {
      std::string macro = name.substr(name[0] == '_' ? 1 : 0);
      std::replace_if(macro.begin(), macro.end(), [](char c) { return !isalnum(static_cast<unsigned char>(c)); }, '_');
      os << "#define XL_GUEST_" << macro << " 0x" << Hex(program.images[index]->host(vmaddr)) << "u\n";
    }
  }
  return true;
}

void RunPipeline(Module &module) {
  LoopAnalysisManager lam;
  FunctionAnalysisManager fam;
  CGSCCAnalysisManager cgam;
  ModuleAnalysisManager mam;
  PassBuilder builder;
  builder.registerModuleAnalyses(mam);
  builder.registerCGSCCAnalyses(cgam);
  builder.registerFunctionAnalyses(fam);
  builder.registerLoopAnalyses(lam);
  builder.crossRegisterProxies(lam, fam, cgam, mam);
  builder.buildPerModuleDefaultPipeline(OptimizationLevel::O2).run(module, mam);
}

int Coverage(Program &program) {
  std::error_code ec;
  raw_fd_ostream os(CoveragePath, ec);
  LLVMContext context;
  Module module("coverage", context);
  module.setDataLayout(HostLayout);
  auto config = ll_config_new();
  ll_config_set_architecture(config, "aarch64");
  uint64_t total = 0, failed = 0;
  for (auto &image : program.images) {
    for (auto &section : image->sections()) {
      if (!(section.flags & MachO::S_ATTR_PURE_INSTRUCTIONS) || section.sectname != "__text") {
        continue;
      }
      for (uint64_t offset = 0; offset + 4 <= section.size; offset += 4) {
        uint64_t address = image->host(section.addr + offset);
        uint8_t bytes[4];
        image->ReadBytes(address, bytes, 4);
        uint32_t word = bytes[0] | bytes[1] << 8 | bytes[2] << 16 | (uint32_t)bytes[3] << 24;
        auto func = ll_func_new(wrap(&module), config);
        bool decoded = ll_func_decode_instr(func, address, ReadCode, &program) == 0;
        LLVMValueRef raw = decoded ? ll_func_lift(func) : nullptr;
        Function *lifted = raw ? unwrap<Function>(raw) : nullptr;
        ll_func_dispose(func);
        ++total;
        bool unhandled = !lifted;
        if (lifted) {
          for (auto &block : *lifted) {
            for (auto &inst : block) {
              auto store = dyn_cast<StoreInst>(&inst);
              if (store && store->getPointerOperand() == lifted->getArg(0)) {
                if (auto pc = dyn_cast<ConstantInt>(store->getValueOperand()); pc && pc->getZExtValue() == address) {
                  unhandled = true;
                }
              }
            }
          }
          lifted->eraseFromParent();
        }
        if (unhandled) {
          ++failed;
          os << Hex(section.addr + offset) << " " << format_hex_no_prefix(word, 8) << "\n";
        }
      }
    }
  }
  ll_config_free(config);
  outs() << "coverage: " << failed << " of " << total << " instructions not lifted\n";
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  cl::ParseCommandLineOptions(argc, argv, "xlate: arm64 Mach-O to armv7 LLVM bitcode");
  Program program;
  for (auto &path : Inputs) {
    std::string error;
    auto image = Image::Load(path, error);
    if (!image) {
      errs() << "xlate: " << error << "\n";
      return 1;
    }
    program.images.push_back(std::move(image));
  }
  if (!CoveragePath.empty()) {
    LayoutAndResolve(program, false);
    return Coverage(program);
  }
  std::vector<xlate::ObjCImage> objc;
  std::vector<const Image *> images;
  if (!ObjCManifest.empty()) {
    LayoutAndResolve(program, false);
    for (auto &image : program.images) {
      images.push_back(image.get());
      objc.push_back(xlate::AnalyzeObjC(*image));
    }
    std::error_code ec;
    raw_fd_ostream os(ObjCManifest, ec);
    if (ec) {
      errs() << "xlate: " << ObjCManifest << ": " << ec.message() << "\n";
      return 1;
    }
    xlate::WriteManifest(os, images, objc);
    return 0;
  }
  if (Output.empty() || !LoadPassthrough(program) || !LayoutAndResolve(program, true)) {
    return 1;
  }
  for (auto &image : program.images) {
    objc.push_back(xlate::AnalyzeObjC(*image));
    for (auto &error : objc.back().errors) {
      errs() << "xlate: " << image->path() << ": " << error << "\n";
    }
  }
  LLVMContext context;
  Module module("xlate", context);
  module.setTargetTriple(Triple(HostTriple));
  module.setDataLayout(HostLayout);
  Lifter lifter(program, module);
  unsigned failures = 0;
  JumpTables tables(program);
  std::set<uint64_t> pending(program.functions.begin(), program.functions.end());
  unsigned table_targets = 0;
  while (!pending.empty()) {
    std::set<uint64_t> discovered;
    for (auto address : pending) {
      auto lifted = lifter.Lift(address);
      if (!lifted) {
        errs() << "xlate: cannot decode function at 0x" << Hex(address) << "\n";
        ++failures;
        continue;
      }
      for (auto target : tables.Targets(*lifted, tables.Owner(address))) {
        if (!program.functions.count(target) && !lifter.functions.count(target)) {
          discovered.insert(target);
        }
      }
    }
    table_targets += discovered.size();
    for (auto target : discovered) {
      program.functions.insert(target);
    }
    pending = discovered;
  }
  auto devirtualized = lifter.Devirtualize();
  errs() << "xlate: lifted " << lifter.functions.size() << " functions, " << devirtualized << " direct calls, " << table_targets << " jump table targets, "
         << failures << " failures\n";
  std::vector<GuestSegment> segments;
  if (!EmitGuestData(program, module, segments, ObjCOverrides(module, objc))) {
    return 1;
  }
  EmitTables(program, lifter, module);
  if (verifyModule(module, &errs())) {
    errs() << "xlate: invalid module before optimization\n";
    return 1;
  }
  if (Optimize) {
    RunPipeline(module);
  }
  std::error_code ec;
  raw_fd_ostream bc(Output, ec, sys::fs::OF_None);
  if (ec) {
    errs() << "xlate: " << Output << ": " << ec.message() << "\n";
    return 1;
  }
  WriteBitcodeToFile(module, bc);
  if (!IROutput.empty()) {
    raw_fd_ostream ir(IROutput, ec, sys::fs::OF_Text);
    module.print(ir, nullptr);
  }
  return WriteLayout(segments) && WriteStateHeader(program) ? 0 : 1;
}
