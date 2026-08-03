#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "network_shim.h"
#include "platform_shim.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <dirent.h>
#endif
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"network check failed at %d: %s\n",__LINE__,#x); abort(); } } while(0)
/* Test adapter for the emitted acknowledgement ABI. Generated-program tests
   separately exercise the actual native runtime, including waker delivery. */
typedef struct { atomic_int ready,refs; } Ack;
static atomic_int acknowledgements;
void *__dmm_async_io_create(void *data,void (*drop)(void *)) {
    CHECK(!data&&!drop); Ack *a=calloc(1,sizeof(*a)); CHECK(a);
    atomic_init(&a->refs,2); atomic_fetch_add(&acknowledgements,1); return a;
}
static void ack_release(Ack *a) { if(atomic_fetch_sub(&a->refs,1)==1) { atomic_fetch_sub(&acknowledgements,1); free(a); } }
int __dmm_async_io_poll(void *io,void *context) { (void)context; return atomic_load(&((Ack *)io)->ready); }
void __dmm_async_io_confirm(void *io) { Ack *a=io; CHECK(!atomic_exchange(&a->ready,1)); ack_release(a); }
void __dmm_async_io_destroy(void *io) { CHECK(atomic_load(&((Ack *)io)->ready)); ack_release(io); }
int __dmm_async_cancel_requested(void *context) { (void)context; return 0; }
extern void __dmm_net_test_dns_hook(int (*)(const char *,const DmmNetAddress *,DmmNetAddress *,void *),void *);
extern void __dmm_net_test_close_error(int);
extern size_t __dmm_net_test_socket_handles(void);
extern void __dmm_net_test_reactor_hook(void (*)(void *),void *);
extern int __dmm_net_test_submitted(DmmNetOperation *);
extern int __dmm_net_test_send(DmmNetSocket *,const unsigned char *,int);
static atomic_int reactor_paused;
static void reactor_gate(void *event) { atomic_store(&reactor_paused,1); __dmm_async_wait(event); }
static void pause_ms(void) {
#ifdef _WIN32
    Sleep(1);
#else
    struct timespec t={0,1000000}; nanosleep(&t,NULL);
#endif
}
static void wait_op(DmmNetOperation *op,unsigned kind) {
    uint64_t end=__dmm_net_now()+5000000000ULL;
    while(!__dmm_net_poll(op,NULL)) { CHECK(__dmm_net_now()<end); pause_ms(); }
    if(__dmm_net_result(op,0)!=kind) fprintf(stderr,"result expected=%u got=%llu domain=%llu code=%lld\n",kind,(unsigned long long)__dmm_net_result(op,0),(unsigned long long)__dmm_net_result(op,1),(long long)__dmm_net_result(op,2));
    CHECK(__dmm_net_result(op,0)==kind);
}
static DmmNetOperation *start(unsigned command,DmmNetSocket *socket,void *buffer,size_t bytes,DmmNetAddress address,uint64_t deadline) {
    DmmNetRequest request={.command=command,.socket=socket,.buffer=buffer,.length=bytes,.deadline=deadline,.address=address};
    return __dmm_net_start(&request);
}
static DmmNetAddress loopback(unsigned family) {
    DmmNetAddress a={.family=family};
    if(family==4) { a.bytes[0]=127; a.bytes[3]=1; } else a.bytes[15]=1;
    return a;
}
static void socket_checks(unsigned family) {
    DmmNetError error; DmmNetAddress address=loopback(family),peer;
    DmmNetSocket *listener=__dmm_net_socket(family,1,&error); CHECK(listener&&!error.kind);
    CHECK(__dmm_net_bind(listener,&address,&error)); CHECK(__dmm_net_listen(listener,4,&error));
    CHECK(__dmm_net_address(listener,0,&address,&error)&&address.port!=0);
    DmmNetSocket *client=__dmm_net_socket(family,1,&error); CHECK(client);
    CHECK(!__dmm_net_address(client,1,&peer,&error)&&error.kind==DMM_NET_STATE);
    DmmNetOperation *accept=start(DMM_NET_ACCEPT,listener,NULL,0,address,DMM_NET_NEVER);
    DmmNetOperation *connect=start(DMM_NET_CONNECT,client,NULL,0,address,DMM_NET_NEVER);
    wait_op(connect,DMM_NET_OK); wait_op(accept,DMM_NET_OK);
    DmmNetSocket *server=(DmmNetSocket *)(uintptr_t)__dmm_net_result(accept,6); CHECK(server);
    __dmm_net_result_address(accept,&peer); CHECK(peer.family==family&&peer.port!=0);
    __dmm_net_release(connect); __dmm_net_release(accept);
    unsigned char input[8]={0},other[8]={0},output[3]={3,5,7};
    DmmNetOperation *read=start(DMM_NET_READ,client,input,sizeof(input),address,DMM_NET_NEVER);
    DmmNetOperation *busy=start(DMM_NET_READ,client,other,sizeof(other),address,DMM_NET_NEVER);
    wait_op(busy,DMM_NET_BUSY); __dmm_net_release(busy);
    DmmNetOperation *write=start(DMM_NET_WRITE,client,output,sizeof(output),address,DMM_NET_NEVER);
    DmmNetOperation *back=start(DMM_NET_WRITE,server,output,sizeof(output),address,DMM_NET_NEVER);
    wait_op(write,DMM_NET_OK); wait_op(back,DMM_NET_OK); wait_op(read,DMM_NET_OK);
    CHECK(__dmm_net_result(read,3)==3&&!memcmp(input,output,3));
    __dmm_net_cancel(read); wait_op(read,DMM_NET_OK); /* published result wins */
    __dmm_net_release(write); __dmm_net_release(back); __dmm_net_release(read);
    read=start(DMM_NET_READ,server,input,sizeof(input),address,DMM_NET_NEVER); wait_op(read,DMM_NET_OK); __dmm_net_release(read);
    read=start(DMM_NET_READ,server,input,sizeof(input),address,__dmm_net_now()+10000000ULL);
    wait_op(read,DMM_NET_TIMEOUT); __dmm_net_release(read);
    read=start(DMM_NET_READ,server,input,sizeof(input),address,DMM_NET_NEVER);
    /* Freeze completion consumption after confirmed OS submission. A cancel
       request alone must not publish acknowledgement or free caller storage. */
    uint64_t submit_limit=__dmm_net_now()+5000000000ULL;
    while(!__dmm_net_test_submitted(read)) { CHECK(__dmm_net_now()<submit_limit); pause_ms(); }
    void *reactor_event=__dmm_async_wait_create(); atomic_store(&reactor_paused,0);
    __dmm_net_test_reactor_hook(reactor_gate,reactor_event);
    while(!atomic_load(&reactor_paused)) { CHECK(__dmm_net_now()<submit_limit); pause_ms(); }
    /* IOCP may already hold a successful kernel completion when cancellation
       is requested. The operation remains alive until that packet is consumed. */
    CHECK(__dmm_net_test_send(client,output,3)==3);
    __dmm_net_cancel(read); CHECK(!__dmm_net_poll(read,NULL));
    __dmm_async_wake(reactor_event); wait_op(read,DMM_NET_CLOSED); __dmm_net_release(read);
    __dmm_async_wait_destroy(reactor_event);
    read=start(DMM_NET_READ,client,input,0,address,DMM_NET_NEVER); wait_op(read,DMM_NET_OK); CHECK(!__dmm_net_result(read,4)); __dmm_net_release(read);
    CHECK(__dmm_net_shutdown_socket(server,1,&error));
    read=start(DMM_NET_READ,client,input,sizeof(input),address,DMM_NET_NEVER); wait_op(read,DMM_NET_OK); CHECK(__dmm_net_result(read,4)); __dmm_net_release(read);
    CHECK(__dmm_net_close(server,&error)); CHECK(__dmm_net_close(client,&error)); CHECK(__dmm_net_close(listener,&error));
    DmmNetSocket *receiver=__dmm_net_socket(family,2,&error),*sender=__dmm_net_socket(family,2,&error); CHECK(receiver&&sender);
    CHECK(!__dmm_net_address(sender,0,&peer,&error)&&error.kind==DMM_NET_STATE);
    address=loopback(family); CHECK(__dmm_net_bind(receiver,&address,&error)); CHECK(__dmm_net_address(receiver,0,&address,&error));
    write=start(DMM_NET_SEND_TO,sender,output,sizeof(output),address,DMM_NET_NEVER); wait_op(write,DMM_NET_OK); __dmm_net_release(write);
    CHECK(__dmm_net_address(sender,0,&peer,&error)&&peer.port!=0);
    CHECK(!__dmm_net_bind(sender,&peer,&error)&&error.kind==DMM_NET_STATE);
    read=start(DMM_NET_RECV_FROM,receiver,input,1,address,DMM_NET_NEVER); wait_op(read,DMM_NET_OK);
    CHECK(__dmm_net_result(read,3)==1&&__dmm_net_result(read,5)); __dmm_net_release(read);
    write=start(DMM_NET_SEND_TO,sender,output,0,address,DMM_NET_NEVER); wait_op(write,DMM_NET_OK); __dmm_net_release(write);
    read=start(DMM_NET_RECV_FROM,receiver,input,sizeof(input),address,DMM_NET_NEVER); wait_op(read,DMM_NET_OK);
    CHECK(!__dmm_net_result(read,3)&&!__dmm_net_result(read,4)); __dmm_net_release(read);
    CHECK(__dmm_net_close(receiver,&error)); CHECK(__dmm_net_close(sender,&error));
    /* Expired Connect performs no OS I/O and invalidates the admitted owner. */
    client=__dmm_net_socket(family,1,&error); CHECK(client);
    connect=start(DMM_NET_CONNECT,client,NULL,0,address,0);
    wait_op(connect,DMM_NET_TIMEOUT); __dmm_net_release(connect);
    CHECK(!__dmm_net_address(client,0,&peer,&error)&&error.kind==DMM_NET_CLOSED);
    CHECK(__dmm_net_close(client,&error));
    client=__dmm_net_socket(family,1,&error); CHECK(client);
    DmmNetAddress wrong=loopback(family==4?6:4);
    connect=start(DMM_NET_CONNECT,client,NULL,0,wrong,DMM_NET_NEVER);
    wait_op(connect,DMM_NET_INPUT); __dmm_net_release(connect);
    CHECK(!__dmm_net_address(client,0,&peer,&error)&&error.kind==DMM_NET_CLOSED);
    CHECK(__dmm_net_close(client,&error));
    client=__dmm_net_socket(family,2,&error); CHECK(client);
    __dmm_net_test_close_error(10000);
    CHECK(!__dmm_net_close(client,&error)&&error.kind==DMM_NET_SYSTEM&&error.code==10000);
}
static atomic_int entered,finished;
static void *gate;
static _Thread_local int worker_tls;
static int resolver_hook(const char *hostname,const DmmNetAddress *request,DmmNetAddress *answer,void *argument) {
    (void)argument; CHECK(!strcmp(hostname,"127.0.0.1")&&request->port==80);
    CHECK(worker_tls==0);
    int identity=atomic_fetch_add(&entered,1)+1;
    worker_tls=17+identity; errno=37+identity;
    __dmm_async_wait(gate); CHECK(worker_tls==17+identity&&errno==37+identity);
    *answer=loopback(4); answer->port=80; return 0;
}
static int resolver_error(const char *hostname,const DmmNetAddress *request,DmmNetAddress *answer,void *argument) {
    (void)hostname; (void)request; (void)answer; (void)argument; return 123;
}
static DmmNetOperation *dns(uint64_t deadline) {
    DmmNetRequest request={.command=DMM_NET_DNS,.hostname="127.0.0.1",.address={.family=4,.port=80},.transport=1,.deadline=deadline};
    return __dmm_net_start(&request);
}
static void finish_thread(void *unused) { (void)unused; __dmm_net_finish(); atomic_store(&finished,1); }
static unsigned resource_count(void) {
#ifdef _WIN32
    DWORD count=0; CHECK(GetProcessHandleCount(GetCurrentProcess(),&count)); return (unsigned)count;
#else
    DIR *directory=opendir("/proc/self/fd"); CHECK(directory); unsigned count=0;
    while(readdir(directory)) count++;
    closedir(directory); return count;
#endif
}
int main(void) {
    unsigned before=resource_count();
    socket_checks(4); unsigned initialized_count=resource_count();
    socket_checks(6); CHECK(resource_count()==initialized_count);
    DmmNetOperation *op=dns(DMM_NET_NEVER); wait_op(op,DMM_NET_OK);
    DmmNetAddresses *list=(DmmNetAddresses *)(uintptr_t)__dmm_net_result(op,7); CHECK(__dmm_net_addresses_count(list)>0);
    DmmNetAddress address; CHECK(__dmm_net_addresses_get(list,0,&address)&&address.port==80); CHECK(!__dmm_net_addresses_get(list,999,&address));
    __dmm_net_addresses_release(list); __dmm_net_release(op);
    __dmm_net_test_dns_hook(resolver_error,NULL);
    op=dns(DMM_NET_NEVER); wait_op(op,DMM_NET_SYSTEM);
    CHECK(__dmm_net_result(op,1)==DMM_NET_RESOLVER&&__dmm_net_result(op,2)==123); __dmm_net_release(op);
    gate=__dmm_async_wait_create(); __dmm_net_test_dns_hook(resolver_hook,NULL);
    DmmNetOperation *running[2]={dns(DMM_NET_NEVER),dns(DMM_NET_NEVER)};
    uint64_t limit=__dmm_net_now()+5000000000ULL;
    while(atomic_load(&entered)!=2) { CHECK(__dmm_net_now()<limit); pause_ms(); }
    DmmNetOperation *queued[64]; for(unsigned i=0;i<64;i++) queued[i]=dns(DMM_NET_NEVER);
    op=dns(DMM_NET_NEVER); wait_op(op,DMM_NET_BUSY); __dmm_net_release(op);
    for(unsigned i=0;i<64;i++) { __dmm_net_cancel(queued[i]); wait_op(queued[i],DMM_NET_CLOSED); __dmm_net_release(queued[i]); }
    op=dns(__dmm_net_now()+10000000ULL); wait_op(op,DMM_NET_TIMEOUT); __dmm_net_release(op);
    for(unsigned i=0;i<2;i++) { __dmm_net_cancel(running[i]); wait_op(running[i],DMM_NET_CLOSED); __dmm_net_release(running[i]); }
    __dmm_net_begin_draining(); DmmNetError error;
    CHECK(!__dmm_net_socket(4,2,&error)&&error.kind==DMM_NET_CLOSED);
    op=dns(DMM_NET_NEVER); wait_op(op,DMM_NET_CLOSED); __dmm_net_release(op);
    void *finish=__dmm_async_thread_create(finish_thread,NULL); pause_ms(); CHECK(!atomic_load(&finished));
    __dmm_async_wake(gate); __dmm_async_thread_join(finish); CHECK(atomic_load(&finished));
    __dmm_async_wait_destroy(gate); CHECK(atomic_load(&acknowledgements)==0);
    CHECK(__dmm_net_test_socket_handles()==0);
#ifndef _WIN32
    CHECK(resource_count()==before);
#else
    /* Winsock retains process-wide provider caches; the repeated socket pass
       and exact shim-owned handle counter above exclude per-operation leaks. */
    (void)before;
#endif
    return 0;
}
