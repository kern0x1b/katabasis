// corpus/tsdscan: a control for xlate's own build-time TSD-offset scan (xlate/src/tsd_scan.cpp),
// which backs coordination/crutches.md's "TPIDRRO_EL0 backed by a mostly-zero block" entry: this
// is what makes that entry's measurement provably scoped rather than a one-time claim by hand.
// The single mrs+ldr pair below is emitted as one inline asm block on purpose (a fixed register,
// clobbered explicitly) so nothing the compiler could insert -- a spill, a reload -- lands
// between the two instructions regardless of optimization level, and read_tsd_slot is kept a
// separate, unstripped, non-inlined symbol so xlate's refusal message can name it.
//
// Built twice by check.sh, from this one file:
//   -DTSD_OFFSET=0x358  Swift's __PTK_FRAMEWORK_SWIFT_KEY7 (slot 107 * 8), the one offset
//                        runtime/runtime.c's fake TSD block is sized and zeroed for: xlate must
//                        accept this build.
//   -DTSD_OFFSET=0x40   an offset nothing has ever measured a guest touching: xlate must refuse
//                        to build it, naming this file's function and the offset.
#ifndef TSD_OFFSET
#error "build with -DTSD_OFFSET=0x358 (must pass) or another offset, e.g. 0x40 (must fail)"
#endif

__attribute__((noinline)) unsigned long read_tsd_slot(void) {
  unsigned long value;
  __asm__ volatile(
      "mrs x9, TPIDRRO_EL0\n\t"
      "ldr %0, [x9, %1]\n\t"
      : "=r"(value)
      : "i"(TSD_OFFSET)
      : "x9");
  return value;
}

int main(void) {
  return (int)read_tsd_slot();
}
