"""Format DMM sources without changing tokens, comments or literal contents.

No parser is required: deliberately invalid diagnostic fixtures are supported.
Generated sources can use format_source before writing their output.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import os
from pathlib import Path
import re
import shutil
import subprocess


@dataclass(frozen=True)
class Token:
    text: str
    kind: str
    start: int
    end: int


OPERATORS = ("=>", "==", "!=", "<=", ">=", "&&", "||", "+=", "-=", "*=", "/=", "++", "--", "->")
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
            elif text == ">":
                depth -= 1
                if depth == 0:
                    after = tokens[end + 1].text if end + 1 < len(tokens) else ""
                    if after in ("", "(", ")", "[", "]", "{", "}", ".", ",", ";", ":", "=", "->", ">"):
                        pairs[index] = end
                        pairs[end] = index
                    break
    return pairs


@dataclass
class Context:
    kind: str
    multiline: bool = False
    for_header: bool = False


class Writer:
    def __init__(self):
        self.lines = []
        self.line = ""
        self.indent = 0

    def put(self, text: str, space: bool = False):
        if not self.line:
            self.line = "    " * self.indent
        if space and self.line.strip() and not self.line.endswith(" "):
            self.line += " "
        self.line += text

    def newline(self, blank: bool = False):
        if self.line.strip():
            self.lines.append(self.line.rstrip())
        self.line = ""
        if blank and self.lines and self.lines[-1] != "":
            self.lines.append("")

    def result(self):
        self.newline()
        return "\n".join(self.lines).rstrip() + "\n"


def format_source(source: str, width: int = 110) -> str:
    source = source.replace("\r\n", "\n")
    tokens = tokenize(source)
    angles = generic_pairs(tokens)
    writer = Writer()
    contexts = []
    previous = ""
    statement_start = 0
    unary = False
    for index, token in enumerate(tokens):
        text = token.text
        next_text = tokens[index + 1].text if index + 1 < len(tokens) else ""
        gap = source[tokens[index - 1].end:token.start] if index else source[:token.start]
        if "\n\n" in gap.replace("\r\n", "\n") and writer.line == "" and not (contexts and contexts[-1].kind in ("paren", "angle")):
            writer.newline(blank=True)
        if token.kind == "comment":
            inline = index > 0 and "\n" not in gap
            if not inline:
                writer.newline()
            writer.put(text, space=inline)
            writer.newline()
            continue
        if token.kind == "unclosed":
            writer.put(text, space=True)
            writer.newline()
            previous = text
            continue
        if text == "{":
            head = [item.text for item in tokens[statement_start:index]]
            declaration = any(value in head for value in ("struct", "union", "enum", "interface", "extern", "destructor", "func"))
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
            span = sum(len(item.text) + 1 for item in tokens[index:closing + 1])
            multiline = not initializer or fields > 3 or span + len(writer.line) > width or any(item.kind == "comment" for item in tokens[index + 1:closing])
            empty = next_text == "}"
            kind = "initializer" if initializer else ("declaration" if declaration else "block")
            contexts.append(Context(kind, multiline and not empty))
            writer.put("{", space=not initializer)
            if multiline and not empty:
                writer.newline()
                writer.indent += 1
            statement_start = index + 1
        elif text == "}":
            context = contexts.pop() if contexts and contexts[-1].kind in ("initializer", "block", "declaration") else Context("block")
            if context.multiline:
                writer.newline()
                writer.indent = max(0, writer.indent - 1)
                while writer.lines and writer.lines[-1] == "":
                    writer.lines.pop()
            writer.put("}", space=context.kind == "initializer" and not context.multiline and previous != "{")
            if context.kind != "initializer" and next_text not in (";", ",", ")", "]", ".", "else", "("):
                writer.newline(blank=context.kind == "declaration" or not contexts)
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
            span = sum(len(item.text) + 1 for item in tokens[index:closing + 1])
            multiline = group or (next_text != ")" and previous != "for" and span + len(writer.line) > width)
            contexts.append(Context("imports" if group else "paren", multiline, previous == "for"))
            writer.put("(", space=previous in ("if", "while", "for", "match", "switch", "import", "return", ",", "=", "=>", "&&", "||"))
            if multiline:
                writer.newline()
                writer.indent += 1
        elif text == ")":
            context = contexts.pop() if contexts and contexts[-1].kind in ("paren", "imports") else Context("paren")
            if context.multiline:
                writer.newline()
                writer.indent = max(0, writer.indent - 1)
            writer.put(")")
        elif text == "[":
            literal = previous in ("=", "return", "(", ",", "[")
            contexts.append(Context("array" if literal else "index"))
            writer.put("[", space=previous in ("=", "return"))
        elif text == "]":
            if contexts and contexts[-1].kind in ("array", "index"):
                contexts.pop()
            writer.put("]")
        elif text in ("<", ">") and index in angles:
            if text == "<":
                contexts.append(Context("angle"))
            elif contexts and contexts[-1].kind == "angle":
                contexts.pop()
            writer.put(text)
        elif text == ";":
            writer.put(";")
            header = any(context.for_header for context in contexts if context.kind == "paren")
            trailing_comment = index + 1 < len(tokens) and tokens[index + 1].kind == "comment" and "\n" not in source[token.end:tokens[index + 1].start]
            if not header and not trailing_comment:
                writer.newline()
                statement_start = index + 1
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
        elif text in ("&", "*", "+", "-") and (not writer.line.strip() or previous in ("", "(", "[", ",", ":", "=", "return", "=>", "->", "&", "*", "&&", "||") or unary):
            writer.put(text, space=previous in (":", "=", "return", "=>", "->", ",", "&&", "||"))
            unary = True
        elif text in ("=", "+", "-", "*", "/", "%", "==", "!=", "<", ">", "<=", ">=", "&&", "||", "+=", "-=", "*=", "/=", "->", "=>", "|", "^"):
            writer.put(text, space=True)
            writer.line += " "
            unary = False
            if text in ("&&", "||") and contexts and contexts[-1].kind == "paren" and contexts[-1].multiline:
                writer.newline()
            if text == "=>":
                statement_start = index + 1
        elif text in ("++", "--", "?"):
            writer.put(text)
        else:
            space = previous not in ("", "(", "[", ".", "@", "<") and not unary
            if previous == ">" and index - 1 in angles:
                space = token.kind == "word"
            writer.put(text, space=space)
            unary = False
            if contexts and contexts[-1].kind == "imports" and token.kind == "literal":
                writer.newline()
        previous = text
    formatted = writer.result()
    if [(token.kind, token.text) for token in tokenize(formatted)] != [(token.kind, token.text) for token in tokens]:
        raise ValueError("Formatting would change tokens or literal/comment contents")
    return formatted


def sources(root: Path) -> list[Path]:
    if shutil.which("rg") is None:
        files = []
        for directory, children, names in os.walk(root):
            children[:] = [name for name in children
                           if name not in {".git", "CMakeFiles"}
                           and not name.startswith(("build", "cmake-build"))]
            files.extend(Path(directory) / name for name in names if name.endswith(".dmm"))
        return sorted(files)
    result = subprocess.run(["rg", "--files", "--hidden", "-g", "*.dmm", "-g", "!**/.git/**",
                             "-g", "!**/cmake-build*/**", "-g", "!**/build*/**", "-g", "!**/CMakeFiles/**"],
                            cwd=root, check=True, capture_output=True, text=True)
    return sorted(root / line for line in result.stdout.splitlines())


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
