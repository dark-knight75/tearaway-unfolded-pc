/* Event flag contracts. Includes runtime_sema.c to reach its static functions: no runtime link. */
#include "../src/runtime_sema.c"
#include <assert.h>

static uint64_t flag;
static void *set_later(void *bits) {
    const struct timespec nap={0,20000000};
    nanosleep(&nap,NULL);
    assert(evf_set(flag,(uint64_t)(uintptr_t)bits)==0);
    return NULL;
}
typedef struct { uint64_t bits,pattern; uint32_t mode,timeout; int32_t result; } Task;
static void *waiter(void *p) { Task *t=p; t->result=evf_wait(flag,t->bits,t->mode,&t->pattern,&t->timeout); return NULL; }

int main(void) {
    uint64_t id,pattern=0;
    uint32_t timeout;
    assert((uint32_t)evf_create(&id,"bad",0x33,0,NULL)==0x80020016);
    assert(evf_create(&id,"JobSync",0x21,0,NULL)==0);

    /* Poll: AND needs every bit, OR any; CLEAR_PAT clears only the waited bits. */
    assert(evf_set(id,0x5)==0);
    assert((uint32_t)evf_poll(id,0x3,EVF_AND,&pattern)==0x80020010);
    assert(evf_poll(id,0x3,EVF_OR|EVF_CLEAR_PAT,&pattern)==0 && pattern==0x5);
    assert(evf_poll(id,0x4,EVF_AND,&pattern)==0 && pattern==0x4);
    assert(evf_poll(id,0x4,EVF_AND|EVF_CLEAR_ALL,&pattern)==0);
    assert((uint32_t)evf_poll(id,0x4,EVF_OR,&pattern)==0x80020010);
    assert((uint32_t)evf_poll(id,0,EVF_OR,&pattern)==0x80020016);
    assert((uint32_t)evf_poll(id,1,EVF_AND|EVF_OR,&pattern)==0x80020016);
    assert(evf_set(id,0xff)==0 && evf_clear(id,0x0f)==0 && evf_poll(id,0xff,EVF_OR,&pattern)==0 && pattern==0x0f);
    assert(evf_clear(id,0)==0);

    /* Timed wait: times out with the remaining time zeroed; a later set wakes a blocked waiter. */
    timeout=1000;
    assert((uint32_t)evf_wait(id,1,EVF_AND,&pattern,&timeout)==0x8002003c && timeout==0);
    flag=id;
    pthread_t t;
    assert(pthread_create(&t,NULL,set_later,(void *)(uintptr_t)0x1000000)==0);
    timeout=5000000;
    assert(evf_wait(id,0x1000000,EVF_AND|EVF_CLEAR_PAT,&pattern,&timeout)==0 && pattern==0x1000000);
    assert(timeout>0 && timeout<5000000);
    assert(pthread_join(t,NULL)==0);

    /* Delete wakes blocked waiters with EACCES; the id is then gone. */
    Task a={.bits=1,.mode=EVF_AND,.timeout=5000000};
    assert(pthread_create(&t,NULL,waiter,&a)==0);
    for (int i=0;i<2000 && !find_flag(id)->first;++i) { const struct timespec nap={0,1000000}; nanosleep(&nap,NULL); }
    assert(evf_delete(id)==0);
    assert(pthread_join(t,NULL)==0 && (uint32_t)a.result==0x8002000d);
    assert((uint32_t)evf_set(id,1)==0x80020003);
    puts("PASS: event flag contracts");
    return 0;
}
