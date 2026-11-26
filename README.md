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

## Generics

Declare type parameters in brackets. Use `Vec[i32]` as a type and
`swap.[i32](...)` to pass an explicit type argument to a function. Type
arguments are full types, so `Vec[[4]u8]` stores four-byte arrays and
`Vec[[]u8]` stores byte slices.

```lunso
struct Vec[T] {
    data: *T
    len: i64
}

fun swap[T](left: *T, right: *T) {
    var temp = *left
    *left = *right
    *right = temp
}

fun example() {
    var a: [4]u8 = [1, 2, 3, 4]
    var b: [4]u8 = [5, 6, 7, 8]
    swap.[[4]u8](&a, &b)
}
```

The compiler checks each used specialization and reuses it for identical
type arguments. It emits concrete functions and layouts, with no runtime
type arguments. Generic functions follow the ordinary parameter rules,
so pass fixed arrays and structs through pointers or slices. Type arguments
at call sites are explicit.

## Enums

Choose an integer backing type after the name. The first member defaults to
zero and each later member defaults to the previous value plus one. A member
can set its value explicitly with an integer literal.

```lunso
enum TokenKind : u8 {
    Identifier,
    Number,
    End = 255,
}

struct Token { kind: TokenKind }
```

Use `TokenKind.Number` to get a value of type `TokenKind`. The backing type
determines its size and range. Compare values within the same enum and use
`as` for explicit conversion between an enum and an integer. An integer
cast may produce a value with no named member. Members can be separated by
commas, newlines, or semicolons. A closing brace ends a struct or enum
declaration by itself.

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

## Dynamic memory

`std.mem` provides independent arenas. Start with a zero-initialized
`mem.Arena` and call `mem.init(&arena, block_size)` to set the minimum size
of each backing block. `mem.alloc(&arena, size, align, &ptr)` writes a `*u8`
on success. Both size and alignment must be positive, and alignment must be a
power of two. Allocation leaves the output pointer untouched on failure.
Successful allocations keep their addresses as the arena grows. Their
contents may carry over from earlier use and should be initialized by the
caller.

`mem.reset(&arena)` invalidates that arena's allocations and keeps its
blocks for reuse. `mem.deinit(&arena)` releases the blocks. A successful
deinitialization leaves the arena ready for another call to `init`. Keep
each initialized arena in one place and pass its address. `init`, `alloc`,
and `deinit` return zero on success or a negative error code.

On Linux x86-64, the same platform object used by `std.io` provides the
memory mappings through direct syscalls.

```sh
make build/lunso build/std-linux-x86_64.o
./build/lunso --build examples/arena.lunso -o build/arena-demo --link build/std-linux-x86_64.o
./build/arena-demo
```

Indexing a raw pointer with `ptr[i]` uses the size of its pointee and has no
bounds check. Only a writable pointer allows stores. Casts between typed and
byte pointers preserve read-only access. `size_of(value)` produces a `u64`
byte size at compile time without evaluating the value. Use
`ptr[start..end]` to borrow a read-only slice over a valid pointer range.

`*[N]T` points to an entire fixed array. For `p: *[4]u8`, `p[1]` is the
next four-byte array and `p[1][2]` selects one byte in that array.
`&array` takes the address of the whole array, and `&array[0]` takes the
address of its first element.

## Files

`std.file` reads and writes byte sequences. Use
`file.read_all(path, &arena, &bytes)` for an entire input and
`file.write_all(path, view)` to create or truncate a file. Both close the file
before returning a byte count or a negative error code. The result of a
successful read is a `file.Bytes`. Pass `bytes.ptr[0..bytes.len]` to any
function that takes `[]u8`. The buffer belongs to the supplied arena. Reads
that outgrow their buffer leave previous buffers in that arena until reset
or deinitialization.

For streaming, start with a zero-initialized `file.File` and call
`open_read` or `open_write`. Use `read_into` with a caller-supplied buffer
or `write_to` with a byte slice, then call `close`. A read with positive
capacity returns zero at end of file. Keep one handle per open file.
`open_write` truncates an existing file. Paths are byte slices and must
contain no zero byte. The Linux platform accepts up to 4095 path bytes.

```sh
make build/lunso build/std-linux-x86_64.o
./build/lunso --build examples/file.lunso -o build/file-demo --link build/std-linux-x86_64.o
demo_exe="$PWD/build/file-demo"
demo=$(mktemp -d)
printf 'Hello from std.file\n' > "$demo/input.txt"
(cd "$demo" && "$demo_exe" && cat output.txt)
```
