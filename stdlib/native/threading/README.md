# stdlib/native/threading

Thread creation, confirmed join and manual-reset events are implemented with direct OS FFI bindings:
pthreads/glibc on Linux and Kernel32/UCRT on Windows. The executor and network packages import these DMM
functions directly. Native ABI callback entry points exist for OS thread callbacks and generated async frames.
There is no precompiled threading component required by applications.
