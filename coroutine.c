#pragma clang diagnostic push
#pragma ide diagnostic ignored "bugprone-reserved-identifier"
#include "coroutine.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>

#if __APPLE__ && __MACH__
#include <sys/ucontext.h>
#else

#include <ucontext.h>

#endif

#define STACK_SIZE (1024*1024)
#define DEFAULT_COROUTINE 16

struct coroutine;

struct schedule {
    char stack[STACK_SIZE];
    ucontext_t main;
    int nco; // 协程数量
    int cap; // 协程容量
    int running; // 运行状态，-1表示未运行，其他表示正运行的协程id
    struct coroutine **co; // 长为 S->cap 的 struct coroutine*指针数组
};

struct coroutine {
    coroutine_func func; // 协程关联的函数
    void *ud; // 要传递给函数的参数
    ucontext_t ctx; // 协程上下文
    struct schedule *sch; // 调度时环境
    ptrdiff_t cap; // 协程栈容量
    ptrdiff_t size; // 协程栈已使用大小
    int status; // 创建时为COROUTINE_READY，被销毁后为COROUTINE_DEAD，运行时为COROUTINE_RUNNING，挂起时为COROUTINE_SUSPEND
    char *stack; // 协程栈
};

struct coroutine *_co_new(struct schedule *S, coroutine_func func, void *ud) {
    struct coroutine *co = malloc(sizeof(*co));
    co->func = func;
    co->ud = ud;
    co->sch = S;
    co->cap = 0;
    co->size = 0;
    co->status = COROUTINE_READY;
    co->stack = NULL; // 创建协程时不分配栈
    return co;
}

void _co_delete(struct coroutine *co) {
    free(co->stack);
    free(co);
}

struct schedule *coroutine_open(void) {
    struct schedule *S = malloc(sizeof(*S));
    S->nco = 0;
    S->cap = DEFAULT_COROUTINE;
    S->running = -1;
    S->co = malloc(sizeof(struct coroutine *) * S->cap);
    memset(S->co, 0, sizeof(struct coroutine *) * S->cap);
    return S;
}

void coroutine_close(struct schedule *S) {
    int i;
    for (i = 0; i < S->cap; i++) {
        struct coroutine *co = S->co[i];
        if (co) {
            _co_delete(co);
        }
    }
    free(S->co);
    S->co = NULL;
    free(S);
}

int coroutine_new(struct schedule *S, coroutine_func func, void *ud) {
    struct coroutine *co = _co_new(S, func, ud);
    if (S->nco >= S->cap) { // S->co空间不足时2倍扩容
        int id = S->cap;
        S->co = realloc(S->co, S->cap * 2 * sizeof(struct coroutine *));
        memset(S->co + S->cap, 0, sizeof(struct coroutine *) * S->cap);
        S->co[S->cap] = co;
        S->cap *= 2;
        ++S->nco;
        return id;
    } else {
        int i;
        for (i = 0; i < S->cap; i++) {
            int id = (i + S->nco) % S->cap; // trick: 优先使用(nco, cap)区间的协程控制块（它们更可能是空闲的）
            if (S->co[id] == NULL) {
                S->co[id] = co;
                ++S->nco;
                return id; // 返回协程id，这也是用户操纵指定协程的句柄
            }
        }
    }
    assert(0);
    return -1;
}

static void mainfunc(uint32_t low32, uint32_t hi32) {
    uintptr_t ptr = (uintptr_t) low32 | ((uintptr_t) hi32 << 32); // 组合两个uint32_t拿到struct schedule*指针，此做法兼容32位/64位指针
    struct schedule *S = (struct schedule *) ptr;
    int id = S->running;
    struct coroutine *C = S->co[id]; // 拿到要执行的协程的指针
    C->func(S, C->ud); // 实际执行协程函数，内部可能会调用coroutine_yield，所以可能不会立即返回
    _co_delete(C); // 一旦返回就说明此协程的函数return了，整个协程执行完毕，销回之
    S->co[id] = NULL;
    --S->nco;
    S->running = -1;
}

void coroutine_resume(struct schedule *S, int id) {
    assert(S->running == -1);
    assert(id >= 0 && id < S->cap);
    struct coroutine *C = S->co[id];
    if (C == NULL)
        return;
    int status = C->status;
    switch (status) {
        case COROUTINE_READY: // 协程第一次resume时获取上下文并设置共享栈为S->stack.
            getcontext(&C->ctx);
            C->ctx.uc_stack.ss_sp = S->stack;
            C->ctx.uc_stack.ss_size = STACK_SIZE;
            C->ctx.uc_link = &S->main; // 协程执行结束/挂起后返回至此函数尾（然后return）
            S->running = id;
            C->status = COROUTINE_RUNNING;
            uintptr_t ptr = (uintptr_t) S;
            makecontext(&C->ctx, (void (*)(void)) mainfunc, 2, (uint32_t) ptr, (uint32_t) (ptr >> 32));
            swapcontext(&S->main, &C->ctx); // 调用mainfunc，运行在共享栈S->stack上
            break;
        case COROUTINE_SUSPEND:
            memcpy(S->stack + STACK_SIZE - C->size, C->stack, C->size); // 拷贝协程栈到共享栈
            S->running = id;
            C->status = COROUTINE_RUNNING;
            swapcontext(&S->main, &C->ctx); // 调用mainfunc，运行在共享栈S->stack上
            break;
        default:
            assert(0);
    }
}

// 保存共享栈S->stack到当前协程的栈C->stack
static void _save_stack(struct coroutine *C, char *top) {
    // trick: 因为协程C运行在S->stack中，所以栈上对象dummy也位于S->stack中
    // [&dummy, top) 就是执行到目前为止整个协程所使用的栈空间，所以保存协程栈时就不需要保存整个S->stack了
    char dummy = 0;
    assert(top - &dummy <= STACK_SIZE);
    if (C->cap < top - &dummy) { // 第一次保存或者协程栈变大了，那么分配C->stack以适应协程栈大小
        free(C->stack);
        C->cap = top - &dummy;
        C->stack = malloc(C->cap);
    }
    C->size = top - &dummy;
    memcpy(C->stack, &dummy, C->size);
}

void coroutine_yield(struct schedule *S) {
    int id = S->running;
    assert(id >= 0);
    struct coroutine *C = S->co[id];
    assert((char *) &C > S->stack); // why? 因为分配schedule结构体S时，后声明的成员变量co的地址肯定要高于先声明的成员变量stack的地址，以满足偏移量关系
    _save_stack(C, S->stack + STACK_SIZE);
    C->status = COROUTINE_SUSPEND;
    S->running = -1;
    swapcontext(&C->ctx, &S->main); // 返回coroutine_resume函数尾（然后return）
}

int coroutine_status(struct schedule *S, int id) {
    assert(id >= 0 && id < S->cap);
    if (S->co[id] == NULL) {
        return COROUTINE_DEAD;
    }
    return S->co[id]->status;
}

int coroutine_running(struct schedule *S) {
    return S->running;
}


#pragma clang diagnostic pop