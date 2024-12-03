#include "co.h"
#include <setjmp.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#ifdef LOCAL_MACHINE
  #define debug(...) printf(__VA_ARGS__)
#else
  #define debug(...)
#endif
#define STACK_SIZE 1024*64
#define alignment16(a) ((a) & (~(16 - 1)))

enum co_status {
  CO_NEW = 1, // 新创建，还未执行过
  CO_RUNNING, // 已经执行过
  CO_WAITING, // 在 co_wait 上等待
  CO_DEAD,    // 已经结束，但还未释放资源
};

struct co {
  char *name;
  void (*func)(void *); // co_start 指定的入口地址和参数
  void *arg;

  enum co_status status;  // 协程的状态
  struct co *    waiter;  // 是否有其他协程在等待当前协程
  jmp_buf        context; // 寄存器现场 (setjmp.h)
  uint8_t stack[STACK_SIZE];
};
struct co *current;
typedef struct co_list {
  struct co *co;
  struct co_list *prev, *next;
} co_node;
static struct co_list *co_head = NULL;

co_node* co_remove();
static inline void stack_switch_call(void *sp, void *entry, uintptr_t arg); 

void insert(struct co *co) {
  struct co_list *node = (struct co_list*) malloc(sizeof (struct co_list));
  node->co = co;
  if (co_head == NULL) {
    co_head = node;
    node->prev = node->next = node;
  } else {
    node->next = co_head;
    node->prev = co_head->prev;
    co_head->prev->next = node;
    co_head->prev = node;
  }
}
struct co *co_start(const char *name, void (*func)(void *), void *arg) {
    struct co *_co = (struct co*) malloc(sizeof (struct co));
    *_co = (struct co) {
        .name = strdup(name),
        .func = func,
        .arg = arg,
        .status = CO_NEW,
        .waiter = NULL,
    };
    insert(_co);
    return _co;
}

void co_wait(struct co *co) {
  if (co->status != CO_DEAD) {
    current->status = CO_WAITING;
    co->waiter = current;
    co_yield();
  }
  else {
    while (co_head->co != co) {
      co_head = co_head->next;
    }

    free(co);
    free(co_remove());
  }

}
co_node * co_remove() {
  co_node *node = co_head;
  if (node->next == node) {
    co_head = NULL;
  } else {
    co_head->prev->next = co_head->next;
    co_head->next->prev = co_head->prev;
    co_head = co_head->next;
  }
  return node;
}
void coroutine_wrapper(struct co *co) {
  co->func(co->arg);
  co->status = CO_DEAD;
  if (co->waiter != NULL) {
    co->waiter->status = CO_RUNNING;
  }
  co_yield();
}

void co_yield() {
  int ret = setjmp(current->context);
  if (ret == 0) {
    co_head = co_head->next;
    while (!(co_head->co->status == CO_NEW || co_head->co->status == CO_RUNNING)) {
      co_head = co_head->next;
    }
    current = co_head->co;
    if (co_head->co->status == CO_NEW) {
      co_head->co->status = CO_RUNNING;
      current = co_head->co;
      stack_switch_call(co_head->co->stack + STACK_SIZE, coroutine_wrapper, (uintptr_t)current);
    } else {
      longjmp(co_head->co->context, 1);
    }
  }

}
static inline void
stack_switch_call(void *sp, void *entry, uintptr_t arg) {
    asm volatile (
#if __x86_64__
        "movq %0, %%rsp; movq %2, %%rdi; jmp *%1"
          :
          : "b"((uintptr_t)sp - 8),
            "d"(entry),
            "a"(arg)
          : "memory"
#else
        "movl %0, %%esp; movl %2, 4(%0); jmp *%1"
          :
          : "b"((uintptr_t)sp - 8),
            "d"(entry),
            "a"(arg)
          : "memory"
#endif
    );
}

static __attribute__((constructor)) void init() {
  debug("init\n");
  current = co_start("main", NULL, NULL);
  current->status = CO_RUNNING;

}
static __attribute__((destructor)) void fini() {
  debug("fini\n");
  while (co_head != NULL) {
    free(co_head->co);
    free(co_remove());
  }
}