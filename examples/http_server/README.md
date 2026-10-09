# HTTP library and Web API

`http/` is an importable HTTP package. The executable defines an in-memory item
API using its router; socket handling, parsing and response framing live in the
package. `http_server.dmm` contains the application handlers and route definitions.

Build and run from the repository root:

```powershell
.\cmake-build-debug\compiler.exe examples/http_server/http_server.dmm -o cmake-build-debug/http-server.exe
.\cmake-build-debug\http-server.exe 8080
```

```sh
./build/compiler examples/http_server/http_server.dmm -o build/http-server
./build/http-server 8080
```

The default port is 8080; port 0 selects and prints a free loopback port.
`http_server 8080 --timeout-ms 2000` changes the per-request timeout; the default
is ten seconds. CLI timeouts must be between 1 and 60000 milliseconds.

## Try the API

```sh
curl -i http://127.0.0.1:8080/health
curl -i http://127.0.0.1:8080/api/items
curl -i -H 'Content-Type: text/plain; charset=utf-8' --data-binary 'Learn DMM' http://127.0.0.1:8080/api/items
curl -i http://127.0.0.1:8080/api/items/1
curl -I http://127.0.0.1:8080/api/items/1
curl -i -X OPTIONS http://127.0.0.1:8080/api/items/1
curl -i 'http://127.0.0.1:8080/api/greet?name=DMM+developer'
curl -i -X DELETE http://127.0.0.1:8080/api/items/1
```

| Route | Behavior |
|---|---|
| `GET /` | Text greeting |
| `GET /health` | Async handler returning `ok` |
| `GET /api/items` | JSON list of all items |
| `POST /api/items` | UTF-8 title in a `text/plain` body; returns 201, a JSON item and `Location` |
| `GET /api/items/:id` | JSON item, or 404 |
| `DELETE /api/items/:id` | Removes an item; returns 204, or 404 |
| `GET /api/greet?name=...` | JSON greeting; echoes an optional `X-Request-ID` response header |

Titles contain 1..256 UTF-8 bytes. At most 1000 items live in memory; IDs increase
throughout the session. Restarting clears the state. JSON strings are escaped,
including quotes, backslashes, NUL and control characters.

## Define your own API

Inside this example module, import `examples/http_server/http`. Route handlers
receive your state and the parsed request and return an owned response:

```dmm
package main;
import ("stdlib/core" "stdlib/net" "examples/http_server/http");

struct State { var visits: u64; }

func hello(state: &mut State, request: &http.Request)
    -> core.Result<http.Response, http.Error> {
    state.visits += 1;
    return http.Response.json(http.Status.Ok, "{\"message\":\"hello\"}\n");
}

async func run() -> core.Result<void, http.Error> {
    var api = http.router<State>();
    api.get("/hello", hello)?;
    var server = http.listen(net.ipv4(2130706433, 8080), http.config())?;
    return http.serve<State>(server, api, State{visits: 0}).await();
}

func main() -> int {
    match (block_on(run())) { Ok => return 0; Err(error) => return 1; }
}
```

Use `get`, `post` and `delete` for common routes, or
`on(http.Method.Put, "/items/:id", updateItem)` for another method.
An async handler has the same parameters and return annotation with `async func`;
register it with `onAsync(http.Method.Get, "/health", health)`.
Handlers can await network I/O or timers. Pass application data through `State`.

For another module, copy `http/` and this directory's `dmm.manifest` into
`vendor/examples/http_server/`, then add this dependency to its manifest:

```text
module my/api
dmm 2026-10-04-dev
features = ["async"]
require examples/http_server v0.1.0
```

The import remains `examples/http_server/http`; the version identifies your local
vendored snapshot. Alternatively, copy `http/` directly into your own module and
import it under that module's package path. The package uses only stdlib imports.

## Package API

| Operation | Result |
|---|---|
| `http.router<State>()` | Router owned by the application |
| `router.get/post/delete(path, handler)` | Checked route registration |
| `router.on/onAsync(method, path, handler)` | Sync or async handler registration |
| `http.listen(endpoint, options)` | Bound `Server`, or typed error |
| `server.localAddress()` | Actual bound address, including the selected port |
| `http.serve<State>(server, router, state).await()` | Accept loop owning the server, routes and state |
| `http.serveOne<State>(&mut server, &mut router, &mut state).await()` | Exactly one connection; caller controls the surrounding loop and lifetime |
| `request.method()` / `request.path()` | Method and raw, query-free path |
| `request.param("id")?` | Optional owned, percent-decoded path parameter |
| `request.query("name")?` | Optional owned query value; percent decoding and `+` as space |
| `request.header("Content-Type")` | Optional borrowed UTF-8 header value; case-insensitive name lookup |
| `request.body()` / `request.bodyText()?` | Borrowed body bytes / owned, checked UTF-8 text |
| `http.Response.text/json(status, literal)?` | Text or already-encoded JSON response |
| `http.Response.ownedText/ownedJson(status, string)?` | Response from an owned `text.String` |
| `http.Response.fromBytes(status, mediaType, bytes)?` | Binary response from an owned byte list |
| `http.Response.empty(status)` | Empty response, including 204 |
| `response.setHeader(name, value)?` | Checked custom header; replaces an existing name ignoring case |
| `http.quote(text)?` / `http.appendUint(&mut builder, value)?` | JSON string escaping / decimal integer formatting |
| `http.parseRequest(&bytes, options)?` | Owned request from a complete message, without opening a socket |
| `router.dispatch(&mut request, &mut state).await()` | Route dispatch without opening a socket |
| `response.encode(head)?` | Complete HTTP response bytes, useful with another transport |

Requests own their paths, header strings and body. Header/path views borrow the
request; parameter and query results own decoded strings. Query lookup returns the
first occurrence and distinguishes a missing key from an empty value.
JSON response constructors accept already-encoded JSON; they set the media type.
Use `quote` when inserting untrusted text, as the example handlers do.

## Routing and transport behavior

Routes match a complete, case-sensitive raw path. `:name` fills one nonempty
segment. Literal matches take priority over parameter patterns; ties keep
registration order. Duplicate route shapes for a method and repeated parameter
names are rejected. Static path segments are not percent-decoded; parameters and
query values are decoded to UTF-8 when accessed.

HEAD uses an explicit HEAD handler if present, otherwise GET. Responses suppress
HEAD bodies while retaining their computed length, including errors. OPTIONS on
a known path returns 204 and `Allow` unless explicitly handled. Other methods on
a known path return 405 and `Allow`; an unknown path returns 404.

The transport accepts HTTP/1.0 and HTTP/1.1 origin-form requests, reads fragmented
headers and Content-Length bodies, handles short writes and closes each
connection after its response. HTTP/1.1 requires one nonempty Host header. Header
names are checked as tokens and values reject control characters; stored header
values must be valid UTF-8. Duplicate Content-Length, invalid lengths and
ambiguous Transfer-Encoding/Content-Length framing return 400. Excessive headers
return 431, excessive bodies return 413, Transfer-Encoding returns 501, and
Expect returns 417 before waiting for a body. Body truncation returns 400.

`http.config()` defaults to 8 KiB of headers, at most 64 header fields, a 64 KiB
body and a ten-second deadline. `maxHeaderBytes` can be 64..8192 and
`maxBodyBytes` 0..1048576. The deadline covers reads, suspended async handlers and
writes; a timed-out client is closed and the next connection can proceed.
Synchronous handler CPU work cannot be preempted. Handler input errors become
400, application/config/response errors become 500, and allocation errors stop
the accept loop. Listener failures propagate; client network errors do not stop
the loop. Response framing headers are managed by the package, and custom headers
cannot inject CR/LF. 204 and 304 responses forbid bodies and omit Content-Length.

The server handles connections sequentially with exclusive access to `State`.
It sends `Connection: close`; persistent connections, chunked bodies, TLS,
streamed responses and concurrent handlers are outside this package's current
scope. Put it behind a reverse proxy when those capabilities are needed.

`http_library_contract` builds a separate vendored consumer and this server with
O0/O1 and ELF/COFF object emission. It checks routing, state mutation, async
cancellation and framing. When Node is available it also checks the API over real
TCP, including CRUD, UTF-8, fragmentation, malformed framing, deadlines and
recovery. The full Linux suite remains a CI check.
