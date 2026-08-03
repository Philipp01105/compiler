#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif
#include "network_shim.h"
#include "platform_shim.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <assert.h>
#include <stdio.h>
#ifdef DMM_NET_TESTING
static size_t test_socket_handles;
#endif
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
typedef SOCKET NetFd;
#define NET_INVALID INVALID_SOCKET
static SRWLOCK net_mutex=SRWLOCK_INIT;
static CONDITION_VARIABLE dns_condition=CONDITION_VARIABLE_INIT;
static HANDLE completion_port;
static void lock_net(void) { AcquireSRWLockExclusive(&net_mutex); }
static void unlock_net(void) { ReleaseSRWLockExclusive(&net_mutex); }
static int os_error(void) { return WSAGetLastError(); }
static int close_fd(NetFd fd) {
    int result=closesocket(fd);
#ifdef DMM_NET_TESTING
    if(!result) { assert(test_socket_handles); test_socket_handles--; }
#endif
    return result;
}
static void wake_reactor(void) { if(completion_port) (void)PostQueuedCompletionStatus(completion_port,0,0,NULL); }
static void wake_dns(void) { WakeAllConditionVariable(&dns_condition); }
#else
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <arpa/inet.h>
typedef int NetFd;
#define NET_INVALID (-1)
static pthread_mutex_t net_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dns_condition=PTHREAD_COND_INITIALIZER;
static int epoll_fd=-1, command_fd=-1;
static void lock_net(void) { if(pthread_mutex_lock(&net_mutex)) abort(); }
static void unlock_net(void) { if(pthread_mutex_unlock(&net_mutex)) abort(); }
static int os_error(void) { return errno; }
static int close_fd(NetFd fd) {
    int result=close(fd);
#ifdef DMM_NET_TESTING
    /* Linux close relinquishes the descriptor even on a late close error. */
    assert(test_socket_handles); test_socket_handles--;
#endif
    return result;
}
static void wake_reactor(void) { uint64_t one=1; if(command_fd>=0) { ssize_t n=write(command_fd,&one,8); (void)n; } }
static void wake_dns(void) { (void)pthread_cond_broadcast(&dns_condition); }
#endif

extern void *__dmm_async_io_create(void *,void (*)(void *));
extern int __dmm_async_io_poll(void *,void *);
extern void __dmm_async_io_confirm(void *);
extern void __dmm_async_io_destroy(void *);
extern int __dmm_async_cancel_requested(void *);
struct DmmNetSocket { NetFd fd; unsigned family,transport,state; DmmNetOperation *read,*write; };
struct DmmNetAddresses { size_t count; DmmNetAddress values[]; };
struct DmmNetOperation {
#ifdef _WIN32
    OVERLAPPED overlapped;
    WSABUF wsabuf;
    DWORD flags,transferred;
    struct sockaddr_storage sockaddr;
    int sockaddr_length;
    unsigned char accept_buffer[2*(sizeof(struct sockaddr_storage)+16)];
    NetFd accepted_fd;
    int submitted;
#endif
    DmmNetRequest request;
    DmmNetError error;
    DmmNetAddress peer;
    DmmNetSocket *accepted;
    DmmNetAddresses *addresses;
    void *io;
    size_t refs;
    size_t heap_index;
    uint64_t count;
    int done,cancel,timeout,eof,truncated,started,notified;
    int dns_active,dns_queued;
    char *hostname;
    DmmNetOperation *next,*dns_next,*notify_next;
};
static DmmNetOperation *operations,*notifications,*dns_head,*dns_tail;
static size_t dns_count;
static DmmNetOperation **deadline_heap;
static size_t heap_count,heap_capacity;
static unsigned runtime_state; /* 0 RUNNING (also uninitialized),1 DRAINING,2 SHUTDOWN */
static int initialized,stop_reactor,stop_dns;
#ifdef DMM_NET_TESTING
static int (*dns_test_hook)(const char *,const DmmNetAddress *,DmmNetAddress *,void *);
static void *dns_test_argument;
static int close_test_error;
static void (*reactor_test_hook)(void *);
static void *reactor_test_argument;
void __dmm_net_test_reactor_hook(void (*hook)(void *),void *argument) {
    lock_net(); reactor_test_hook=hook; reactor_test_argument=argument; wake_reactor(); unlock_net();
}
int __dmm_net_test_submitted(DmmNetOperation *op) {
    lock_net();
#ifdef _WIN32
    int submitted=op->submitted;
#else
    int submitted=op->started;
#endif
    unlock_net(); return submitted;
}
int __dmm_net_test_send(DmmNetSocket *socket,const unsigned char *bytes,int length) {
    lock_net();
#ifdef _WIN32
    int count=send(socket->fd,(const char *)bytes,length,0);
#else
    int count=(int)send(socket->fd,bytes,(size_t)length,MSG_NOSIGNAL);
#endif
    unlock_net(); return count;
}
void __dmm_net_test_close_error(int code) { lock_net(); close_test_error=code; unlock_net(); }
size_t __dmm_net_test_socket_handles(void) { lock_net(); size_t count=test_socket_handles; unlock_net(); return count; }
void __dmm_net_test_dns_hook(int (*hook)(const char *,const DmmNetAddress *,DmmNetAddress *,void *),void *argument) {
    lock_net(); dns_test_hook=hook; dns_test_argument=argument; unlock_net();
}
#endif
static void *reactor_thread,*dns_threads[2];
#ifdef _WIN32
static int winsock_started;
#endif

static DmmNetError net_error(unsigned kind) { return (DmmNetError){kind,DMM_NET_RUNTIME,0}; }
uint64_t __dmm_net_pointer(void *pointer) { return (uint64_t)(uintptr_t)pointer; }
#ifdef _WIN32
static DmmNetError win32_error(int code) {
    unsigned kind=DMM_NET_SYSTEM;
    switch(code) {
        case ERROR_CONNECTION_REFUSED: kind=DMM_NET_REFUSED; break;
        case ERROR_NETNAME_DELETED: case ERROR_CONNECTION_ABORTED: kind=DMM_NET_RESET; break;
        case ERROR_NETWORK_UNREACHABLE: case ERROR_HOST_UNREACHABLE: kind=DMM_NET_UNREACHABLE; break;
        case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: kind=DMM_NET_MEMORY; break;
        case ERROR_INVALID_PARAMETER: kind=DMM_NET_INPUT; break;
        case ERROR_SEM_TIMEOUT: case WAIT_TIMEOUT: kind=DMM_NET_TIMEOUT; break;
    }
    return (DmmNetError){kind,DMM_NET_WIN32,code};
}
#endif
static DmmNetError native_error(int code) {
    unsigned kind=DMM_NET_SYSTEM;
#ifdef _WIN32
    switch(code) {
        case WSAECONNREFUSED: kind=DMM_NET_REFUSED; break;
        case WSAECONNRESET: case WSAECONNABORTED: kind=DMM_NET_RESET; break;
        case WSAENETUNREACH: case WSAEHOSTUNREACH: kind=DMM_NET_UNREACHABLE; break;
        case WSAEADDRINUSE: kind=DMM_NET_ADDRESS_IN_USE; break;
        case WSAENOBUFS: case WSA_NOT_ENOUGH_MEMORY: kind=DMM_NET_MEMORY; break;
        case WSAEINVAL: case WSAEAFNOSUPPORT: kind=DMM_NET_INPUT; break;
        case WSAETIMEDOUT: kind=DMM_NET_TIMEOUT; break;
    }
    return (DmmNetError){kind,DMM_NET_WINSOCK,code};
#else
    switch(code) {
        case ECONNREFUSED: kind=DMM_NET_REFUSED; break;
        case ECONNRESET: case ECONNABORTED: case EPIPE: kind=DMM_NET_RESET; break;
        case ENETUNREACH: case EHOSTUNREACH: kind=DMM_NET_UNREACHABLE; break;
        case EADDRINUSE: kind=DMM_NET_ADDRESS_IN_USE; break;
        case ENOMEM: case ENOBUFS: kind=DMM_NET_MEMORY; break;
        case EINVAL: case EAFNOSUPPORT: kind=DMM_NET_INPUT; break;
        case ETIMEDOUT: kind=DMM_NET_TIMEOUT; break;
    }
    return (DmmNetError){kind,DMM_NET_POSIX,code};
#endif
}
uint64_t __dmm_net_now(void) {
#ifdef _WIN32
    LARGE_INTEGER value,frequency;
    if(!QueryPerformanceCounter(&value)||!QueryPerformanceFrequency(&frequency)||frequency.QuadPart<=0||value.QuadPart<0) abort();
    uint64_t ticks=(uint64_t)value.QuadPart,hz=(uint64_t)frequency.QuadPart;
    uint64_t seconds=ticks/hz,remainder=ticks%hz;
    if(seconds>UINT64_MAX/1000000000ULL) return UINT64_MAX-1;
    /* QPC frequencies fit the documented platform counter range. Splitting
       avoids multiplication overflow even for unusually large frequencies. */
    uint64_t fraction=(uint64_t)((long double)remainder*1000000000.0L/(long double)hz);
    uint64_t whole=seconds*1000000000ULL;
    return fraction>=UINT64_MAX-whole?UINT64_MAX-1:whole+fraction;
#else
    struct timespec value;
    if(clock_gettime(CLOCK_MONOTONIC,&value)) abort();
    uint64_t seconds=(uint64_t)value.tv_sec,fraction=(uint64_t)value.tv_nsec;
    if(seconds>UINT64_MAX/1000000000ULL) return UINT64_MAX-1;
    uint64_t whole=seconds*1000000000ULL;
    return fraction>=UINT64_MAX-whole?UINT64_MAX-1:whole+fraction;
#endif
}
static int to_native(const DmmNetAddress *in,struct sockaddr_storage *out,int *length) {
    memset(out,0,sizeof(*out));
    if(in->port>65535||in->scope>UINT32_MAX) return 0;
    if(in->family==4) {
        struct sockaddr_in *a=(struct sockaddr_in *)out;
        a->sin_family=AF_INET; a->sin_port=htons((uint16_t)in->port); memcpy(&a->sin_addr,in->bytes,4);
        *length=(int)sizeof(*a); return 1;
    }
    if(in->family==6) {
        struct sockaddr_in6 *a=(struct sockaddr_in6 *)out;
        a->sin6_family=AF_INET6; a->sin6_port=htons((uint16_t)in->port); a->sin6_scope_id=(uint32_t)in->scope;
        memcpy(&a->sin6_addr,in->bytes,16); *length=(int)sizeof(*a); return 1;
    }
    return 0;
}
static int from_native(const struct sockaddr *in,DmmNetAddress *out) {
    memset(out,0,sizeof(*out));
    if(in->sa_family==AF_INET) {
        const struct sockaddr_in *a=(const struct sockaddr_in *)in;
        out->family=4; out->port=ntohs(a->sin_port); memcpy(out->bytes,&a->sin_addr,4); return 1;
    }
    if(in->sa_family==AF_INET6) {
        const struct sockaddr_in6 *a=(const struct sockaddr_in6 *)in;
        out->family=6; out->port=ntohs(a->sin6_port); out->scope=a->sin6_scope_id;
        memcpy(out->bytes,&a->sin6_addr,16); return 1;
    }
    return 0;
}
static void reactor_main(void *unused);
static void dns_main(void *unused);
static int initialize(DmmNetError *error) {
    if(runtime_state) { *error=net_error(DMM_NET_CLOSED); return 0; }
    if(initialized) return 1;
#ifdef _WIN32
    WSADATA data; int code=WSAStartup(MAKEWORD(2,2),&data);
    if(code) { *error=native_error(code); return 0; }
    if(data.wVersion!=MAKEWORD(2,2)) { WSACleanup(); *error=net_error(DMM_NET_SYSTEM); return 0; }
    winsock_started=1;
    completion_port=CreateIoCompletionPort(INVALID_HANDLE_VALUE,NULL,0,1);
    if(!completion_port) { *error=win32_error((int)GetLastError()); WSACleanup(); winsock_started=0; return 0; }
#else
    epoll_fd=epoll_create1(EPOLL_CLOEXEC); command_fd=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);
    struct epoll_event event={.events=EPOLLIN,.data.u64=0};
    if(epoll_fd<0||command_fd<0||epoll_ctl(epoll_fd,EPOLL_CTL_ADD,command_fd,&event)) {
        *error=native_error(errno); if(epoll_fd>=0) close(epoll_fd); if(command_fd>=0) close(command_fd);
        epoll_fd=command_fd=-1; return 0;
    }
#endif
    initialized=1;
    reactor_thread=__dmm_async_thread_create(reactor_main,NULL);
    return 1;
}
static NetFd new_fd(unsigned family,unsigned transport,DmmNetError *error) {
#ifdef _WIN32
    NetFd fd=WSASocketW(family==4?AF_INET:AF_INET6,transport==1?SOCK_STREAM:SOCK_DGRAM,0,NULL,0,WSA_FLAG_OVERLAPPED|WSA_FLAG_NO_HANDLE_INHERIT);
#ifdef DMM_NET_TESTING
    if(fd!=NET_INVALID) test_socket_handles++;
#endif
    if(fd!=NET_INVALID) { u_long one=1;
        if(ioctlsocket(fd,(long)FIONBIO,&one)) { *error=native_error(os_error()); close_fd(fd); return NET_INVALID; }
        if(!CreateIoCompletionPort((HANDLE)fd,completion_port,0,0)) { *error=win32_error((int)GetLastError()); close_fd(fd); return NET_INVALID; } }
#else
    NetFd fd=socket(family==4?AF_INET:AF_INET6,(transport==1?SOCK_STREAM:SOCK_DGRAM)|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
#ifdef DMM_NET_TESTING
    if(fd!=NET_INVALID) test_socket_handles++;
#endif
#endif
    if(fd==NET_INVALID) { *error=native_error(os_error()); return fd; }
    if(fd!=NET_INVALID&&family==6) {
        int one=1;
        if(setsockopt(fd,IPPROTO_IPV6,IPV6_V6ONLY,(const char *)&one,sizeof(one))) { *error=native_error(os_error()); close_fd(fd); return NET_INVALID; }
    }
    return fd;
}
DmmNetSocket *__dmm_net_socket(uint64_t family,uint64_t transport,DmmNetError *error) {
    *error=net_error(DMM_NET_OK); lock_net();
    if(!initialize(error)) { unlock_net(); return NULL; }
    if((family!=4&&family!=6)||(transport!=1&&transport!=2)) { *error=net_error(DMM_NET_INPUT); unlock_net(); return NULL; }
    DmmNetSocket *s=calloc(1,sizeof(*s));
    if(!s) { *error=net_error(DMM_NET_MEMORY); unlock_net(); return NULL; }
    s->fd=new_fd((unsigned)family,(unsigned)transport,error); s->family=(unsigned)family; s->transport=(unsigned)transport;
    if(s->fd==NET_INVALID) { free(s); s=NULL; }
    unlock_net(); return s;
}
static int usable(DmmNetSocket *s,DmmNetError *error,int admission) {
    if(admission&&runtime_state) { *error=net_error(DMM_NET_CLOSED); return 0; }
    if(!s||s->fd==NET_INVALID) { *error=net_error(DMM_NET_CLOSED); return 0; }
    *error=net_error(DMM_NET_OK); return 1;
}
int __dmm_net_bind(DmmNetSocket *s,const DmmNetAddress *address,DmmNetError *error) {
    lock_net(); struct sockaddr_storage native; int length;
    int ok=usable(s,error,1);
    if(ok&&(s->read||s->write||s->state)) { *error=net_error(DMM_NET_STATE); ok=0; }
    if(ok&&(!to_native(address,&native,&length)||address->family!=s->family)) { *error=net_error(DMM_NET_INPUT); ok=0; }
    if(ok&&bind(s->fd,(struct sockaddr *)&native,(socklen_t)length)) { *error=native_error(os_error()); ok=0; }
    if(ok) s->state=1;
    unlock_net(); return ok;
}
int __dmm_net_listen(DmmNetSocket *s,uint64_t backlog,DmmNetError *error) {
    lock_net(); int ok=usable(s,error,1);
    if(ok&&(s->transport!=1||s->state!=1||s->read||s->write)) { *error=net_error(DMM_NET_STATE); ok=0; }
    if(ok&&backlog>INT_MAX) { *error=net_error(DMM_NET_INPUT); ok=0; }
    if(ok&&listen(s->fd,(int)backlog)) { *error=native_error(os_error()); ok=0; }
    if(ok) s->state=2;
    unlock_net(); return ok;
}
int __dmm_net_address(DmmNetSocket *s,uint64_t peer,DmmNetAddress *out,DmmNetError *error) {
    lock_net(); int ok=usable(s,error,1); struct sockaddr_storage native; socklen_t length=sizeof(native);
    if(ok&&(peer?s->state!=3:s->state==0)) { *error=net_error(DMM_NET_STATE); ok=0; }
    if(ok&&(peer?getpeername(s->fd,(struct sockaddr *)&native,&length):getsockname(s->fd,(struct sockaddr *)&native,&length))) { *error=native_error(os_error()); ok=0; }
    if(ok) ok=from_native((struct sockaddr *)&native,out);
    unlock_net(); return ok;
}
int __dmm_net_shutdown_socket(DmmNetSocket *s,uint64_t direction,DmmNetError *error) {
    lock_net(); int ok=usable(s,error,1);
    if(ok&&(s->read||s->write||s->state!=3)) { *error=net_error(DMM_NET_STATE); ok=0; }
    if(ok&&direction>2) { *error=net_error(DMM_NET_INPUT); ok=0; }
    if(ok&&shutdown(s->fd,(int)direction)) { *error=native_error(os_error()); ok=0; }
    unlock_net(); return ok;
}
int __dmm_net_address_packet(DmmNetSocket *s,uint64_t *packet,DmmNetError *error) {
    return __dmm_net_address(s,packet[0],(DmmNetAddress *)(packet+1),error);
}
int __dmm_net_close(DmmNetSocket *s,DmmNetError *error) {
    *error=net_error(DMM_NET_OK); if(!s) return 1;
    lock_net(); assert(!s->read&&!s->write);
    int ok=1;
    if(s->fd!=NET_INVALID&&close_fd(s->fd)) { *error=native_error(os_error()); ok=0; }
#ifdef DMM_NET_TESTING
    /* Inject only after the real close: the test exercises reporting and
       consumption without leaking the underlying kernel resource. */
    if(close_test_error) { *error=native_error(close_test_error); close_test_error=0; ok=0; }
#endif
    s->fd=NET_INVALID; unlock_net(); free(s); return ok;
}
static void release_locked(DmmNetOperation *op) {
    if(--op->refs) return;
    assert(op->done&&op->notified);
    __dmm_async_io_destroy(op->io);
    if(op->accepted) { if(op->accepted->fd!=NET_INVALID) close_fd(op->accepted->fd); free(op->accepted); }
    free(op->addresses); free(op->hostname); free(op);
}
static void heap_swap(size_t a,size_t b) {
    DmmNetOperation *saved=deadline_heap[a]; deadline_heap[a]=deadline_heap[b]; deadline_heap[b]=saved;
    deadline_heap[a]->heap_index=a; deadline_heap[b]->heap_index=b;
}
static void heap_up(size_t index) {
    while(index&&deadline_heap[index]->request.deadline<deadline_heap[(index-1)/2]->request.deadline) {
        heap_swap(index,(index-1)/2); index=(index-1)/2;
    }
}
static void heap_remove(DmmNetOperation *op) {
    size_t index=op->heap_index;
    if(index==SIZE_MAX) return;
    op->heap_index=SIZE_MAX; heap_count--;
    if(index==heap_count) return;
    DmmNetOperation *moved=deadline_heap[heap_count];
    deadline_heap[index]=moved; moved->heap_index=index;
    heap_up(index);
    index=moved->heap_index;
    for(;;) {
        size_t child=index*2+1;
        if(child>=heap_count) break;
        if(child+1<heap_count&&deadline_heap[child+1]->request.deadline<deadline_heap[child]->request.deadline) child++;
        if(deadline_heap[index]->request.deadline<=deadline_heap[child]->request.deadline) break;
        heap_swap(index,child); index=child;
    }
}
static int heap_insert(DmmNetOperation *op) {
    if(op->request.deadline==DMM_NET_NEVER) return 1;
    if(heap_count==heap_capacity) {
        size_t capacity=heap_capacity?heap_capacity*2:32;
        if(capacity<heap_capacity||capacity>SIZE_MAX/sizeof(*deadline_heap)) return 0;
        void *grown=realloc(deadline_heap,capacity*sizeof(*deadline_heap));
        if(!grown) return 0;
        deadline_heap=grown; heap_capacity=capacity;
    }
    op->heap_index=heap_count; deadline_heap[heap_count++]=op; heap_up(op->heap_index); return 1;
}
static void complete_locked(DmmNetOperation *op,DmmNetError error) {
    if(op->done) return;
    heap_remove(op); op->done=1; op->error=error;
    DmmNetSocket *s=op->request.socket;
    if(s) {
        int claimed=s->read==op||s->write==op;
        if(s->read==op) s->read=NULL;
        if(s->write==op) s->write=NULL;
        if(op->request.command==DMM_NET_CONNECT&&claimed) {
            if(error.kind) { if(s->fd!=NET_INVALID) close_fd(s->fd); s->fd=NET_INVALID; }
            else s->state=3;
        }
#ifndef _WIN32
        if(s->fd!=NET_INVALID) {
            struct epoll_event ev={0};
            /* No event pointer is retained: readiness only schedules a scan of
               current operations, so stale fd events cannot touch old owners. */
            ev.events=(s->read?EPOLLIN:0U)|(s->write?EPOLLOUT:0U);
            if(ev.events) { if(epoll_ctl(epoll_fd,EPOLL_CTL_MOD,s->fd,&ev)&&errno==ENOENT) (void)epoll_ctl(epoll_fd,EPOLL_CTL_ADD,s->fd,&ev); }
            else (void)epoll_ctl(epoll_fd,EPOLL_CTL_DEL,s->fd,NULL);
        }
#endif
    }
    op->refs++; op->notify_next=notifications; notifications=op;
}
static void deliver(void) {
    for(;;) {
        lock_net(); DmmNetOperation *op=notifications;
        if(!op) { unlock_net(); break; }
        notifications=op->notify_next; op->notified=1; unlock_net();
        __dmm_async_io_confirm(op->io);
        lock_net(); release_locked(op); unlock_net();
    }
}
static int milliseconds(uint64_t now,uint64_t deadline) {
    if(deadline==DMM_NET_NEVER) return -1;
    if(deadline<=now) return 0;
    uint64_t delta=deadline-now,ms=delta/1000000ULL+(delta%1000000ULL!=0);
    return ms>(uint64_t)INT_MAX?INT_MAX:(int)ms;
}
#ifndef _WIN32
static void linux_step(DmmNetOperation *op) {
    DmmNetSocket *s=op->request.socket; uint64_t command=op->request.command;
    if(command!=DMM_NET_CONNECT) op->started=1;
    struct sockaddr_storage address; int length=0; ssize_t count=-1; int code=0;
    if(command==DMM_NET_CONNECT) {
        if(!op->started) {
            (void)to_native(&op->request.address,&address,&length); op->started=1;
            int result=connect(s->fd,(struct sockaddr *)&address,(socklen_t)length);
            s->state=1; /* an admitted Connect owns the implicit bind */
            if(!result) { complete_locked(op,net_error(DMM_NET_OK)); return; }
            code=errno;
            if(code==EINPROGRESS||code==EALREADY||code==EINTR) goto pending;
        } else {
            /* SO_ERROR alone can report zero before connection completion. */
            struct sockaddr_storage peer; socklen_t peer_size=sizeof(peer);
            socklen_t size=sizeof(code);
            if(getsockopt(s->fd,SOL_SOCKET,SO_ERROR,&code,&size)) code=errno;
            if(!code&&getpeername(s->fd,(struct sockaddr *)&peer,&peer_size)) {
                if(errno==ENOTCONN) goto pending;
                code=errno;
            }
        }
        complete_locked(op,code?native_error(code):net_error(DMM_NET_OK)); return;
    }
    if(command==DMM_NET_ACCEPT) {
        socklen_t size=sizeof(address);
        NetFd fd=accept4(s->fd,(struct sockaddr *)&address,&size,SOCK_NONBLOCK|SOCK_CLOEXEC);
        if(fd>=0) {
#ifdef DMM_NET_TESTING
            test_socket_handles++;
#endif
            op->accepted=calloc(1,sizeof(*op->accepted));
            if(!op->accepted) { close_fd(fd); complete_locked(op,net_error(DMM_NET_MEMORY)); return; }
            *op->accepted=(DmmNetSocket){.fd=fd,.family=s->family,.transport=1,.state=3};
            (void)from_native((struct sockaddr *)&address,&op->peer); complete_locked(op,net_error(DMM_NET_OK)); return;
        }
    } else if(command==DMM_NET_READ) {
        count=recv(s->fd,op->request.buffer,(size_t)op->request.length,0);
    } else if(command==DMM_NET_WRITE) {
        count=send(s->fd,op->request.buffer,(size_t)op->request.length,MSG_NOSIGNAL);
    } else if(command==DMM_NET_SEND_TO) {
        (void)to_native(&op->request.address,&address,&length);
        count=sendto(s->fd,op->request.buffer,(size_t)op->request.length,MSG_NOSIGNAL,(struct sockaddr *)&address,(socklen_t)length);
    } else if(command==DMM_NET_RECV_FROM) {
        struct iovec buffer={op->request.buffer,(size_t)op->request.length};
        struct msghdr message={.msg_name=&address,.msg_namelen=sizeof(address),.msg_iov=&buffer,.msg_iovlen=1};
        count=recvmsg(s->fd,&message,0);
        if(count>=0) { op->truncated=(message.msg_flags&MSG_TRUNC)!=0; (void)from_native((struct sockaddr *)&address,&op->peer); }
    }
    if(count>=0) { if(command==DMM_NET_SEND_TO) s->state=1;
        op->count=(uint64_t)count; op->eof=command==DMM_NET_READ&&count==0&&op->request.length!=0; complete_locked(op,net_error(DMM_NET_OK)); return; }
    code=errno;
    if(code==EINTR) { wake_reactor(); return; }
    if(code!=EAGAIN&&code!=EWOULDBLOCK) { complete_locked(op,native_error(code)); return; }
pending:;
    struct epoll_event ev={.events=(s->read?EPOLLIN:0U)|(s->write?EPOLLOUT:0U),.data.u64=1};
    if(epoll_ctl(epoll_fd,EPOLL_CTL_MOD,s->fd,&ev)&&
       (errno!=ENOENT||epoll_ctl(epoll_fd,EPOLL_CTL_ADD,s->fd,&ev))) complete_locked(op,native_error(errno));
}
#else
static void windows_submit(DmmNetOperation *op) {
    DmmNetSocket *s=op->request.socket; uint64_t command=op->request.command;
    DWORD transferred=0; int result=SOCKET_ERROR;
    op->wsabuf.buf=op->request.buffer; op->wsabuf.len=(ULONG)op->request.length;
    if(command==DMM_NET_CONNECT) {
        GUID guid=WSAID_CONNECTEX; LPFN_CONNECTEX connect_ex=NULL;
        if(WSAIoctl(s->fd,SIO_GET_EXTENSION_FUNCTION_POINTER,&guid,sizeof(guid),&connect_ex,sizeof(connect_ex),&transferred,NULL,NULL)) goto failed;
        if(!s->state) {
            DmmNetAddress any={.family=s->family}; struct sockaddr_storage address; int length;
            (void)to_native(&any,&address,&length);
            if(bind(s->fd,(struct sockaddr *)&address,length)) goto failed;
            s->state=1;
        }
        (void)to_native(&op->request.address,&op->sockaddr,&op->sockaddr_length);
        result=connect_ex(s->fd,(struct sockaddr *)&op->sockaddr,op->sockaddr_length,NULL,0,&transferred,&op->overlapped)?0:SOCKET_ERROR;
    } else if(command==DMM_NET_ACCEPT) {
        GUID guid=WSAID_ACCEPTEX; LPFN_ACCEPTEX accept_ex=NULL;
        if(WSAIoctl(s->fd,SIO_GET_EXTENSION_FUNCTION_POINTER,&guid,sizeof(guid),&accept_ex,sizeof(accept_ex),&transferred,NULL,NULL)) goto failed;
        DmmNetError error=net_error(DMM_NET_OK);
        op->accepted_fd=new_fd(s->family,1,&error);
        if(op->accepted_fd==NET_INVALID) { complete_locked(op,error); return; }
        result=accept_ex(s->fd,op->accepted_fd,op->accept_buffer,0,sizeof(struct sockaddr_storage)+16,sizeof(struct sockaddr_storage)+16,&transferred,&op->overlapped)?0:SOCKET_ERROR;
    } else if(command==DMM_NET_READ) result=WSARecv(s->fd,&op->wsabuf,1,&op->transferred,&op->flags,&op->overlapped,NULL);
    else if(command==DMM_NET_WRITE) result=WSASend(s->fd,&op->wsabuf,1,&op->transferred,0,&op->overlapped,NULL);
    else if(command==DMM_NET_SEND_TO) {
        (void)to_native(&op->request.address,&op->sockaddr,&op->sockaddr_length);
        result=WSASendTo(s->fd,&op->wsabuf,1,&op->transferred,0,(struct sockaddr *)&op->sockaddr,op->sockaddr_length,&op->overlapped,NULL);
    } else if(command==DMM_NET_RECV_FROM) {
        op->sockaddr_length=sizeof(op->sockaddr);
        result=WSARecvFrom(s->fd,&op->wsabuf,1,&op->transferred,&op->flags,(struct sockaddr *)&op->sockaddr,&op->sockaddr_length,&op->overlapped,NULL);
    }
    if(!result||WSAGetLastError()==WSA_IO_PENDING) { op->submitted=1; return; }
failed:
    complete_locked(op,native_error(WSAGetLastError()));
    if(op->accepted_fd!=NET_INVALID) { close_fd(op->accepted_fd); op->accepted_fd=NET_INVALID; }
}
static void windows_complete(DmmNetOperation *op,DWORD bytes,int code) {
    assert(op->submitted&&!op->done); op->submitted=0;
    if(op->request.deadline!=DMM_NET_NEVER&&op->request.deadline<=__dmm_net_now()) op->timeout=1;
    uint64_t command=op->request.command; DmmNetSocket *s=op->request.socket;
    int winsock_domain=0;
    if(op->cancel||op->timeout) {
        if(op->accepted_fd!=NET_INVALID) { close_fd(op->accepted_fd); op->accepted_fd=NET_INVALID; }
        complete_locked(op,net_error(op->timeout?DMM_NET_TIMEOUT:DMM_NET_CLOSED)); return;
    }
    if(command==DMM_NET_RECV_FROM&&(code==WSAEMSGSIZE||code==ERROR_MORE_DATA)) { op->truncated=1; bytes=(DWORD)op->request.length; code=0; }
    if(!code&&command==DMM_NET_CONNECT) {
        if(setsockopt(s->fd,SOL_SOCKET,SO_UPDATE_CONNECT_CONTEXT,NULL,0)) { code=WSAGetLastError(); winsock_domain=1; }
    }
    if(!code&&command==DMM_NET_ACCEPT) {
        if(setsockopt(op->accepted_fd,SOL_SOCKET,SO_UPDATE_ACCEPT_CONTEXT,(const char *)&s->fd,sizeof(s->fd))) { code=WSAGetLastError(); winsock_domain=1; }
        else {
            op->accepted=calloc(1,sizeof(*op->accepted));
            if(!op->accepted) code=WSA_NOT_ENOUGH_MEMORY;
            else {
                *op->accepted=(DmmNetSocket){.fd=op->accepted_fd,.family=s->family,.transport=1,.state=3};
                struct sockaddr_storage address; int length=sizeof(address);
                if(getpeername(op->accepted_fd,(struct sockaddr *)&address,&length)) { code=WSAGetLastError(); winsock_domain=1; }
                else (void)from_native((struct sockaddr *)&address,&op->peer);
                op->accepted_fd=NET_INVALID;
            }
        }
    }
    if(op->accepted_fd!=NET_INVALID) { close_fd(op->accepted_fd); op->accepted_fd=NET_INVALID; }
    if(!code) {
        if(command==DMM_NET_SEND_TO) s->state=1;
        op->count=bytes; op->eof=command==DMM_NET_READ&&bytes==0&&op->request.length!=0;
        if(command==DMM_NET_RECV_FROM) (void)from_native((struct sockaddr *)&op->sockaddr,&op->peer);
    }
    complete_locked(op,code?(winsock_domain?native_error(code):win32_error(code)):net_error(DMM_NET_OK));
}
#endif

DmmNetOperation *__dmm_net_start(const DmmNetRequest *request) {
    DmmNetOperation *op=calloc(1,sizeof(*op));
    /* Allocating the Future/acknowledgement itself follows the existing fatal
       runtime allocation contract; operational resource failures are Result. */
    if(!op) abort();
    op->request=*request; op->refs=1; op->heap_index=SIZE_MAX; op->io=__dmm_async_io_create(NULL,NULL);
#ifdef _WIN32
    op->accepted_fd=NET_INVALID;
#endif
    lock_net(); op->next=operations; operations=op;
    DmmNetError error=net_error(DMM_NET_OK);
    if(!initialize(&error)) { complete_locked(op,error); goto done; }
    if(!heap_insert(op)) { complete_locked(op,net_error(DMM_NET_MEMORY)); goto done; }
    if(request->command==DMM_NET_DNS) {
        if(request->deadline!=DMM_NET_NEVER&&request->deadline<=__dmm_net_now()) { complete_locked(op,net_error(DMM_NET_TIMEOUT)); goto done; }
        if(!request->hostname||(request->address.family!=0&&request->address.family!=4&&request->address.family!=6)||request->address.port>65535||(request->transport!=1&&request->transport!=2)) { complete_locked(op,net_error(DMM_NET_INPUT)); goto done; }
        if(dns_count==64) { complete_locked(op,net_error(DMM_NET_BUSY)); goto done; }
        size_t length=strlen(request->hostname);
        op->hostname=malloc(length+1);
        if(!op->hostname) { complete_locked(op,net_error(DMM_NET_MEMORY)); goto done; }
        memcpy(op->hostname,request->hostname,length+1); op->request.hostname=NULL;
        op->dns_queued=1; op->refs++;
        if(dns_tail) dns_tail->dns_next=op; else dns_head=op;
        dns_tail=op; dns_count++;
        if(!dns_threads[0]) { dns_threads[0]=__dmm_async_thread_create(dns_main,NULL); dns_threads[1]=__dmm_async_thread_create(dns_main,NULL); }
        wake_dns(); goto done;
    }
    DmmNetSocket *s=request->socket;
    if(!usable(s,&error,1)) { complete_locked(op,error); goto done; }
    uint64_t command=request->command;
    int reads=command==DMM_NET_CONNECT||command==DMM_NET_ACCEPT||command==DMM_NET_READ||command==DMM_NET_RECV_FROM;
    int writes=command==DMM_NET_CONNECT||command==DMM_NET_WRITE||command==DMM_NET_SEND_TO;
    if(command<DMM_NET_CONNECT||command>DMM_NET_SEND_TO||
       (command==DMM_NET_CONNECT&&(s->transport!=1||s->state>1))||
       (command==DMM_NET_ACCEPT&&(s->transport!=1||s->state!=2))||
       ((command==DMM_NET_READ||command==DMM_NET_WRITE)&&(s->transport!=1||s->state!=3))||
       ((command==DMM_NET_RECV_FROM||command==DMM_NET_SEND_TO)&&s->transport!=2)) { complete_locked(op,net_error(DMM_NET_STATE)); goto done; }
    if((reads&&s->read)||(writes&&s->write)) { complete_locked(op,net_error(DMM_NET_BUSY)); goto done; }
    if(reads) s->read=op;
    if(writes) s->write=op;
    if(request->length>INT_MAX||(request->length&&!request->buffer)) { complete_locked(op,net_error(DMM_NET_INPUT)); goto done; }
    if(command==DMM_NET_CONNECT||command==DMM_NET_SEND_TO) {
        struct sockaddr_storage address; int length;
        if(!to_native(&request->address,&address,&length)||request->address.family!=s->family) { complete_locked(op,net_error(DMM_NET_INPUT)); goto done; }
    }
    if(request->deadline!=DMM_NET_NEVER&&request->deadline<=__dmm_net_now()) { complete_locked(op,net_error(DMM_NET_TIMEOUT)); goto done; }
    if((command==DMM_NET_READ||command==DMM_NET_WRITE)&&!request->length) complete_locked(op,net_error(DMM_NET_OK));
done:
    wake_reactor(); unlock_net(); deliver(); return op;
}
void __dmm_net_cancel(DmmNetOperation *op) {
    lock_net(); if(!op->done) op->cancel=1; wake_reactor(); unlock_net();
}
int __dmm_net_poll(DmmNetOperation *op,void *context) { return __dmm_async_io_poll(op->io,context); }
void __dmm_net_release(DmmNetOperation *op) {
    if(!op) return;
    lock_net(); assert(op->done);
    DmmNetOperation **link=&operations; while(*link&&*link!=op) link=&(*link)->next;
    assert(*link==op); *link=op->next; release_locked(op); unlock_net();
}
uint64_t __dmm_net_result(DmmNetOperation *op,uint64_t field) {
    lock_net(); assert(op->done); uint64_t result=0;
    switch(field) {
        case 0: result=op->error.kind; break; case 1: result=op->error.domain; break;
        case 2: result=(uint64_t)op->error.code; break; case 3: result=op->count; break;
        case 4: result=(uint64_t)op->eof; break; case 5: result=(uint64_t)op->truncated; break;
        case 6: result=(uint64_t)(uintptr_t)op->accepted; op->accepted=NULL; break;
        case 7: result=(uint64_t)(uintptr_t)op->addresses; op->addresses=NULL; break;
    }
    unlock_net(); return result;
}
void __dmm_net_result_address(DmmNetOperation *op,DmmNetAddress *out) { lock_net(); assert(op->done); *out=op->peer; unlock_net(); }
uint64_t __dmm_net_addresses_count(DmmNetAddresses *list) { return list?(uint64_t)list->count:0; }
int __dmm_net_addresses_get(DmmNetAddresses *list,uint64_t index,DmmNetAddress *out) {
    if(!list||index>=list->count) return 0;
    *out=list->values[index]; return 1;
}
void __dmm_net_addresses_release(DmmNetAddresses *list) { free(list); }
static void remove_queued(DmmNetOperation *op) {
    DmmNetOperation **link=&dns_head,*previous=NULL;
    while(*link&&*link!=op) { previous=*link; link=&(*link)->dns_next; }
    if(*link) { *link=op->dns_next; if(dns_tail==op) dns_tail=previous; dns_count--; op->dns_queued=0; release_locked(op); }
}
static void dns_main(void *unused) {
    (void)unused;
    for(;;) {
        lock_net();
        while(!dns_head&&!stop_dns) {
#ifdef _WIN32
            if(!SleepConditionVariableSRW(&dns_condition,&net_mutex,INFINITE,0)) abort();
#else
            if(pthread_cond_wait(&dns_condition,&net_mutex)) abort();
#endif
        }
        if(!dns_head&&stop_dns) { unlock_net(); return; }
        DmmNetOperation *op=dns_head; dns_head=op->dns_next; if(!dns_head) dns_tail=NULL;
        dns_count--; op->dns_queued=0; op->dns_active=1;
#ifdef DMM_NET_TESTING
        int (*hook)(const char *,const DmmNetAddress *,DmmNetAddress *,void *)=dns_test_hook;
        void *hook_argument=dns_test_argument;
#endif
        unlock_net();
        struct addrinfo hints={0},*answers=NULL;
        hints.ai_family=op->request.address.family==4?AF_INET:op->request.address.family==6?AF_INET6:AF_UNSPEC;
        hints.ai_socktype=op->request.transport==1?SOCK_STREAM:SOCK_DGRAM;
        char service[6]; snprintf(service,sizeof(service),"%u",(unsigned)op->request.address.port);
        hints.ai_flags=AI_NUMERICSERV;
        DmmNetAddresses *list=NULL; DmmNetError error=net_error(DMM_NET_OK);
#ifdef DMM_NET_TESTING
        if(hook) {
            DmmNetAddress address;
            int code=hook(op->hostname,&op->request.address,&address,hook_argument);
            if(code) error=(DmmNetError){DMM_NET_SYSTEM,DMM_NET_RESOLVER,code};
            else {
                list=malloc(sizeof(*list)+sizeof(address));
                if(!list) error=net_error(DMM_NET_MEMORY);
                else { list->count=1; list->values[0]=address; }
            }
        } else
#endif
        {
        int code=getaddrinfo(op->hostname,service,&hints,&answers);
        if(code) error=(DmmNetError){code==EAI_MEMORY?DMM_NET_MEMORY:DMM_NET_SYSTEM,DMM_NET_RESOLVER,code};
        else {
            size_t count=0; for(struct addrinfo *a=answers;a;a=a->ai_next) if(a->ai_family==AF_INET||a->ai_family==AF_INET6) count++;
            if(count>(SIZE_MAX-sizeof(*list))/sizeof(DmmNetAddress)) error=net_error(DMM_NET_MEMORY);
            else list=malloc(sizeof(*list)+count*sizeof(DmmNetAddress));
            if(!list) error=net_error(DMM_NET_MEMORY);
            else { list->count=0; for(struct addrinfo *a=answers;a;a=a->ai_next) if(from_native(a->ai_addr,&list->values[list->count])) list->count++; }
        }
        if(answers) freeaddrinfo(answers);
        }
        lock_net(); op->dns_active=0;
        if(!op->done) { op->addresses=list; list=NULL; complete_locked(op,error); }
        free(list); release_locked(op); unlock_net(); deliver();
    }
}
static void reactor_main(void *unused) {
    (void)unused;
    for(;;) {
        lock_net(); uint64_t now=__dmm_net_now();
        while(heap_count&&deadline_heap[0]->request.deadline<=now) {
            DmmNetOperation *expired=deadline_heap[0]; heap_remove(expired); expired->timeout=1;
        }
        int wait=heap_count?milliseconds(now,deadline_heap[0]->request.deadline):-1;
        for(DmmNetOperation *op=operations;op;op=op->next) {
            if(op->done) continue;
            if(op->cancel||op->timeout) {
#ifdef _WIN32
                if(op->submitted) { (void)CancelIoEx((HANDLE)op->request.socket->fd,&op->overlapped); continue; }
#endif
                complete_locked(op,net_error(op->timeout?DMM_NET_TIMEOUT:DMM_NET_CLOSED));
                if(op->dns_queued) remove_queued(op);
                continue;
            }
            if(op->request.command!=DMM_NET_DNS) {
#ifdef _WIN32
                if(!op->submitted) windows_submit(op);
#else
                linux_step(op);
#endif
            }
        }
        int stop=stop_reactor;
#ifdef DMM_NET_TESTING
        void (*hook)(void *)=reactor_test_hook; void *hook_argument=reactor_test_argument;
        reactor_test_hook=NULL;
#endif
        unlock_net(); deliver(); if(stop) return;
#ifdef DMM_NET_TESTING
        if(hook) hook(hook_argument);
#endif
#ifdef _WIN32
        DWORD bytes=0; ULONG_PTR key=0; OVERLAPPED *overlapped=NULL;
        BOOL ok=GetQueuedCompletionStatus(completion_port,&bytes,&key,&overlapped,wait<0?INFINITE:(DWORD)wait);
        int code=ok?0:(int)GetLastError(); (void)key;
        if(overlapped) { lock_net(); windows_complete((DmmNetOperation *)overlapped,bytes,code); unlock_net(); deliver(); }
#else
        struct epoll_event events[32]; int count=epoll_wait(epoll_fd,events,32,wait);
        if(count<0&&errno!=EINTR) abort();
        uint64_t value; while(read(command_fd,&value,8)>0) {}
#endif
    }
}
/* Layout is the generated Future<void> ABI, with no DMM-owned allocations
   handed to C. Its operation is owned by the enclosing DMM Operation guard. */
typedef struct {
    int (*poll)(void *,void *);
    void (*destroy)(void *,int);
    int64_t state;
    int (*cancel_poll)(void *,void *);
    void (*result_destroy)(void *);
    void *context;
    uint64_t result_present,cancel_state;
    DmmNetOperation *operation;
    void *result_cancel;
} NetFuture;
_Static_assert(sizeof(NetFuture)==80,"private Future header size");
static int net_future_cancel(void *frame,void *context) {
    NetFuture *f=frame; __dmm_net_cancel(f->operation);
    int ready=__dmm_net_poll(f->operation,context); if(ready) f->state=-1; return ready;
}
static int net_future_poll(void *frame,void *context) {
    NetFuture *f=frame;
    if(__dmm_async_cancel_requested(context)) return net_future_cancel(frame,context);
    int ready=__dmm_net_poll(f->operation,context); if(ready) { f->state=-1; f->result_present=1; } return ready;
}
static void net_future_destroy(void *frame,int dispose) { (void)dispose; free(frame); }
void *__dmm_net_wait(uint64_t operation) {
    NetFuture *f=calloc(1,sizeof(*f)); if(!f) abort();
    f->poll=net_future_poll; f->cancel_poll=net_future_cancel; f->destroy=net_future_destroy;
    f->operation=(DmmNetOperation *)(uintptr_t)operation; return f;
}
void __dmm_net_begin_draining(void) { lock_net(); if(!runtime_state) runtime_state=1; unlock_net(); }
void __dmm_net_finish(void) {
    __dmm_net_begin_draining(); lock_net(); stop_dns=1; wake_dns(); unlock_net();
    for(unsigned i=0;i<2;i++) if(dns_threads[i]) { __dmm_async_thread_join(dns_threads[i]); dns_threads[i]=NULL; }
    lock_net(); assert(!operations); stop_reactor=1; wake_reactor(); unlock_net();
    if(reactor_thread) { __dmm_async_thread_join(reactor_thread); reactor_thread=NULL; }
#ifdef _WIN32
    if(completion_port) { CloseHandle(completion_port); completion_port=NULL; }
    if(winsock_started) { WSACleanup(); winsock_started=0; }
#else
    if(epoll_fd>=0) close(epoll_fd);
    if(command_fd>=0) close(command_fd);
    epoll_fd=command_fd=-1;
#endif
    lock_net(); free(deadline_heap); deadline_heap=NULL; heap_count=heap_capacity=0; runtime_state=2; unlock_net();
}
