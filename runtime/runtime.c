#include "xl_state.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>

typedef struct State State;
typedef struct Memory Memory;
typedef void (*xl_lifted)(State *);

struct xl_function {
    uint32_t address;
    xl_lifted function;
};

extern const struct xl_function xl_functions[];
extern const uint32_t xl_function_count;
extern const uint32_t xl_entry_address;

#define XL_REG(state, name) (*(uint64_t *)((char *)(state) + XL_OFFSET_##name))
#define XL_GUEST_STACK_SIZE (16u << 20)

static pthread_key_t xl_state_key;
static pthread_once_t xl_state_once = PTHREAD_ONCE_INIT;

struct xl_thread {
    void *stack;
};

#define XL_THREAD_HEADER 16u

static void xl_state_destroy(void *value)
{
    struct xl_thread *thread = (struct xl_thread *)((char *)value - XL_THREAD_HEADER);
    munmap(thread->stack, XL_GUEST_STACK_SIZE);
    free(thread);
}

static void xl_state_key_create(void)
{
    pthread_key_create(&xl_state_key, xl_state_destroy);
}

State *xl_current_state(void)
{
    pthread_once(&xl_state_once, xl_state_key_create);
    State *state = pthread_getspecific(xl_state_key);
    if (state)
        return state;
    struct xl_thread *thread;
    if (posix_memalign((void **)&thread, 16, XL_THREAD_HEADER + XL_STATE_SIZE))
        abort();
    memset(thread, 0, XL_THREAD_HEADER + XL_STATE_SIZE);
    thread->stack = mmap(NULL, XL_GUEST_STACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (thread->stack == MAP_FAILED)
        abort();
    state = (State *)((char *)thread + XL_THREAD_HEADER);
    XL_REG(state, SP) = ((uintptr_t)thread->stack + XL_GUEST_STACK_SIZE) & ~15u;
    pthread_setspecific(xl_state_key, state);
    return state;
}

static void xl_dump(State *state)
{
    fprintf(stderr, "xlate: pc=0x%llx lr=0x%llx sp=0x%llx\n", XL_REG(state, PC), XL_REG(state, X30), XL_REG(state, SP));
    for (int i = 0; i < 8; i++)
        fprintf(stderr, "xlate: x%d=0x%llx\n", i, *(uint64_t *)((char *)state + XL_OFFSET_X0 + (XL_OFFSET_X1 - XL_OFFSET_X0) * i));
}

void xl_fault(State *state, const char *reason)
{
    fprintf(stderr, "xlate: %s\n", reason);
    xl_dump(state);
    abort();
}

static void xl_walk_guest(int fd)
{
    State *state = xl_current_state();
    dprintf(fd, "xlate: guest pc=0x%llx lr=0x%llx sp=0x%llx\n", XL_REG(state, PC), XL_REG(state, X30), XL_REG(state, SP));
    for (int i = 0; i < 8; i++)
        dprintf(fd, "xlate: guest x%d=0x%llx\n", i, *(uint64_t *)((char *)state + XL_OFFSET_X0 + (XL_OFFSET_X1 - XL_OFFSET_X0) * i));
    uint64_t fp = XL_REG(state, X29);
    for (int depth = 0; depth < 16 && fp && fp >> 32 == 0; depth++) {
        uint64_t saved_fp = *(uint64_t *)(uintptr_t)fp;
        uint64_t saved_lr = *(uint64_t *)(uintptr_t)(fp + 8);
        dprintf(fd, "xlate: guest frame %d fp=0x%llx lr=0x%llx\n", depth, fp, saved_lr);
        if (saved_fp <= fp)
            break;
        fp = saved_fp;
    }
}

void xl_report_guest_frame(void)
{
    xl_walk_guest(fileno(stderr));
}

static int xl_crash_fd(void)
{
    int fd = open("/private/var/charon/xlate-crash.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
    return fd >= 0 ? fd : fileno(stderr);
}

static volatile sig_atomic_t xl_crashing;

static void xl_crash_handler(int signal, siginfo_t *info, void *context)
{
    if (xl_crashing) {
        struct sigaction reset = {0};
        reset.sa_handler = SIG_DFL;
        sigaction(signal, &reset, NULL);
        raise(signal);
        return;
    }
    xl_crashing = 1;
    int fd = xl_crash_fd();
    dprintf(fd, "xlate: signal %d at fault 0x%lx\n", signal, (unsigned long)(info ? info->si_addr : 0));
    ucontext_t *uc = context;
    if (uc) {
        _STRUCT_ARM_THREAD_STATE *ss = &uc->uc_mcontext->__ss;
        dprintf(fd, "xlate: host pc=0x%x lr=0x%x sp=0x%x cpsr=0x%x\n", ss->__pc, ss->__lr, ss->__sp, ss->__cpsr);
        for (int i = 0; i < 13; i++)
            dprintf(fd, "xlate: host r%d=0x%x\n", i, ss->__r[i]);
    }
    xl_walk_guest(fd);
    if (fd != fileno(stderr))
        close(fd);
    struct sigaction reset = {0};
    reset.sa_handler = SIG_DFL;
    sigaction(signal, &reset, NULL);
    raise(signal);
}

__attribute__((constructor)) static void xl_install_crash_handler(void)
{
    // A guest-stack overflow (deep static init, e.g. a statically linked C++ library) faults
    // with the stack pointer already off the mapping, so the handler needs its own stack to
    // run at all -- without SA_ONSTACK such a crash kills the process silently, writing no log.
    static char alt_stack[SIGSTKSZ < (64 * 1024) ? (64 * 1024) : SIGSTKSZ];
    stack_t alt = {0};
    alt.ss_sp = alt_stack;
    alt.ss_size = sizeof alt_stack;
    sigaltstack(&alt, NULL);
    struct sigaction action = {0};
    action.sa_sigaction = xl_crash_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (int i = 0; i < 5; i++)
        sigaction((int[]){SIGSEGV, SIGBUS, SIGILL, SIGABRT, SIGTRAP}[i], &action, NULL);
}

xl_lifted xl_lookup(uint64_t address)
{
    uint32_t low = 0, high = xl_function_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        if (xl_functions[middle].address < address)
            low = middle + 1;
        else
            high = middle;
    }
    if (low < xl_function_count && xl_functions[low].address == address && address >> 32 == 0)
        return xl_functions[low].function;
    return NULL;
}

void xl_call(State *state)
{
    xl_lifted function = xl_lookup(XL_REG(state, PC));
    if (!function)
        xl_fault(state, "call to an address without translated code (or an unsupported instruction)");
    function(state);
}

void xl_tail(State *state)
{
    xl_lifted function = xl_lookup(XL_REG(state, PC));
    if (!function)
        xl_fault(state, "jump to an address that does not start a translated function (or an unsupported instruction)");
    [[clang::musttail]] return function(state);
}

extern void xl_bridge_init(void);

uint64_t xl_invoke(uint64_t address, uint64_t argument)
{
    xl_bridge_init();
    State *state = xl_current_state();
    xl_lifted function = xl_lookup(address);
    if (!function)
        xl_fault(state, "callback into an address without translated code");
    uint64_t saved_pc = XL_REG(state, PC), saved_lr = XL_REG(state, X30);
    XL_REG(state, PC) = address;
    XL_REG(state, X0) = argument;
    XL_REG(state, X30) = 0;
    function(state);
    uint64_t result = XL_REG(state, X0);
    XL_REG(state, PC) = saved_pc;
    XL_REG(state, X30) = saved_lr;
    return result;
}

uint64_t xl_invoke_n(uint64_t address, const uint64_t *arguments, unsigned count)
{
    xl_bridge_init();
    State *state = xl_current_state();
    xl_lifted function = xl_lookup(address);
    if (!function)
        xl_fault(state, "callback into an address without translated code");
    uint64_t saved_pc = XL_REG(state, PC), saved_lr = XL_REG(state, X30);
    uint64_t saved_arg[8];
    for (unsigned i = 0; i < 8; i++)
        saved_arg[i] = *(uint64_t *)((char *)state + XL_OFFSET_X0 + (XL_OFFSET_X1 - XL_OFFSET_X0) * i);
    for (unsigned i = 0; i < count && i < 8; i++)
        *(uint64_t *)((char *)state + XL_OFFSET_X0 + (XL_OFFSET_X1 - XL_OFFSET_X0) * i) = arguments[i];
    XL_REG(state, PC) = address;
    XL_REG(state, X30) = 0;
    function(state);
    uint64_t result = XL_REG(state, X0);
    XL_REG(state, PC) = saved_pc;
    XL_REG(state, X30) = saved_lr;
    for (unsigned i = 0; i < 8; i++)
        *(uint64_t *)((char *)state + XL_OFFSET_X0 + (XL_OFFSET_X1 - XL_OFFSET_X0) * i) = saved_arg[i];
    return result;
}

static uint64_t *xl_guest_vector(char **vector)
{
    size_t count = 0;
    while (vector && vector[count])
        count++;
    uint64_t *copy = calloc(count + 1, sizeof *copy);
    for (size_t i = 0; i < count; i++)
        copy[i] = (uintptr_t)vector[i];
    return copy;
}

extern void xl_run_initializers(void);

int main(int argc, char **argv, char **envp, char **apple)
{
    xl_run_initializers();
    State *state = xl_current_state();
    XL_REG(state, X0) = (uint64_t)argc;
    XL_REG(state, X1) = (uintptr_t)xl_guest_vector(argv);
    XL_REG(state, X2) = (uintptr_t)xl_guest_vector(envp);
    XL_REG(state, X3) = (uintptr_t)xl_guest_vector(apple);
    XL_REG(state, X30) = 0;
    xl_lifted entry = xl_lookup(xl_entry_address);
    XL_REG(state, PC) = xl_entry_address;
    if (!entry)
        xl_fault(state, "entry point was not translated");
    entry(state);
    return (int)(uint32_t)XL_REG(state, X0);
}
