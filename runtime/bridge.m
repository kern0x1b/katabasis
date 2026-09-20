#import <Foundation/Foundation.h>

#include "xl_bridge.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(va_list) == sizeof(char *), "host va_list must be a pointer");

#include <fcntl.h>
// Mirror fatal diagnostics into the crash-log file too: under SpringBoard stderr goes to a
// console we cannot fetch, so a trap's message (which symbol) would otherwise be lost.
static void xl_diag(const char *line)
{
    int fd = open("/private/var/charon/xlate-crash.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) { dprintf(fd, "%s", line); close(fd); }
}

void xl_unsupported(const char *message)
{
    char buf[256];
    snprintf(buf, sizeof buf, "xlate: unsupported: %s\n", message);
    fprintf(stderr, "%s", buf);
    xl_diag(buf);
    abort();
}

void xl_report_guest_frame(void);

void xl_narrowing_fault(const char *symbol, unsigned index, uint64_t value)
{
    char buf[256];
    snprintf(buf, sizeof buf, "xlate: %s argument %u value 0x%llx does not fit the host type\n", symbol, index, value);
    fprintf(stderr, "%s", buf);
    xl_diag(buf);
    xl_report_guest_frame();
    abort();
}

// __cxa_atexit(func, arg, dso): a translated app's C++ static initializers register their
// destructors here. The destructor is guest code, so we cannot hand its address to the
// host C++ runtime (which would call it with the host ABI); we record (func, arg) and run
// them ourselves, newest first, through xl_invoke at process exit. On iOS the app is
// usually killed rather than exited cleanly, so these seldom run -- matching the platform.
static pthread_mutex_t xl_cxa_lock = PTHREAD_MUTEX_INITIALIZER;
static struct xl_cxa_entry { uint64_t func, arg; } *xl_cxa_list;
static unsigned xl_cxa_count, xl_cxa_cap, xl_cxa_armed;

static void xl_run_cxa_dtors(void)
{
    pthread_mutex_lock(&xl_cxa_lock);
    unsigned n = xl_cxa_count;
    pthread_mutex_unlock(&xl_cxa_lock);
    for (unsigned i = n; i-- > 0;)
        xl_invoke(xl_cxa_list[i].func, xl_cxa_list[i].arg);
}

void xl_manual___cxa_atexit(void *pack)
{
    struct __attribute__((packed)) { uint64_t a0, a1, a2, r; } *p = pack;
    pthread_mutex_lock(&xl_cxa_lock);
    if (xl_cxa_count == xl_cxa_cap) {
        xl_cxa_cap = xl_cxa_cap ? xl_cxa_cap * 2 : 128;
        xl_cxa_list = realloc(xl_cxa_list, xl_cxa_cap * sizeof *xl_cxa_list);
    }
    xl_cxa_list[xl_cxa_count].func = p->a0;
    xl_cxa_list[xl_cxa_count].arg = p->a1;
    xl_cxa_count++;
    if (!xl_cxa_armed) { xl_cxa_armed = 1; atexit(xl_run_cxa_dtors); }
    pthread_mutex_unlock(&xl_cxa_lock);
    p->r = 0;
}

// C++ ABI operator new/delete -> host allocator. The returned host-heap pointer is within
// the process, so guest code uses it directly; new[]/delete[] share the same allocation.
struct xl_alloc_pack { uint64_t a0, r; } __attribute__((packed));
void xl_manual__Znwm(void *pack) { struct xl_alloc_pack *p = pack; p->r = (uint64_t)(uintptr_t)malloc((size_t)p->a0); }
void xl_manual__Znam(void *pack) { struct xl_alloc_pack *p = pack; p->r = (uint64_t)(uintptr_t)malloc((size_t)p->a0); }
void xl_manual__ZdlPv(void *pack) { struct xl_alloc_pack *p = pack; free((void *)(uintptr_t)p->a0); }
void xl_manual__ZdaPv(void *pack) { struct xl_alloc_pack *p = pack; free((void *)(uintptr_t)p->a0); }

// NSGetUncaughtExceptionHandler returns a function pointer (unbridgeable result). A fresh
// process has no guest-installed handler, so return NULL; the runtime's own uncaught handler
// remains in effect.
void xl_manual_NSGetUncaughtExceptionHandler(void *pack) { struct { uint64_t r; } *p = pack; p->r = 0; }

static pthread_mutex_t xl_callback_lock = PTHREAD_MUTEX_INITIALIZER;

unsigned xl_callback_slot(unsigned signature, uint64_t target)
{
    pthread_mutex_lock(&xl_callback_lock);
    unsigned slot = 0;
    for (; slot < 32; slot++) {
        if (xl_callback_targets[signature][slot] == target || !xl_callback_targets[signature][slot])
            break;
    }
    if (slot == 32) {
        fprintf(stderr, "xlate: callback pool %u exhausted\n", signature);
        abort();
    }
    xl_callback_targets[signature][slot] = target;
    pthread_mutex_unlock(&xl_callback_lock);
    return slot;
}

struct xl_buffer {
    char *data;
    size_t size;
    size_t capacity;
};

static void xl_append(struct xl_buffer *buffer, const void *data, size_t size)
{
    if (buffer->size + size + 1 > buffer->capacity) {
        buffer->capacity = (buffer->size + size + 1) * 2;
        buffer->data = realloc(buffer->data, buffer->capacity);
    }
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    buffer->data[buffer->size] = 0;
}

void xl_format_marshal(struct xl_format *format, const char *text, void *object, const uint64_t *arguments, const char *symbol)
{
    int object_format = object != NULL;
    if (object_format)
        text = [(NSString *)object UTF8String];
    struct xl_buffer out = {0}, args = {0};
    size_t next = 0;
    xl_append(&out, "", 0);
    for (const char *c = text; *c;) {
        if (*c != '%') {
            xl_append(&out, c++, 1);
            continue;
        }
        const char *start = c++;
        if (*c == '%') {
            xl_append(&out, "%%", 2);
            c++;
            continue;
        }
        xl_append(&out, "%", 1);
        while (*c && strchr("-+ #0'", *c))
            xl_append(&out, c++, 1);
        for (int part = 0; part < 2; part++) {
            if (part == 1) {
                if (*c != '.')
                    break;
                xl_append(&out, c++, 1);
            }
            if (*c == '*') {
                int32_t value = (int32_t)arguments[next++];
                xl_append(&args, &value, sizeof value);
                xl_append(&out, c++, 1);
            } else {
                while (*c >= '0' && *c <= '9')
                    xl_append(&out, c++, 1);
            }
            if (*c == '$') {
                fprintf(stderr, "xlate: %s: positional format arguments are not supported: %s\n", symbol, start);
                abort();
            }
        }
        int wide = 0, is_long_double = 0;
        for (;;) {
            if (*c == 'h') {
                c++;
            } else if (*c == 'l' || *c == 'q' || *c == 'j' || *c == 'z' || *c == 't') {
                wide = 1;
                c++;
            } else if (*c == 'L') {
                is_long_double = 1;
                c++;
            } else {
                break;
            }
        }
        char conversion = *c ? *c++ : 0;
        switch (conversion) {
        case 'd': case 'i': case 'o': case 'u': case 'x': case 'X': case 'D': case 'O': case 'U': {
            if (wide) {
                int64_t value = (int64_t)arguments[next++];
                xl_append(&args, &value, sizeof value);
                xl_append(&out, "ll", 2);
            } else {
                int32_t value = (int32_t)arguments[next++];
                xl_append(&args, &value, sizeof value);
            }
            xl_append(&out, &conversion, 1);
            break;
        }
        case 'c': case 'C': {
            int32_t value = (int32_t)arguments[next++];
            xl_append(&args, &value, sizeof value);
            if (wide)
                xl_append(&out, "l", 1);
            xl_append(&out, &conversion, 1);
            break;
        }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
            double value;
            memcpy(&value, &arguments[next++], sizeof value);
            xl_append(&args, &value, sizeof value);
            (void)is_long_double;
            xl_append(&out, &conversion, 1);
            break;
        }
        case 's': case 'S': case 'p': case '@': {
            if (conversion == '@' && !object_format) {
                fprintf(stderr, "xlate: %s: %%@ outside an object format\n", symbol);
                abort();
            }
            uint64_t value = arguments[next++];
            if (value >> 32) {
                fprintf(stderr, "xlate: %s: pointer argument 0x%llx above 4 GiB\n", symbol, value);
                abort();
            }
            uint32_t pointer = (uint32_t)value;
            xl_append(&args, &pointer, sizeof pointer);
            if (wide && conversion == 's')
                xl_append(&out, "l", 1);
            xl_append(&out, &conversion, 1);
            break;
        }
        case 'm':
            xl_append(&out, "m", 1);
            break;
        default:
            fprintf(stderr, "xlate: %s: unsupported conversion in format \"%s\"\n", symbol, text);
            abort();
        }
    }
    xl_append(&args, "", 0);
    format->text = out.data;
    format->arguments = args.data;
    format->object = object_format ? [[NSString alloc] initWithUTF8String:out.data] : NULL;
}

// On Apple's arm64 ABI every variadic argument is passed on the stack and va_list is a plain
// pointer to those contiguous 8-byte slots — the same shape the variadic bridge already spills
// — so a guest va_list marshals exactly like the flat argument array. (This differs from the
// AAPCS64 register-save-area va_list used on other platforms.)
void xl_format_marshal_valist(struct xl_format *format, const char *text, void *object, uint64_t guest_valist, const char *symbol)
{
    xl_format_marshal(format, text, object, (const uint64_t *)(uintptr_t)guest_valist, symbol);
}

void xl_format_release(struct xl_format *format)
{
    [(NSString *)format->object release];
    free(format->text);
    free(format->arguments);
}
