#import <Foundation/Foundation.h>

#include "xl_bridge.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(va_list) == sizeof(char *), "host va_list must be a pointer");

void xl_unsupported(const char *message)
{
    fprintf(stderr, "xlate: unsupported: %s\n", message);
    abort();
}

void xl_report_guest_frame(void);

void xl_narrowing_fault(const char *symbol, unsigned index, uint64_t value)
{
    fprintf(stderr, "xlate: %s argument %u value 0x%llx does not fit the host type\n", symbol, index, value);
    xl_report_guest_frame();
    abort();
}

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
