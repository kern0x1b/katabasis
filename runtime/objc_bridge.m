#include "objc_abi.h"
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <Block.h>

#include "xl_bridge.h"
#include "xl_host.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

extern const struct xl_selector_shim xl_selector_shims[];
extern const struct xl_imp_entry xl_imp_map[];
extern const struct xl_variadic_shim xl_variadic_shims[];
extern void xl_bridge_init_generated(void);
extern const uint32_t xl_initializers[];
extern const uint32_t xl_initializer_count;
extern void xl_tail(State *state);
extern void xl_fault(State *state, const char *reason);

static CFMutableDictionaryRef xl_selector_table;
static CFMutableDictionaryRef xl_guest_imps;
static CFMutableDictionaryRef xl_variadic_table;
static pthread_once_t xl_setup_once = PTHREAD_ONCE_INIT;
static pthread_key_t xl_super_key;
static void xl_load_rules(void);

static void xl_uncaught(NSException *exception)
{
    FILE *log = fopen("/private/var/charon/xlate-exception.log", "a");
    if (log) {
        fprintf(log, "uncaught %s: %s\n", [[exception name] UTF8String] ?: "?", [[exception reason] UTF8String] ?: "?");
        for (NSString *frame in [exception callStackSymbols]) {
            fprintf(log, "  %s\n", [frame UTF8String] ?: "?");
        }
        fclose(log);
    }
    fprintf(stderr, "xlate: uncaught %s: %s\n", [[exception name] UTF8String] ?: "?", [[exception reason] UTF8String] ?: "?");
}

static void xl_fix_layouts(void);

static void xl_setup(void)
{
    NSSetUncaughtExceptionHandler(xl_uncaught);
    { FILE *f = fopen("/private/var/charon/xlate-trace.log", "a"); if (f) { fprintf(f, "fix start\n"); fclose(f); } }
    xl_fix_layouts();
    { FILE *f = fopen("/private/var/charon/xlate-trace.log", "a"); if (f) { fprintf(f, "fix done\n"); fclose(f); } }
    xl_load_rules();
    pthread_key_create(&xl_super_key, NULL);
    xl_selector_table = CFDictionaryCreateMutable(NULL, 0, NULL, NULL);
    xl_guest_imps = CFDictionaryCreateMutable(NULL, 0, NULL, NULL);
    for (const struct xl_selector_shim *shim = xl_selector_shims; shim->selector; shim++)
        CFDictionarySetValue(xl_selector_table, sel_registerName(shim->selector), (void *)(uintptr_t)shim->guest);
    for (const struct xl_imp_entry *entry = xl_imp_map; entry->imp; entry++)
        CFDictionarySetValue(xl_guest_imps, entry->imp, (void *)(uintptr_t)entry->guest);
    xl_variadic_table = CFDictionaryCreateMutable(NULL, 0, NULL, NULL);
    for (const struct xl_variadic_shim *shim = xl_variadic_shims; shim->selector; shim++)
        CFDictionarySetValue(xl_variadic_table, sel_registerName(shim->selector), (void *)shim->handler);
    xl_bridge_init_generated();
}

void xl_run_initializers(void)
{
    xl_bridge_init();
    int trace = getenv("XL_INIT_TRACE") ? open("/private/var/charon/init-trace.log",
                                               O_WRONLY | O_CREAT | O_TRUNC, 0666) : -1;
    for (uint32_t i = 0; i < xl_initializer_count; i++) {
        if (trace >= 0)
            dprintf(trace, "init %u/%u guest=0x%x\n", i, xl_initializer_count, xl_initializers[i]);
        xl_invoke(xl_initializers[i], 0);
    }
    if (trace >= 0) { dprintf(trace, "all initializers done\n"); close(trace); }
}

void xl_bridge_init(void)
{
    pthread_once(&xl_setup_once, xl_setup);
}

void *xl_take_super_class(State *state)
{
    void *cls = pthread_getspecific(xl_super_key);
    if (cls)
        pthread_setspecific(xl_super_key, NULL);
    return cls;
}

static void xl_zero_result(State *state)
{
    XL_REG(state, X0) = 0;
    XL_REG(state, X1) = 0;
    for (unsigned i = 0; i < 4; i++) {
        *(uint64_t *)((char *)state + XL_OFFSET_V0 + 16 * i) = 0;
        *(uint64_t *)((char *)state + XL_OFFSET_V0 + 16 * i + 8) = 0;
    }
}

static NSMutableArray *xl_neutralized_prefixes;
static NSMutableSet *xl_neutralized_selectors;

extern CGImageRef UIGetScreenImage(void);
static double xl_shot_delay;
static NSString *xl_shot_path;

static void *xl_shot_thread(void *unused)
{
    struct timespec ts = {(time_t)xl_shot_delay, (long)((xl_shot_delay - (time_t)xl_shot_delay) * 1e9)};
    nanosleep(&ts, NULL);
    dispatch_async(dispatch_get_main_queue(), ^{
        CGImageRef image = UIGetScreenImage();
        if (image) {
            NSData *png = UIImagePNGRepresentation([UIImage imageWithCGImage:image]);
            CGImageRelease(image);
            [png writeToFile:xl_shot_path atomically:YES];
        }
    });
    return NULL;
}

static void xl_load_rules(void)
{
    xl_neutralized_prefixes = [[NSMutableArray alloc] init];
    xl_neutralized_selectors = [[NSMutableSet alloc] init];
    NSString *path = [[NSBundle mainBundle] pathForResource:@"xlate" ofType:@"rules"];
    NSString *text = path ? [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:NULL] : nil;
    for (NSString *raw in [text componentsSeparatedByString:@"\n"]) {
        NSString *line = [raw stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        NSCharacterSet *ws = [NSCharacterSet whitespaceCharacterSet];
        if ([line hasPrefix:@"neutralize-class "])
            [xl_neutralized_prefixes addObject:[[line substringFromIndex:[@"neutralize-class " length]] stringByTrimmingCharactersInSet:ws]];
        else if ([line hasPrefix:@"neutralize-selector "])
            [xl_neutralized_selectors addObject:[[line substringFromIndex:[@"neutralize-selector " length]] stringByTrimmingCharactersInSet:ws]];
        else if ([line hasPrefix:@"screenshot "]) {
            NSArray *parts = [[line substringFromIndex:11] componentsSeparatedByString:@" "];
            if (parts.count >= 2) {
                xl_shot_delay = [parts[0] doubleValue];
                xl_shot_path = [parts[1] copy];
            }
        }
    }
    if (xl_shot_path) {
        pthread_t t;
        pthread_create(&t, NULL, xl_shot_thread, NULL);
        pthread_detach(t);
    }
}

static int xl_selector_neutralized(SEL selector)
{
    return xl_neutralized_selectors.count && [xl_neutralized_selectors containsObject:[NSString stringWithUTF8String:sel_getName(selector)]];
}

static int xl_class_neutralized(Class cls)
{
    if (!xl_neutralized_prefixes.count)
        return 0;
    // Match the class or any ancestor, so a neutralized service is absent as a whole:
    // this catches the KVO runtime subclass (NSKVONotifying_GAI, whose superclass is GAI)
    // and every subclass of a neutralized class, not just names carrying the prefix.
    for (Class c = cls; c; c = class_getSuperclass(c)) {
        const char *name = class_getName(c);
        if (!name)
            continue;
        NSString *n = [NSString stringWithUTF8String:name];
        for (NSString *prefix in xl_neutralized_prefixes)
            if ([n hasPrefix:prefix])
                return 1;
    }
    return 0;
}

void xl_report_guest_frame(void);

static uint64_t xl_route(State *state, Class lookup, SEL selector, id receiver, Class super_class)
{
    if (xl_selector_neutralized(selector) || xl_class_neutralized(lookup))
        return 0;
    IMP imp = class_getMethodImplementation(lookup, selector);
    uintptr_t guest = (uintptr_t)CFDictionaryGetValue(xl_guest_imps, imp);
    if (guest)
        return guest;
    guest = (uintptr_t)CFDictionaryGetValue(xl_selector_table, selector);
    if (!guest) {
        fprintf(stderr, "xlate: no bridge for -[%s %s]\n", class_getName(lookup), sel_getName(selector));
        fprintf(stderr, "xlate:   receiver=%p isa=0x%x lookup=%p(%s) isMeta=%d imp=%p\n",
                (void *)receiver, receiver ? *(uint32_t *)receiver : 0, (void *)lookup, class_getName(lookup),
                class_isMetaClass(lookup), (void *)imp);
        xl_report_guest_frame();
        // Collect mode: env XL_COLLECT, or a flag file (env does not propagate to a
        // SpringBoard-launched app on iOS 6). Log the missing selector and neutralize it
        // (return 0) so the app runs on and surfaces every gap in one pass.
        if (getenv("XL_COLLECT") || access("/private/var/charon/xl-collect", F_OK) == 0) {
            FILE *log = fopen("/private/var/charon/xlate-missing.log", "a");
            if (log) { fprintf(log, "-[%s %s]\n", class_getName(lookup), sel_getName(selector)); fclose(log); }
            return 0;
        }
        xl_fault(state, "message without a bridge");
    }
    if (super_class)
        pthread_setspecific(xl_super_key, super_class);
    return guest;
}

static int xl_is_guest_block(uint64_t guest);
static struct xl_block_wrapper *xl_wrapper_for(uint64_t guest);
uint64_t xl_object_out(uintptr_t host);

void xl_h_objc_msgSend(State *state)
{
    uint64_t self = XL_REG(state, X0);
    if (!self) {
        xl_zero_result(state);
        xl_return(state);
        return;
    }
    if (xl_is_guest_block(self)) {
        // Capture the selector and arguments before xl_wrapper_for: it re-enters the
        // guest (guest _Block_copy) and clobbers the caller-saved state registers.
        SEL bsel = (SEL)(uintptr_t)XL_REG(state, X1);
        uint64_t a2 = XL_REG(state, X2), a3 = XL_REG(state, X3), a4 = XL_REG(state, X4);
        struct xl_block_wrapper *w = xl_wrapper_for(self);
        id result = ((id (*)(id, SEL, uint64_t, uint64_t, uint64_t))objc_msgSend)((id)w, bsel, a2, a3, a4);
        objc_release((id)w);
        XL_REG(state, X0) = xl_object_out((uintptr_t)result);
        xl_return(state);
        return;
    }
    id receiver = (id)xl_narrow_pointer(self, "objc_msgSend", 0);
    SEL selector = (SEL)(uintptr_t)XL_REG(state, X1);
    if (getenv("XL_TRACK_DIAG")) {
        const char *sn = sel_getName(selector);
        if (sn && (!strcmp(sn, "trackAction:") || !strcmp(sn, "defaultTracker") || !strcmp(sn, "sharedInstance"))) {
            uint32_t classref = *(uint32_t *)(uintptr_t)0x1010ee60u;
            fprintf(stderr, "xlate: DIAG %s self=%p isa=0x%x class=%s classref[0x1010ee60]=0x%x\n",
                    sn, (void *)receiver, receiver ? *(uint32_t *)receiver : 0,
                    class_getName(object_getClass(receiver)), classref);
        }
    }
    if (xl_class_neutralized(object_getClass(receiver)) || xl_selector_neutralized(selector)) {
        xl_zero_result(state);
        xl_return(state);
        return;
    }
    void (*variadic)(State *) = (void (*)(State *))CFDictionaryGetValue(xl_variadic_table, selector);
    if (variadic) {
        variadic(state);
        return;
    }
    uint64_t target = xl_route(state, object_getClass(receiver), selector, receiver, Nil);
    if (!target) {
        xl_zero_result(state);
        xl_return(state);
        return;
    }
    XL_REG(state, PC) = target;
    [[clang::musttail]] return xl_tail(state);
}

void xl_h_objc_msgSendSuper2(State *state)
{
    uint64_t *guest_super = (uint64_t *)xl_narrow_pointer(XL_REG(state, X0), "objc_msgSendSuper2", 0);
    id receiver = (id)(uintptr_t)guest_super[0];
    Class current = (Class)(uintptr_t)guest_super[1];
    SEL selector = (SEL)(uintptr_t)XL_REG(state, X1);
    XL_REG(state, X0) = (uintptr_t)receiver;
    uint64_t target = xl_route(state, class_getSuperclass(current), selector, receiver, current);
    if (!target) {
        xl_zero_result(state);
        xl_return(state);
        return;
    }
    XL_REG(state, PC) = target;
    [[clang::musttail]] return xl_tail(state);
}

void xl_h_xl_unsupported(State *state)
{
    xl_unsupported((const char *)xl_narrow_pointer(XL_REG(state, X0), "xl_unsupported", 0));
}

void xl_unsupported_imp(id self, SEL selector)
{
    fprintf(stderr, "xlate: host called -[%s %s], whose bridge was not generated\n", object_getClassName(self), sel_getName(selector));
    abort();
}

@implementation XLGuestBlock {
    uint64_t _guestBlock;
}

+ (instancetype)holderWithGuestBlock:(uint64_t)guest
{
    XLGuestBlock *holder = [[self alloc] init];
    holder->_guestBlock = xl_invoke(XL_GUEST__Block_copy, guest);
    return [holder autorelease];
}

- (uint64_t)guestBlock
{
    return _guestBlock;
}

- (void)dealloc
{
    xl_invoke(XL_GUEST__Block_release, _guestBlock);
    [super dealloc];
}

@end

struct __attribute__((packed)) xl_fast_enumeration_pack {
    uint64_t self;
    uint64_t sel;
    uint64_t state;
    uint64_t buffer;
    uint64_t length;
    uint64_t r;
};

struct xl_guest_enumeration {
    uint64_t state;
    uint64_t items;
    uint64_t mutations;
    uint64_t extra[5];
};

void xl_manual_fast_enumeration(void *pack)
{
    struct xl_fast_enumeration_pack *p = pack;
    struct xl_guest_enumeration *guest = (struct xl_guest_enumeration *)xl_narrow_pointer(p->state, "countByEnumeratingWithState:objects:count:", 0);
    NSFastEnumerationState *host = (NSFastEnumerationState *)&guest->extra[0];
    id buffer[16];
    NSUInteger length = p->length < 16 ? (NSUInteger)p->length : 16;
    NSUInteger count = [(id)(uintptr_t)p->self countByEnumeratingWithState:host objects:buffer count:length];
    if (count) {
        uint64_t *items = [[NSMutableData dataWithLength:count * sizeof(uint64_t)] mutableBytes];
        for (NSUInteger i = 0; i < count; i++)
            items[i] = (uintptr_t)host->itemsPtr[i];
        guest->items = (uintptr_t)items;
        guest->extra[4] = host->mutationsPtr ? *host->mutationsPtr : 0;
        guest->mutations = (uintptr_t)&guest->extra[4];
        guest->state = 1;
    }
    p->r = count;
}

id xl_format_string_with_format(id self, SEL selector, id format, va_list arguments)
{
    return [[[(Class)self alloc] initWithFormat:format arguments:arguments] autorelease];
}

id xl_format_init_with_format(id self, SEL selector, id format, va_list arguments)
{
    return [self initWithFormat:format arguments:arguments];
}

void xl_format_append_format(id self, SEL selector, id format, va_list arguments)
{
    NSString *text = [[NSString alloc] initWithFormat:format arguments:arguments];
    [self appendString:text];
    [text release];
}

struct __attribute__((packed)) xl_init_format_pack {
    uint64_t self;
    uint64_t sel;
    uint64_t format;
    uint64_t arguments;
    uint64_t r;
};

// initWithFormat:arguments: receives an explicit va_list, so unlike the variadic format
// methods it cannot be reached through the inline-vararg path. The guest hands over a guest
// arm64 va_list, which the host method would misread as an armv7 one; rebuild the arguments
// from the guest va_list and format the string on the host side.
void xl_manual_init_with_format_arguments(void *pack)
{
    struct xl_init_format_pack *p = pack;
    struct xl_format format;
    xl_format_marshal_valist(&format, 0, (void *)xl_object_in(p->format, "initWithFormat:arguments:", 0), p->arguments,
                             "initWithFormat:arguments:");
    id result = [(id)(uintptr_t)p->self initWithFormat:(NSString *)format.object arguments:(va_list)format.arguments];
    xl_format_release(&format);
    p->r = xl_object_out((uintptr_t)result);
}

struct __attribute__((packed)) xl_weak_pack {
    uint64_t a0;
    uint64_t a1;
    uint64_t r;
};

struct __attribute__((packed)) xl_weak_pack1 {
    uint64_t a0;
    uint64_t r;
};

static id *xl_weak_slot(uint64_t address)
{
    return (id *)xl_narrow_pointer(address, "weak reference slot", 0);
}

void xl_manual_objc_initWeak(struct xl_weak_pack *p)
{
    id *slot = xl_weak_slot(p->a0);
    *(uint64_t *)slot = 0;
    p->r = (uintptr_t)objc_initWeak(slot, (id)xl_narrow_pointer(p->a1, "objc_initWeak", 1));
}

void xl_manual_objc_storeWeak(struct xl_weak_pack *p)
{
    p->r = (uintptr_t)objc_storeWeak(xl_weak_slot(p->a0), (id)xl_narrow_pointer(p->a1, "objc_storeWeak", 1));
}

void xl_manual_objc_destroyWeak(struct xl_weak_pack1 *p)
{
    objc_destroyWeak(xl_weak_slot(p->a0));
}

void xl_manual_objc_loadWeakRetained(struct xl_weak_pack1 *p)
{
    p->r = (uintptr_t)objc_loadWeakRetained(xl_weak_slot(p->a0));
}

void xl_manual_objc_loadWeak(struct xl_weak_pack1 *p)
{
    p->r = (uintptr_t)objc_loadWeak(xl_weak_slot(p->a0));
}

void xl_manual_objc_copyWeak(struct xl_weak_pack *p)
{
    id *to = xl_weak_slot(p->a0);
    *(uint64_t *)to = 0;
    objc_copyWeak(to, xl_weak_slot(p->a1));
}

void xl_manual_objc_moveWeak(struct xl_weak_pack *p)
{
    id *to = xl_weak_slot(p->a0);
    *(uint64_t *)to = 0;
    objc_moveWeak(to, xl_weak_slot(p->a1));
}

struct xl_guest_host_block {
    uint64_t isa;
    int32_t flags;
    int32_t reserved;
    uint64_t invoke;
    uint64_t descriptor;
    uint64_t host;
};

uint64_t xl_host_block_wrap(id block, uint32_t invoke, uint32_t descriptor)
{
    if (!block)
        return 0;
    struct xl_guest_host_block *guest = calloc(1, sizeof *guest);
    guest->isa = XL_GUEST__NSConcreteMallocBlock;
    guest->flags = (1 << 24) | (1 << 25) | 1;
    guest->invoke = invoke;
    guest->descriptor = descriptor;
    guest->host = (uintptr_t)_Block_copy(block);
    return (uintptr_t)guest;
}

void xl_h_xl_host_block_release(State *state)
{
    _Block_release((void *)xl_narrow_pointer(XL_REG(state, X0), "host block", 0));
    xl_return(state);
}

// A guest block and its host wrapper are one logical block with two ABI shapes: the
// invoke pointer sits at a different offset in each (armv7 12, arm64 16) so neither
// runtime can drive the other's representation. A+ keeps both, with a canonical
// mapping so the same guest block always yields the same wrapper (pointer identity,
// keys, comparison), one linked reference count (the wrapper holds one guest ref for
// its whole life), and per-side invocation.

extern void *_NSConcreteMallocBlock;

struct xl_block_desc {
    unsigned long reserved;
    unsigned long size;
    void (*copy)(void *dst, const void *src);
    void (*dispose)(const void *);
    const char *signature;
};

struct xl_block_wrapper {
    void *isa;
    volatile int32_t flags;
    int32_t reserved;
    void *invoke;
    struct xl_block_desc *descriptor;
    uint64_t guest;
};

enum { XL_BLOCK_HAS_COPY_DISPOSE = 1 << 25, XL_BLOCK_HAS_SIGNATURE = 1 << 30, XL_BLOCK_NEEDS_FREE = 1 << 24 };

static pthread_mutex_t xl_block_lock = PTHREAD_MUTEX_INITIALIZER;
static CFMutableDictionaryRef xl_block_forward;

static int xl_is_guest_block(uint64_t guest)
{
    if (!guest || guest >> 32)
        return 0;
    uint32_t isa = *(uint32_t *)(uintptr_t)guest;
    return isa == XL_GUEST__NSConcreteMallocBlock || isa == XL_GUEST__NSConcreteStackBlock ||
           isa == XL_GUEST__NSConcreteGlobalBlock || isa == XL_GUEST__NSConcreteAutoBlock;
}

// One ObjC type token: 'V' void, 'I' integer-like, 'P' pointer-like, 'X' unsupported.
static char xl_type_token(const char **p)
{
    while (**p == 'r' || **p == 'n' || **p == 'N' || **p == 'o' || **p == 'O' || **p == 'R' || **p == 'V')
        (*p)++;
    char c = **p;
    switch (c) {
    case 'v': (*p)++; return 'V';
    case 'c': case 'i': case 's': case 'l': case 'q':
    case 'C': case 'I': case 'S': case 'L': case 'Q': case 'B': (*p)++; return 'I';
    case '@':
        (*p)++;
        if (**p == '?') (*p)++;
        else if (**p == '"') { (*p)++; while (**p && **p != '"') (*p)++; if (**p == '"') (*p)++; }
        return 'P';
    case '#': case ':': case '*': (*p)++; return 'P';
    case '^': (*p)++; xl_type_token(p); return 'P';
    default: return 'X';
    }
}

// Count of real arguments (past the block parameter) when the signature is simple
// (void/integer/pointer return and args only); -1 if unsupported.
static int xl_block_simple_args(const char *sig)
{
    if (!sig)
        return -1;
    const char *p = sig;
    char ret = xl_type_token(&p);
    if (ret == 'X')
        return -1;
    while (*p >= '0' && *p <= '9') p++;
    if (xl_type_token(&p) != 'P')
        return -1;
    while (*p >= '0' && *p <= '9') p++;
    int count = 0;
    while (*p) {
        char t = xl_type_token(&p);
        if (t == 'X' || t == 'V')
            return -1;
        while (*p >= '0' && *p <= '9') p++;
        count++;
    }
    return count > 4 ? -1 : count;
}

static uintptr_t xl_block_call(struct xl_block_wrapper *w, const uint64_t *args, unsigned n)
{
    uint64_t invoke = *(uint64_t *)(uintptr_t)(w->guest + 16);
    uint64_t all[5];
    all[0] = w->guest;
    for (unsigned i = 0; i < n; i++)
        all[i + 1] = args[i];
    return (uintptr_t)xl_invoke_n(invoke, all, n + 1);
}

static uintptr_t xl_block_t0(void *b) { return xl_block_call(b, 0, 0); }
static uintptr_t xl_block_t1(void *b, uintptr_t a0) { uint64_t a[1] = {a0}; return xl_block_call(b, a, 1); }
static uintptr_t xl_block_t2(void *b, uintptr_t a0, uintptr_t a1) { uint64_t a[2] = {a0, a1}; return xl_block_call(b, a, 2); }
static uintptr_t xl_block_t3(void *b, uintptr_t a0, uintptr_t a1, uintptr_t a2) { uint64_t a[3] = {a0, a1, a2}; return xl_block_call(b, a, 3); }
static uintptr_t xl_block_t4(void *b, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) { uint64_t a[4] = {a0, a1, a2, a3}; return xl_block_call(b, a, 4); }
static void xl_block_unsupported_invoke(void *b) { (void)b; xl_unsupported("host-invoked guest block with a signature the bridge cannot marshal"); }

static const char *xl_guest_block_signature(uint64_t guest)
{
    int32_t flags = *(int32_t *)(uintptr_t)(guest + 8);
    if (!(flags & XL_BLOCK_HAS_SIGNATURE))
        return 0;
    uint64_t descriptor = *(uint64_t *)(uintptr_t)(guest + 24);
    if (!descriptor || descriptor >> 32)
        return 0;
    uint64_t at = descriptor + 16;
    if (flags & XL_BLOCK_HAS_COPY_DISPOSE)
        at += 16;
    uint64_t signature = *(uint64_t *)(uintptr_t)at;
    return (signature && signature >> 32 == 0) ? (const char *)(uintptr_t)signature : 0;
}

static void xl_block_copy_helper(void *dst, const void *src) { (void)dst; (void)src; }

static void xl_block_dispose_helper(const void *b)
{
    struct xl_block_wrapper *w = (struct xl_block_wrapper *)b;
    pthread_mutex_lock(&xl_block_lock);
    if (xl_block_forward)
        CFDictionaryRemoveValue(xl_block_forward, (const void *)(uintptr_t)(uint32_t)w->guest);
    pthread_mutex_unlock(&xl_block_lock);
    xl_invoke(XL_GUEST__Block_release, w->guest);
}

static struct xl_block_desc xl_block_descriptor = {
    0, sizeof(struct xl_block_wrapper), xl_block_copy_helper, xl_block_dispose_helper, 0
};

// Returns the canonical wrapper for a guest block with one extra reference for the
// caller: the same guest block always maps to the same wrapper, and every reference
// count (guest _Block_copy/_Block_release, host retain/release) funnels through the
// wrapper's single host count. The wrapper holds exactly one guest reference for its
// whole life; when the count reaches zero the dispose helper drops it.
static struct xl_block_wrapper *xl_wrapper_for(uint64_t guest)
{
    pthread_mutex_lock(&xl_block_lock);
    if (!xl_block_forward)
        xl_block_forward = CFDictionaryCreateMutable(NULL, 0, NULL, NULL);
    struct xl_block_wrapper *w = (struct xl_block_wrapper *)CFDictionaryGetValue(xl_block_forward, (const void *)(uintptr_t)(uint32_t)guest);
    if (w) {
        _Block_copy(w);
        pthread_mutex_unlock(&xl_block_lock);
        return w;
    }
    w = calloc(1, sizeof *w);
    w->isa = &_NSConcreteMallocBlock;
    w->flags = XL_BLOCK_NEEDS_FREE | XL_BLOCK_HAS_COPY_DISPOSE | XL_BLOCK_HAS_SIGNATURE | 2;
    w->descriptor = &xl_block_descriptor;
    w->guest = xl_invoke(XL_GUEST__Block_copy, guest);
    int args = xl_block_simple_args(xl_guest_block_signature(guest));
    switch (args) {
    case 0: w->invoke = (void *)xl_block_t0; break;
    case 1: w->invoke = (void *)xl_block_t1; break;
    case 2: w->invoke = (void *)xl_block_t2; break;
    case 3: w->invoke = (void *)xl_block_t3; break;
    case 4: w->invoke = (void *)xl_block_t4; break;
    default: w->invoke = (void *)xl_block_unsupported_invoke; break;
    }
    CFDictionarySetValue(xl_block_forward, (const void *)(uintptr_t)(uint32_t)w->guest, w);
    pthread_mutex_unlock(&xl_block_lock);
    return w;
}

static struct xl_block_wrapper *xl_wrapper_of(uintptr_t host)
{
    if (host && *(void **)host == &_NSConcreteMallocBlock &&
        ((struct xl_block_wrapper *)host)->descriptor == &xl_block_descriptor)
        return (struct xl_block_wrapper *)host;
    return 0;
}

void xl_report_guest_frame(void);

uintptr_t xl_object_in(uint64_t guest, const char *symbol, unsigned index)
{
    if (guest && guest < 0x4000) {  // below __PAGEZERO: an integer reached an object slot
        fprintf(stderr, "xlate: %s argument %u is 0x%llx, not an object pointer\n", symbol, index, guest);
        xl_report_guest_frame();
        abort();
    }
    if (!xl_is_guest_block(guest))
        return xl_narrow_pointer(guest, symbol, index);
    return (uintptr_t)objc_autorelease((id)xl_wrapper_for(guest));
}

uint64_t xl_object_out(uintptr_t host)
{
    struct xl_block_wrapper *w = xl_wrapper_of(host);
    return w ? w->guest : host;
}

// Reference counting of a guest block routes to its wrapper so the two ABI shapes
// share one count.
uint64_t xl_block_retain(uint64_t guest)
{
    xl_wrapper_for(guest);
    return guest;
}

void xl_block_release(uint64_t guest)
{
    pthread_mutex_lock(&xl_block_lock);
    struct xl_block_wrapper *w = xl_block_forward ? (struct xl_block_wrapper *)CFDictionaryGetValue(xl_block_forward, (const void *)(uintptr_t)(uint32_t)guest) : 0;
    pthread_mutex_unlock(&xl_block_lock);
    if (w)
        _Block_release(w);
}

uint64_t xl_block_autorelease(uint64_t guest)
{
    objc_autorelease((id)xl_wrapper_for(guest));
    return guest;
}

uintptr_t xl_protocol(uint64_t guest)
{
    if (!guest)
        return 0;
    const char *name = (const char *)xl_narrow_pointer(*(uint32_t *)(uintptr_t)(guest + 4), "protocol name", 0);
    return name ? (uintptr_t)objc_getProtocol(name) : 0;
}

static uint64_t xl_variadic_arg(State *state, unsigned index)
{
    // Apple arm64: the named first object is in x2; the variadic rest are on the guest stack.
    if (index == 0)
        return XL_REG(state, X2);
    uint64_t sp = XL_REG(state, SP);
    return *(uint64_t *)(uintptr_t)xl_narrow_pointer(sp + 8 * (index - 1), "variadic argument", index);
}

void xl_h_dictionaryWithObjectsAndKeys(State *state)
{
    id self = (id)xl_narrow_pointer(XL_REG(state, X0), "dictionaryWithObjectsAndKeys:", 0);
    NSMutableDictionary *dict = [NSMutableDictionary dictionary];
    for (unsigned i = 0;; i += 2) {
        uint64_t value = xl_variadic_arg(state, i);
        if (!value)
            break;
        uint64_t key = xl_variadic_arg(state, i + 1);
        if (!key)
            break;
        [dict setObject:(id)(uintptr_t)value forKey:(id)(uintptr_t)key];
    }
    id result = [self isKindOfClass:object_getClass([NSMutableDictionary class])] ? dict : [[dict copy] autorelease];
    XL_REG(state, X0) = (uintptr_t)result;
    xl_return(state);
}

static void xl_collect_objects(State *state, NSMutableArray *array)
{
    for (unsigned i = 0;; i++) {
        uint64_t value = xl_variadic_arg(state, i);
        if (!value)
            break;
        [array addObject:(id)(uintptr_t)value];
    }
}

void xl_h_arrayWithObjects(State *state)
{
    NSMutableArray *array = [NSMutableArray array];
    xl_collect_objects(state, array);
    XL_REG(state, X0) = (uintptr_t)[[array copy] autorelease];
    xl_return(state);
}

void xl_h_initWithObjects(State *state)
{
    id self = (id)xl_narrow_pointer(XL_REG(state, X0), "initWithObjects:", 0);
    NSMutableArray *array = [NSMutableArray array];
    xl_collect_objects(state, array);
    id result = [self initWithArray:array];
    XL_REG(state, X0) = (uintptr_t)result;
    xl_return(state);
}

void xl_h_setWithObjects(State *state)
{
    NSMutableArray *array = [NSMutableArray array];
    xl_collect_objects(state, array);
    XL_REG(state, X0) = (uintptr_t)[NSSet setWithArray:array];
    xl_return(state);
}

// ARC return-value handling across the bridge must use the slow path: the fast
// path stores a token in thread-local storage expecting objc_retainAutoreleasedReturnValue
// to run as the very next call, but host ObjC inside the bridges runs in between and
// corrupts that token, which turned returned objects into nil downstream.
struct __attribute__((packed)) xl_arc_pack {
    uint64_t a0;
    uint64_t r;
};

void xl_manual_objc_autoreleaseReturnValue(struct xl_arc_pack *p)
{
    if (p->a0 && xl_is_guest_block(p->a0)) { p->r = xl_block_autorelease(p->a0); return; }
    p->r = (uintptr_t)objc_autorelease((id)xl_narrow_pointer(p->a0, "objc_autoreleaseReturnValue", 0));
}

void xl_manual_objc_retainAutoreleasedReturnValue(struct xl_arc_pack *p)
{
    if (p->a0 && xl_is_guest_block(p->a0)) { p->r = xl_block_retain(p->a0); return; }
    p->r = (uintptr_t)objc_retain((id)xl_narrow_pointer(p->a0, "objc_retainAutoreleasedReturnValue", 0));
}

void xl_manual_objc_claimAutoreleasedReturnValue(struct xl_arc_pack *p)
{
    if (p->a0 && xl_is_guest_block(p->a0)) { p->r = xl_block_retain(p->a0); return; }
    p->r = (uintptr_t)objc_retain((id)xl_narrow_pointer(p->a0, "objc_claimAutoreleasedReturnValue", 0));
}

void xl_manual_objc_retainAutoreleaseReturnValue(struct xl_arc_pack *p)
{
    if (p->a0 && xl_is_guest_block(p->a0)) { objc_autorelease((id)xl_wrapper_for(p->a0)); p->r = p->a0; return; }
    id object = (id)xl_narrow_pointer(p->a0, "objc_retainAutoreleaseReturnValue", 0);
    p->r = (uintptr_t)objc_autorelease(objc_retain(object));
}

void xl_manual_objc_unsafeClaimAutoreleasedReturnValue(struct xl_arc_pack *p)
{
    p->r = p->a0;
}

// A guest block carries the arm64 Block_layout (8-byte isa/invoke/descriptor, 8-byte
// descriptor size). Handing it to the host block runtime copies it by the armv7 layout,
// reading the size field at the wrong offset, so _Block_copy runs memmove over a garbage
// length. ARC block copies stay guest-side: copy through the guest's own Block runtime.
void xl_manual_objc_retainBlock(struct xl_arc_pack *p)
{
    p->r = (p->a0 && xl_is_guest_block(p->a0)) ? xl_block_retain(p->a0) : p->a0;
}

void xl_manual_objc_retain(struct xl_arc_pack *p)
{
    if (p->a0 && xl_is_guest_block(p->a0))
        p->r = xl_block_retain(p->a0);
    else
        p->r = (uintptr_t)objc_retain((id)xl_narrow_pointer(p->a0, "objc_retain", 0));
}

void xl_manual_objc_release(struct xl_arc_pack *p)
{
    if (p->a0 && xl_is_guest_block(p->a0))
        xl_block_release(p->a0);
    else
        objc_release((id)xl_narrow_pointer(p->a0, "objc_release", 0));
    p->r = 0;
}

void xl_manual_objc_autorelease(struct xl_arc_pack *p)
{
    if (p->a0 && xl_is_guest_block(p->a0))
        p->r = xl_block_autorelease(p->a0);
    else
        p->r = (uintptr_t)objc_autorelease((id)xl_narrow_pointer(p->a0, "objc_autorelease", 0));
}

extern const struct xl_class_layout xl_class_layouts[];

// Variant A: reconcile guest ObjC subclass ivar layout with the host superclass.
// The guest metadata carries arm64 offsets (8-byte slots, instanceStart sized for the
// arm64 superclass). On armv7 the host superclass has a different instanceSize, so the
// runtime's own ivar slide would use the wrong base and corrupt every offset. We do the
// slide ourselves with the real host superclass size, keeping the guest's 8-byte slots
// (no per-ivar repacking, so no NSInteger/int64 ambiguity), then set instanceStart to the
// host superclass size so the runtime leaves the class alone.
static const struct xl_class_layout *xl_find_layout(uint32_t address)
{
    for (const struct xl_class_layout *l = xl_class_layouts; l->ro; l++)
        if (l->address == address)
            return l;
    return NULL;
}

static CFMutableSetRef xl_fixed_layouts;

static uint32_t xl_fix_layout(const struct xl_class_layout *l)
{
    if (!l)
        return 0;
    if (CFSetContainsValue(xl_fixed_layouts, l->ro))
        return l->ro->instance_size;
    CFSetAddValue(xl_fixed_layouts, l->ro);
    // Compute from the ORIGINAL guest values recorded at translation time, not from the
    // current class_ro: the ObjC runtime may have already realized (and mis-slid) this
    // class before this runs, so reading current fields would double-apply or read garbage.
    uint32_t super_size;
    if (l->super_guest) {
        super_size = xl_fix_layout(xl_find_layout(l->super_guest));
    } else if (l->super_host) {
        Class super = objc_getClass(l->super_host);
        super_size = super ? (uint32_t)class_getInstanceSize(super) : l->guest_instance_start;
    } else {
        super_size = l->guest_instance_start;
    }
    int32_t shift = (int32_t)super_size - (int32_t)l->guest_instance_start;
    // Only ever slide ivars UP. When the host superclass is SMALLER than the arm64
    // superclass (the common case, since 32-bit classes are smaller) the shift is
    // negative, but the lifted code still addresses ivars with the arm64 offsets --
    // baked as immediates (`str x0, [self, #40]`) where the compiler knew the layout,
    // or loaded from an offset variable it may have constant-folded. Packing the offsets
    // down would leave those accesses pointing at the old slots while the shrunk
    // instance size drops the last ivar's 8-byte slot past the end of the allocation,
    // so its high half is read/written out of bounds. Keeping the guest's arm64 slots
    // (a harmless gap after the smaller host isa) costs a few bytes and stays correct.
    if (shift < 0)
        shift = 0;
    for (uint32_t i = 0; i < l->ivar_count; i++)
        *l->ivars[i].offset_var = (int32_t)l->ivars[i].guest_offset + shift;
    l->ro->instance_start = (uint32_t)((int32_t)l->guest_instance_start + shift);
    l->ro->instance_size = (uint32_t)((int32_t)l->guest_instance_size + shift);
    return l->ro->instance_size;
}

static void xl_fix_layouts(void)
{
    xl_fixed_layouts = CFSetCreateMutable(NULL, 0, NULL);
    for (const struct xl_class_layout *l = xl_class_layouts; l->ro; l++)
        xl_fix_layout(l);
}
