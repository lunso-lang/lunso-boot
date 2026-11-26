# lunso-boot

Bootstrap compiler for the Lunso programming language, written in C11 using a [QBE](https://c9x.me/compile/) backend.

## Requirements

- C11 compiler (`cc`, override with `CC`)
- `qbe` in `PATH` (override with `QBE`)
- `python3` (for the test suite)

## Build & Test

```sh
make          # builds build/lunso
make test     # builds std runtime and runs tests/run.py
make fuzz     # builds and runs the ASan/UBSan fuzzer
```

## CLI

```sh
./build/lunso --run examples/arena.lunso
./build/lunso --build examples/answer.lunso -o build/answer
./build/lunso --check examples/modules/main.lunso
```

| Flag | Description |
| :--- | :--- |
| `(default)` | Dump lexer tokens |
| `--parse` | Parse and print AST summary |
| `--check` | Load modules and run semantic checks |
| `--emit-ir` | Emit Lunso IR to `stdout` |
| `--emit-qbe` | Emit QBE IL to `stdout` |
| `--emit-asm` | Emit assembly (`.s`) |
| `-c`, `--emit-obj` | Compile to an object file (`.o`) |
| `--build` | Compile and link an executable (defaults to `<stem>`) |
| `--run ... [-- args]` | Compile to a temp binary and run it |
| `--std <dir>` / `--std=<dir>` | Override standard library directory |
| `-o <file>` | Output path for `--build`, `-c`, `--emit-asm` |
| `--link <obj>` | Link an extra object or archive |
| `-nostdlib` | Link without standard C runtime / `std` auto-linking |

`std-linux-x86_64.o` is auto-linked for `--build` and `--run` when present (override with `LUNSO_STD_OBJ`).

## Language Highlights

- **Types & inference**: `int`, `i8`..`i64`, `u8`..`u64`, `f32`, `f64`, `bool`, `*T`, `*const T`, `[N]T`, `[]T`, `struct`, `enum`, `const`, `var x = expr`, explicit `as` casts, `size_of(expr)`, `len(x)`, `data(x)`.
- **Structs**: passed by value or pointer (`*T`), with auto-deref on `.` (`p.field`), respecting `*const`.
- **Generics**: monomorphized `struct Box[T]` and `fun swap[T](...)` with explicit `swap.[i32](...)` call sites.
- **Modules**: relative to the entry file (or `std.*`), `pub` / `priv` visibility, wildcards (`import mod.*`), aliases (`import mod as m`), and selective imports (`import mod { A, B as C }`).

```lunso
import std.io
import std.mem
import std.vec

struct Point {
    x: i32
    y: i32
}

fun move_by(p: *Point, dx: i32, dy: i32) {
    p.x += dx
    p.y += dy
}

fun main(): i32 {
    var arena: mem.Arena
    mem.init(&arena, 4096)

    var pts: vec.Vec[Point]
    vec.init.[Point](&pts, &arena, 4)
    vec.push_value.[Point](&pts, Point { x: 1, y: 2 })

    move_by(&pts.data[0], 3, 4)
    io.println("done")

    mem.deinit(&arena)
    return 0
}
```

## Standard Library (`std/`)

- `std.io` — `print`, `println`, `eprint`, `eprintln`, `write_bytes`, `read_into`, `read_line`
- `std.mem` — `Arena` (`init`, `alloc`, `reset`, `deinit`)
- `std.vec` — generic arena-backed `Vec[T]` (`init`, `reserve`, `push`, `push_value`, `alloc_slot`, `pop`, `get`, `clear`, `as_slice`)
- `std.file` — `read_all`, `write_all`, `File` (`open_read`, `open_write`, `read_into`, `write_to`, `close`)
- `std.fmt` — `u64_to_buf`, `i64_to_buf`, `hex_to_buf`, `format_u64`, `format_i64`, `print_u64`, `print_i64`, `parse_u64`, `parse_i64`
- `std.str` — `equals`, `starts_with`, `ends_with`, `index_of`, `last_index_of`, `trim`
- `std.args` — `load` (`/proc/self/cmdline`)
- `std.os` — `run`, `getenv`, `exit`, `access`, `mkdir`, `unlink`
- `std.time` — `Instant`, `now_realtime`, `now_monotonic`, `monotonic_ns`, `monotonic_ms`, `sleep_ms`
- `std.rand` — `fill_bytes`, `u64_entropy`, `Rng` (`init`, `seed`, `next_u64`, `range_i64`, `range_i32`)
