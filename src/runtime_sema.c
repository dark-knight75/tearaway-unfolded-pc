/* Kernel semaphores: 32-bit IDs, counted tokens and FIFO waiters.
 * Priority-ordered semaphores are approximated by FIFO order. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
typedef struct Waiter {
    int32_t need, result;
    int done;
    pthread_cond_t event;
    struct Waiter *next;
} Waiter;
typedef struct Sema {
    uint32_t id, attr;
    int32_t count, initial, maximum;
    unsigned active;
    int deleted;
    Waiter *first;
    struct Sema *next;
} Sema;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static Sema *registry;
static uint32_t next_id=1;
static size_t created, deleted, acquired, signaled, timed_out;
static Sema *find(uint32_t id) {
    for (Sema *s=registry;s;s=s->next) if (s->id==id) return s;
    return NULL;
}
static void host_check(int e) {
    if (e) { fprintf(stderr,"STOP: semaphore host error %d\n",e); exit(21); }
}
/* Waits use this clock for their deadlines. winpthreads condition variables accept only
 * CLOCK_REALTIME (pthread_condattr_setclock(CLOCK_MONOTONIC) fails with EINVAL). */
#ifdef _WIN32
#define WAIT_CLOCK CLOCK_REALTIME
#else
#define WAIT_CLOCK CLOCK_MONOTONIC
#endif
static uint64_t now_ns(void) {
    struct timespec t;
    if (clock_gettime(WAIT_CLOCK,&t)) { perror("clock_gettime"); exit(1); }
    return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;
}
static ABI int32_t sem_create(uint32_t *out,const char *name,uint32_t attr,
                              int32_t initial,int32_t maximum,const void *options) {
    if (!out || !name || attr>2 || initial<0 || maximum<=0 || initial>maximum) return ERR(22);
    if (options) { fputs("STOP: semaphore options are not implemented\n",stderr); exit(21); }
    Sema *s=calloc(1,sizeof(*s));
    if (!s) return ERR(12);
    pthread_mutex_lock(&lock);
    if (next_id==UINT32_MAX) { pthread_mutex_unlock(&lock); free(s); return ERR(28); }
    s->id=next_id++; s->attr=attr; s->count=s->initial=initial; s->maximum=maximum;
    s->next=registry; registry=s; *out=s->id; ++created;
    printf("Runtime: semaphore id=%u, attr=%u, initial=%d, max=%d\n",s->id,attr,initial,maximum);
    pthread_mutex_unlock(&lock);
    return 0;
}
static int32_t wait_count(uint32_t id,int32_t need,uint32_t *timeout,int block) {
    pthread_mutex_lock(&lock);
    Sema *s=find(id);
    int32_t result=0;
    if (!s) result=ERR(3);
    else if (need<=0 || need>s->maximum) result=ERR(22);
    else if (s->count>=need) { s->count-=need; ++acquired; }
    else if (!block) result=ERR(16);
    else if (timeout && !*timeout) { result=ERR(60); ++timed_out; }
    else {
        /* Priority-ordered semaphores (attr 2) are served FIFO: there is no
           PS4 priority scheduler, host threads run at equal priority. */
        if (getenv("BB_TRACE_SEMA")) fprintf(stderr,"Runtime: blocking wait on semaphore %u (need %d, count %d)\n",id,need,s->count); /* deadlock diagnosis */
        Waiter w={.need=need};
        pthread_condattr_t attr;
        host_check(pthread_condattr_init(&attr));
        host_check(pthread_condattr_setclock(&attr,WAIT_CLOCK));
        host_check(pthread_cond_init(&w.event,&attr));
        host_check(pthread_condattr_destroy(&attr));
        Waiter **tail=&s->first;
        while (*tail) tail=&(*tail)->next;
        *tail=&w; ++s->active;
        uint64_t deadline=timeout ? now_ns()+(uint64_t)*timeout*1000 : 0;
        struct timespec end={.tv_sec=(time_t)(deadline/1000000000),.tv_nsec=(long)(deadline%1000000000)};
        while (!w.done) {
            int e=timeout ? pthread_cond_timedwait(&w.event,&lock,&end) : pthread_cond_wait(&w.event,&lock);
            if (e==ETIMEDOUT && !w.done) {
                Waiter **p=&s->first;
                while (*p!=&w) p=&(*p)->next;
                *p=w.next; w.result=ERR(60); w.done=1; ++timed_out;
            } else if (e!=ETIMEDOUT) host_check(e);
        }
        result=w.result;
        if (!result) ++acquired;
        if (timeout) {
            uint64_t now=now_ns();
            *timeout=result ? 0 : (uint32_t)(now>=deadline ? 0 : (deadline-now)/1000);
        }
        host_check(pthread_cond_destroy(&w.event));
        if (!--s->active && s->deleted) free(s);
    }
    pthread_mutex_unlock(&lock);
    return result;
}
static ABI int32_t sem_wait(uint32_t id,int32_t need,uint32_t *timeout) { return wait_count(id,need,timeout,1); }
static ABI int32_t sem_poll(uint32_t id,int32_t need) { return wait_count(id,need,NULL,0); }
static ABI int32_t sem_signal(uint32_t id,int32_t count) {
    pthread_mutex_lock(&lock);
    Sema *s=find(id);
    int32_t result=0;
    if (!s) result=ERR(3);
    else if (count<=0 || count>s->maximum-s->count) result=ERR(22);
    else {
        s->count+=count; ++signaled;
        Waiter **p=&s->first;
        while (*p) {
            Waiter *w=*p;
            if (w->need>s->count) { p=&w->next; continue; }
            s->count-=w->need; *p=w->next; w->done=1;
            host_check(pthread_cond_signal(&w->event));
        }
    }
    pthread_mutex_unlock(&lock);
    return result;
}
static int32_t wake_all(Sema *s,int32_t result) {
    int32_t count=0;
    while (s->first) {
        Waiter *w=s->first; s->first=w->next;
        w->result=result; w->done=1; ++count;
        host_check(pthread_cond_signal(&w->event));
    }
    return count;
}
static ABI int32_t sem_cancel(uint32_t id,int32_t count,int32_t *waiters) {
    pthread_mutex_lock(&lock);
    Sema *s=find(id);
    int32_t result=0;
    if (!s) result=ERR(3);
    else if (count>s->maximum) result=ERR(22);
    else {
        int32_t n=wake_all(s,ERR(85));
        if (waiters) *waiters=n;
        s->count=count<0 ? s->initial : count;
    }
    pthread_mutex_unlock(&lock);
    return result;
}
static ABI int32_t sem_delete(uint32_t id) {
    pthread_mutex_lock(&lock);
    Sema **p=&registry;
    while (*p && (*p)->id!=id) p=&(*p)->next;
    if (!*p) { pthread_mutex_unlock(&lock); return ERR(3); }
    Sema *s=*p; *p=s->next; s->deleted=1; ++deleted;
    wake_all(s,ERR(13));
    if (!s->active) free(s);
    pthread_mutex_unlock(&lock);
    return 0;
}
/* Event flags: a 64-bit pattern; waiters wait for all (AND) or any (OR) of their bits and may
 * clear the whole pattern or their bits once satisfied. Same lock, clock and FIFO order as above. */
enum { EVF_SINGLE=0x10, EVF_AND=1, EVF_OR=2, EVF_CLEAR_ALL=0x10, EVF_CLEAR_PAT=0x20 };
typedef struct FlagWaiter {
    uint64_t bits, result_pattern;
    uint32_t mode;
    int32_t result;
    int done;
    pthread_cond_t event;
    struct FlagWaiter *next;
} FlagWaiter;
typedef struct Flag {
    uint64_t id, pattern, initial;
    uint32_t attr;
    unsigned active;
    int deleted;
    FlagWaiter *first;
    struct Flag *next;
} Flag;
static Flag *flags;
static uint64_t next_flag=1;
static size_t flags_created, flag_waits, flag_sets;
static Flag *find_flag(uint64_t id) {
    for (Flag *f=flags;f;f=f->next) if (f->id==id) return f;
    return NULL;
}
static int flag_valid_mode(uint32_t mode) {
    return ((mode&3)==EVF_AND || (mode&3)==EVF_OR) && !(mode&~0x33u) &&
           (mode&(EVF_CLEAR_ALL|EVF_CLEAR_PAT))!=(EVF_CLEAR_ALL|EVF_CLEAR_PAT);
}
/* Satisfied: report the pattern as it was, then apply the waiter's clear. */
static int flag_take(Flag *f,uint64_t bits,uint32_t mode,uint64_t *result_pattern) {
    if ((mode&3)==EVF_AND ? (f->pattern&bits)!=bits : !(f->pattern&bits)) return 0;
    if (result_pattern) *result_pattern=f->pattern;
    if (mode&EVF_CLEAR_ALL) f->pattern=0;
    else if (mode&EVF_CLEAR_PAT) f->pattern&=~bits;
    return 1;
}
static ABI int32_t evf_create(uint64_t *out,const char *name,uint32_t attr,uint64_t initial,const void *options) {
    if (!out || !name || (attr&~0x33u) || (attr&3)==3 || (attr&0x30)==0x30) return ERR(22);
    (void)options; /* only reserved fields */
    Flag *f=calloc(1,sizeof(*f));
    if (!f) return ERR(12);
    pthread_mutex_lock(&lock);
    f->id=next_flag++; f->attr=attr; f->pattern=f->initial=initial;
    f->next=flags; flags=f; *out=f->id; ++flags_created;
    pthread_mutex_unlock(&lock);
    return 0;
}
static int32_t flag_wait(uint64_t id,uint64_t bits,uint32_t mode,uint64_t *result_pattern,uint32_t *timeout,int block) {
    pthread_mutex_lock(&lock);
    Flag *f=find_flag(id);
    int32_t result=0;
    if (!f) result=ERR(3);
    else if (!bits || !flag_valid_mode(mode)) result=ERR(22);
    else if ((f->attr&EVF_SINGLE) && f->first) result=ERR(1);
    else if (flag_take(f,bits,mode,result_pattern)) ++flag_waits;
    else if (!block) result=ERR(16);
    else if (timeout && !*timeout) result=ERR(60);
    else {
        FlagWaiter w={.bits=bits,.mode=mode};
        pthread_condattr_t attr;
        host_check(pthread_condattr_init(&attr));
        host_check(pthread_condattr_setclock(&attr,WAIT_CLOCK));
        host_check(pthread_cond_init(&w.event,&attr));
        host_check(pthread_condattr_destroy(&attr));
        FlagWaiter **tail=&f->first;
        while (*tail) tail=&(*tail)->next;
        *tail=&w; ++f->active;
        uint64_t deadline=timeout ? now_ns()+(uint64_t)*timeout*1000 : 0;
        struct timespec end={.tv_sec=(time_t)(deadline/1000000000),.tv_nsec=(long)(deadline%1000000000)};
        while (!w.done) {
            int e=timeout ? pthread_cond_timedwait(&w.event,&lock,&end) : pthread_cond_wait(&w.event,&lock);
            if (e==ETIMEDOUT && !w.done) {
                FlagWaiter **p=&f->first;
                while (*p!=&w) p=&(*p)->next;
                *p=w.next; w.result=ERR(60); w.result_pattern=f->pattern; w.done=1;
            } else if (e!=ETIMEDOUT) host_check(e);
        }
        result=w.result;
        if (result_pattern) *result_pattern=w.result_pattern;
        if (!result) ++flag_waits;
        if (timeout) {
            uint64_t now=now_ns();
            *timeout=result ? 0 : (uint32_t)(now>=deadline ? 0 : (deadline-now)/1000);
        }
        host_check(pthread_cond_destroy(&w.event));
        if (!--f->active && f->deleted) free(f);
    }
    pthread_mutex_unlock(&lock);
    return result;
}
static ABI int32_t evf_wait(uint64_t id,uint64_t bits,uint32_t mode,uint64_t *result_pattern,uint32_t *timeout) {
    return flag_wait(id,bits,mode,result_pattern,timeout,1);
}
static ABI int32_t evf_poll(uint64_t id,uint64_t bits,uint32_t mode,uint64_t *result_pattern) {
    return flag_wait(id,bits,mode,result_pattern,NULL,0);
}
static ABI int32_t evf_set(uint64_t id,uint64_t bits) {
    pthread_mutex_lock(&lock);
    Flag *f=find_flag(id);
    if (!f) { pthread_mutex_unlock(&lock); return ERR(3); }
    f->pattern|=bits; ++flag_sets;
    FlagWaiter **p=&f->first;
    while (*p) {
        FlagWaiter *w=*p;
        if (!flag_take(f,w->bits,w->mode,&w->result_pattern)) { p=&w->next; continue; }
        *p=w->next; w->done=1;
        host_check(pthread_cond_signal(&w->event));
    }
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t evf_clear(uint64_t id,uint64_t bits) {
    pthread_mutex_lock(&lock);
    Flag *f=find_flag(id);
    if (f) f->pattern&=bits;
    pthread_mutex_unlock(&lock);
    return f ? 0 : ERR(3);
}
static int32_t flag_wake_all(Flag *f,int32_t result) {
    int32_t count=0;
    while (f->first) {
        FlagWaiter *w=f->first; f->first=w->next;
        w->result=result; w->result_pattern=f->pattern; w->done=1; ++count;
        host_check(pthread_cond_signal(&w->event));
    }
    return count;
}
static ABI int32_t evf_cancel(uint64_t id,uint64_t pattern,int32_t *waiters) {
    pthread_mutex_lock(&lock);
    Flag *f=find_flag(id);
    if (f) {
        f->pattern=pattern;
        int32_t n=flag_wake_all(f,ERR(85));
        if (waiters) *waiters=n;
    }
    pthread_mutex_unlock(&lock);
    return f ? 0 : ERR(3);
}
static ABI int32_t evf_delete(uint64_t id) {
    pthread_mutex_lock(&lock);
    Flag **p=&flags;
    while (*p && (*p)->id!=id) p=&(*p)->next;
    if (!*p) { pthread_mutex_unlock(&lock); return ERR(3); }
    Flag *f=*p; *p=f->next; f->deleted=1;
    flag_wake_all(f,ERR(13));
    if (!f->active) free(f);
    pthread_mutex_unlock(&lock);
    return 0;
}
/* Read-only test/diagnostic snapshot, not exposed to guest code. */
unsigned runtime_sema_waiters(uint32_t id) {
    pthread_mutex_lock(&lock);
    Sema *s=find(id); unsigned n=0;
    if (s) for (Waiter *w=s->first;w;w=w->next) ++n;
    pthread_mutex_unlock(&lock);
    return n;
}
uintptr_t runtime_sema_resolve(const char *name) {
    if (!strcmp(name,"188x57JYp0g#p#J")) return (uintptr_t)sem_create;
    if (!strcmp(name,"Zxa0VhQVTsk#p#J")) return (uintptr_t)sem_wait;
    if (!strcmp(name,"4czppHBiriw#p#J")) return (uintptr_t)sem_signal;
    if (!strcmp(name,"12wOHk8ywb0#p#J")) return (uintptr_t)sem_poll;
    if (!strcmp(name,"4DM06U2BNEY#p#J")) return (uintptr_t)sem_cancel;
    if (!strcmp(name,"R1Jvn8bSCW8#p#J")) return (uintptr_t)sem_delete;
    if (!strcmp(name,"BpFoboUJoZU#p#J")) return (uintptr_t)evf_create;
    if (!strcmp(name,"8mql9OcQnd4#p#J")) return (uintptr_t)evf_delete;
    if (!strcmp(name,"IOnSvHzqu6A#p#J")) return (uintptr_t)evf_set;
    if (!strcmp(name,"7uhBFWRAS60#p#J")) return (uintptr_t)evf_clear;
    if (!strcmp(name,"JTvBflhYazQ#p#J")) return (uintptr_t)evf_wait;
    if (!strcmp(name,"9lvj5DjHZiA#p#J")) return (uintptr_t)evf_poll;
    if (!strcmp(name,"PZku4ZrXJqg#p#J")) return (uintptr_t)evf_cancel;
    return 0;
}
void runtime_sema_report(void) {
    pthread_mutex_lock(&lock);
    printf("Runtime: semaphores created=%zu, deleted=%zu, acquired=%zu, signals=%zu, timeouts=%zu\n",
           created,deleted,acquired,signaled,timed_out);
    printf("Runtime: event flags created=%zu, waits satisfied=%zu, sets=%zu\n",flags_created,flag_waits,flag_sets);
    pthread_mutex_unlock(&lock);
}

