/* Host stand-ins for locks, allocation and port references; real XNU bodies
 * are extracted into mach-ports.inc by check-picasso-mach-ports.py. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int kern_return_t;
typedef unsigned int mach_msg_type_number_t;
struct port { int ip_immovable_send; int refs; };
typedef struct port *ipc_port_t;
typedef ipc_port_t mach_port_t;
/* Extracted XNU_KERNEL_PRIVATE declarations; host ptrauth is disabled. */
#define __mach_port_array_auth
#include "xnu-port-array.h"
#define TASK_PORT_REGISTER_MAX 3
#define KERN_SUCCESS 0
#define KERN_INVALID_ARGUMENT 4
#define KERN_INVALID_RIGHT 17
#define IP_NULL NULL
#define IP_DEAD ((ipc_port_t)(uintptr_t)UINTPTR_MAX)
#define IP_VALID(p) ((p) != IP_NULL && (p) != IP_DEAD)
#define TASK_NULL NULL
#define Z_WAITOK 1
#define Z_ZERO 2
#define Z_NOFAIL 4
struct task { int ipc_active; int locked; ipc_port_t itk_registered[3]; };
typedef struct task *task_t;
static unsigned allocations, frees, locks;
static void *alloc_array(size_t count, size_t size, int flags)
{
    void *p;
    assert(flags == (Z_WAITOK | Z_ZERO | Z_NOFAIL));
    p = calloc(count, size);
    assert(p);
    allocations++;
    return p;
}
static void free_array(void *p, size_t count)
{
    assert(p && count > 0 && count <= 3);
    free(p);
    frees++;
}
#define kalloc_type(type, count, flags) ((type *)alloc_array(count, sizeof(type), flags))
#define kfree_type(type, count, p) free_array(p, count)
static void itk_lock(task_t task)
{
    assert(!task->locked);
    task->locked = 1;
    locks++;
}
static void itk_unlock(task_t task)
{
    assert(task->locked);
    task->locked = 0;
}
static void ipc_port_release_send(ipc_port_t p)
{
    if (IP_VALID(p)) { assert(p->refs > 0); p->refs--; }
}
static ipc_port_t ipc_port_copy_send_any(ipc_port_t p)
{
    if (IP_VALID(p)) { assert(p->refs > 0); p->refs++; }
    return p;
}

#include "mach-ports.inc"

static void drop_result(mach_port_array_t p, unsigned count)
{
    for (unsigned i = 0; i < count; i++) { ipc_port_release_send(p[i].port); }
    free_array(p, count);
}
static void check_result(mach_port_array_t p, const ipc_port_t *expected, unsigned count)
{
    for (unsigned i = 0; i < count; i++) { assert(p[i].port == expected[i]); }
}
int main(void)
{
    unsigned cases = 0;
    /* Every legal count, including duplicate/null/dead rights. */
    for (unsigned count = 0; count <= 3; count++) {
        for (unsigned pattern = 0; pattern < 64; pattern++) {
            struct port a = {0, 1}, b = {0, 1}, old = {0, 3};
            struct task task = {1, 0, {&old, &old, &old}};
            mach_port_array_t input = count ? mach_port_array_alloc(count, 7) : NULL;
            ipc_port_t expected[3] = {0};
            unsigned refs_a = 1, refs_b = 1;
            for (unsigned i = 0; i < count; i++) {
                ipc_port_t options[] = {IP_NULL, IP_DEAD, &a, &b};
                expected[i] = options[(pattern >> (i * 2)) & 3];
                input[i].port = ipc_port_copy_send_any(expected[i]);
                if (expected[i] == &a) { refs_a++; }
                if (expected[i] == &b) { refs_b++; }
            }
            assert(mach_ports_register(&task, input, count) == KERN_SUCCESS);
            assert(old.refs == 0 && !task.locked);
            assert(memcmp(expected, task.itk_registered, sizeof(expected)) == 0);
            assert(a.refs == (int)refs_a && b.refs == (int)refs_b);
            mach_port_array_t result = NULL;
            unsigned result_count = 99;
            assert(mach_ports_lookup(&task, &result, &result_count) == KERN_SUCCESS);
            assert(result_count == 3 && result);
            check_result(result, expected, result_count);
            assert(a.refs == (int)(2 * refs_a - 1));
            assert(b.refs == (int)(2 * refs_b - 1));
            drop_result(result, result_count);
            /* Children inherit actual references through the existing task path. */
            struct task child = {1, 0, {0}};
            for (unsigned i = 0; i < 3; i++) {
                child.itk_registered[i] = ipc_port_copy_send_any(task.itk_registered[i]);
            }
            assert(mach_ports_register(&task, NULL, 0) == KERN_SUCCESS);
            assert(mach_ports_lookup(&child, &result, &result_count) == KERN_SUCCESS);
            check_result(result, expected, result_count);
            drop_result(result, result_count);
            assert(mach_ports_register(&child, NULL, 0) == KERN_SUCCESS);
            assert(a.refs == 1 && b.refs == 1);
            cases++;
        }
    }
    /* Failures leave the incoming memory/rights for MIG error cleanup. */
    for (unsigned scenario = 0; scenario < 7; scenario++) {
        struct port old = {0, 3}, supplied = {0, 3};
        struct task task = {1, 0, {&old, &old, &old}};
        mach_port_array_t input = mach_port_array_alloc(3, 7);
        for (unsigned i = 0; i < 3; i++) { input[i].port = &supplied; }
        task_t target = &task;
        unsigned count = 3;
        mach_port_array_t argument = input;
        if (scenario == 0) { target = NULL; }
        if (scenario == 1) { task.ipc_active = 0; }
        if (scenario == 2) { count = 4; }
        if (scenario == 3) { count = UINT32_MAX; }
        if (scenario == 4) { argument = NULL; }
        if (scenario == 5) { supplied.ip_immovable_send = 1; }
        if (scenario == 6) { input[0].port = IP_DEAD; supplied.refs = 2; supplied.ip_immovable_send = 1; }
        unsigned before_frees = frees;
        int expected = scenario >= 5 ? KERN_INVALID_RIGHT : KERN_INVALID_ARGUMENT;
        assert(mach_ports_register(target, argument, count) == expected);
        assert(frees == before_frees && old.refs == 3 && !task.locked);
        assert(task.itk_registered[0] == &old && task.itk_registered[2] == &old);
        assert(supplied.refs == (scenario == 6 ? 2 : 3));
        drop_result(input, 3);
        assert(supplied.refs == 0);
        cases++;
    }
    for (unsigned scenario = 0; scenario < 2; scenario++) {
        struct task dead = {0, 0, {0}};
        mach_port_array_t result = (mach_port_array_t)(uintptr_t)0x1234;
        unsigned count = 99, before = allocations - frees;
        assert(mach_ports_lookup(scenario ? &dead : NULL, &result, &count) == KERN_INVALID_ARGUMENT);
        assert(result == (mach_port_array_t)(uintptr_t)0x1234 && count == 99);
        assert(!dead.locked && allocations - frees == before);
        cases++;
    }
    assert(allocations == frees && locks > 0);
    printf("PASS mach ports: %u cases, no leaked arrays\n", cases);
    return 0;
}
