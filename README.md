# lunso-boot
C version of lunso compiler

Build with `make`. Native compilation uses `qbe` and `cc` from `PATH`.
Set `QBE` or `CC` to select a different executable.

```sh
./build/lunso --emit-qbe examples/answer.lunso
./build/lunso --build examples/libc.lunso -o build/libc-demo
./build/libc-demo
```

Project module names are relative to the entry file's directory. When a standard
library directory is available, `import std.io` loads `io.lunso` from that
directory and binds `io`. `import std` binds the root namespace, so its I/O
module can be reached as `std.io`.

`make STD_DIR=/path/to/std` configures the default directory. The Makefile
defaults to `std/` in this repository. When the default directory is absent,
`std` resolves through the project directory. `--std=/path/to/std` selects an
existing directory for a single invocation.

```sh
./build/lunso --std=/path/to/std --check main.lunso
```

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

Pass an object file to the native linker with `--link`. Repeat the option for
multiple inputs. This Linux x86-64 example calls a `write` syscall from an
assembly object through `ext fun`.

```sh
cc -c examples/link/linux_x86_64.s -o build/linux_x86_64.o
./build/lunso --build examples/link/main.lunso -o build/link-demo --link build/linux_x86_64.o
./build/link-demo
```
