"""Format DMM sources without changing tokens, comments or literal contents.

No parser is required: deliberately invalid diagnostic fixtures are supported.
Generated sources can use format_source before writing their output.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
from collections import Counter
from pathlib import Path
import re
import subprocess


@dataclass(frozen=True)
class Token:
    text: str
    kind: str
    start: int
    end: int


OPERATORS = ("=>", "==", "!=", "<=", ">=", "&&", "||", "+=", "-=", "*=", "/=", "++", "--", "->")
PREFIX_PRECEDERS = {"", "(", "[", ",", ":", "=", "return", "=>", "->", "&", "*",
                    "&&", "||", "==", "!=", "<", ">", "<=", ">=", "+", "-", "/", "%", "|", "^"}
WORD = re.compile(r"[\w]+", re.UNICODE)
NUMBER = re.compile(r"(?:\d+(?:\.(?!\()\d*)*|\.\d+)(?:[eE][+-]?\d*)?")


def tokenize(source: str) -> list[Token]:
    tokens = []
    position = 0
    while position < len(source):
        start = position
        char = source[position]
        if char.isspace():
            position += 1
            continue
        if source.startswith("//", position):
            end = source.find("\n", position)
            position = len(source) if end < 0 else end
            kind = "comment"
        elif source.startswith("/*", position):
            end = source.find("*/", position + 2)
            position = len(source) if end < 0 else end + 2
            kind = "comment"
        elif char == "'" and tokens and tokens[-1].text in ("&", "<", ",") and re.match(r"'[A-Za-z_]\w*", source[position:]) and not re.match(r"'[A-Za-z_]\w*'", source[position:]):
            position += len(re.match(r"'[A-Za-z_]\w*", source[position:]).group())
            kind = "lifetime"
        elif char in "\"'":
            position += 1
            while position < len(source) and source[position] not in "\r\n":
                if source[position] == "\\" and position + 1 < len(source) and source[position + 1] not in "\r\n":
                    position += 2
                elif source[position] == char:
                    position += 1
                    break
                else:
                    position += 1
            kind = "literal" if source[position - 1] == char and position > start + 1 else "unclosed"
        elif char.isdigit() or (char == "." and position + 1 < len(source) and source[position + 1].isdigit()):
            match = NUMBER.match(source, position)
            position = match.end()
            kind = "number"
        elif WORD.match(source, position):
            position = WORD.match(source, position).end()
            kind = "word"
        else:
            operator = next((value for value in OPERATORS if source.startswith(value, position)), char)
            position += len(operator)
            kind = "punctuation"
        tokens.append(Token(source[start:position], kind, start, position))
    return tokens


def generic_pairs(tokens: list[Token]) -> dict[int, int]:
    pairs = {}
    for index, token in enumerate(tokens):
        if token.text != "<" or index == 0 or tokens[index - 1].kind != "word":
            continue
        depth = 1
        for end in range(index + 1, len(tokens)):
            text = tokens[end].text
            if text in (";", "{", "}", "=", "&&", "||", "=>"):
                break
            if text == "<":
                depth += 1
            elif text in (">", ">="):
                depth -= 1
                if depth == 0:
                    after = tokens[end + 1].text if end + 1 < len(tokens) else ""
                    # A lower-case expression like a < b > c is not a type.
                    before = tokens[index - 1].text
                    type_hint = before[:1].isupper() or after == "(" or (index > 1 and tokens[index - 2].text in (":", "->", "func", "struct", "enum", "interface", "."))
                    if type_hint and (text == ">=" or after in ("", "(", ")", "[", "]", "{", "}", ".", ",", ";", ":", "=", "->", ">", "where")):
                        pairs[index] = end
                        pairs[end] = index
                    break
    return pairs


@dataclass
class Context:
    kind: str
    multiline: bool = False
    for_header: bool = False


def flat_width(tokens: list[Token], angles: dict[int, int], start: int, end: int) -> int:
    """Measure the compact spelling of a delimiter group, without source gaps."""
    size = 0
    previous = ""
    unary = False
    groups = []
    binary = {"=", "+", "-", "*", "/", "%", "==", "!=", "<", ">", "<=", ">=",
              "&&", "||", "+=", "-=", "*=", "/=", "->", "=>", "|", "^"}
    for index in range(start, end):
        text = tokens[index].text
        space = bool(previous) and previous not in ("(", "[", "{", ":", ".", "@") and not unary
        if previous == "<" and index - 1 in angles:
            space = False
        if previous == "," and groups and groups[-1] in ("angle", "typeparen"):
            space = False
        if text in (")", "]", "}", ",", ";", ":", ".", "++", "--", "?") or index in angles:
            space = False
        if text == "(":
            space = previous in ("if", "while", "for", "match", "switch", "import", "return", ",", "=", "=>", "&&", "||")
            groups.append("typeparen" if previous == "func" else "paren")
        elif text == "[":
            space = previous in ("=", "return")
            groups.append("index")
        elif text == "{":
            space = previous == ")"
            groups.append("initializer")
        elif text == "<" and index in angles:
            groups.append("angle")
        elif text in (")", "]", "}") or (text in (">", ">=") and index in angles):
            if groups:
                groups.pop()
        is_unary = text in ("!", "~", "@") or (text in ("&", "*", "+", "-") and
                   (previous in PREFIX_PRECEDERS or unary))
        if is_unary:
            space = previous in ("=", "return", "=>", "->", "&&", "||") or (previous == "," and not (groups and groups[-1] in ("angle", "typeparen")))
        elif text in binary and index not in angles:
            space = bool(previous)
        if previous in binary and index - 1 not in angles and not unary:
            space = True
        if previous == ">=" and index - 1 in angles:
            space = True
        size += len(text) + int(space)
        unary = is_unary
        previous = text
    return size


def return_type_end(tokens: list[Token], angles: dict[int, int], start: int) -> int:
    depth = 0
    end = start + 1
    while end < len(tokens):
        text = tokens[end].text
        if depth == 0 and text in ("{", ";", "=", ",", ")", "}"):
            break
        if text in ("(", "[") or (text == "<" and end in angles):
            depth += 1
        elif text in (")", "]") or (text in (">", ">=") and end in angles):
            depth -= 1
        end += 1
    return end


def expanded_groups(tokens: list[Token], angles: dict[int, int]) -> set[int]:
    """Propagate mandatory line breaks through enclosing expression lists."""
    expanded = set()
    stack = []
    closers = {"(": ")", "[": "]", "{": "}", "<": ">"}
    for index, token in enumerate(tokens):
        text = token.text
        if text in ("(", "[", "{") or (text == "<" and index in angles):
            stack.append([index, text, 0, 0, False])
        elif stack and (text == closers[stack[-1][1]] or (text == ">=" and stack[-1][1] == "<" and index in angles)):
            opening, kind, commas, colons, hard = stack.pop()
            nonempty = index > opening + 1
            previous = tokens[opening - 1].text if opening else ""
            trailing = tokens[index - 1].text == "," if nonempty else False
            elements = int(nonempty) + commas - int(trailing)
            literal_array = kind == "[" and previous in ("", "=", "return", "(", ",", "[", ":", "=>")
            hard = hard or (kind == "{" and nonempty and (colons == 0 or colons > 3)) or (literal_array and elements > 3)
            if hard and kind != "<" and not (kind == "[" and previous == "@"):
                expanded.add(opening)
                if stack:
                    stack[-1][4] = True
        elif stack:
            if text == ",":
                stack[-1][2] += 1
            elif text == ":":
                stack[-1][3] += 1
            elif token.kind == "comment":
                stack[-1][4] = True
    return expanded


def normalize_imports(source: str) -> str:
    """Group complete import declarations, retaining each entry and its comments.

    A malformed declaration makes the entire import region opaque to sorting.
    Adjacent leading comments move with their entry; separated headers stay put.
    """
    tokens = tokenize(source)
    starts = [i for i, t in enumerate(tokens) if t.text == "import"]
    if not starts:
        return source
    first = starts[0]
    i = first
    entries = []
    pending = []
    region_start = tokens[first].start
    grouped_header = i + 1 < len(tokens) and tokens[i + 1].text == "("
    while not grouped_header and first > 0 and tokens[first - 1].kind == "comment" and not re.search(r"\n\s*\n", source[tokens[first - 1].end:region_start]):
        first -= 1
        pending.insert(0, tokens[first].text)
        region_start = tokens[first].start
    last_end = tokens[first].start
    while i < len(tokens):
        if tokens[i].kind == "comment":
            pending.append(tokens[i].text)
            i += 1
            continue
        if tokens[i].text != "import":
            break
        i += 1
        grouped = i < len(tokens) and tokens[i].text == "("
        if grouped:
            i += 1
        count = 0
        while i < len(tokens):
            if tokens[i].kind == "comment":
                pending.append(tokens[i].text)
                i += 1
                continue
            if grouped and tokens[i].text == ")":
                if pending and entries:
                    entries[-1][1].extend(pending)
                    pending = []
                i += 1
                break
            alias = ""
            if tokens[i].kind == "word":
                alias = tokens[i].text + " "
                i += 1
            if i >= len(tokens) or tokens[i].kind != "literal" or not tokens[i].text.startswith('"'):
                return source
            path = tokens[i].text
            end = tokens[i].end
            i += 1
            trailing = []
            if i < len(tokens) and tokens[i].kind == "comment" and "\n" not in source[end:tokens[i].start]:
                trailing.append(tokens[i].text)
                i += 1
            entries.append((path, pending, alias + path, trailing))
            pending = []
            count += 1
            if not grouped:
                break
        if not count or i >= len(tokens) or tokens[i].text != ";":
            return source
        last_end = tokens[i].end
        i += 1
        if i < len(tokens) and tokens[i].kind == "comment" and "\n" not in source[last_end:tokens[i].start]:
            if entries[-1][3]:
                entries[-1][1].append(tokens[i].text)
            else:
                entries[-1][3].append(tokens[i].text)
            last_end = tokens[i].end
            i += 1
    # Imports separated by declarations cannot safely be moved across them.
    if any(t.text == "import" for t in tokens[i:]):
        return source
    entries.sort(key=lambda entry: (not entry[0].startswith('"stdlib/'), entry[0]))
    lines = ["import ("]
    was_stdlib = False
    for path, comments, entry, trailing in entries:
        stdlib = path.startswith('"stdlib/')
        if was_stdlib and not stdlib:
            lines.append("")
        lines.extend("    " + comment for comment in comments)
        lines.append("    " + entry + (" " + " ".join(trailing) if trailing else ""))
        was_stdlib = stdlib
    lines.append(");")
    replacement = "\n".join(lines)
    syntax = {"import", "(", ")", ";"}
    before = Counter((t.kind, t.text) for t in tokenize(source[region_start:last_end]) if t.text not in syntax)
    after = Counter((t.kind, t.text) for t in tokenize(replacement) if t.text not in syntax)
    if before != after:
        raise ValueError("Import normalization would change entries or comments")
    return source[:region_start] + replacement + source[last_end:]


class Writer:
    def __init__(self):
        self.lines = []
        self.line = ""
        self.indent = 0

    def put(self, text: str, space: bool = False):
        if not self.line:
            self.line = "    " * self.indent
        # Adjacent unary punctuation must not become a different operator.
        if self.line and text and self.line[-1] + text[0] in (*OPERATORS, "//", "/*"):
            space = True
        if space and self.line.strip() and not self.line.endswith(" "):
            self.line += " "
        self.line += text

    def newline(self, blank: bool = False, preserve: bool = False):
        if self.line.strip():
            self.lines.append(self.line if preserve else self.line.rstrip())
        self.line = ""
        if blank and self.lines and self.lines[-1] != "":
            self.lines.append("")

    def result(self):
        self.newline()
        while self.lines and self.lines[-1] == "":
            self.lines.pop()
        return "\n".join(self.lines) + "\n"


def wrap_expression_lines(source: str, width: int) -> str:
    """Wrap remaining long binary expressions at existing operator spaces."""
    lines = []
    binary = {"&&", "||", "+", "-", "*", "/", "%", "==", "!=", "<", ">", "<=", ">=", "|", "^"}
    protected = set()
    for token in tokenize(source):
        if token.kind == "comment":
            protected.update(range(source.count("\n", 0, token.start), source.count("\n", 0, token.end) + 1))
    for number, line in enumerate(source.split("\n")[:-1]):
        indent = len(line) - len(line.lstrip())
        continuation = " " * (indent + 4)
        while number not in protected and len(line) > width:
            tokens = tokenize(line)
            if any(t.kind == "comment" or "\n" in t.text for t in tokens):
                break
            candidates = [t for t in tokens if t.text in binary and t.start > indent
                          and line[t.start - 1].isspace() and line[t.end:t.end + 1].isspace()]
            fitting = [t for t in candidates if t.start <= width]
            logical = [t for t in fitting if t.text in ("&&", "||")]
            if not fitting:
                break
            point = (logical or fitting)[-1].start
            lines.append(line[:point].rstrip())
            line = continuation + line[point:]
            indent = len(continuation)
        lines.append(line)
    return "\n".join(lines) + "\n"


def format_source(source: str, width: int = 100) -> str:
    source = source.replace("\r\n", "\n")
    source = normalize_imports(source)
    tokens = tokenize(source)
    angles = generic_pairs(tokens)
    expanded = expanded_groups(tokens, angles)
    writer = Writer()
    contexts = []
    previous = ""
    statement_start = 0
    unary = False
    pending_blank = False
    for index, token in enumerate(tokens):
        text = token.text
        next_text = tokens[index + 1].text if index + 1 < len(tokens) else ""
        gap = source[tokens[index - 1].end:token.start] if index else source[:token.start]
        if re.search(r"\n\s*\n", gap) and writer.line == "" and not (contexts and contexts[-1].kind in ("paren", "typeparen", "angle", "declaration")):
            writer.newline(blank=True)
        if token.kind == "comment":
            inline = index > 0 and "\n" not in gap
            if not inline:
                writer.newline()
            writer.put(text, space=inline)
            writer.newline(blank=pending_blank, preserve=True)
            pending_blank = False
            continue
        if token.kind == "unclosed":
            writer.put(text, space=True)
            writer.newline(preserve=True)
            previous = text
            continue
        if text == "{":
            head = []
            annotation_depth = 0
            for item in tokens[statement_start:index]:
                if item.kind == "comment" or item.text == "@":
                    continue
                if item.text == "[":
                    annotation_depth += 1
                if not annotation_depth:
                    head.append(item.text)
                if item.text == "]":
                    annotation_depth = max(0, annotation_depth - 1)
            declaration_head = [value for value in head if value not in ("pub", "async", "unsafe", "static", "mut")]
            declaration = bool(declaration_head) and declaration_head[0] in ("struct", "union", "enum", "interface", "extern", "destructor", "func")
            function = declaration and declaration_head[0] in ("func", "destructor")
            initializer = not declaration and (tokens[index - 1].kind == "word" or previous in (">", "]")) and previous not in ("else", "do", "defer")
            # Count only this initializer's attributes, not nested values.
            depth = 0
            fields = 0
            closing = index
            for closing in range(index + 1, len(tokens)):
                part = tokens[closing].text
                if part == "}" and depth == 0:
                    break
                if part == ":" and depth == 0:
                    fields += 1
                if part in ("{", "(", "["):
                    depth += 1
                elif part in ("}", ")", "]"):
                    depth -= 1
            span = flat_width(tokens, angles, index, closing + 1)
            multiline = not initializer or index in expanded or fields > 3 or span + len(writer.line) > width
            empty = next_text == "}"
            kind = "initializer" if initializer else ("function" if function else "declaration" if declaration else "block")
            contexts.append(Context(kind, multiline and not empty))
            writer.put("{", space=not initializer)
            if multiline and not empty:
                writer.newline()
                writer.indent += 1
            statement_start = index + 1
        elif text == "}":
            context = contexts.pop() if contexts and contexts[-1].kind in ("initializer", "block", "declaration", "function") else Context("block")
            if context.multiline:
                writer.newline()
                writer.indent = max(0, writer.indent - 1)
                while writer.lines and writer.lines[-1] == "":
                    writer.lines.pop()
            writer.put("}")
            if context.kind != "initializer" and next_text not in (";", ",", ")", "]", ".", "else", "("):
                blank = context.kind in ("declaration", "function") or not contexts
                trailing_comment = index + 1 < len(tokens) and tokens[index + 1].kind == "comment" and "\n" not in source[token.end:tokens[index + 1].start]
                if trailing_comment:
                    pending_blank = blank
                else:
                    writer.newline(blank=blank)
            statement_start = index + 1
        elif text == "(":
            group = previous == "import"
            depth = 0
            closing = index
            for closing in range(index + 1, len(tokens)):
                part = tokens[closing].text
                if part == ")" and depth == 0:
                    break
                if part == "(":
                    depth += 1
                elif part == ")":
                    depth -= 1
            span = flat_width(tokens, angles, index, closing + 1)
            # Include the return suffix when deciding whether parameters fit.
            suffix = 2 if closing + 1 < len(tokens) and tokens[closing + 1].text == "{" else 1
            if closing + 1 < len(tokens) and tokens[closing + 1].text == "->":
                end = return_type_end(tokens, angles, closing + 1)
                suffix = 1 + flat_width(tokens, angles, closing + 1, end)
                if end < len(tokens) and tokens[end].text == "{":
                    suffix += 2
            typeparen = previous == "func"
            opening_space = int(previous in ("if", "while", "for", "match", "switch", "import", "return", ",", "=", "=>", "&&", "||"))
            multiline = group or index in expanded or (not typeparen and next_text != ")" and previous != "for" and span + suffix + opening_space + len(writer.line) > width)
            contexts.append(Context("imports" if group else "typeparen" if typeparen else "paren", multiline, previous == "for"))
            writer.put("(", space=previous in ("if", "while", "for", "match", "switch", "import", "return", ",", "=", "=>", "&&", "||"))
            if multiline:
                writer.newline()
                writer.indent += 1
        elif text == ")":
            context = contexts.pop() if contexts and contexts[-1].kind in ("paren", "typeparen", "imports") else Context("paren")
            if context.multiline:
                writer.newline()
                writer.indent = max(0, writer.indent - 1)
            writer.put(")")
        elif text == "[":
            annotation = previous == "@"
            literal = previous in ("", "=", "return", "(", ",", "[", ":", "=>") or (previous == ">=" and index - 1 in angles)
            depth = 0
            elements = 1 if next_text != "]" else 0
            closing = index
            for closing in range(index + 1, len(tokens)):
                part = tokens[closing].text
                if part == "]" and depth == 0:
                    break
                if part == "," and depth == 0 and closing + 1 < len(tokens) and tokens[closing + 1].text != "]":
                    elements += 1
                if part in ("[", "(", "{") or (part == "<" and closing in angles):
                    depth += 1
                elif part in ("]", ")", "}") or (part in (">", ">=") and closing in angles):
                    depth -= 1
            span = flat_width(tokens, angles, index, closing + 1)
            multiline = literal and next_text != "]" and (index in expanded or elements > 3 or span + len(writer.line) > width)
            contexts.append(Context("annotation" if annotation else "array" if literal else "index", multiline))
            writer.put("[", space=previous in ("=", "return"))
            if multiline:
                writer.newline()
                writer.indent += 1
        elif text == "]":
            context = Context("index")
            if contexts and contexts[-1].kind in ("array", "index", "annotation"):
                context = contexts.pop()
                if context.multiline:
                    writer.newline()
                    writer.indent = max(0, writer.indent - 1)
            writer.put("]")
            if context.kind == "annotation":
                writer.newline()
        elif text in ("<", ">", ">=") and index in angles:
            if text == "<":
                contexts.append(Context("angle"))
            elif contexts and contexts[-1].kind == "angle":
                contexts.pop()
            writer.put(text)
            if text == ">=":
                writer.line += " "
        elif text == ";":
            writer.put(";")
            header = any(context.for_header for context in contexts if context.kind == "paren")
            trailing_comment = index + 1 < len(tokens) and tokens[index + 1].kind == "comment" and "\n" not in source[token.end:tokens[index + 1].start]
            if not header:
                statement_start = index + 1
            if not header and not trailing_comment:
                writer.newline()
                if not contexts and next_text in ("import", "pub", "func", "struct", "enum", "interface"):
                    if next_text != "import" or (tokens[statement_start - 2].text == "package" if statement_start > 1 else False):
                        writer.newline(blank=True)
        elif text == ",":
            writer.put(",")
            if contexts and contexts[-1].multiline:
                writer.newline()
            elif contexts and contexts[-1].kind == "declaration":
                writer.newline()
        elif text == ":":
            writer.put(":")
        elif text == ".":
            writer.put(".")
        elif text in ("!", "~", "@"):
            writer.put(text, space=previous not in ("", "(", "[", ".", "@", "!", "~", "&", "*"))
            unary = True
        elif text in ("&", "*", "+", "-") and (not writer.line.strip() or previous in PREFIX_PRECEDERS or unary):
            writer.put(text, space=previous in ("=", "return", "=>", "->", "&&", "||") or (previous == "," and not (contexts and contexts[-1].kind in ("angle", "typeparen"))))
            unary = True
        elif text in ("=", "+", "-", "*", "/", "%", "==", "!=", "<", ">", "<=", ">=", "&&", "||", "+=", "-=", "*=", "/=", "->", "=>", "|", "^"):
            if text in ("&&", "||") and contexts and contexts[-1].kind == "paren" and contexts[-1].multiline:
                writer.newline()
            if text == "->":
                end = return_type_end(tokens, angles, index)
                if len(writer.line) + 1 + flat_width(tokens, angles, index, end) + 2 > width:
                    writer.newline()
                    writer.put("    ")
            writer.put(text, space=True)
            writer.line += " "
            unary = False
            if text == "=>":
                statement_start = index + 1
        elif text in ("++", "--", "?"):
            writer.put(text)
        else:
            if contexts and contexts[-1].kind == "declaration" and not writer.line and writer.lines and not writer.lines[-1].endswith("{") and (text == "func" or (text in ("pub", "async", "static", "mut") and next_text in ("func", "async", "static", "mut"))):
                writer.newline(blank=True)
            space = previous not in ("", "(", "[", "{", ":", ".", "@") and not unary
            if previous == "<" and index - 1 in angles:
                space = False
            if previous == "," and contexts and contexts[-1].kind in ("angle", "typeparen"):
                space = False
            if previous == ">" and index - 1 in angles:
                space = token.kind == "word"
            writer.put(text, space=space)
            unary = False
            if contexts and contexts[-1].kind == "imports" and token.kind == "literal" and not (index + 1 < len(tokens) and tokens[index + 1].kind == "comment" and "\n" not in source[token.end:tokens[index + 1].start]):
                writer.newline()
        previous = text
    formatted = wrap_expression_lines(writer.result(), width)
    if [(token.kind, token.text) for token in tokenize(formatted)] != [(token.kind, token.text) for token in tokens]:
        raise ValueError("Formatting would change tokens or literal/comment contents")
    return formatted


def sources(root: Path) -> list[Path]:
    """Only tracked files in this repository; never descend into submodules."""
    result = subprocess.run(["git", "ls-files", "-z", "--", "*.dmm"], cwd=root,
                            check=True, capture_output=True)
    excluded = {".git", "CMakeFiles", "vendor", "vendored", "third_party", "external"}
    return sorted(root / name for name in result.stdout.decode("utf-8").split("\0")
                  if name and not any(part in excluded or part.startswith(("build", "cmake-build"))
                                      for part in Path(name).parts))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*", type=Path)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    changed = 0
    files = args.paths or sources(args.root)
    for path in files:
        original = path.read_bytes()
        source = original.decode("utf-8-sig")
        formatted = format_source(source)
        if format_source(formatted) != formatted:
            raise ValueError(f"Non-idempotent formatting: {path}")
        if source.replace("\r\n", "\n") != formatted:
            changed += 1
            if not args.check:
                path.write_bytes(formatted.encode("utf-8"))
    print(f"{len(files)} DMM files checked; {changed} {'need formatting' if args.check else 'formatted'}")
    return 1 if args.check and changed else 0


if __name__ == "__main__":
    raise SystemExit(main())
