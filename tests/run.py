#!/usr/bin/env python3
"""File-based test runner for the lunso-boot compiler and standard library."""

from __future__ import annotations

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
TESTS_DIR = ROOT / "tests"
LUNSO = ROOT / "build" / "lunso"
STD_OBJ = ROOT / "build" / "std-linux-x86_64.o"
STD_DIR = ROOT / "std"


def run_cmd(args: list[str | Path], cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(arg) for arg in args],
        cwd=str(cwd or ROOT),
        text=True,
        capture_output=True,
        check=False,
    )


def parse_directives(path: Path) -> dict[str, str]:
    directives: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.startswith("//"):
            break
        body = line[2:].strip()
        if ":" in body:
            key, value = body.split(":", 1)
            directives[key.strip()] = value.strip()
    return directives


def unescape_text(value: str) -> str:
    return value.encode("utf-8").decode("unicode_escape")


class TestPassPrograms(unittest.TestCase):
    def _run_pass_file(self, path: Path, *, link_std: bool = False) -> None:
        directives = parse_directives(path)
        expected_exit = int(directives.get("expect-exit", "0"))
        expected_stdout = directives.get("expect-stdout")

        with tempfile.TemporaryDirectory() as tmp:
            workdir = Path(tmp)
            exe_path = workdir / "prog"
            cmd: list[str | Path] = [LUNSO, "--build", path, "-o", exe_path]
            if link_std:
                cmd.extend(["--link", STD_OBJ])

            build_res = run_cmd(cmd, cwd=workdir)
            self.assertEqual(
                build_res.returncode,
                0,
                f"build failed for {path.relative_to(ROOT)}:\n{build_res.stderr}",
            )

            run_res = run_cmd([exe_path], cwd=workdir)
            self.assertEqual(
                run_res.returncode,
                expected_exit,
                f"unexpected exit code for {path.relative_to(ROOT)}: "
                f"got {run_res.returncode}, expected {expected_exit}\nstderr:\n{run_res.stderr}",
            )
            if expected_stdout is not None:
                self.assertEqual(run_res.stdout, unescape_text(expected_stdout))

    def test_pass_files(self) -> None:
        for lunso_file in sorted((TESTS_DIR / "pass").glob("*.lunso")):
            with self.subTest(test=lunso_file.relative_to(ROOT)):
                self._run_pass_file(lunso_file)

    def test_modules(self) -> None:
        self._run_pass_file(TESTS_DIR / "modules" / "main.lunso")

    def test_std_files(self) -> None:
        for lunso_file in sorted((TESTS_DIR / "std").glob("*.lunso")):
            with self.subTest(test=lunso_file.relative_to(ROOT)):
                self._run_pass_file(lunso_file, link_std=True)


class TestFailPrograms(unittest.TestCase):
    def test_fail_cases(self) -> None:
        fail_dir = TESTS_DIR / "fail"
        cases = list(sorted(fail_dir.glob("*.lunso")))
        cases.extend(sorted(p / "main.lunso" for p in fail_dir.iterdir() if p.is_dir()))

        for lunso_file in cases:
            with self.subTest(test=lunso_file.relative_to(ROOT)):
                directives = parse_directives(lunso_file)
                mode = directives.get("mode", "check")
                mode_flag = {
                    "tokens": [],
                    "parse": ["--parse"],
                    "check": ["--check"],
                }[mode]

                res = run_cmd([LUNSO, *mode_flag, lunso_file])
                self.assertNotEqual(
                    res.returncode,
                    0,
                    f"expected failure for {lunso_file.relative_to(ROOT)}, but exited 0",
                )
                expected_stderr = directives.get("expect-stderr")
                if expected_stderr is not None:
                    self.assertIn(expected_stderr, res.stderr)
                else:
                    self.assertTrue(res.stderr.strip())


class TestCliAndFrontEnd(unittest.TestCase):
    def test_modes_on_answer_example(self) -> None:
        sample = ROOT / "examples" / "answer.lunso"

        tok_res = run_cmd([LUNSO, sample])
        self.assertEqual(tok_res.returncode, 0, tok_res.stderr)
        self.assertIn("1:1 FUN", tok_res.stdout)
        self.assertIn("EOF", tok_res.stdout)

        parse_res = run_cmd([LUNSO, "--parse", sample])
        self.assertEqual(parse_res.returncode, 0, parse_res.stderr)
        self.assertIn("Parsed 2 function(s)", parse_res.stdout)

        check_res = run_cmd([LUNSO, "--check", sample])
        self.assertEqual(check_res.returncode, 0, check_res.stderr)
        self.assertIn("Checked 2 function(s)", check_res.stdout)

        ir_res = run_cmd([LUNSO, "--emit-ir", sample])
        self.assertEqual(ir_res.returncode, 0, ir_res.stderr)
        self.assertIn("main", ir_res.stdout)

        qbe_res = run_cmd([LUNSO, "--emit-qbe", sample])
        self.assertEqual(qbe_res.returncode, 0, qbe_res.stderr)
        self.assertIn("export function", qbe_res.stdout)
        self.assertIn("$main", qbe_res.stdout)

    def test_cli_validation_errors(self) -> None:
        no_args = run_cmd([LUNSO])
        self.assertNotEqual(no_args.returncode, 0)
        self.assertIn("usage:", no_args.stderr)

        empty_std = run_cmd([LUNSO, "--std=", "--check", ROOT / "examples" / "answer.lunso"])
        self.assertNotEqual(empty_std.returncode, 0)
        self.assertIn("--std requires a directory path", empty_std.stderr)

        dup_std = run_cmd(
            [LUNSO, f"--std={STD_DIR}", f"--std={STD_DIR}", "--check", ROOT / "examples" / "answer.lunso"]
        )
        self.assertNotEqual(dup_std.returncode, 0)
        self.assertIn("--std can only be supplied once", dup_std.stderr)

        missing_out = run_cmd([LUNSO, "--build", ROOT / "examples" / "answer.lunso"])
        self.assertNotEqual(missing_out.returncode, 0)

        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "same.lunso"
            src.write_text((ROOT / "examples" / "answer.lunso").read_text(encoding="utf-8"), encoding="utf-8")
            overlap = run_cmd([LUNSO, "--build", src, "-o", src])
            self.assertNotEqual(overlap.returncode, 0)
            self.assertIn("output would overwrite the source file", overlap.stderr)


class TestExamples(unittest.TestCase):
    def test_examples_check(self) -> None:
        for lunso_file in sorted((ROOT / "examples").rglob("*.lunso")):
            if lunso_file.relative_to(ROOT) == Path("examples/main.lunso"):
                continue
            with self.subTest(example=lunso_file.relative_to(ROOT)):
                res = run_cmd([LUNSO, "--check", lunso_file])
                self.assertEqual(res.returncode, 0, res.stderr)

    def test_answer_example_runs(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "answer"
            build = run_cmd([LUNSO, "--build", ROOT / "examples" / "answer.lunso", "-o", exe])
            self.assertEqual(build.returncode, 0, build.stderr)
            run = run_cmd([exe])
            self.assertEqual(run.returncode, 42)

    def test_inventory_example_runs(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "inventory"
            build = run_cmd([LUNSO, "--build", ROOT / "examples" / "inventory.lunso", "-o", exe])
            self.assertEqual(build.returncode, 0, build.stderr)
            run = run_cmd([exe])
            self.assertEqual(run.returncode, 0)
            self.assertEqual(run.stdout.strip(), "80")

    def test_modules_example_runs(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "modules"
            build = run_cmd([LUNSO, "--build", ROOT / "examples" / "modules" / "main.lunso", "-o", exe])
            self.assertEqual(build.returncode, 0, build.stderr)
            run = run_cmd([exe])
            self.assertEqual(run.returncode, 0)
            self.assertEqual(run.stdout, "OK\n")

    def test_arena_example_runs(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "arena"
            build = run_cmd(
                [LUNSO, "--build", ROOT / "examples" / "arena.lunso", "-o", exe, "--link", STD_OBJ]
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            run = run_cmd([exe])
            self.assertEqual(run.returncode, 0, run.stderr)

    def test_file_example_runs(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            workdir = Path(tmp)
            exe = workdir / "file-demo"
            build = run_cmd(
                [LUNSO, "--build", ROOT / "examples" / "file.lunso", "-o", exe, "--link", STD_OBJ]
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            (workdir / "input.txt").write_text("Hello from std.file\n", encoding="utf-8")
            run = run_cmd([exe], cwd=workdir)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertEqual((workdir / "output.txt").read_text(encoding="utf-8"), "Hello from std.file\n")


if __name__ == "__main__":
    if not LUNSO.exists() or not STD_OBJ.exists():
        print("missing build artifacts; run `make all build/std-linux-x86_64.o` first", file=sys.stderr)
        sys.exit(1)
    unittest.main(verbosity=2 if "-v" in sys.argv else 1)
