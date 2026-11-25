# lunso-boot
C version of lunso compiler

Build with `make`. Native compilation uses `qbe` and `cc` from `PATH`.
Set `QBE` or `CC` to select a different executable.

```sh
./build/lunso --emit-qbe examples/answer.lunso
./build/lunso --build examples/libc.lunso -o build/libc-demo
./build/libc-demo
```

## Local variables and numeric promotion

`var name = expression` infers a local's type from its initializer. Untyped
integer literals default to `i32` or `i64` according to their range, and untyped
decimal literals default to `f64`. A string literal still has type `[N]u8`.

```lunso
fun widen(value: u32): i64 {
    return value
}

fun total(value: u32, adjustment: i32): i64 {
    var sum = value + adjustment
    return sum
}
```

Initializers, assignments, arguments, and returns allow implicit widening when
the destination can represent every value of the source type. Arithmetic and
comparisons use a common type for both operands. For example, `u32 + i32`
uses `i64`, and `f32` promotes to `f64`. Shifts keep the type of the left
operand and accept any integer type as the count. Integer and floating-point
types stay separate. Use an explicit cast such as `value as i32` for integer
conversions that can lose values.

## Modules and I/O

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
import support.*
import json.io as json_io
import support.io { Counter, value as counter_value }
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

On Linux x86-64, `std.io` uses direct syscalls from `build/std-linux-x86_64.o`.
`print` and `println` write to standard output. `write_bytes` takes a file
descriptor, and `read_into` reads standard input into a caller-supplied buffer.
The functions return the number of bytes transferred or a negative error code.

```sh
make build/lunso build/std-linux-x86_64.o
./build/lunso --build examples/io.lunso -o build/io-demo --link build/std-linux-x86_64.o
./build/io-demo
```

Use `data(array_or_slice)` to borrow a read-only pointer to the first element.
For example, the result for `[]u8` has type `*const u8`.
