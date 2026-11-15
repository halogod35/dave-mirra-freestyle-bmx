"""A small interpreter for the MAXScript subset used by tools/3dsmax/maxscript/DaveMirraImport.ms.

3ds Max is not installed on this PC, so the tests run the importer's MAXScript *source* with this
interpreter: the parsing functions against the game files, and the scene-building functions against
the recording 3ds Max mock (tests/max_mock.py). It is deliberately strict: unknown functions,
properties or undeclared variables raise, so a typo in the .ms fails the tests.

Supported subset (what the importer uses above its "UI" marker):
  -- comments, "strings" with \\ \" \n escapes, numbers (int, float, 0x hex), #names, #(arrays),
  [x, y, z] point3 literals, (blocks; separated by newlines or ;), fn name args = expr,
  fn name args key:default = expr (keyword parameters with a default),
  struct Name (f1, f2), local/global declarations, if/then/else, if/do, while/do,
  for i = a to b [by s] do, for x in arr do, return, throw, max modify mode,
  assignment = += -= *= /=, or/and/not, == != < > <= >=, + - * /, as integer|string|float,
  calls by juxtaposition with keyword arguments (f a b key:v), zero-argument calls f(),
  property access a.b, indexing a[i] (1-based), 'x' adjacency rules: a[i] / a.b without spaces,
  the context expressions 'animate on <expr>' and 'at time <operand> <expr>',
  'with undo on|off <expr>' and 'with redraw on|off <expr>',
  units.SystemType / units.SystemScale (the mock scene's System Unit Setup) and formattedPrint,
  getUserProp, objects / selection, node.controller sub-anims (c[i].controller, c.numSubs), numKeys / getKeyTime
  (Time values: .frame), animationRange / interval, inverse and floor.
The dialog: 'try <expr> catch <expr>', 'rollout name "title" kw:v ( ... )' with controls (checkbox,
  spinner, label, button, edittext, dropdownlist, dotNetControl, subRollout), 'group "title" ( ... )', rollout
  locals and functions, 'on <control> <event> [args] do <expr>' handlers, createDialog / destroyDialog,
  addSubRollout / removeSubRollout, a .NET TabControl with TabPages, the file dialogs and messageBox (answered
  by the test). See the "rollouts" section below for the rules modelled.
MAXScript semantics kept: case-insensitive names, 1-based arrays and strings, integer / integer
division truncates, strings compare case-sensitively, integers are 32-bit (bit.* wraps like the
C int it is; + - * that would overflow raise here, because 3ds Max would silently wrap), sin/cos
take degrees.
"""
import glob
import math
import os
import re
import struct

from max_mock import M3, Interval, MxsName, MxsTime, P3, _Controller


class MXSError(Exception):
    pass


class Name(str):
    """#name value (case-insensitive)."""

    def __eq__(self, o):
        return isinstance(o, str) and self.lower() == o.lower()

    def __hash__(self):
        return hash(self.lower())


# MAXScript reserved words (3ds Max docs, "Reserved Keywords"): never valid as variable, parameter or
# field names. ('on'/'off' are also boolean literals.) Property names after '.' may be keywords (bit.and).
RESERVED = {"about", "and", "animate", "as", "at", "by", "case", "catch", "collect", "continue", "coordsys", "do",
            "else", "exit", "fn", "for", "from", "function", "global", "if", "in", "local", "macroscript", "mapped",
            "max", "not", "of", "off", "on", "or", "parameters", "persistent", "plugin", "rcmenu", "return",
            "rollout", "set", "struct", "then", "throw", "to", "tool", "try", "undo", "utility", "when", "where",
            "while", "with", "true", "false", "undefined"}
KEYWORDS = RESERVED


# ============================================================================ lexer

class Tok:
    __slots__ = ("kind", "val", "line", "space")

    def __init__(self, kind, val, line, space):
        self.kind, self.val, self.line, self.space = kind, val, line, space

    def __repr__(self):
        return f"{self.kind}:{self.val!r}@{self.line}"


def tokenize(src):
    toks, i, line, n = [], 0, 1, len(src)
    space = True
    while i < n:
        c = src[i]
        if c in " \t\r":
            i += 1
            space = True
            continue
        if c == "\n":
            toks.append(Tok("NL", None, line, True))
            line += 1
            i += 1
            space = True
            continue
        if src.startswith("--", i):
            while i < n and src[i] != "\n":
                i += 1
            continue
        if src.startswith("/*", i):
            j = src.index("*/", i)
            line += src.count("\n", i, j)
            i = j + 2
            continue
        if c == '"':
            j, out = i + 1, []
            while src[j] != '"':
                if src[j] == "\\":
                    nx = src[j + 1]
                    out.append({"n": "\n", "t": "\t", '"': '"', "\\": "\\"}.get(nx, "\\" + nx))
                    j += 2
                else:
                    out.append(src[j])
                    j += 1
            toks.append(Tok("STR", "".join(out), line, space))
            i = j + 1
        elif c.isdigit() or (c == "." and i + 1 < n and src[i + 1].isdigit()):
            j = i
            if src.startswith("0x", i) or src.startswith("0X", i):
                j += 2
                while j < n and src[j] in "0123456789abcdefABCDEF":
                    j += 1
                v = int(src[i:j], 16)
                if v > 0x7FFFFFFF:          # MAXScript integer literals are 32-bit signed: 0xFFFFFFFF == -1
                    v -= 1 << 32
                toks.append(Tok("NUM", v, line, space))
            else:
                while j < n and (src[j].isdigit() or src[j] == "."):
                    j += 1
                if j < n and src[j] in "eE":
                    j += 1
                    if src[j] in "+-":
                        j += 1
                    while j < n and src[j].isdigit():
                        j += 1
                t = src[i:j]
                toks.append(Tok("NUM", float(t) if any(ch in t for ch in ".eE") else int(t), line, space))
            i = j
        elif c.isalpha() or c == "_":
            j = i
            while j < n and (src[j].isalnum() or src[j] == "_"):
                j += 1
            toks.append(Tok("NAME", src[i:j], line, space))
            i = j
        elif c == "#":
            if src[i + 1] == "(":
                toks.append(Tok("HASHPAREN", "#(", line, space))
                i += 2
            else:
                j = i + 1
                while j < n and (src[j].isalnum() or src[j] == "_"):
                    j += 1
                toks.append(Tok("HASHNAME", Name(src[i + 1:j]), line, space))
                i = j
        else:
            for op in ("==", "!=", "<=", ">=", "+=", "-=", "*=", "/=", "(", ")", "[", "]", ",", ".", "=", "<", ">",
                       "+", "-", "*", "/", ":", ";"):
                if src.startswith(op, i):
                    toks.append(Tok("OP", op, line, space))
                    i += len(op)
                    break
            else:
                raise MXSError(f"line {line}: unexpected character {c!r}")
        space = False
    toks.append(Tok("EOF", None, line, True))
    return toks


# ============================================================================ parser

class Parser:
    def __init__(self, toks):
        self.t, self.i = toks, 0

    def peek(self, k=0):
        return self.t[self.i + k]

    def next(self):
        tok = self.t[self.i]
        self.i += 1
        return tok

    def is_op(self, v, tok=None):
        tok = tok or self.peek()
        return tok.kind == "OP" and tok.val == v

    def is_kw(self, v, tok=None):
        tok = tok or self.peek()
        return tok.kind == "NAME" and tok.val.lower() == v

    def expect_op(self, v):
        tok = self.next()
        if not (tok.kind == "OP" and tok.val == v):
            raise MXSError(f"line {tok.line}: expected {v!r}, got {tok}")
        return tok

    def expect_kw(self, v):
        tok = self.next()
        if not self.is_kw(v, tok):
            raise MXSError(f"line {tok.line}: expected {v!r}, got {tok}")

    def ident(self):
        tok = self.next()
        if tok.kind != "NAME" or tok.val.lower() in RESERVED:
            raise MXSError(f"line {tok.line}: {tok.val!r} cannot be used as a name (MAXScript reserved word)")
        return tok.val.lower()

    def skip_nl(self):
        while self.peek().kind == "NL":
            self.i += 1

    def program(self):
        out = []
        while True:
            while self.peek().kind == "NL" or self.is_op(";"):
                self.i += 1
            if self.peek().kind == "EOF":
                return out
            out.append(self.expr())

    def block(self):
        """after '(' : statements until ')'"""
        out = []
        while True:
            while self.peek().kind == "NL" or self.is_op(";"):
                self.i += 1
            if self.is_op(")"):
                self.i += 1
                return ("block", out)
            out.append(self.expr())
            tok = self.peek()
            if not (tok.kind == "NL" or self.is_op(";") or self.is_op(")")):
                raise MXSError(f"line {tok.line}: expected end of expression, got {tok}")

    def expr(self):
        tok = self.peek()
        if tok.kind == "NAME":
            kw = tok.val.lower()
            if kw in ("fn", "function"):
                return self.fndef()
            if kw == "struct":
                return self.structdef()
            if kw in ("local", "global"):
                return self.decl()
            if kw == "if":
                return self.if_()
            if kw == "while":
                self.next()
                cond = self.expr()
                self.expect_kw("do")
                self.skip_nl()
                return ("while", cond, self.expr())
            if kw == "for":
                return self.for_()
            if kw == "return":
                self.next()
                return ("return", self.expr())
            if kw == "throw":
                self.next()
                return ("throw", self.expr())
            if kw == "try":                           # try <expr> catch <expr>
                self.next()
                self.skip_nl()
                body = self.expr()
                self.skip_nl()
                self.expect_kw("catch")
                self.skip_nl()
                return ("try", body, self.expr())
            if kw == "rollout":
                return self.rollout()
            if kw == "animate":                       # animate on|off <expr>
                self.next()
                flag = self.next()
                if flag.kind == "NAME" and flag.val.lower() in ("on", "off", "true", "false"):
                    on = ("lit", flag.val.lower() in ("on", "true"))
                else:
                    raise MXSError(f"line {flag.line}: animate needs on/off")
                self.skip_nl()
                return ("animate", on, self.expr())
            if kw == "with":                          # with undo|redraw on|off <expr>
                self.next()
                ctx = self.next()
                if not (ctx.kind == "NAME" and ctx.val.lower() in ("undo", "redraw")):
                    raise MXSError(f"line {ctx.line}: only 'with undo' and 'with redraw' are modelled")
                flag = self.next()
                if not (flag.kind == "NAME" and flag.val.lower() in ("on", "off", "true", "false")):
                    raise MXSError(f"line {flag.line}: with {ctx.val} needs on/off")
                self.skip_nl()
                return ("with", ctx.val.lower(), ("lit", flag.val.lower() in ("on", "true")), self.expr())
            if kw == "at":                            # at time <operand> <expr>
                self.next()
                self.expect_kw("time")
                t = self.operand()
                self.skip_nl()
                return ("attime", t, self.expr())
            if kw == "max":
                self.next()
                words = []
                while self.peek().kind == "NAME" and self.peek().line == tok.line:
                    words.append(self.next().val.lower())
                return ("maxcmd", " ".join(words))
        lhs = self.or_()
        tok = self.peek()
        if tok.kind == "OP" and tok.val in ("=", "+=", "-=", "*=", "/="):
            self.next()
            self.skip_nl()
            return ("assign", tok.val, lhs, self.expr())
        return lhs

    def fndef(self):
        self.next()
        name = self.ident()
        params, kwparams = [], {}
        while not self.is_op("="):
            p = self.ident()
            if self.is_op(":"):                       # keyword parameter: name:default
                self.next()
                if self.is_op("="):
                    raise MXSError(f"line {self.peek().line}: keyword parameter {p} needs a default here")
                kwparams[p] = self.operand()
            else:
                if kwparams:
                    raise MXSError(f"line {self.peek().line}: positional parameter {p} after keyword parameters")
                params.append(p)
        self.next()
        self.skip_nl()
        return ("fn", name, params, self.expr(), kwparams)

    # rollout <name> "<title>" kw:v ... ( <items> ). Items: controls, group "t" ( controls ), local, fn, on ... do.
    CONTROL_TYPES = ("checkbox", "spinner", "label", "button", "edittext", "dropdownlist", "dotnetcontrol",
                     "subrollout")

    def rollout(self):
        line = self.next().line
        name = self.ident()
        title = self.next()
        if title.kind != "STR":
            raise MXSError(f"line {line}: rollout {name} needs a title string")
        kw = self.kwargs(line)
        self.skip_nl()
        self.expect_op("(")
        return ("rollout", name, title.val, kw, self.rollout_items(False))

    def kwargs(self, line):
        kw = {}
        while self.peek().kind == "NAME" and self.peek().line == line and self.is_op(":", self.peek(1)):
            k = self.next().val.lower()
            self.next()
            kw[k] = self.operand()
        return kw

    def rollout_items(self, in_group):
        items = []
        while True:
            while self.peek().kind == "NL" or self.is_op(";"):
                self.i += 1
            if self.is_op(")"):
                self.i += 1
                return items
            tok = self.peek()
            if tok.kind != "NAME":
                raise MXSError(f"line {tok.line}: rollout item expected, got {tok}")
            kw = tok.val.lower()
            if kw == "group" and not in_group:
                self.next()
                title = self.next()
                if title.kind != "STR":
                    raise MXSError(f"line {tok.line}: group needs a title string")
                self.skip_nl()
                self.expect_op("(")
                items.append(("group", title.val, self.rollout_items(True)))
            elif kw in self.CONTROL_TYPES:
                self.next()
                cname = self.ident()
                cap = self.next() if self.peek().kind == "STR" and self.peek().line == tok.line else None
                items.append(("control", kw, cname, cap.val if cap else None, self.kwargs(tok.line), tok.line))
            elif in_group:
                raise MXSError(f"line {tok.line}: only controls can be inside a group, got {tok.val!r}")
            elif kw == "local":
                items.append(self.decl())
            elif kw in ("fn", "function"):
                items.append(self.fndef())
            elif kw == "on":
                self.next()
                target = self.ident()
                event = self.next()
                if event.kind != "NAME":
                    raise MXSError(f"line {tok.line}: event name expected")
                params = []
                while not self.is_kw("do"):
                    params.append(self.ident())
                self.next()
                self.skip_nl()
                items.append(("on", target, event.val.lower(), params, self.expr(), tok.line))
            else:
                raise MXSError(f"line {tok.line}: {tok.val!r} is not a rollout item (control, group, local, fn, on)")

    def structdef(self):
        self.next()
        name = self.ident()
        self.expect_op("(")
        fields = []
        while True:
            self.skip_nl()
            fields.append(self.ident())
            self.skip_nl()
            if self.is_op(")"):
                self.next()
                return ("struct", name, fields)
            self.expect_op(",")

    def decl(self):
        scope = self.next().val.lower()
        items = []
        while True:
            name = self.ident()
            val = None
            if self.is_op("="):
                self.next()
                self.skip_nl()
                val = self.expr()
            items.append((name, val))
            if not self.is_op(","):
                return ("decl", scope, items)
            self.next()

    def if_(self):
        self.next()
        cond = self.expr()
        tok = self.next()
        if not (self.is_kw("then", tok) or self.is_kw("do", tok)):
            raise MXSError(f"line {tok.line}: expected then/do")
        self.skip_nl()
        then = self.expr()
        els = None
        if not self.is_kw("do", tok):
            j = self.i
            self.skip_nl()
            if self.is_kw("else"):
                self.next()
                self.skip_nl()
                els = self.expr()
            else:
                self.i = j
        return ("if", cond, then, els)

    def for_(self):
        self.next()
        var = self.ident()
        if self.is_kw("in"):
            self.next()
            seq = self.expr()
            self.expect_kw("do")
            self.skip_nl()
            return ("forin", var, seq, self.expr())
        self.expect_op("=")
        a = self.expr()
        self.expect_kw("to")
        b = self.expr()
        step = None
        if self.is_kw("by"):
            self.next()
            step = self.expr()
        self.expect_kw("do")
        self.skip_nl()
        return ("for", var, a, b, step, self.expr())

    def or_(self):
        e = self.and_()
        while self.is_kw("or"):
            self.next()
            self.skip_nl()
            e = ("or", e, self.and_())
        return e

    def and_(self):
        e = self.not_()
        while self.is_kw("and"):
            self.next()
            self.skip_nl()
            e = ("and", e, self.not_())
        return e

    def not_(self):
        if self.is_kw("not"):
            self.next()
            return ("not", self.not_())
        return self.compare()

    def compare(self):
        e = self.sum()
        tok = self.peek()
        if tok.kind == "OP" and tok.val in ("==", "!=", "<", ">", "<=", ">="):
            self.next()
            self.skip_nl()
            e = ("bin", tok.val, e, self.sum())
        return e

    def sum(self):
        e = self.term()
        while self.peek().kind == "OP" and self.peek().val in ("+", "-"):
            op = self.next().val
            self.skip_nl()
            e = ("bin", op, e, self.term())
        return e

    def term(self):
        e = self.as_()
        while self.peek().kind == "OP" and self.peek().val in ("*", "/"):
            op = self.next().val
            self.skip_nl()
            e = ("bin", op, e, self.as_())
        return e

    def as_(self):
        e = self.unary()
        while self.is_kw("as"):
            self.next()
            e = ("as", e, self.next().val.lower())
        return e

    def unary(self):
        if self.is_op("-"):
            self.next()
            return ("neg", self.unary())
        return self.call()

    def starts_arg(self, tok):
        if tok.kind in ("NUM", "STR", "HASHNAME", "HASHPAREN"):
            return True
        if tok.kind == "NAME":
            return tok.val.lower() not in KEYWORDS or tok.val.lower() in ("true", "false", "undefined")
        if tok.kind == "OP" and tok.val in ("(", "["):
            return tok.space            # "f (x)" / "f [1,2,3]" are arguments; "a[i]" is indexing
        return False

    def call(self):
        head = self.operand()
        line = self.t[self.i - 1].line
        args, kwargs = [], {}
        if self.is_op("(") and not self.peek().space and self.is_op(")", self.peek(1)):
            self.i += 2
            return ("call", head, [], {})
        while self.peek().kind != "NL" and self.peek().line == line and self.starts_arg(self.peek()):
            tok = self.peek()
            if tok.kind == "NAME" and self.is_op(":", self.peek(1)) and tok.val.lower() not in KEYWORDS:
                self.i += 2
                kwargs[tok.val.lower()] = self.operand()
            else:
                args.append(self.operand())
        if args or kwargs:
            return ("call", head, args, kwargs)
        return head

    def operand(self):
        e = self.primary()
        while True:
            tok = self.peek()
            if tok.kind == "OP" and tok.val == "." and not tok.space:
                self.next()
                e = ("prop", e, self.next().val)
            elif tok.kind == "OP" and tok.val == "[" and not tok.space:
                self.next()
                idx = self.expr()
                self.expect_op("]")
                e = ("index", e, idx)
            else:
                return e

    def primary(self):
        tok = self.next()
        if tok.kind in ("NUM", "STR"):
            return ("lit", tok.val)
        if tok.kind == "HASHNAME":
            return ("lit", tok.val)
        if tok.kind == "HASHPAREN":
            items = []
            self.skip_nl()
            while not self.is_op(")"):
                items.append(self.expr())
                self.skip_nl()
                if self.is_op(","):
                    self.next()
                    self.skip_nl()
            self.next()
            return ("array", items)
        if tok.kind == "OP" and tok.val == "[":
            items = [self.expr()]
            while self.is_op(","):
                self.next()
                items.append(self.expr())
            self.expect_op("]")
            if len(items) != 3:
                raise MXSError(f"line {tok.line}: only point3 literals are supported")
            return ("point3", items)
        if tok.kind == "OP" and tok.val == "(":
            return self.block()
        if tok.kind == "NAME":
            v = tok.val.lower()
            if v == "true":
                return ("lit", True)
            if v == "false":
                return ("lit", False)
            if v == "undefined":
                return ("lit", None)
            if v in KEYWORDS:
                raise MXSError(f"line {tok.line}: unexpected keyword {tok.val!r}")
            return ("name", v)
        raise MXSError(f"line {tok.line}: unexpected {tok}")


# ============================================================================ runtime values

class Struct:
    def __init__(self, name, fields):
        self.name, self.fields = name, fields

    def __call__(self, **kw):
        bad = set(kw) - set(self.fields)
        if bad:
            raise MXSError(f"struct {self.name}: unknown fields {bad}")
        return Inst(self, {f: kw.get(f) for f in self.fields})


class Inst:
    def __init__(self, st, vals):
        object.__setattr__(self, "_st", st)
        object.__setattr__(self, "_v", vals)

    def get(self, k):
        if k not in self._v:
            raise MXSError(f"{self._st.name} has no field {k}")
        return self._v[k]

    def set(self, k, v):
        if k not in self._v:
            raise MXSError(f"{self._st.name} has no field {k}")
        self._v[k] = v


class Frame(dict):
    """A function call's locals; ``rollout``: the rollout whose function or handler runs (its controls, locals
    and functions are visible by name), else None."""
    rollout = None


class Fn:
    def __init__(self, name, params, body, interp, kwparams=None, rollout=None):
        self.name, self.params, self.body, self.interp = name, params, body, interp
        self.kwparams = kwparams or {}
        self.rollout = rollout

    def __call__(self, *args, **kw):
        kw = {k.lower(): v for k, v in kw.items()}
        # (an unknown keyword is an error here; 3ds Max may ignore it, which would hide a typo)
        if len(args) != len(self.params) or set(kw) - set(self.kwparams):
            raise MXSError(f"{self.name}: called with {len(args)} args {list(kw)}, needs {self.params} "
                           f"+ keywords {list(self.kwparams)}")
        vals = {k: (kw[k] if k in kw else self.interp.eval(d)) for k, d in self.kwparams.items()}
        return self.interp.call_fn(self, args, vals)


class _Return(Exception):
    def __init__(self, v):
        self.v = v


class Thrown(Exception):
    pass


class MXSRuntimeError(MXSError):
    """An error 3ds Max raises at run time that a script's try/catch catches (unlike the interpreter's own
    strictness errors, MXSError, which always fail the test)."""


class _BinFile:
    def __init__(self, path, mode):
        self.f = open(path, mode.replace("b", "") + "b")


def _i32(v):
    """Wrap to a 32-bit signed integer (two's complement), as the C int behind MAXScript's bit.*."""
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def _int_arg(v):
    if isinstance(v, bool) or not isinstance(v, int):
        raise MXSError(f"bit.*: integer expected, got {v!r}")
    if not -(1 << 31) <= v < (1 << 31):
        raise MXSError(f"bit.*: {v} is not a 32-bit integer")
    return v


def _check_int(v):
    """MAXScript Integer arithmetic is 32-bit and wraps silently in 3ds Max; an overflow is always a bug
    in the script, so it raises here."""
    if isinstance(v, int) and not isinstance(v, bool) and not -(1 << 31) <= v < (1 << 31):
        raise MXSError(f"integer overflow: {v} does not fit MAXScript's 32-bit Integer")
    return v


def _bit():
    class Bit:
        # MAXScript's bit.* work on its 32-bit signed Integer: results wrap (bit.shift 1 31 == -2147483648)
        @staticmethod
        def And(a, b):
            return _i32(_int_arg(a) & _int_arg(b))

        @staticmethod
        def Or(a, b):
            return _i32(_int_arg(a) | _int_arg(b))

        @staticmethod
        def shift(a, n):
            a, n = _int_arg(a), _int_arg(n)
            return _i32(a << n) if n >= 0 else a >> (-n)

        @staticmethod
        def intAsChar(i):
            return chr(i)

        @staticmethod
        def charAsInt(c):
            return ord(c)
    b = Bit()
    b.__dict__.update({"and": Bit.And, "or": Bit.Or})
    return b


# --- the .NET classes the importer uses, modelled on real 3ds Max behaviour:
# * dotNetObject "System.Byte[]" n does NOT construct an array: it reaches .NET as null, and
#   FileStream.Read/Write then throw "Buffer cannot be null. Parameter name: array" (field report, 3ds Max).
# * a .NET array *returned* by a method is converted to a MAXScript array (a list here) unless the call
#   passes asDotNetObject:true; FileStream.Write needs the real byte[] (_NetBytes).

class _NetBytes:
    def __init__(self, n):
        self.b = bytearray(n)


class _Enum:
    def __init__(self, **kw):
        self.__dict__.update(kw)


class _NetFileStream:
    def __init__(self, path, mode, access=None):
        assert mode in ("Create", "Open")
        self._f = open(path, "wb" if mode == "Create" else "rb")

    def Seek(self, off, origin):
        assert origin == "Begin"
        self._f.seek(off)

    def Read(self, buf, off, n):
        if buf is None:
            raise MXSError(".NET runtime exception: Buffer cannot be null. Parameter name: array")
        assert isinstance(buf, _NetBytes), "FileStream.Read needs a .NET byte[], not a MAXScript array"
        assert off + n <= len(buf.b)
        d = self._f.read(n)
        buf.b[off:off + len(d)] = d
        return len(d)

    def Write(self, buf, off, n):
        if buf is None:
            raise MXSError(".NET runtime exception: Buffer cannot be null. Parameter name: array")
        assert isinstance(buf, _NetBytes), "pass a .NET byte[] (asDotNetObject:true), not a MAXScript array"
        assert 0 <= off and off + n <= len(buf.b)
        self._f.write(bytes(buf.b[off:off + n]))

    def Close(self):
        self._f.close()


class _NetBinaryReader:
    def __init__(self, stream):
        assert isinstance(stream, _NetFileStream)
        self._s = stream

    def ReadBytes(self, n, asDotNetObject=False):
        b = _NetBytes(0)
        b.b = bytearray(self._s._f.read(n))
        b.Length = len(b.b)
        return b if asDotNetObject else list(b.b)

    def Close(self):
        self._s.Close()


class _NetColor:
    @staticmethod
    def FromArgb(r, g, b):
        return ("argb", 255, r, g, b)


class _NetBitmap:
    """System.Drawing.Bitmap(path): MakeTransparent(color) and Save(path, ImageFormat.Png), modelled with
    dmparse's BMP decoder / PNG encoder. GDI+ draws the key colour as transparent black (0,0,0,0)."""

    def __init__(self, path):
        import dmparse
        self._dm = dmparse
        self.w, self.h, rgba = dmparse.decode_bmp(open(path, "rb").read())
        self.rgba = bytearray(rgba)
        self.disposed = False

    def MakeTransparent(self, color):
        assert color[0] == "argb"
        key = bytes((color[2], color[3], color[4], 255))
        for i in range(0, len(self.rgba), 4):
            if self.rgba[i:i + 4] == key:
                self.rgba[i:i + 4] = b"\0\0\0\0"

    def Save(self, path, fmt):
        assert fmt == "Png" and not self.disposed
        open(path, "wb").write(self._dm.encode_png(self.w, self.h, bytes(self.rgba)))

    def Dispose(self):
        self.disposed = True


def _dotnet_class(name):
    return {"System.IO.FileMode": _Enum(Create="Create", Open="Open"),
            "System.IO.FileAccess": _Enum(Read="Read"),
            "System.IO.SeekOrigin": _Enum(Begin="Begin"),
            "System.Drawing.Color": _NetColor(),
            "System.Drawing.Imaging.ImageFormat": _Enum(Png="Png")}[name]


def _dotnet_object(name, *args):
    if name == "System.Windows.Forms.TabPage":
        return NetTabPage(*args)
    if name == "System.Byte[]":
        return None  # not a constructor in MAXScript: .NET sees null
    if name == "System.IO.BinaryReader":
        return _NetBinaryReader(*args)
    if name == "System.Drawing.Bitmap":
        return _NetBitmap(*args)
    assert name == "System.IO.FileStream", name
    return _NetFileStream(*args)


def prop_get(obj, name):
    if hasattr(obj, "mxs_get"):
        return obj.mxs_get(name.lower())
    if isinstance(obj, Inst):
        return obj.get(name.lower())
    if isinstance(obj, (list, str)) and name.lower() == "count":
        return len(obj)
    if isinstance(obj, P3) and name.lower() in "xyz":
        return obj["xyz".index(name.lower())]
    if isinstance(obj, M3) and name.lower() in ("row1", "row2", "row3", "row4"):
        k = int(name[-1]) - 1
        return P3(*(obj.rows[k] if k < 3 else obj.pos))
    d = getattr(obj, "__dict__", {})
    for k in list(d) + [a for a in dir(obj) if not a.startswith("__")]:
        if k.lower() == name.lower():
            return getattr(obj, k)
    raise MXSError(f"unknown property {name!r} of {obj!r}")


def prop_set(obj, name, val):
    if hasattr(obj, "mxs_set"):
        return obj.mxs_set(name.lower(), val)
    if isinstance(obj, Inst):
        return obj.set(name.lower(), val)
    for k in list(getattr(obj, "__dict__", {})):
        if k.lower() == name.lower():
            return setattr(obj, k, val)
    setattr(obj, name, val)


def _p3(v):
    return v if isinstance(v, P3) else None


def arith(op, a, b):
    if isinstance(a, M3) and isinstance(b, M3) and op == "*":
        rows = tuple(tuple(sum(a.rows[i][k] * b.rows[k][j] for k in range(3)) for j in range(3)) for i in range(3))
        pos = tuple(sum(a.pos[k] * b.rows[k][j] for k in range(3)) + b.pos[j] for j in range(3))
        return M3(*rows, pos)
    if isinstance(a, P3) and isinstance(b, M3) and op == "*":
        return P3(*b.xf(a))
    if isinstance(a, P3) or isinstance(b, P3):
        if isinstance(a, P3) and isinstance(b, P3) and op in "+-":
            return P3(*(x + y if op == "+" else x - y for x, y in zip(a, b)))
        if isinstance(a, P3) and isinstance(b, (int, float)) and op in "*/":
            return P3(*(x * b if op == "*" else x / b for x in a))
        if isinstance(b, P3) and isinstance(a, (int, float)) and op == "*":
            return P3(*(a * x for x in b))
        raise MXSError(f"unsupported point3 operation {op} on {a!r}, {b!r}")
    if op == "+":
        if isinstance(a, str) != isinstance(b, str):
            raise MXSError(f"cannot add {a!r} and {b!r} (use 'as string')")
        return _check_int(a + b)
    if op == "-":
        return _check_int(a - b)
    if op == "*":
        return _check_int(a * b)
    if op == "/":
        if isinstance(a, int) and isinstance(b, int):
            if b == 0:
                raise MXSError("integer division by zero")
            q = abs(a) // abs(b)
            return q if (a >= 0) == (b >= 0) else -q
        return a / b
    raise MXSError(f"bad operator {op}")


class _Units:
    """MAXScript's ``units`` struct over the mock scene's System Unit Setup (max_mock._Units): SystemType is a
    #name value, SystemScale a float. Only these two properties exist here; a #name is required to set the type."""

    def __init__(self, units):
        object.__setattr__(self, "_u", units)

    @property
    def SystemType(self):
        return Name(str(self._u.SystemType))

    @property
    def SystemScale(self):
        return self._u.SystemScale

    def __setattr__(self, k, v):
        if k.lower() == "systemtype":
            if not isinstance(v, Name):
                raise MXSError(f"units.SystemType: a #name is needed, got {v!r}")
            self._u.SystemType = MxsName(str(v))
        elif k.lower() == "systemscale":
            self._u.SystemScale = v
        else:
            raise MXSError(f"unknown property {k!r} of units")


def formatted_print(v, format):
    """formattedPrint <number> format:<printf spec without the %> (userLocale off: '.' decimal point). Only
    plain specs are modelled; a float conversion (e f g) needs a float and an integer one (d i x) an integer."""
    m = re.fullmatch(r"([-+ #0]*)([0-9]*)(\.[0-9]+)?([diouxXeEfFgG])", format)
    if m is None:
        raise MXSError(f"formattedPrint: format {format!r} not modelled")
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        raise MXSError(f"formattedPrint: number expected, got {v!r}")
    if (m.group(4) in "diouxX") != isinstance(v, int):
        raise MXSError(f"formattedPrint: format {format!r} does not match {type(v).__name__} {v!r}")
    return ("%" + format) % v



# ============================================================================ rollouts (the dialog)
# Modelled on 3ds Max's rollout rules, never more permissive:
# * a rollout's controls, locals and functions exist only while it is open (createDialog, addSubRollout); each
#   open builds them again from their definitions (defaults, 'local x = ...'), so a tab rollout that is removed
#   from a subRollout and added again starts from its defaults. Using a closed rollout's members raises here;
# * every control has the properties and events listed below, with their value types checked; setting a value
#   from script does not fire the control's event (a user action does: press, click, enter, choose, select);
# * a handler may name fewer arguments than its event passes (MAXScript passes the first ones), never more;
# * a rollout can be open once: createDialog / addSubRollout of an open rollout raises; removeSubRollout needs
#   a rollout that is in that subRollout; destroyDialog undefined is a run-time error (caught by try/catch);
# * the .NET TabControl: TabPages.Add (a TabPage or a string), TabPages.Count, SelectedIndex (-1 with no page,
#   0 after the first Add, which fires no event; setting it out of range throws ArgumentOutOfRangeException;
#   a change fires Selecting, Selected and SelectedIndexChanged, with (sender, args)).

class NetTabPage:
    """System.Windows.Forms.TabPage(text)."""

    def __init__(self, text=""):
        if not isinstance(text, str):
            raise MXSError(f"TabPage: string expected, got {text!r}")
        self.Text = text

    def mxs_get(self, k):
        if k == "text":
            return self.Text
        raise MXSError(f"TabPage.{k} is not modelled")


class _NetTabPages:
    def __init__(self, owner):
        self._owner, self._pages = owner, []

    def mxs_get(self, k):
        if k == "add":
            return self.Add
        if k == "count":
            return len(self._pages)
        raise MXSError(f"TabPageCollection.{k} is not modelled")

    def Add(self, page):
        if isinstance(page, str):
            page = NetTabPage(page)
        if not isinstance(page, NetTabPage):
            raise MXSError(f"TabPages.Add: a TabPage or a string, got {page!r}")
        self._pages.append(page)
        if self._owner._index < 0:
            self._owner._index = 0              # the first page is selected; no event (handle not shown yet)
        return None


class NetTabControl:
    """System.Windows.Forms.TabControl inside a dotNetControl. ``fire(event, args)`` calls the rollout handler."""

    def __init__(self):
        self.TabPages = _NetTabPages(self)
        self._index = -1
        self.fire = None

    def mxs_get(self, k):
        if k == "tabpages":
            return self.TabPages
        if k == "selectedindex":
            return self._index
        if k == "tabcount":
            return len(self.TabPages._pages)
        raise MXSError(f"TabControl.{k} is not modelled")

    def mxs_set(self, k, v):
        if k != "selectedindex":
            raise MXSError(f"TabControl.{k} cannot be set here (not modelled)")
        if isinstance(v, bool) or not isinstance(v, int):
            raise MXSError(f"TabControl.SelectedIndex: integer expected, got {v!r}")
        if not -1 <= v < len(self.TabPages._pages):
            raise MXSRuntimeError(f".NET runtime exception: InvalidArgument=Value of '{v}' is not valid for "
                                  "'SelectedIndex'. (ArgumentOutOfRangeException)")
        if v != self._index:
            self._index = v
            for ev in ("selecting", "selected", "selectedindexchanged"):
                if self.fire:
                    self.fire(ev, (self, ("eventargs", ev, v)))

    # -- the user (test helper)
    def click_tab(self, i):
        """The user clicks tab i (0-based)."""
        assert 0 <= i < len(self.TabPages._pages)
        self.mxs_set("selectedindex", i)


def _type_check(v, kinds, what):
    if isinstance(v, bool) and bool not in kinds:
        raise MXSError(f"{what}: {v!r} has the wrong type")
    if not isinstance(v, kinds):
        raise MXSError(f"{what}: {v!r} has the wrong type")
    return v


class Control:
    """A rollout control. KW: the keyword parameters its definition may use; PROPS: readable properties;
    SETTABLE: properties a script may set; EVENTS: event name -> number of arguments it passes."""
    KW = ("width", "height", "align", "offset", "enabled", "tooltip", "across")
    PROPS = ("enabled", "caption", "text", "width", "height")
    SETTABLE = ("enabled", "caption", "text")
    EVENTS = {}

    def __init__(self, rollout, name, caption, kw):
        self.rollout, self.name = rollout, name
        bad = set(kw) - set(self.KW) - set(Control.KW)
        if bad:
            raise MXSError(f"{type(self).__name__.lower()} {name}: unknown parameters {sorted(bad)}")
        self.caption = caption if caption is not None else ""
        self.enabled = _type_check(kw.get("enabled", True), (bool,), f"{name}.enabled")
        self.width, self.height = kw.get("width"), kw.get("height")

    def mxs_get(self, k):
        if k not in self.PROPS:
            raise MXSError(f"{self.name}.{k}: not a property of this control (or not modelled)")
        return getattr(self, "_get_" + k, lambda: getattr(self, k if k != "text" else "caption"))()

    def mxs_set(self, k, v):
        if k not in self.SETTABLE:
            raise MXSError(f"{self.name}.{k} cannot be set")
        if k == "enabled":
            self.enabled = _type_check(v, (bool,), f"{self.name}.enabled")
        elif k in ("caption", "text"):
            self.caption = _type_check(v, (str,), f"{self.name}.{k}")
        else:
            getattr(self, "_set_" + k)(v)

    def _user(self):
        assert self.rollout.is_open, f"{self.name}: the rollout {self.rollout.name} is not open"
        assert self.enabled, f"{self.name} is disabled: the user cannot use it"


class Checkbox(Control):
    KW = ("checked",)
    PROPS = Control.PROPS + ("checked", "state")
    SETTABLE = Control.SETTABLE + ("checked", "state")
    EVENTS = {"changed": 1}

    def __init__(self, rollout, name, caption, kw):
        super().__init__(rollout, name, caption, kw)
        self.checked = _type_check(kw.get("checked", False), (bool,), f"{name} checked:")

    def _get_state(self):
        return self.checked

    def mxs_set(self, k, v):
        if k in ("checked", "state"):
            self.checked = _type_check(v, (bool,), f"{self.name}.checked")
        else:
            super().mxs_set(k, v)

    def click(self):
        """The user clicks the checkbox."""
        self._user()
        self.checked = not self.checked
        self.rollout.fire(self.name, "changed", self.checked)


class Spinner(Control):
    KW = ("range", "type", "scale", "fieldwidth")
    PROPS = Control.PROPS + ("value", "range", "indeterminate")
    SETTABLE = Control.SETTABLE + ("value", "range")
    EVENTS = {"changed": 2, "entered": 2, "buttondown": 0, "buttonup": 1}

    def __init__(self, rollout, name, caption, kw):
        super().__init__(rollout, name, caption, kw)
        t = kw.get("type", Name("float"))
        if t not in ("float", "integer", "worldunits"):
            raise MXSError(f"spinner {name}: type {t!r}")
        self.type = t
        rg = kw.get("range", P3(0, 100, 0))
        if not isinstance(rg, P3):
            raise MXSError(f"spinner {name}: range must be [min, max, value]")
        self.range = rg
        self.indeterminate = False
        self.value = self._coerce(rg[2])

    def _coerce(self, v):
        if isinstance(v, bool) or not isinstance(v, (int, float)):
            raise MXSError(f"{self.name}.value: number expected, got {v!r}")
        v = min(max(v, self.range[0]), self.range[1])          # Max clamps to the range
        return int(v) if self.type == "integer" else float(v)

    def mxs_set(self, k, v):
        if k == "value":
            self.value = self._coerce(v)
        elif k == "range":
            if not isinstance(v, P3):
                raise MXSError(f"{self.name}.range: point3 expected")
            self.range = v
            self.value = self._coerce(v[2])
        else:
            super().mxs_set(k, v)

    def enter(self, v):
        """The user types v and presses Enter."""
        self._user()
        self.value = self._coerce(v)
        self.rollout.fire(self.name, "changed", self.value, False)
        self.rollout.fire(self.name, "entered", False, False)


class Label(Control):
    KW = ("style_sunkenedge",)
    PROPS = ("enabled", "caption", "text", "width", "height")
    EVENTS = {}

    def __init__(self, rollout, name, caption, kw):
        super().__init__(rollout, name, caption, kw)


class Button(Control):
    KW = ("images", "border")
    EVENTS = {"pressed": 0, "rightclick": 0}

    def press(self):
        """The user presses the button."""
        self._user()
        self.rollout.fire(self.name, "pressed")


class EditText(Control):
    KW = ("text", "labelontop", "fieldwidth", "readonly", "bold")
    PROPS = Control.PROPS + ("readonly",)
    EVENTS = {"changed": 1, "entered": 1}

    def __init__(self, rollout, name, caption, kw):
        super().__init__(rollout, name, caption, kw)
        self.label = self.caption
        self.caption = _type_check(kw.get("text", ""), (str,), f"{name} text:")
        self.readonly = kw.get("readonly", False)

    def type_text(self, v):
        """The user types the whole text v and presses Enter."""
        self._user()
        self.caption = _type_check(v, (str,), self.name)
        self.rollout.fire(self.name, "changed", v)
        self.rollout.fire(self.name, "entered", v)


class DropDownList(Control):
    KW = ("items", "selection")
    PROPS = Control.PROPS + ("items", "selection", "selected")
    SETTABLE = Control.SETTABLE + ("items", "selection", "selected")
    EVENTS = {"selected": 1}

    def __init__(self, rollout, name, caption, kw):
        super().__init__(rollout, name, caption, kw)
        self.items = self._items(kw.get("items", []))
        sel = kw.get("selection", 1)
        self.selection = 0
        self._select(sel if self.items else 0)

    def _items(self, v):
        if not isinstance(v, list) or not all(isinstance(x, str) for x in v):
            raise MXSError(f"{self.name}.items: an array of strings is needed, got {v!r}")
        return list(v)

    def _select(self, i):
        if isinstance(i, bool) or not isinstance(i, int) or not 0 <= i <= len(self.items):
            raise MXSError(f"{self.name}.selection: {i!r} is not 0..{len(self.items)}")
        self.selection = i

    def _get_selected(self):
        return self.items[self.selection - 1] if self.selection > 0 else None

    def mxs_set(self, k, v):
        if k == "items":
            self.items = self._items(v)
            # the selection is kept when still valid, else the first item (none for an empty list)
            if not 1 <= self.selection <= len(self.items):
                self.selection = 1 if self.items else 0
        elif k == "selection":
            self._select(v)
        elif k == "selected":
            if v not in self.items:
                raise MXSError(f"{self.name}.selected: {v!r} is not an item")
            self.selection = self.items.index(v) + 1
        else:
            super().mxs_set(k, v)

    def choose(self, i):
        """The user picks item i (1-based)."""
        self._user()
        assert 1 <= i <= len(self.items), f"{self.name}: no item {i}"
        self.selection = i
        self.rollout.fire(self.name, "selected", i)


class DotNetControl(Control):
    """dotNetControl name "<class>": only System.Windows.Forms.TabControl is modelled. Its properties are the
    .NET object's; its events are the .NET events (sender, args)."""
    EVENTS = {"selecting": 2, "selected": 2, "selectedindexchanged": 2, "click": 2}

    def __init__(self, rollout, name, caption, kw):
        super().__init__(rollout, name, None, kw)
        if caption != "System.Windows.Forms.TabControl":
            raise MXSError(f"dotNetControl {name}: class {caption!r} is not modelled")
        self.net = NetTabControl()
        self.net.fire = lambda ev, args: self.rollout.fire(self.name, ev, *args)

    def mxs_get(self, k):
        return self.net.mxs_get(k)

    def mxs_set(self, k, v):
        return self.net.mxs_set(k, v)


class SubRollout(Control):
    PROPS = ("rollouts", "height", "width")
    EVENTS = {}

    def __init__(self, rollout, name, caption, kw):
        super().__init__(rollout, name, caption, kw)
        self.rollouts = []

    def _get_rollouts(self):
        return list(self.rollouts)


CONTROLS = {"checkbox": Checkbox, "spinner": Spinner, "label": Label, "button": Button, "edittext": EditText,
            "dropdownlist": DropDownList, "dotnetcontrol": DotNetControl, "subrollout": SubRollout}
ROLLOUT_EVENTS = {"open": 0, "close": 0, "resized": 1, "moved": 1}


class Rollout:
    """A rollout definition (the global value) and, while it is open, its controls, locals and functions."""

    def __init__(self, interp, name, title, kw, items):
        self.interp, self.name, self.title, self.items = interp, name, title, items
        bad = set(kw) - {"width", "height"}
        if bad:
            raise MXSError(f"rollout {name}: unknown parameters {sorted(bad)}")
        self.kw = kw
        self.is_open, self.host = False, None
        self.controls, self.locals, self.handlers = {}, {}, {}
        for it in self._flat():
            if it[0] == "on":
                key = (it[1], it[2])
                if key in self.handlers:
                    raise MXSError(f"line {it[5]}: rollout {name}: two handlers for {it[1]} {it[2]}")
                self.handlers[key] = it

    def _flat(self):
        for it in self.items:
            if it[0] == "group":
                yield from it[2]
            else:
                yield it

    # -- scope
    def has(self, name):
        return self.is_open and (name in self.controls or name in self.locals)

    def member(self, name):
        return self.controls[name] if name in self.controls else self.locals[name]

    def set_local(self, name, val):
        if name in self.controls:
            raise MXSError(f"{self.name}.{name} is a control: it cannot be assigned")
        self.locals[name] = val

    def mxs_get(self, k):
        if k == "open":
            return self.is_open
        if not self.is_open:
            raise MXSError(f"{self.name}.{k}: the rollout is not open")
        if k in self.controls or k in self.locals:
            return self.member(k)
        raise MXSError(f"{self.name} has no control, local or function {k!r}")

    def mxs_set(self, k, v):
        if not self.is_open:
            raise MXSError(f"{self.name}.{k}: the rollout is not open")
        if k in self.locals and not isinstance(self.locals[k], Fn):
            self.locals[k] = v
        else:
            raise MXSError(f"{self.name}.{k} cannot be assigned")

    # -- open / close
    def open(self, host):
        if self.is_open:
            raise MXSRuntimeError(f"rollout {self.name} is already open")
        it = self.interp
        self.controls, self.locals = {}, {}
        for item in self._flat():
            if item[0] == "control":
                _, ctype, cname, cap, kw, line = item
                if cname in self.controls:
                    raise MXSError(f"line {line}: rollout {self.name}: two controls named {cname}")
                self.controls[cname] = CONTROLS[ctype](self, cname, cap, {k: it.eval(v) for k, v in kw.items()})
        for (target, event), h in self.handlers.items():
            if target == self.name:
                n = ROLLOUT_EVENTS.get(event)
            elif target in self.controls:
                n = type(self.controls[target]).EVENTS.get(event)
            else:
                raise MXSError(f"line {h[5]}: rollout {self.name}: handler for unknown control {target}")
            if n is None:
                raise MXSError(f"line {h[5]}: {target} has no event {event!r}")
            if len(h[3]) > n:
                raise MXSError(f"line {h[5]}: on {target} {event}: {len(h[3])} arguments, the event passes {n}")
        self.is_open, self.host = True, host
        for item in self.items:                     # locals and functions, in order, in the rollout's scope
            if item[0] == "decl":
                for name, val in item[2]:
                    if name in self.controls or name in self.locals:
                        raise MXSError(f"rollout {self.name}: {name} is defined twice")
                    self.locals[name] = self._run(val) if val is not None else None
            elif item[0] == "fn":
                if item[1] in self.controls or item[1] in self.locals:
                    raise MXSError(f"rollout {self.name}: {item[1]} is defined twice")
                self.locals[item[1]] = Fn(item[1], item[2], item[3], it, item[4], rollout=self)
        self.fire(self.name, "open")

    def close(self):
        assert self.is_open
        for c in self.controls.values():
            if isinstance(c, SubRollout):
                for r in list(c.rollouts):
                    r.close()
                c.rollouts = []
        self.fire(self.name, "close")
        self.is_open, self.host = False, None

    def _run(self, expr, args=None):
        f = Frame(args or {})
        f.rollout = self
        it = self.interp
        it.frames.append(f)
        try:
            return it.eval(expr)
        except _Return as r:
            return r.v
        finally:
            it.frames.pop()

    def fire(self, target, event, *args):
        h = self.handlers.get((target, event))
        if h is None or not self.is_open:
            return None
        return self._run(h[4], dict(zip(h[3], args)))

    # -- test helpers
    def ctrl(self, name):
        assert self.is_open, f"rollout {self.name} is not open"
        return self.controls[name.lower()]

# ============================================================================ interpreter

class Interp:
    def __init__(self, scene=None, stop_marker="-- ============================================================================ UI"):
        self.globals = {}
        self.stop_marker = stop_marker
        self.frames = []
        self.sysglobals = {}
        self.exceptions = []                  # the exceptions being handled by catch blocks (getCurrentException)
        self.ui_answers = []                  # what the user picks in the next file dialogs (None = Cancel)
        self.messages = []                    # messageBox texts
        self.builtins = self._builtins(scene)
        self.builtins.update(self._ui_builtins())

    def run_file(self, path):
        src = open(path, encoding="utf-8").read()
        if self.stop_marker and self.stop_marker in src:
            src = src[:src.index(self.stop_marker)]
        for e in Parser(tokenize(src)).program():
            self.eval(e)

    def fn(self, name):
        return self.globals[name.lower()]

    # -- builtins ------------------------------------------------------------
    def _builtins(self, scene):
        b = {}

        def fopen(path, mode):
            try:
                return _BinFile(path, mode)
            except OSError:
                return None

        def readByte(f, sign=Name("signed")):
            d = f.f.read(1)
            if len(d) < 1:
                return None
            return d[0] if sign == "unsigned" else struct.unpack("<b", d)[0]

        def readShort(f, sign=Name("signed")):
            d = f.f.read(2)
            if len(d) < 2:
                return None
            return struct.unpack("<H" if sign == "unsigned" else "<h", d)[0]

        def readLong(f, sign=Name("signed")):
            d = f.f.read(4)
            if len(d) < 4:
                return None
            return struct.unpack("<I" if sign == "unsigned" else "<i", d)[0]

        def readFloat(f):
            d = f.f.read(4)
            return struct.unpack("<f", d)[0] if len(d) == 4 else None

        def writeByte(f, v, sign=Name("signed")):
            f.f.write(bytes((v & 0xFF,)))
            return True

        def fseek(f, pos, how):
            assert how == "seek_set"
            f.f.seek(pos)
            return True

        def fclose(f):
            f.f.close()
            return True

        def append(arr, v):
            if not isinstance(arr, list):
                raise MXSError("append: not an array")
            arr.append(v)
            return arr

        def getDir(which):
            assert which == "temp"
            return self.temp_dir

        def makeDir(path, all=False):
            os.makedirs(path, exist_ok=True)
            return True

        def getFilenamePath(p):
            i = max(p.rfind("\\"), p.rfind("/"))
            return p[:i + 1] if i >= 0 else ""

        def filenameFromPath(p):
            i = max(p.rfind("\\"), p.rfind("/"))
            return p[i + 1:]

        def getFilenameFile(p):
            return os.path.splitext(filenameFromPath(p))[0]

        def trimRight(s, chars=" \t\n"):
            return s.rstrip(chars)

        def findString(s, sub):
            i = s.find(sub)
            return None if i < 0 else i + 1

        def getFiles(pattern):
            d, pat = os.path.split(pattern)
            return sorted(os.path.join(d, f) for f in os.listdir(d)
                          if glob.fnmatch.fnmatch(f.lower(), pat.lower())) if os.path.isdir(d) else []

        def toUpper(s):
            return s.upper()

        def findItem(arr, v):
            for i, x in enumerate(arr, 1):
                if x == v:
                    return i
            return 0

        def ftell(f):
            return f.f.tell()

        def _num(x, fname):
            if isinstance(x, bool) or not isinstance(x, (int, float)):
                raise MXSError(f"{fname}: number expected, got {x!r}")
            return x

        def sin(x):                      # MAXScript trig takes degrees
            return math.sin(math.radians(_num(x, "sin")))

        def cos(x):
            return math.cos(math.radians(_num(x, "cos")))

        def filterString(s, delims):
            # splits at any one of the delimiter characters and drops empty tokens (splitEmptyTokens:false)
            out, cur = [], ""
            for ch in s:
                if ch in delims:
                    if cur:
                        out.append(cur)
                    cur = ""
                else:
                    cur += ch
            if cur:
                out.append(cur)
            return out

        def trimLeft(s, chars=" \t\n"):
            return s.lstrip(chars)

        def length(v):                   # length <point3> (vector length); not for arrays/strings
            if not isinstance(v, P3):
                raise MXSError(f"length: point3 expected, got {v!r}")
            return math.sqrt(sum(c * c for c in v))

        def _pt(v, fname):
            if not isinstance(v, P3):
                raise MXSError(f"{fname}: point3 expected, got {v!r}")
            return v

        def cross(a, b):                 # cross <point3> <point3>
            a, b = _pt(a, "cross"), _pt(b, "cross")
            return P3(a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])

        def dot(a, b):                   # dot <point3> <point3>
            a, b = _pt(a, "dot"), _pt(b, "dot")
            return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]

        def normalize(v):                # normalize <point3>; a zero vector has no direction: refused here
            v = _pt(v, "normalize")
            n = math.sqrt(sum(c * c for c in v))
            if n == 0.0:
                raise MXSError("normalize: zero-length point3")
            return P3(*(c / n for c in v))

        def abs_(x):
            return abs(_num(x, "abs"))

        def floor_(x):                   # floor <number> -> float
            return float(math.floor(_num(x, "floor")))

        def inverse(m):                  # inverse <matrix3>
            if not isinstance(m, M3):
                raise MXSError(f"inverse: matrix3 expected, got {m!r}")
            return m.inverse()

        def _ctrl(c, fname):
            if not isinstance(c, _Controller):
                raise MXSError(f"{fname}: controller expected, got {c!r}")
            return c

        def numKeys(c):                  # numKeys <controller>: -1 for a controller without keys of its own
            return _ctrl(c, "numKeys").numKeys()

        def getKeyTime(c, i):            # getKeyTime <controller> <index> -> Time
            return _ctrl(c, "getKeyTime").getKeyTime(i)

        def interval(a, b):              # interval <start> <end> (frames)
            for v in (a, b):
                _num(v, "interval")
            return Interval(a, b)

        b.update({k.lower(): v for k, v in {
            "cross": cross, "dot": dot, "normalize": normalize, "abs": abs_, "floor": floor_, "inverse": inverse,
            "numKeys": numKeys, "getKeyTime": getKeyTime, "interval": interval,
            "fopen": fopen, "readByte": readByte, "readShort": readShort, "readLong": readLong,
            "readFloat": readFloat, "writeByte": writeByte, "fseek": fseek, "fclose": fclose,
            "getFileSize": lambda p: os.path.getsize(p) if os.path.isfile(p) else 0, "append": append,
            "toUpper": toUpper, "getDir": getDir, "makeDir": makeDir, "doesFileExist": os.path.exists,
            "getFilenamePath": getFilenamePath, "getFilenameFile": getFilenameFile,
            "filenameFromPath": filenameFromPath, "trimRight": trimRight, "findString": findString,
            "getFiles": getFiles, "bit": _bit(), "matrix3": lambda a, b_, c, d: M3(a, b_, c, d),
            "dotNetClass": _dotnet_class, "dotNetObject": _dotnet_object, "findItem": findItem, "ftell": ftell,
            "sin": sin, "cos": cos, "filterString": filterString, "trimLeft": trimLeft, "length": length,
            "formattedPrint": formatted_print,
        }.items()})
        if scene is not None:
            rt = scene
            for name in ("mesh", "meshop", "setFaceSmoothGroup", "update", "BoneSys", "addModifier", "select",
                         "modPanel", "skinOps", "classOf", "setSubMtl", "setUserProp", "getUserProp", "showTextureMap",
                         "instance"):
                b[name.lower()] = getattr(rt, name)
            b.update({"dummy": rt.Dummy, "skin": rt.Skin, "multimaterial": rt.MultiMaterial,
                      "standardmaterial": rt.StandardMaterial, "color": rt.Color, "bitmaptexture": rt.Bitmaptexture,
                      "edit_normals": rt.Edit_Normals, "units": _Units(rt.units)})
            self.scene = rt

            def set_range(v):
                if not isinstance(v, Interval):
                    raise MXSError(f"animationRange: an interval is needed, got {v!r}")
                if not v.start.ticks < v.end.ticks:
                    raise MXSRuntimeError(f"animationRange: the start must be before the end, got {v!r}")
                rt.animationRange = v

            # read live from the scene; only animationRange can be assigned
            self.sysglobals = {"objects": (lambda: rt.objects, None), "selection": (lambda: list(rt.selection), None),
                               "animationrange": (lambda: rt.animationRange, set_range)}
        self.temp_dir = os.environ.get("TEMP", "/tmp")
        return b

    def _ui_builtins(self):
        def answer(kind, caption):
            if not self.ui_answers:
                raise MXSError(f"{kind} {caption!r} opened, but the test gave no answer")
            a = self.ui_answers.pop(0)
            if a is not None and not isinstance(a, str):
                raise MXSError(f"{kind}: test answer must be a path or None")
            return a

        def getOpenFileName(caption="", types="", filename=""):
            return answer("getOpenFileName", caption)

        def getSavePath(caption="", initialDir=""):
            return answer("getSavePath", caption)

        def messageBox(msg, title="MAXScript", beep=True):
            if not isinstance(msg, str) or not isinstance(title, str):
                raise MXSError(f"messageBox: strings expected, got {msg!r}")
            self.messages.append((title, msg))
            return True

        def getCurrentException():
            if not self.exceptions:
                raise MXSError("getCurrentException: not inside a catch block")
            x = self.exceptions[-1]
            return str(x.args[0]) if x.args else ""

        def substituteString(src, frm, to):
            for v in (src, frm, to):
                if not isinstance(v, str):
                    raise MXSError(f"substituteString: strings expected, got {v!r}")
            return src.replace(frm, to)

        def createDialog(r, width=None, height=None):
            if not isinstance(r, Rollout):
                raise MXSRuntimeError(f"createDialog: a rollout is needed, got {r!r}")
            r.open("dialog")
            return True

        def destroyDialog(r):
            if not isinstance(r, Rollout):
                raise MXSRuntimeError(f"destroyDialog: a rollout is needed, got {r!r}")
            if r.is_open and r.host == "dialog":
                r.close()
            return None

        def _sub(sr, fname):
            if not isinstance(sr, SubRollout):
                raise MXSError(f"{fname}: a subRollout control is needed, got {sr!r}")
            if not sr.rollout.is_open:
                raise MXSError(f"{fname}: the subRollout's rollout is not open")
            return sr

        def addSubRollout(sr, r, rolledUp=False):
            _sub(sr, "addSubRollout")
            if not isinstance(r, Rollout):
                raise MXSRuntimeError(f"addSubRollout: a rollout is needed, got {r!r}")
            r.open(sr)
            sr.rollouts.append(r)
            return True

        def removeSubRollout(sr, r):
            _sub(sr, "removeSubRollout")
            if r not in sr.rollouts:
                raise MXSRuntimeError(f"removeSubRollout: {getattr(r, 'name', r)!r} is not in {sr.name}")
            sr.rollouts.remove(r)
            r.close()
            return True

        return {k.lower(): v for k, v in {
            "getOpenFileName": getOpenFileName, "getSavePath": getSavePath, "messageBox": messageBox,
            "getCurrentException": getCurrentException, "substituteString": substituteString,
            "createDialog": createDialog, "destroyDialog": destroyDialog, "addSubRollout": addSubRollout,
            "removeSubRollout": removeSubRollout}.items()}

    # -- evaluation ----------------------------------------------------------
    def lookup(self, name):
        if self.frames and name in self.frames[-1]:
            return self.frames[-1][name]
        if self.frames and self.frames[-1].rollout is not None and self.frames[-1].rollout.has(name):
            return self.frames[-1].rollout.member(name)
        if name in self.globals:
            return self.globals[name]
        if name in self.sysglobals:
            return self.sysglobals[name][0]()
        if name in self.builtins:
            return self.builtins[name]
        raise MXSError(f"undefined variable or function {name!r}")

    def store(self, name, val, declare=None):
        if declare == "local" and self.frames:
            self.frames[-1][name] = val
        elif declare in ("global", "local"):
            self.globals[name] = val
        elif self.frames and name in self.frames[-1]:
            self.frames[-1][name] = val
        elif self.frames and self.frames[-1].rollout is not None and self.frames[-1].rollout.has(name):
            self.frames[-1].rollout.set_local(name, val)
        elif name in self.globals:
            self.globals[name] = val
        elif name in self.sysglobals and declare is None:
            if self.sysglobals[name][1] is None:
                raise MXSError(f"{name} cannot be assigned")
            self.sysglobals[name][1](val)
        else:
            raise MXSError(f"assignment to undeclared variable {name!r}")

    def call_fn(self, fn, args, kwvals=None):
        f = Frame({**dict(zip(fn.params, args)), **(kwvals or {})})
        f.rollout = fn.rollout
        self.frames.append(f)
        try:
            return self.eval(fn.body)
        except _Return as r:
            return r.v
        finally:
            self.frames.pop()

    def eval(self, e):
        k = e[0]
        if k == "lit":
            return e[1]
        if k == "name":
            return self.lookup(e[1])
        if k == "block":
            v = None
            for s in e[1]:
                v = self.eval(s)
            return v
        if k == "array":
            return [self.eval(x) for x in e[1]]
        if k == "point3":
            return P3(*(self.eval(x) for x in e[1]))
        if (k in ("fn", "struct") and isinstance(self.globals.get(e[1]), (Fn, Struct))
                and getattr(self.globals[e[1]], "ast", None) != e):
            # 3ds Max silently replaces the global (names are case-insensitive: struct DM_MotionRec and
            # fn DM_motionRec are one name), which breaks every later use of the first one. The same definition
            # again (the script run a second time in one session) is fine.
            raise MXSError(f"{k} {e[1]}: redefines an existing function/struct of the same (case-insensitive) name")
        if k == "fn":
            f = Fn(e[1], e[2], e[3], self, e[4])
            f.ast = e
            self.globals[e[1]] = f
            return f
        if k == "struct":
            s = Struct(e[1], e[2])
            s.ast = e
            self.globals[e[1]] = s
            return s
        if k == "decl":
            for name, val in e[2]:
                if e[1] == "global" and val is None and name in self.globals:
                    continue                  # 'global x' keeps an existing global's value
                self.store(name, self.eval(val) if val is not None else None, e[1])
            return None
        if k == "try":
            try:
                return self.eval(e[1])
            except (Thrown, MXSRuntimeError) as x:
                self.exceptions.append(x)
                try:
                    return self.eval(e[2])
                finally:
                    self.exceptions.pop()
        if k == "rollout":
            old = self.globals.get(e[1])
            if isinstance(old, (Fn, Struct)):
                raise MXSError(f"rollout {e[1]}: redefines an existing function/struct")
            if isinstance(old, Rollout) and old.is_open:
                raise MXSError(f"rollout {e[1]}: redefined while open (destroy the dialog first)")
            r = Rollout(self, e[1], e[2], e[3], e[4])
            self.globals[e[1]] = r
            return r
        if k == "assign":
            op, lhs, rhs = e[1], e[2], e[3]
            val = self.eval(rhs)
            if op != "=":
                val = arith(op[0], self.eval(lhs), val)
            if lhs[0] == "name":
                self.store(lhs[1], val)
            elif lhs[0] == "prop":
                prop_set(self.eval(lhs[1]), lhs[2], val)
            elif lhs[0] == "index":
                arr, i = self.eval(lhs[1]), self.eval(lhs[2])
                if not isinstance(arr, list) or not 1 <= i <= len(arr):
                    raise MXSError(f"index {i} out of range")
                arr[i - 1] = val
            else:
                raise MXSError("bad assignment target")
            return val
        if k == "if":
            if self._truth(self.eval(e[1])):
                return self.eval(e[2])
            return self.eval(e[3]) if e[3] is not None else None
        if k == "while":
            n = 0
            while self._truth(self.eval(e[1])):
                self.eval(e[2])
                n += 1
                if n > 10_000_000:
                    raise MXSError("while loop runaway")
            return None
        if k == "for":
            a, b = self.eval(e[2]), self.eval(e[3])
            step = self.eval(e[4]) if e[4] is not None else 1
            i = a
            while (i <= b) if step > 0 else (i >= b):
                self.store(e[1], i, "local")
                self.eval(e[5])
                i += step
            return None
        if k == "forin":
            seq = self.eval(e[2])
            if not isinstance(seq, (list, str)):
                raise MXSError("for ... in: not an array")
            for v in list(seq):
                self.store(e[1], v, "local")
                self.eval(e[3])
            return None
        if k == "return":
            raise _Return(self.eval(e[1]))
        if k == "throw":
            raise Thrown(self.eval(e[1]))
        if k == "maxcmd":
            self.scene.execute("max " + e[1])
            return True
        if k == "animate":
            # 'animate on': property changes become keys (the mock records them at the current 'at time')
            old, self.scene._animate = self.scene._animate, self.eval(e[1])
            try:
                return self.eval(e[2])
            finally:
                self.scene._animate = old
        if k == "with":
            # 'with undo off' / 'with redraw off': the mock records the state (Edit_Normals counts the
            # calls made with undo or redraw still on)
            attr = {"undo": "undo_on", "redraw": "redraw_on"}[e[1]]
            old = getattr(self.scene, attr)
            setattr(self.scene, attr, self.eval(e[2]))
            try:
                return self.eval(e[3])
            finally:
                setattr(self.scene, attr, old)
        if k == "attime":
            t = self.eval(e[1])
            if isinstance(t, bool) or not isinstance(t, (int, float)):
                raise MXSError(f"at time: a time (frame number) is needed, got {t!r}")
            old, self.scene.time = self.scene.time, t
            try:
                return self.eval(e[2])
            finally:
                self.scene.time = old
        if k == "and":
            return self._truth(self.eval(e[1])) and self._truth(self.eval(e[2]))
        if k == "or":
            return self._truth(self.eval(e[1])) or self._truth(self.eval(e[2]))
        if k == "not":
            return not self._truth(self.eval(e[1]))
        if k == "neg":
            v = self.eval(e[1])
            return P3(*(-x for x in v)) if isinstance(v, P3) else -v
        if k == "bin":
            op, a, b = e[1], self.eval(e[2]), self.eval(e[3])
            if op == "==":
                return a == b
            if op == "!=":
                return a != b
            if op in ("<", ">", "<=", ">="):
                if a is None or b is None:
                    raise MXSError(f"comparison {op} with undefined")
                return {"<": a < b, ">": a > b, "<=": a <= b, ">=": a >= b}[op]
            return arith(op, a, b)
        if k == "as":
            v, t = self.eval(e[1]), e[2]
            if t == "integer":
                if isinstance(v, str):
                    # only plain digit strings are modelled ("8" as integer == 8); anything else raises
                    # rather than guess what 3ds Max returns
                    if not re.fullmatch(r"[0-9]{1,9}", v):
                        raise MXSError(f"{v!r} as integer: only plain digit strings are modelled")
                    return int(v)
                if v is None:
                    raise MXSError("undefined as integer")
                return int(v)
            if t == "float":
                return float(v)
            if t == "string":
                if isinstance(v, bool):
                    return "true" if v else "false"
                if isinstance(v, float):
                    raise MXSError("float as string: formatting not modelled")
                return str(v)
            raise MXSError(f"as {t} not supported")
        if k == "prop":
            return prop_get(self.eval(e[1]), e[2])
        if k == "index":
            arr, i = self.eval(e[1]), self.eval(e[2])
            if isinstance(arr, _Controller):         # MAXWrapper[i]: the i-th sub-anim
                return arr.subanim(i)
            if not isinstance(arr, (list, str)) or not isinstance(i, int) or not 1 <= i <= len(arr):
                raise MXSError(f"index {i!r} out of range for {type(arr).__name__} of {len(arr) if hasattr(arr, '__len__') else '?'}")
            return arr[i - 1]
        if k == "call":
            f = self.eval(e[1])
            args = [self.eval(a) for a in e[2]]
            kw = {n: self.eval(v) for n, v in e[3].items()}
            if isinstance(f, (Fn, Struct)) or callable(f):
                if isinstance(f, (Fn, Struct)):
                    return f(*args, **kw)
                return f(*args, **{_kw(f, n): v for n, v in kw.items()})
            raise MXSError(f"{e[1]} is not a function")
        raise MXSError(f"unknown node {k}")

    @staticmethod
    def _truth(v):
        if not isinstance(v, bool):
            raise MXSError(f"condition is not a boolean: {v!r}")
        return v


def _kw(f, name):
    """Map a MAXScript keyword (case-insensitive) to the Python parameter name of a builtin/mock."""
    import inspect
    try:
        params = inspect.signature(f).parameters
    except (TypeError, ValueError):
        return name
    for p in params:
        if p.lower() == name.lower():
            return p
    raise MXSError(f"keyword {name}: not accepted by {getattr(f, '__name__', f)}")
