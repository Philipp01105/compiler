# HTTP server

This example uses the portable TCP API and async functions to serve
`http://127.0.0.1:8080`. An optional numeric port selects another port; zero asks
the OS for a free port, which the server prints. Run from the repository root,
substituting your build directory:

```powershell
.\cmake-build-debug\compiler.exe examples/http_server/http_server.dmm -o cmake-build-debug/http-server.exe
.\cmake-build-debug\http-server.exe
```

On Linux, build and run with:

```sh
./build/compiler examples/http_server/http_server.dmm -o build/http-server
./build/http-server
```

In another terminal:

```sh
curl -i http://127.0.0.1:8080/
curl -i http://127.0.0.1:8080/health
curl -I http://127.0.0.1:8080/
curl -i http://127.0.0.1:8080/missing
```

GET `/` returns `Hello from DMM!`, and GET `/health` returns `ok`.
HEAD returns the same headers without a body. Unknown paths return 404;
unsupported methods return 405 with `Allow: GET, HEAD`. Invalid request lines
return 400, and headers exceeding the 8 KiB buffer return 431.

The server accepts HTTP/1.0 and HTTP/1.1 request lines and reads until the header
terminator, including when headers arrive in separate TCP reads. Each connection
has one ten-second deadline covering reads and writes. `writeAll` handles short
writes; the owned connection closes automatically after the response or an error.
Press Ctrl+C to stop.

This is a small sequential example: it handles one connection at a time, sends
`Connection: close`, and ignores request bodies and header fields. It demonstrates
request-line parsing, rather than a complete HTTP implementation; keep-alive,
chunked encoding, TLS and full header validation are outside its scope.
