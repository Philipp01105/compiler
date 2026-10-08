# DMM applications

Five small applications show how current DMM fits together. Each directory is an
independent `2026-10-04-dev` module. They use ordinary language constructs and
the current stdlib; compiler edge cases belong in `tests/`.

| Application | Run it for |
|---|---|
| [HTTP server](http_server/README.md) | Async TCP, request parsing, routing and deadlines |
| [Todo CLI](todo_cli/README.md) | An interactive session with owned tasks and checked mutation |
| [JSON parser](json_parser/README.md) | Owned JSON documents, decoded values and checked key/index access |
| [Mini-Grep](mini_grep/README.md) | Streaming file/stdin processing through generic Reader/Writer contracts |
| [C interop](c_interop/README.md) | Calling C from DMM and calling an exported DMM function from C |

Build from the repository root, substituting your configured build directory:

```sh
./build/compiler examples/todo_cli/todo_cli.dmm -o build/todo_cli
./build/todo_cli
```

On Windows:

```powershell
.\cmake-build-debug\compiler.exe examples/todo_cli/todo_cli.dmm -o cmake-build-debug/todo-cli.exe
.\cmake-build-debug\todo-cli.exe
```

The other DMM applications build the same way. C interop additionally compiles
and links the accompanying C source; its README gives the complete commands.
The HTTP server runs until Ctrl+C; the Todo CLI exits on `quit` or EOF. Todo
state exists only in memory. JSON and Mini-Grep read user-supplied input and do
not modify it.

`application_examples_contract` checks CLI behavior, valid/invalid JSON, streaming
boundaries and the native C link with O0/O1 and ELF/COFF object emission. When Node
is available, it also checks generated JSON trees and real HTTP traffic against an
automatically selected loopback port. `frontend_pipeline_unit` checks all DMM example sources, including
the JSON library package.
