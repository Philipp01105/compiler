# In-memory Todo CLI

```sh
./build/compiler examples/todo_cli/todo_cli.dmm -o build/todo_cli
./build/todo_cli
```

Enter commands, one per line:

```text
add Read the DMM examples
add Try the HTTP server
list
done 1
remove 2
list
quit
```

IDs stay stable when tasks are removed. `help` prints the commands. EOF also
ends the session. No files, database or environment variables store the tasks;
restarting the application starts an empty list.

Tasks own validated UTF-8 titles. The command reader is generic over `io.Reader`,
so a terminal or redirected input works without changing the session logic.
Commands are limited to 4096 bytes and the session to 1000 live tasks. Invalid
commands report an error and leave the session running; input/output failure
ends it with exit code 1.

On Windows, use `compiler.exe` and an `.exe` output name as shown in the
[example catalog](../README.md).
