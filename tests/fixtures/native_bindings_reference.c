#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
uint64_t binding_size(uint32_t index) {
    switch(index) {
    case 0: return sizeof(OVERLAPPED); case 1: return _Alignof(OVERLAPPED);
    case 2: return sizeof(OVERLAPPED_ENTRY); case 3: return sizeof(SECURITY_ATTRIBUTES);
    case 4: return sizeof(WSADATA); case 5: return _Alignof(WSADATA);
    case 6: return sizeof(WSABUF); case 7: return sizeof(ADDRINFOA);
    case 8: return sizeof(SOCKADDR_IN); case 9: return sizeof(SOCKADDR_IN6);
    case 10: return sizeof(SOCKADDR_STORAGE); default: return 0;
    }
}
#else
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
uint64_t binding_size(uint32_t index) {
    switch(index) {
    case 0: return sizeof(pthread_mutex_t); case 1: return _Alignof(pthread_mutex_t);
    case 2: return sizeof(pthread_cond_t); case 3: return _Alignof(pthread_cond_t);
    case 4: return sizeof(pthread_attr_t); case 5: return sizeof(struct timespec);
    case 6: return sizeof(struct epoll_event); case 7: return _Alignof(struct epoll_event);
    case 8: return sizeof(struct addrinfo); case 9: return sizeof(struct sockaddr_in);
    case 10: return sizeof(struct sockaddr_in6); case 11: return sizeof(struct sockaddr_storage);
    default: return 0;
    }
}
#endif
