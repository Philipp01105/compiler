/* Test-only ABI client. This is deliberately not a public executor API. */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct Frame Frame;
struct Frame {
    uint64_t (*poll)(Frame *, void *);
    void (*cleanup)(Frame *, uint64_t);
    int64_t state;
    uint64_t (*cancel_poll)(Frame *, void *);
    void (*result_drop)(void *);
    void *context;
    uint64_t result_present, cancelling, auxiliary, size;
    uint64_t result[2];
};
static size_t created, destroyed, pending, child_destroyed;
#ifndef DRIVER_NATIVE
void *__dmm_core_calloc(size_t n, size_t size) {
    created++;
    return calloc(n, size);
}
void __dmm_core_free(void *frame) { destroyed++; free(frame); }
uint64_t __dmm_async_cancel_requested(void *context) { (void)context; return 0; }
void __dmm_async_frame_destroy(Frame *f,uint64_t discard) {
    if(discard && f->result_present && f->result_drop) f->result_drop(f->result);
    __dmm_core_free(f);
}
uint64_t __dmm_async_discard_poll(Frame *f,void *context) {
    uint64_t (*cancel_output)(Frame *,void *)=NULL;
    memcpy(&cancel_output,&f->size,sizeof(cancel_output));
    f->auxiliary=(uintptr_t)f->result;
    return cancel_output ? cancel_output(f,context):1;
}
#endif
extern Frame *immediate(int);
extern Frame *cleanupAwait(Frame *,Frame *);
extern Frame *ownedResult(void);
extern Frame *pendingResult(Frame *);
extern Frame *enumCleanup(Frame *,Frame *);
extern Frame *floats(double);
extern Frame *array(void);
extern Frame *arrayUse(void);
extern Frame *arrayParam(int32_t *);
extern Frame *errorUse(Frame *, int);
extern Frame *loopCleanup(Frame *);
extern Frame *structParam(int32_t *, Frame *);
extern Frame *many(int,double,int,double,int,double,int,int);
extern Frame *indirect(void);
extern Frame *futureChain(void);
extern Frame *nested(Frame *, Frame *);
extern Frame *relay(Frame *, Frame *);
extern Frame *loop(Frame *);
extern Frame *branch(Frame *, int);
extern int cleanups(void);

static uint64_t child_poll(Frame *f,void *context) {
    (void)context;
    assert(f->state >= 0);
    if (f->state) { f->state--; pending++; return 0; }
    f->state=-1;
    return 1;
}
static void child_cleanup(Frame *f,uint64_t discard) {
    (void)discard;
    assert(f->state == -1 || f->state == -2);
    child_destroyed++;
    free(f);
}
static uint64_t child_cancel(Frame *f,void *context) {
    (void)context;
    assert(f->state>=0);
    if(f->state) { f->state--; return 0; }
    f->state=-2; return 1;
}
static Frame *child(unsigned delay, int result) {
    Frame *f=calloc(1, sizeof(*f));
    assert(f);
    f->poll=child_poll;
    f->cancel_poll=child_cancel;
    f->cleanup=child_cleanup;
    f->state=delay;
    f->result[0]=(uint64_t) result;
    return f;
}
static uint64_t run(Frame *f, unsigned expected_pending) {
    unsigned polls=0;
    while (!f->poll(f,NULL)) { assert(++polls <= expected_pending); }
    assert(polls == expected_pending && f->state == -1);
    uint64_t result=f->result[0];
    f->cleanup(f,0);
    assert(created == destroyed);
    return result;
}
int main(int argc, char **argv) {
    if (argc > 1) {
        Frame *invalid=immediate(1);
        if (!strcmp(argv[1], "cleanup_pending")) invalid->cleanup(invalid,0);
        else { assert(invalid->poll(invalid,NULL) == 1); invalid->poll(invalid,NULL); }
        return 99; /* invalid transitions must trap before returning */
    }
    Frame *f=immediate(41);
    assert(f->state == 0);
#ifndef DRIVER_NATIVE
    assert(created == 1 && destroyed == 0);
#endif
    assert(run(f,0) == 42);
    uint64_t bits=run(floats(1.25),0);
    double real;
    memcpy(&real,&bits,sizeof(real));
    assert(real == 1.75);
    f=array();
    assert(f->poll(f,NULL) == 1);
    int32_t values[2];
    memcpy(values, f->result, sizeof(values));
    assert(values[0] == 7 && values[1] == 8);
    f->cleanup(f,0);
    assert(run(arrayUse(),0) == 15);
    assert(cleanups() == 0);
    f=nested(child(2,10),child(3,20));
    assert(cleanups() == 0); /* construction is lazy */
    assert(run(f,5) == 56);
    assert(cleanups() == 11 && child_destroyed == 2 && pending == 5);
    assert(run(relay(child(2,3),child(1,4)),3) == 33);
    assert(cleanups() == 22 && child_destroyed == 4);
    assert(run(loop(child(3,4)),3) == 10);
    assert(run(branch(child(2,7),1),2) == 7);
    assert(run(branch(child(1,7),0),1) == 8);
    assert(cleanups() == 24 && created == destroyed && child_destroyed == 7);
    int32_t input[2]={5,6};
    f=arrayParam(input);
    input[0]=100; /* by-value capture must already be copied into the frame */
    assert(run(f,0) == 11);
    assert(run(errorUse(child(2,7),1),2) == 23);
    assert(run(errorUse(child(3,7),0),3) == 8);
    assert(cleanups() == 226 && child_destroyed == 9);
    assert(run(loopCleanup(child(1,10)),1) == 3);
    assert(cleanups() == 229);
    int32_t guard=12;
    f=structParam(&guard,child(2,10));
    guard=100;
    assert(run(f,2) == 29);
    assert(cleanups() == 230 && child_destroyed == 11);
    bits=run(many(1,2.5,3,4.5,5,6.5,7,8),0);
    memcpy(&real,&bits,sizeof(real));
    assert(real == 37.5);
    assert(run(indirect(),0) == 52);
    assert(run(futureChain(),0) == 5);
    int before=cleanups();
    f=nested(child(2,10),child(3,20));
    unsigned cancelled_polls=0;
    while(!f->cancel_poll(f,NULL)) assert(++cancelled_polls<=5);
    assert(cancelled_polls==5 && f->state==-2 && cleanups()==before);
    f->cleanup(f,1);
    f=nested(child(2,10),child(3,20));
    assert(!f->poll(f,NULL));
    cancelled_polls=0;
    while(!f->cancel_poll(f,NULL)) assert(++cancelled_polls<=4);
    assert(cancelled_polls==4 && f->state==-2 && cleanups()==before+11);
    f->cleanup(f,1);
    before=cleanups();
    f=cleanupAwait(child(2,1),child(3,2));
    assert(!f->poll(f,NULL));
    cancelled_polls=0;
    while(!f->cancel_poll(f,NULL)) assert(++cancelled_polls<=4);
    assert(cancelled_polls==4 && cleanups()==before+11 && f->state==-2);
    f->cleanup(f,1);
    before=cleanups(); f=ownedResult();
    assert(f->poll(f,NULL) && cleanups()==before);
    f->cleanup(f,1);
    assert(cleanups()==before+1 && created==destroyed);
    f=pendingResult(child(3,10));
    assert(f->poll(f,NULL));
    cancelled_polls=0;
    while(!f->cancel_poll(f,NULL)) assert(++cancelled_polls<=3);
    assert(cancelled_polls==3);
    f->cleanup(f,1);
    assert(created==destroyed);
    f=enumCleanup(child(3,10),child(2,20));
    assert(!f->poll(f,NULL));
    cancelled_polls=0;
    while(!f->cancel_poll(f,NULL)) assert(++cancelled_polls<=4);
    assert(cancelled_polls==4 && f->state==-2);
    f->cleanup(f,1);
    assert(created==destroyed);
    return 0;
}
