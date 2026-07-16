#!/usr/bin/env python3
"""
add_license_headers.py - Idempotently prepend MIT license headers to all
.c, .h, and .asm source files in src/, include/, user/, tests/stubs/, and
tests/unit/.  Run from the repository root:

    python3 scripts/add_license_headers.py
"""

import os

C_HEADER = """\
// MIT License
//
// Copyright (c) 2026 Christian Spoo
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

"""

ASM_HEADER = """\
; MIT License
;
; Copyright (c) 2026 Christian Spoo
;
; Permission is hereby granted, free of charge, to any person obtaining a copy
; of this software and associated documentation files (the "Software"), to deal
; in the Software without restriction, including without limitation the rights
; to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
; copies of the Software, and to permit persons to whom the Software is
; furnished to do so, subject to the following conditions:
;
; The above copyright notice and this permission notice shall be included in all
; copies or substantial portions of the Software.
;
; THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
; IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
; FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
; AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
; LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
; OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
; SOFTWARE.

"""

SCAN_DIRS = [
    "src",
    "include",
    "user",
    "tests/stubs",
    "tests/unit",
]

SKIP_DIRS = [
    "tests/vendor",
]

EXTENSIONS = {".c", ".h", ".asm"}


def collect_files():
    files = []
    for base in SCAN_DIRS:
        for root, dirs, names in os.walk(base):
            # Normalize root path for skip comparison
            norm_root = root.replace("\\", "/")
            skip = False
            for sd in SKIP_DIRS:
                if norm_root == sd or norm_root.startswith(sd + "/"):
                    skip = True
                    break
            if skip:
                dirs[:] = []  # don't descend
                continue
            for name in names:
                ext = os.path.splitext(name)[1].lower()
                if ext in EXTENSIONS:
                    files.append(os.path.join(root, name))
    return sorted(files)


def process_file(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        content = f.read()

    first_line = content.split("\n", 1)[0]
    if "MIT License" in first_line:
        print(f"SKIP: {path}")
        return False

    ext = os.path.splitext(path)[1].lower()
    if ext == ".asm":
        header = ASM_HEADER
    else:
        header = C_HEADER

    with open(path, "w", encoding="utf-8") as f:
        f.write(header + content)

    print(f"OK: {path}")
    return True


def main():
    files = collect_files()
    updated = 0
    skipped = 0
    for path in files:
        if process_file(path):
            updated += 1
        else:
            skipped += 1
    print(f"\nDone. {updated} files updated, {skipped} files skipped.")


if __name__ == "__main__":
    main()
