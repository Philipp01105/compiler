# Process fundamentals

Import "stdlib/process". args() returns Result<List<text.String>,Error> containing arguments after argv[0]. Startup captures argv directly on Linux and the Windows command line on Windows; no procfs dependency is used. Returned arguments, environment values and cwd are independent UTF-8 owners.

env(name) returns Result<Option<text.String>,Error>; absence is None. setEnv/removeEnv validate input and report errors. cwd()/setCwd(path) access the process directory. The library serializes environment access, cwd access and child creation with one process-global mutex. This cannot serialize foreign code that calls OS environment/cwd functions independently.

run(program,&args) executes directly without a shell, inherits the environment and cwd at creation, waits and reaps the child, and returns ExitStatus.Code(i32) or Signal(i32). Linux uses posix_spawnp and retries waitpid on EINTR. Windows uses CreateProcessW and quotes each argument for the standard CRT command-line parser, including empty arguments, quotes and trailing backslashes. Child programs with their own command-line parser may interpret that command line differently.

Error includes kind/code: InvalidInput, InvalidEncoding, OutOfMemory, CapacityOverflow, NotFound, AccessDenied or System. Normal string inputs and snapshots require valid UTF-8; native input rejects embedded NUL and invalid environment-name delimiters. Allocating operations use Result and RAII temporaries. Global synchronization initialization follows the runtime fatal resource contract.

Use [process/native](native/README.md) for byte-preserving native operations. There are no pipe/redirection or asynchronous child-process APIs.
