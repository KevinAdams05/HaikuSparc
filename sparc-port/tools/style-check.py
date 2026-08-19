#!/usr/bin/env python3
#
# Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
# Distributed under the terms of the MIT License.
#
"""Check Haiku coding style, scoped to the code this fork actually owns.

This is a *checker*, never a reformatter. Every finding is reported for a
human to act on.

The scoping is the whole point, and it is what makes this different from the
style checkers in our standalone projects. This repository is a Haiku fork:
the overwhelming majority of the tree is upstream code that we must not
restyle, because doing so would create exactly the unreviewable churn that
makes a fork expensive to merge. So:

  * A file we CREATED is checked in full.
  * A file we MODIFIED is checked only on the lines we changed. Upstream's
    pre-existing findings are not ours to report or to fix.
  * A file we have not touched is not checked at all.

That gives the same benefit a baseline file would, without a baseline file to
maintain or to quietly rot. The comparison point is `master`, which this repo
keeps as pristine upstream, so "our delta" is always well defined.

Ported donor code is exempt: per sparc-port/THIRD_PARTY.md, BSD-derived code
keeps its original formatting so it stays diff-comparable against the upstream
it came from. Mark such a file with a comment line reading exactly

    // style-check<colon> donor

(with a real colon) anywhere in its first 40 lines. The marker must be a
comment on a line of its own -- prose that merely mentions it, such as this
paragraph, must not disarm the checker.

The rule set is taken from Haiku's own src/tools/checkstyle/checkstyle.py --
including its 100-column limit, which is the real Haiku limit -- plus a few
rules from the published coding guidelines that a script can judge safely.
Rules that cannot be checked without false positives (naming conventions,
"explain why not what", const correctness) are deliberately absent: a checker
that cries wolf gets ignored, and then it is worse than nothing.

Usage:
  python3 sparc-port/tools/style-check.py              # our delta vs master
  python3 sparc-port/tools/style-check.py --ref REF    # delta vs another ref
  python3 sparc-port/tools/style-check.py --all        # whole files, not just
                                                       #   changed lines
  python3 sparc-port/tools/style-check.py PATH...      # explicit files, in full
  python3 sparc-port/tools/style-check.py --list-rules
  python3 sparc-port/tools/style-check.py --self-test

Exit status is 0 when there are no findings, 1 otherwise, so it can gate a
build or a push directly.
"""

import argparse
import os
import re
import subprocess
import sys


REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

TAB_WIDTH = 4
MAX_COLUMNS = 100
DONOR_SCAN_LINES = 40

# Built from parts so that this file's own source never contains the literal
# marker: an earlier version exempted itself, which is a memorable way to
# learn that a checker must be tested against itself.
DONOR_RE = re.compile(r"^\s*(?://|#|\*)\s*style-check" + r":\s*donor\s*$",
    re.MULTILINE)

SOURCE_EXT = (".c", ".cpp", ".h")
NATIVE_EXT = SOURCE_EXT + (".S",)
SCRIPT_EXT = (".py", ".sh")
TEXT_EXT = NATIVE_EXT + SCRIPT_EXT + (".md", ".rdef")

# Scope decides what a rule applies to, and it matters more than it looks.
# Tab indentation is a Haiku rule about Haiku's own languages; applying it to
# Python, where indentation is spaces by definition, or to a shell heredoc,
# where the "indentation" is really content, produces nothing but noise.
#   "text"   - any tracked text file, including markdown
#   "code"   - anything we would call code: C/C++/asm, shell, Python
#   "native" - C/C++/asm, the languages Haiku's tab rule is about
#   "source" - C/C++ only, where the grammar assumptions below hold
RULES = {
    "eol-crlf":
        ("text", "Line endings must be LF, not CRLF"),
    "no-final-eol":
        ("text", "File must end with a newline"),
    "trailing-space":
        ("code", "Trailing whitespace at end of line"),
    "line-too-long":
        ("code", "Line exceeds %d columns with tabs expanded to %d"
            % (MAX_COLUMNS, TAB_WIDTH)),
    "space-indent":
        ("native", "Indent with tabs, not spaces"),
    "mixed-tab-space":
        ("native", "Mixed tabs and spaces in indentation"),
    "control-space":
        ("source", "Space required after if/for/while/switch/catch"),
    "comment-space":
        ("source", "Space required after // at the start of a comment"),
    "brace-space":
        ("source", "Space required between ) and {"),
    "malformed-else":
        ("source", "else must be on the same line as the closing brace"),
    "operator-eol":
        ("source", "Wrapped lines break before a binary operator, not after"),
    "pointer-style":
        ("source", "Bind * and & to the type: write 'char* name', not 'char *name'"),
    "cast-space":
        ("source", "No space after a C-style cast"),
    "func-blank-lines":
        ("source", "Exactly two blank lines between top-level definitions"),
    "missing-copyright":
        ("code", "New file needs a copyright header"),
    "header-guard":
        ("source", "Header needs an include guard"),
}

# Purely cosmetic rules, applied only to files we wrote ourselves.
#
# On a file we merely edit, these would fire on upstream's formatting whenever
# we touch one of its lines for an unrelated reason -- and "fixing" them would
# restyle upstream code, which is precisely the churn that makes a fork
# expensive to merge (see PORTING_PLAN.md section 7). Rules that indicate a
# real defect still apply to every line we touch.
AUTHORED_ONLY_RULES = frozenset((
    "pointer-style",
    "cast-space",
    "func-blank-lines",
    "space-indent",
    "mixed-tab-space",
    "missing-copyright",
    "header-guard",
))

CONTROL_KEYWORDS = ("if", "for", "while", "switch", "catch")

# Words that can legitimately precede '(' with no space, so that control-space
# does not fire on a function called e.g. notify() or a macro named IF().
CONTROL_RE = re.compile(r"(?<![A-Za-z0-9_])(%s)\(" % "|".join(CONTROL_KEYWORDS))

COMMENT_SPACE_RE = re.compile(r"(?<!:)//[A-Za-z0-9]")
BRACE_SPACE_RE = re.compile(r"\)\{")
POINTER_RE = re.compile(r"(?<=[A-Za-z0-9_]) [*&](?=[A-Za-z_])")
CAST_SPACE_RE = re.compile(r"\((?:const\s+)?[A-Za-z_][A-Za-z0-9_:]*\s*\*?\)\s+"
    r"(?=[A-Za-z_(])")
OPERATOR_EOL_RE = re.compile(r"(?:\|\||&&|[-+*/%|&^<>=!]=?|\?)\s*$")
STRING_RE = re.compile(r'"(?:[^"\\]|\\.)*"')
CHAR_RE = re.compile(r"'(?:[^'\\]|\\.)*'")


def run_git(args):
    """Run git in the repo, or return None if this is not a usable checkout."""
    try:
        result = subprocess.run(["git"] + args, cwd=REPO, check=True,
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    except (OSError, subprocess.CalledProcessError):
        return None
    return result.stdout


def is_text_path(path):
    return path.endswith(TEXT_EXT)


def scope_of(path):
    if path.endswith(SOURCE_EXT):
        return "source"
    if path.endswith(".S"):
        return "native"
    if path.endswith(SCRIPT_EXT):
        return "code"
    return "text"


# Widest to narrowest. A rule of scope X applies to a file whose own scope is
# X or anything narrower.
SCOPE_ORDER = ("text", "code", "native", "source")


def rule_applies(rule, path):
    required = RULES[rule][0]
    actual = scope_of(path)
    return SCOPE_ORDER.index(actual) >= SCOPE_ORDER.index(required)


def changed_files(ref):
    """Files we added or modified relative to ref, plus untracked ones.

    Returns a dict of path -> set of 1-based line numbers to check, or None
    to mean "check the whole file".
    """
    files = {}

    names = run_git(["diff", "--name-status", "--diff-filter=AM",
        "%s...HEAD" % ref])
    if names is None:
        return files

    for line in names.splitlines():
        parts = line.split("\t", 1)
        if len(parts) != 2:
            continue
        status, path = parts
        if not is_text_path(path):
            continue
        if status.startswith("A"):
            files[path] = None
        else:
            files[path] = diff_line_numbers(ref, path)

    untracked = run_git(["ls-files", "--others", "--exclude-standard"])
    for path in (untracked or "").splitlines():
        if is_text_path(path):
            files[path] = None

    # Uncommitted edits to tracked files count as ours too.
    dirty = run_git(["diff", "--name-only", "HEAD"])
    for path in (dirty or "").splitlines():
        if not is_text_path(path):
            continue
        if files.get(path) is None and path in files:
            continue
        merged = files.get(path) or set()
        files[path] = merged | diff_line_numbers("HEAD", path)

    return files


def diff_line_numbers(ref, path):
    """1-based line numbers on the new side of the diff of path against ref."""
    lines = set()
    out = run_git(["diff", "-U0", ref, "--", path])
    if out is None:
        return lines
    for line in out.splitlines():
        if not line.startswith("@@"):
            continue
        match = re.search(r"\+(\d+)(?:,(\d+))?", line)
        if match is None:
            continue
        start = int(match.group(1))
        count = 1 if match.group(2) is None else int(match.group(2))
        lines.update(range(start, start + count))
    return lines


def strip_literals(line):
    """Blank out string and char literals so rules do not fire inside them."""
    line = STRING_RE.sub(lambda m: '"' + " " * (len(m.group(0)) - 2) + '"', line)
    return CHAR_RE.sub(lambda m: "'" + " " * (len(m.group(0)) - 2) + "'", line)


BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/")


def strip_comment(line):
    """Drop comments, so rules do not fire on prose.

    Block comments that open and close on one line have to go too, not just
    "//" ones: such a line ends in '/', which otherwise reads as a trailing
    division operator.
    """
    line = BLOCK_COMMENT_RE.sub(" ", line)
    index = strip_literals(line).find("//")
    return line if index < 0 else line[:index]


def expanded_width(line):
    width = 0
    for char in line:
        width += TAB_WIDTH - (width % TAB_WIDTH) if char == "\t" else 1
    return width


def is_donor(text):
    head = "\n".join(text.splitlines()[:DONOR_SCAN_LINES])
    return DONOR_RE.search(head) is not None


def check_file(path, wanted_lines, findings):
    full = os.path.join(REPO, path)
    try:
        with open(full, "r", encoding="utf-8", errors="replace") as handle:
            raw = handle.read()
    except (OSError, IsADirectoryError):
        return

    if not raw:
        return
    if is_donor(raw):
        return

    def want(number):
        return wanted_lines is None or number in wanted_lines

    authored = wanted_lines is None

    def report(number, rule, detail=""):
        if rule in AUTHORED_ONLY_RULES and not authored:
            return
        if rule_applies(rule, path) and want(number):
            findings.append((path, number, rule, detail))

    if raw.endswith("\n") is False:
        report(raw.count("\n") + 1, "no-final-eol")

    lines = raw.split("\n")
    if lines and lines[-1] == "":
        lines.pop()

    in_block_comment = False
    blank_run = 0
    for index, line in enumerate(lines, start=1):
        if line.endswith("\r"):
            report(index, "eol-crlf")
            line = line[:-1]

        if line.strip() == "":
            blank_run += 1
            if line != "":
                report(index, "trailing-space")
            continue

        if line != line.rstrip():
            report(index, "trailing-space")

        if expanded_width(line) > MAX_COLUMNS:
            report(index, "line-too-long",
                "%d columns" % expanded_width(line))

        indent = re.match(r"^[ \t]*", line).group(0)
        if " \t" in indent or (indent.startswith(" ") and "\t" in indent):
            report(index, "mixed-tab-space")
        elif indent.startswith("   "):
            report(index, "space-indent")

        # Two blank lines between top-level definitions. Only a bare closing
        # brace counts: '};' ends a class or an initialiser, where the rule
        # does not apply in the same way.
        if (blank_run and index >= 2 and lines[index - 2 - blank_run] == "}"
                and line and not line.startswith((" ", "\t"))
                and blank_run != 2 and not line.startswith("#")):
            report(index, "func-blank-lines", "%d blank lines" % blank_run)
        blank_run = 0

        if scope_of(path) != "source":
            continue

        stripped = strip_literals(line)
        if "/*" in stripped and "*/" not in stripped:
            in_block_comment = True
        if in_block_comment:
            if "*/" in stripped:
                in_block_comment = False
            continue

        if COMMENT_SPACE_RE.search(stripped):
            report(index, "comment-space")

        code = strip_comment(line)
        code_stripped = strip_literals(code)
        if not code_stripped.strip():
            continue

        # Preprocessor directives are not C expressions and must not be judged
        # as such: "#include <string.h>" ends in '>', which reads as a trailing
        # binary operator, and macro bodies routinely break lines wherever they
        # like.
        if code_stripped.lstrip().startswith("#"):
            continue

        if CONTROL_RE.search(code_stripped):
            report(index, "control-space")
        if BRACE_SPACE_RE.search(code_stripped):
            report(index, "brace-space")
        if POINTER_RE.search(code_stripped):
            report(index, "pointer-style")
        if CAST_SPACE_RE.search(code_stripped):
            report(index, "cast-space")
        if OPERATOR_EOL_RE.search(code_stripped.rstrip()):
            report(index, "operator-eol")
        if code_stripped.strip() == "else" and index >= 2 \
                and lines[index - 2].rstrip().endswith("}"):
            report(index, "malformed-else")

    if wanted_lines is None:
        check_whole_file_rules(path, raw, lines, findings)


def check_whole_file_rules(path, raw, lines, findings):
    """Rules that only make sense for a file we created outright."""
    head = "\n".join(lines[:15])
    if "Copyright" not in head and rule_applies("missing-copyright", path):
        findings.append((path, 1, "missing-copyright", ""))

    if path.endswith(".h") and rule_applies("header-guard", path):
        if "#pragma once" not in raw and not re.search(
                r"#ifndef\s+\w+\s*\n\s*#define\s+\w+", raw):
            findings.append((path, 1, "header-guard", ""))


SELF_TEST_CASES = [
    ("a.cpp", "if(x)\n", "control-space"),
    ("a.cpp", "if (x) {\n", None),
    ("a.cpp", "\tnotify(x);\n", None),
    ("a.cpp", "//comment\n", "comment-space"),
    ("a.cpp", "// comment\n", None),
    ("a.cpp", "const char* url = \"http://x\";\n", None),
    ("a.cpp", "if (x){\n", "brace-space"),
    ("a.cpp", "char *name;\n", "pointer-style"),
    ("a.cpp", "char* name;\n", None),
    ("a.cpp", "a = b *c;\n", "pointer-style"),
    ("a.cpp", "int x = 1; \n", "trailing-space"),
    ("a.cpp", "value = one +\n", "operator-eol"),
    # A directive is not an expression: this ends in '>', not an operator.
    ("a.cpp", "#include <string.h>\n", None),
    # A one-line block comment ends in '/', which is not a trailing operator.
    ("a.cpp", "/*! Checks the thing. */\n", None),
    ("a.cpp", "int x; /* note */\n", None),
    ("a.cpp", "#define JOIN(a, b) a##b\n", None),
    ("a.cpp", "value = one\n\t+ two;\n", None),
    ("a.cpp", "   int x;\n", "space-indent"),
    ("a.cpp", "\tint x;\n", None),
    ("a.S", "    nop\n", "space-indent"),
    # Python indents with spaces by definition, and a shell heredoc's leading
    # whitespace is content. Neither is a Haiku tab-rule violation.
    ("a.py", "    import os\n", None),
    ("a.sh", "    echo hello\n", None),
    ("a.cpp", "x = (char*) y;\n", "cast-space"),
    ("a.cpp", "x = (char*)y;\n", None),
    ("a.cpp", "int x;", "no-final-eol"),
    ("a.cpp", "\t" * 2 + "x" * 200 + "\n", "line-too-long"),
    ("a.md", "\t" * 2 + "x" * 200 + "\n", None),
    ("a.cpp", "// style-check: donor\nif(x)\n", None),
    # Prose that merely mentions the marker must not disarm the checker.
    ("a.cpp", "// see style-check: donor for details\nif(x)\n", "control-space"),
]


def self_test():
    import tempfile
    failures = 0
    for name, text, expected in SELF_TEST_CASES:
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, name)
            with open(path, "w") as handle:
                handle.write(text)
            global REPO
            saved = REPO
            REPO = directory
            findings = []
            check_file(name, None, findings)
            REPO = saved
        rules = {finding[2] for finding in findings}
        rules.discard("missing-copyright")
        ok = (expected in rules) if expected else not rules
        if not ok:
            failures += 1
            print("FAIL %-14s %-32r expected=%s got=%s"
                % (name, text, expected, sorted(rules) or "none"))
    total = len(SELF_TEST_CASES)
    print("self-test: %d/%d passed" % (total - failures, total))
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(add_help=True,
        description="Check Haiku coding style on this fork's own code.")
    parser.add_argument("paths", nargs="*",
        help="specific files to check in full; default is our delta vs master")
    parser.add_argument("--ref", default="master",
        help="compare against this ref instead of master")
    parser.add_argument("--all", action="store_true",
        help="check changed files in full, not only the lines we changed")
    parser.add_argument("--list-rules", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.list_rules:
        for name in sorted(RULES):
            scope, description = RULES[name]
            print("%-20s %-7s %s" % (name, scope, description))
        return 0

    if args.self_test:
        return self_test()

    if args.paths:
        targets = {}
        for path in args.paths:
            relative = os.path.relpath(os.path.abspath(path), REPO)
            targets[relative] = None
    else:
        targets = changed_files(args.ref)
        if args.all:
            targets = {path: None for path in targets}

    findings = []
    for path in sorted(targets):
        check_file(path, targets[path], findings)

    for path, number, rule, detail in findings:
        suffix = "  (%s)" % detail if detail else ""
        print("%s:%d: %s -- %s%s"
            % (path, number, rule, RULES[rule][1], suffix))

    scope = "%d file(s)" % len(targets)
    if findings:
        print("\n%d finding(s) across %s" % (len(findings), scope))
        return 1
    print("style-check: clean (%s)" % scope)
    return 0


if __name__ == "__main__":
    sys.exit(main())
