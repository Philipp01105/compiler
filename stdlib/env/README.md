# Environment

Import "stdlib/env". get(name) returns Result<Bytes,EnvError>; Missing is
distinct from an empty value. set(name,value) and remove(name) return Result.
Names must be nonempty and cannot contain NUL or '='; values cannot contain NUL.
Windows uses UTF-16 wide APIs, Linux uses libc and copies retrieved values.
Errors retain native codes where available. Environment mutations are process-wide;
serialize concurrent environment access in application code.

