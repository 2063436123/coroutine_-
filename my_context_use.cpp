//
// Created by Hello Peter on 2021/11/28.
//
#include <iostream>
#include <ucontext.h>

using namespace std;

ucontext_t ctx[3];

using ucfunc_t = void(*)(void);

static void f1(int p) {
    printf("start f1 of %d\n", p);
    swapcontext(&ctx[1], &ctx[2]);
    puts("finish f1");
}

static void f2(int p) {
    printf("start f2 of %d\n", p);
    swapcontext(&ctx[2], &ctx[1]);
    puts("finish f2");
}

int main() {
    cout << "main begin" << endl;

    char stk1[8192];
    char stk2[8192];

    getcontext(&ctx[1]);
    ctx[1].uc_link = &ctx[0];
    ctx[1].uc_stack.ss_sp = stk1;
    ctx[1].uc_stack.ss_size = sizeof stk1;
    makecontext(&ctx[1], (ucfunc_t)f1, 1, 1);

    getcontext(&ctx[2]);
    ctx[2].uc_link = &ctx[1];
    ctx[2].uc_stack.ss_sp = stk2;
    ctx[2].uc_stack.ss_size = sizeof stk2;
    makecontext(&ctx[2], (ucfunc_t)f2, 1, 2);

    // 执行流：f2.1 -> f1.1 -> f2.2 -> f1.2 -> main
    swapcontext(&ctx[0], &ctx[2]);

    cout << "main end" << endl;
}
