# Native process bytes

Import an explicit alias for "stdlib/process/native" (local package env). args() owns List<List<u8>>, excludes argv[0], and get(name) owns List<u8>; Missing is an Error. set/remove, cwd/setCwd and run use byte descriptors and explicit Result errors. Argument{bytes:u8[]} supplies each run argument; run waits for ExitStatus.Code/Signal.

Linux snapshots preserve arbitrary non-NUL native bytes. Windows converts the command line/environment/cwd between UTF-16 and UTF-8 with strict codecs; invalid encoding reports InvalidEncoding. Temporary owners are cleaned on allocation or native failure. No getenv pointer escapes the package. Environment/cwd access and process creation share the library mutex; independently mutating foreign code must coordinate separately.

Portable UTF-8 owners and absence-as-Option live in [process](../README.md). Native allocator handles and conversion buffers are private.
