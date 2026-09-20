#pragma once

#include "xl_state.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct State State;
typedef struct Memory Memory;

#define XL_REG(state, name) (*(uint64_t *)((char *)(state) + XL_OFFSET_##name))

static inline void xl_return(State *state)
{
    XL_REG(state, PC) = XL_REG(state, X30);
}

static inline void *xl_argument(State *state)
{
    return (void *)(uintptr_t)XL_REG(state, X0);
}

struct xl_selector_shim {
    const char *selector;
    uint32_t guest;
};

struct xl_imp_entry {
    void *imp;
    uint32_t guest;
};

struct xl_variadic_shim {
    const char *selector;
    void (*handler)(State *);
};

void xl_bridge_init(void);
void *xl_take_super_class(State *state);
uintptr_t xl_protocol(uint64_t guest);
_Noreturn void xl_unsupported(const char *message);
_Noreturn void xl_narrowing_fault(const char *symbol, unsigned index, uint64_t value);
uint64_t xl_invoke(uint64_t address, uint64_t argument);
uint64_t xl_invoke_n(uint64_t address, const uint64_t *arguments, unsigned count);
uintptr_t xl_object_in(uint64_t guest, const char *symbol, unsigned index);
uint64_t xl_object_out(uintptr_t host);
unsigned xl_callback_slot(unsigned signature, uint64_t target);
extern uint64_t xl_callback_targets[][32];

static inline int64_t xl_narrow_signed(int64_t value, int64_t host_min, int64_t host_max, int64_t guest_min, int64_t guest_max, const char *symbol, unsigned index)
{
    if (value == guest_max)
        return host_max;
    if (value == guest_min)
        return host_min;
    if (value < host_min || value > host_max)
        xl_narrowing_fault(symbol, index, (uint64_t)value);
    return value;
}

static inline uint64_t xl_narrow_unsigned(uint64_t value, uint64_t host_max, uint64_t guest_max, const char *symbol, unsigned index)
{
    if (value == guest_max)
        return host_max;
    if (value > host_max)
        xl_narrowing_fault(symbol, index, value);
    return value;
}

static inline uintptr_t xl_narrow_pointer(uint64_t value, const char *symbol, unsigned index)
{
    if (value >> 32)
        xl_narrowing_fault(symbol, index, value);
    return (uintptr_t)value;
}

static inline int64_t xl_widen_signed(int64_t value, int64_t host_min, int64_t host_max, int64_t guest_min, int64_t guest_max)
{
    if (value == host_max)
        return guest_max;
    if (value == host_min)
        return guest_min;
    return value;
}

static inline uint64_t xl_widen_unsigned(uint64_t value, uint64_t host_max, uint64_t guest_max)
{
    return value == host_max ? guest_max : value;
}

struct xl_format {
    char *text;
    void *object;
    char *arguments;
};

void xl_format_marshal(struct xl_format *format, const char *text, void *object, const uint64_t *arguments, const char *symbol);
void xl_format_marshal_valist(struct xl_format *format, const char *text, void *object, uint64_t guest_valist, const char *symbol);
void xl_format_release(struct xl_format *format);

// Shims for libc functions absent on the target release (the *at family is not on iOS 6). The
// bridge generator routes the guest call here instead of the missing libSystem symbol.
int xl_shim_openat(int dirfd, const char *path, int flags, int mode);
