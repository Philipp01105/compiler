#ifndef DMM_PLATFORM_SHIM_H
#define DMM_PLATFORM_SHIM_H

/* Private ABI v1, x86-64 System V on ELF and Microsoft x64 on COFF.
   Handles are non-null opaque pointers. Resource/API failures terminate.
   Callback returns normally; join consumes its handle after confirmed exit.
   Events are initially unsignalled and manual-reset. A signal is retained until
   reset; the scheduler resets under its predicate lock. Destroy requires no
   waiters. Each allocation is released by its own runtime, never across ABI. */
typedef void (*DmmPlatformThreadCallback)(void *argument);
void *__dmm_async_thread_create(DmmPlatformThreadCallback callback, void *argument);
void __dmm_async_thread_join(void *thread);
void *__dmm_async_wait_create(void);
void __dmm_async_wait(void *event);
void __dmm_async_wake(void *event);
void __dmm_async_wait_reset(void *event);
void __dmm_async_wait_destroy(void *event);
_Noreturn void __dmm_platform_exit(int code);

/* Generated object owns initialization, DMM main, drain and cleanup. Call once
   on the process main thread. Fatal exits bypass normal lifecycle cleanup. */
int __dmm_runtime_main(void);
#endif
