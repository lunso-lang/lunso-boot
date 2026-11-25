# lunso-boot
C version of lunso compiler

Build with `make`. Native compilation uses `qbe` and `cc` from `PATH`.
Set `QBE` or `CC` to select a different executable.

```sh
./build/lunso --emit-qbe examples/answer.lunso
./build/lunso --build examples/libc.lunso -o build/libc-demo
./build/libc-demo
```

Module names are relative to the entry file's directory. For example,
`import std.io` loads `std/io.lunso` and binds `io`. `import std` binds `std`,
so its I/O module can be reached as `std.io`.

```lunso
import std.*
import json.io as json_io
import std.io { Counter, value as counter_value }
```

A wildcard brings public declarations and direct child modules into scope.
Functions and structs are private by default. Use `pub` to export a declaration
or `priv` to mark its visibility explicitly.

```sh
./build/lunso --build examples/modules/main.lunso -o build/modules-demo
./build/modules-demo
```
