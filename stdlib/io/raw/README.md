# Raw descriptors

Import an explicit alias for "stdlib/io/raw". core_read(fd,pointer,count) and core_write return byte count, zero at EOF for read, or a negative native failure. Short operations remain visible. core_open(path,flags,permissions) returns a descriptor or negative error; core_close returns zero or negative error. Descriptor ownership and buffer validity are caller responsibilities.

CORE_STDIN/STDOUT/STDERR are 0/1/2. Combine one of CORE_READ_ONLY/WRITE_ONLY/READ_WRITE with CORE_CREATE/TRUNCATE/APPEND by addition. Permissions are Linux mode bits and ignored on Windows. Linux uses generated syscalls; Windows uses the generated descriptor table and bounded native counts. The generated adapter preserves the existing ABI. Use io Reader/Writer and fs RAII files for ordinary resource operations.
