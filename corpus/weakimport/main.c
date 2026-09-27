#include <stdio.h>
#include <unistd.h>

// Three weak imports, and what each answers once translated. The first two exist on iOS 6; getppid is bridged and
// execl is not (xlgen cannot bridge a variadic function that names no format), so the translation leaves it unbound and
// the program finds it absent although the device has it, which the build reports as UNSUPPORTED. The third
// is absent from iOS 6 itself, so absent is what a native build says too.
extern int getppid(void) __attribute__((weak_import));
extern int execl(const char *, const char *, ...) __attribute__((weak_import));
extern void os_unfair_lock_lock(void *) __attribute__((weak_import));

int main(void) {
  printf("getppid %s\n", getppid ? "present" : "absent");
  printf("execl %s\n", execl ? "present" : "absent");
  printf("os_unfair_lock_lock %s\n", os_unfair_lock_lock ? "present" : "absent");
  return 0;
}
