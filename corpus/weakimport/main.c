#include <stdio.h>
#include <unistd.h>

// iOS 6 has execl, and xlgen cannot bridge a variadic function that names no format: a weak import that
// the device has and the translation leaves unbound, which the program must find absent, not crash on.
extern int execl(const char *, const char *, ...) __attribute__((weak_import));

int main(void) {
  printf(execl ? "execl present\n" : "execl absent\n");
  return 0;
}
