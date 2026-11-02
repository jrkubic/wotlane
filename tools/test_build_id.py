#!/usr/bin/env python3
# ABOUTME: Tests for tools/build_id.py (the build identity the netcode compares): the same input in different folders gives the same id,
# ABOUTME: any change of code changes it, and the compiler doesn't. Run: python tools/test_build_id.py
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_id  # noqa: E402

CACHE = """# CMake cache
CMAKE_BUILD_TYPE:STRING=Release
CMAKE_CXX_COMPILER:FILEPATH=C:/VS/VC/Tools/MSVC/{ver}/bin/Hostx64/x64/cl.exe
CMAKE_CXX_FLAGS_RELEASE:STRING=/O2 /Ob2 /DNDEBUG
WOTM_NATIVE_DIR:PATH={repo}/native
"""


def make_tree(root, ver='14.44.35207', native_text='int a;\n', crlf=False):
    """Minimal repo + runtime: native/, patches, recomp/retail, PS2Recomp (HEAD in a file) and the build_dev CMakeCache."""
    repo, runtime = root / 'repo', root / 'repo' / 'runtime'
    nl = '\r\n' if crlf else '\n'
    files = {
        repo / 'native' / 'a.hpp': native_text,
        repo / 'native' / 'runtime' / 'b.inc': 'void f();\n',
        repo / 'native' / 'net' / 'build' / 'junk.obj': 'ignored\n',   # build output: not part of the id
        repo / 'tools' / 'runtime_patch' / 'wotm-runtime.patch': '+x\n',
        repo / 'recomp' / 'retail' / 'f_100.cpp': 'void f_100() {}\n',
        runtime / 'build_dev' / 'CMakeCache.txt': CACHE.format(ver=ver, repo=str(repo).replace('\\', '/')),
    }
    for p, text in files.items():
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(text.replace('\n', nl).encode())
    return repo, runtime


class BuildIdTest(unittest.TestCase):
    def compute(self, repo, runtime, commit='2c5fbb9'):
        return build_id.compute(repo, runtime, 'build_dev', ps2recomp_commit=commit)

    def test_same_sources_in_other_folders_give_the_same_id(self):
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            ida = self.compute(*make_tree(Path(a)))
            idb = self.compute(*make_tree(Path(b) / 'other folder'))
            self.assertEqual(ida, idb)
            self.assertRegex(ida, r'^wl-[0-9a-f]{16}$')

    def test_line_endings_do_not_matter(self):
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            self.assertEqual(self.compute(*make_tree(Path(a))), self.compute(*make_tree(Path(b), crlf=True)))

    def test_code_and_commit_changes_change_the_id(self):
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            base = self.compute(*make_tree(Path(a)))
            self.assertNotEqual(base, self.compute(*make_tree(Path(b), native_text='int b;\n')))
            self.assertNotEqual(base, self.compute(*make_tree(Path(a)), commit='0000000'))

    def test_the_compiler_does_not_change_the_id(self):
        # game code builds with strict FP, so Windows (MSVC) and the Mac (AppleClang, arm64) simulate alike and must be able to
        # play together; if two compilers ever disagree, the server's per-frame state hash stops the match
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b, tempfile.TemporaryDirectory() as c:
            msvc = self.compute(*make_tree(Path(a)))
            self.assertEqual(msvc, self.compute(*make_tree(Path(b), ver='14.44.35208')))
            self.assertEqual(msvc, self.compute(*make_clang_tree(Path(c), '17.0.0.17000013', 'arm64')))

    def test_build_output_is_ignored(self):
        with tempfile.TemporaryDirectory() as a:
            repo, runtime = make_tree(Path(a))
            before = self.compute(repo, runtime)
            (repo / 'native' / 'net' / 'build' / 'junk.obj').write_text('changed')
            self.assertEqual(before, self.compute(repo, runtime))
            # any build folder inside native/ (tests, future tools) is build output
            other = repo / 'native' / 'tests' / 'build' / 'x.obj'
            other.parent.mkdir(parents=True)
            other.write_text('output')
            self.assertEqual(before, self.compute(repo, runtime))

    def test_file_order_is_the_same_on_every_os(self):
        # Path objects sort case-insensitively on Windows and by path parts on POSIX: the tree digest must order files by their
        # relative path string (case-sensitive), or the same files give a different id on Windows and the Mac
        with tempfile.TemporaryDirectory() as a:
            root = Path(a)
            for rel in ('README.md', 'ps2recomp.patch', 'a-c.txt', 'a/b.txt'):
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
                (root / rel).write_text(rel)
            h = build_id.hashlib.sha256()
            for rel in sorted(('README.md', 'ps2recomp.patch', 'a-c.txt', 'a/b.txt')):
                h.update(rel.encode() + b'\0' + build_id.text_digest(root / rel).encode() + b'\n')
            self.assertEqual(build_id.tree_digest(root), h.hexdigest())


def make_clang_tree(root, ver, cpu):
    """make_tree with a clang build: CMakeCache with /usr/bin/c++ and the compiler/system files CMake writes under CMakeFiles/<ver>."""
    repo, runtime = make_tree(root)
    build = runtime / 'build_dev'
    (build / 'CMakeCache.txt').write_text('CMAKE_BUILD_TYPE:STRING=Release\nCMAKE_CXX_COMPILER:FILEPATH=/usr/bin/c++\n'
                                          'CMAKE_CXX_FLAGS_RELEASE:STRING=-O3 -DNDEBUG\n')
    files = build / 'CMakeFiles' / '4.4.5'
    files.mkdir(parents=True)
    (files / 'CMakeCXXCompiler.cmake').write_text(f'set(CMAKE_CXX_COMPILER "/usr/bin/c++")\nset(CMAKE_CXX_COMPILER_ID "AppleClang")\n'
                                                  f'set(CMAKE_CXX_COMPILER_VERSION "{ver}")\n')
    (files / 'CMakeSystem.cmake').write_text(f'set(CMAKE_SYSTEM_NAME "Darwin")\nset(CMAKE_SYSTEM_PROCESSOR "{cpu}")\n')
    return repo, runtime


if __name__ == '__main__':
    unittest.main()
