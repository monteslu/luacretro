// luacretro dynamic tier - parser for the full PICO-8 Lua dialect.
//
// Recursive descent over lexFull() tokens, following z8lua's lparser.c:
// Lua 5.2 grammar plus PICO-8's one-line `if (c) stmt` / `while (c) stmt`
// forms (the body ends at the end of the line), `?expr` print, compound
// assignment for every binary operator, `do` accepted for `then`, unary
// peek operators @ % $, and z8lua's operator priorities.

// [left, right] priorities (z8lua lparser.c priority[])
const BINPRI = {
  "+": [10, 10], "-": [10, 10],
  "*": [11, 11], "/": [11, 11], "%": [11, 11], "\\": [11, 11],
  "^": [13, 12],
  "&": [6, 6], "|": [4, 4], "^^": [5, 5], "~": [5, 5],
  "<<": [7, 7], ">>": [7, 7], ">>>": [7, 7], "<<>": [7, 7], ">><": [7, 7],
  "..": [9, 8],
  "==": [3, 3], "<": [3, 3], "<=": [3, 3], "~=": [3, 3], ">": [3, 3], ">=": [3, 3],
  "and": [2, 2], "or": [1, 1],
};
const UNARY_PRI = 12;
const UNOPS = new Set(["not", "-", "~", "@", "%", "$", "#"]);
const COMPOUND = {
  "+=": "+", "-=": "-", "*=": "*", "/=": "/", "%=": "%", "^=": "^", "\\=": "\\",
  "..=": "..", "&=": "&", "|=": "|", "^^=": "^^", "<<=": "<<", ">>=": ">>",
  ">>>=": ">>>", "<<>=": "<<>", ">><=": ">><",
};

class ParseError extends Error {}

/**
 * @param {import('./lex.js').Token[]} tokens
 * @param {string} file
 * @returns {{chunk: object|null, diagnostics: object[]}}
 */
export function parseFull(tokens, file) {
  let p = 0;
  const diagnostics = [];
  const peek = (k = 0) => tokens[Math.min(p + k, tokens.length - 1)];
  const at = (t) => peek().t === t;
  const next = () => tokens[p++ < tokens.length ? p - 1 : tokens.length - 1];
  const fail = (msg, tok = peek()) => {
    diagnostics.push({ file, line: tok.line, col: tok.col, severity: "error", message: msg });
    throw new ParseError(msg);
  };
  const expect = (t, what) => {
    if (at(t)) return next();
    const tk = peek();
    return fail(`'${what ?? t}' expected near '${tk.t === "eof" ? "<eof>" : tk.v}'`);
  };
  const testnext = (t) => (at(t) ? (next(), true) : false);
  const node = (kind, tok, props) => ({ kind, line: tok.line, col: tok.col, ...props });

  // ---- blocks ------------------------------------------------------------------------

  const blockEnd = (t) => t === "end" || t === "else" || t === "elseif" || t === "until" || t === "eof";

  function block(untilLine = -1) {
    const tok = peek();
    const stmts = [];
    for (;;) {
      const t = peek();
      if (blockEnd(t.t)) break;
      if (untilLine >= 0 && t.line !== untilLine) break;
      if (t.t === "return") { stmts.push(retstat(untilLine)); break; }
      const s = statement(untilLine);
      if (s) stmts.push(s);
    }
    return node("Block", tok, { stmts });
  }

  function retstat(untilLine) {
    const tok = next();
    let exprs = [];
    const t = peek();
    if (!blockEnd(t.t) && t.t !== ";" && !(untilLine >= 0 && t.line !== untilLine)) exprs = explist();
    testnext(";");
    return node("Return", tok, { exprs });
  }

  function statement(untilLine) {
    const tok = peek();
    switch (tok.t) {
      case ";": next(); return null;
      case "if": return ifstat();
      case "while": return whilestat();
      case "do": {
        next();
        const body = block();
        expect("end");
        return node("Do", tok, { body });
      }
      case "for": return forstat();
      case "repeat": {
        next();
        const body = block();
        expect("until");
        const cond = expr();
        return node("Repeat", tok, { body, cond });
      }
      case "function": return funcstat();
      case "local":
        next();
        if (testnext("function")) {
          const name = expect("name", "<name>");
          const func = funcbody(tok, name.v, false);
          return node("LocalFunction", tok, { name: name.v, func });
        }
        return localstat(tok);
      case "::": {
        next();
        const name = expect("name", "<name>");
        expect("::");
        return node("Label", tok, { name: name.v });
      }
      case "return": return retstat(untilLine);
      case "break": next(); return node("Break", tok, {});
      case "goto": {
        next();
        const name = expect("name", "<name>");
        return node("Goto", tok, { name: name.v });
      }
      case "?": return printstat();
      default: return exprstat();
    }
  }

  function printstat() {
    const tok = next();
    const args = [];
    if (!at("eof") && peek().line === tok.line) args.push(...explist());
    return node("CallStat", tok, {
      call: node("Call", tok, { fn: node("Name", tok, { name: "print" }), args }),
    });
  }

  function ifstat() {
    const tok = next();
    const line = tok.line;
    const clauses = [];
    const cond = expr();
    const t = peek();
    const isShort = t.t !== "then" && t.t !== "do" && t.t !== "eof" && t.line === line;
    if (isShort) {
      const body = block(line);
      clauses.push({ cond, body });
      let orelse = null;
      while (at("elseif") && peek().line === line) {
        next();
        const c = expr();
        if (!testnext("then")) testnext("do");
        clauses.push({ cond: c, body: block(line) });
      }
      if (at("else") && peek().line === line) { next(); orelse = block(line); }
      return node("If", tok, { clauses, orelse, short: true });
    }
    if (!testnext("then") && !testnext("do")) fail(`'then' expected near '${peek().v}'`);
    clauses.push({ cond, body: block() });
    let orelse = null;
    for (;;) {
      if (at("elseif")) {
        next();
        const c = expr();
        if (!testnext("then") && !testnext("do")) fail(`'then' expected near '${peek().v}'`);
        clauses.push({ cond: c, body: block() });
        continue;
      }
      if (testnext("else")) orelse = block();
      break;
    }
    if (!at("end")) fail(`'end' expected (to close 'if' at line ${line}) near '${peek().t === "eof" ? "<eof>" : peek().v}'`);
    next();
    return node("If", tok, { clauses, orelse });
  }

  function whilestat() {
    const tok = next();
    const cond = expr();
    const t = peek();
    if (t.t !== "do" && t.t !== "eof" && t.line === tok.line) {
      return node("While", tok, { cond, body: block(tok.line) });
    }
    expect("do");
    const body = block();
    if (!at("end")) fail(`'end' expected (to close 'while' at line ${tok.line})`);
    next();
    return node("While", tok, { cond, body });
  }

  function forstat() {
    const tok = next();
    const n1 = expect("name", "<name>");
    if (at("=")) {
      next();
      const start = expr();
      expect(",");
      const limit = expr();
      const step = testnext(",") ? expr() : null;
      expect("do");
      const body = block();
      if (!at("end")) fail(`'end' expected (to close 'for' at line ${tok.line})`);
      next();
      return node("NumericFor", tok, { name: n1.v, start, limit, step, body });
    }
    const names = [n1.v];
    while (testnext(",")) names.push(expect("name", "<name>").v);
    expect("in");
    const exprs = explist();
    expect("do");
    const body = block();
    if (!at("end")) fail(`'end' expected (to close 'for' at line ${tok.line})`);
    next();
    return node("GenericFor", tok, { names, exprs, body });
  }

  function funcstat() {
    const tok = next();
    const n1 = expect("name", "<name>");
    let target = node("Name", n1, { name: n1.v });
    let fname = n1.v;
    let method = false;
    while (at(".") || at(":")) {
      const sep = next();
      const k = expect("name", "<name>");
      target = node("Index", sep, { obj: target, key: node("String", k, { value: k.v }) });
      fname += sep.t + k.v;
      if (sep.t === ":") { method = true; break; }
    }
    const func = funcbody(tok, fname, method);
    return node("Assign", tok, { targets: [target], exprs: [func] });
  }

  function funcbody(tok, name, method) {
    expect("(");
    const params = method ? ["self"] : [];
    let vararg = false;
    if (!at(")")) {
      for (;;) {
        if (at("...")) { next(); vararg = true; break; }
        params.push(expect("name", "<name>").v);
        if (!testnext(",")) break;
      }
    }
    expect(")");
    const body = block();
    if (!at("end")) fail(`'end' expected (to close 'function' at line ${tok.line}) near '${peek().t === "eof" ? "<eof>" : peek().v}'`);
    next();
    return node("Function", tok, { params, vararg, body, name });
  }

  function localstat(tok) {
    const names = [expect("name", "<name>").v];
    while (testnext(",")) names.push(expect("name", "<name>").v);
    const exprs = testnext("=") ? explist() : [];
    return node("Local", tok, { names, exprs });
  }

  function exprstat() {
    const tok = peek();
    const e = suffixedexp();
    if (at("=") || at(",")) {
      const targets = [e];
      while (testnext(",")) targets.push(suffixedexp());
      expect("=");
      const exprs = explist();
      for (const t of targets) if (t.kind !== "Name" && t.kind !== "Index") fail("syntax error (cannot assign)", tok);
      return node("Assign", tok, { targets, exprs });
    }
    const op = COMPOUND[peek().t];
    if (op) {
      next();
      if (e.kind !== "Name" && e.kind !== "Index") fail("syntax error (cannot assign)", tok);
      const value = expr();
      return node("CompoundAssign", tok, { target: e, op, value });
    }
    if (e.kind !== "Call" && e.kind !== "MethodCall") fail(`syntax error near '${peek().t === "eof" ? "<eof>" : peek().v}'`);
    return node("CallStat", tok, { call: e });
  }

  // ---- expressions -------------------------------------------------------------------

  function explist() {
    const list = [expr()];
    while (testnext(",")) list.push(expr());
    return list;
  }

  function expr(limit = 0) {
    const tok = peek();
    let left;
    if (UNOPS.has(tok.t)) {
      next();
      const operand = expr(UNARY_PRI);
      left = node("Unop", tok, { op: tok.t, expr: operand });
    } else left = simpleexp();
    for (;;) {
      const t = peek();
      const pri = BINPRI[t.t];
      if (!pri || pri[0] <= limit) break;
      next();
      const right = expr(pri[1]);
      left = node("Binop", t, { op: t.t === "~" ? "^^" : t.t, left, right });
    }
    return left;
  }

  function simpleexp() {
    const tok = peek();
    switch (tok.t) {
      case "number": next(); return node("Number", tok, { bits: tok.v });
      case "string": next(); return node("String", tok, { value: tok.v });
      case "nil": next(); return node("Nil", tok, {});
      case "true": next(); return node("True", tok, {});
      case "false": next(); return node("False", tok, {});
      case "...": next(); return node("Vararg", tok, {});
      case "{": return constructor();
      case "function": next(); return funcbody(tok, "anonymous", false);
      default: return suffixedexp();
    }
  }

  function primaryexp() {
    const tok = peek();
    if (tok.t === "name") { next(); return node("Name", tok, { name: tok.v }); }
    if (tok.t === "(") {
      next();
      const e = expr();
      expect(")");
      return node("Paren", tok, { expr: e });
    }
    if (tok.t === "?") {
      // z8lua lets ?... appear where an expression starts (it is a call)
      next();
      const args = at("eof") || peek().line !== tok.line ? [] : explist();
      return node("Call", tok, { fn: node("Name", tok, { name: "print" }), args });
    }
    return fail(`unexpected symbol near '${tok.t === "eof" ? "<eof>" : tok.v}'`);
  }

  function callargs() {
    const tok = peek();
    if (tok.t === "string") { next(); return [node("String", tok, { value: tok.v })]; }
    if (tok.t === "{") return [constructor()];
    expect("(");
    if (testnext(")")) return [];
    const args = explist();
    expect(")");
    return args;
  }

  function suffixedexp() {
    let e = primaryexp();
    for (;;) {
      const tok = peek();
      switch (tok.t) {
        case ".": {
          next();
          const k = expect("name", "<name>");
          e = node("Index", tok, { obj: e, key: node("String", k, { value: k.v }) });
          continue;
        }
        case "[": {
          next();
          const k = expr();
          expect("]");
          e = node("Index", tok, { obj: e, key: k });
          continue;
        }
        case ":": {
          next();
          const name = expect("name", "<name>");
          const args = callargs();
          e = node("MethodCall", tok, { obj: e, name: name.v, args });
          continue;
        }
        case "(": case "string": case "{": {
          // Lua 5.2: a call's '(' must be on the same line as the function
          // expression only to avoid ambiguity warnings; z8lua accepts both
          e = node("Call", tok, { fn: e, args: callargs() });
          continue;
        }
        default:
          return e;
      }
    }
  }

  function constructor() {
    const tok = expect("{");
    const items = [];
    while (!at("}")) {
      const t = peek();
      if (t.t === "[") {
        next();
        const key = expr();
        expect("]");
        expect("=");
        items.push({ kind: "keyed", key, value: expr() });
      } else if (t.t === "name" && peek(1).t === "=") {
        next(); next();
        items.push({ kind: "named", name: t.v, value: expr() });
      } else {
        items.push({ kind: "pos", value: expr() });
      }
      if (!testnext(",") && !testnext(";")) break;
    }
    expect("}");
    return node("Table", tok, { items });
  }

  let chunk = null;
  try {
    const tok = peek();
    const body = block();
    if (!at("eof")) fail(`'<eof>' expected near '${peek().v}'`);
    chunk = node("Function", tok, { params: [], vararg: true, body, name: "main chunk", isMain: true });
  } catch (e) {
    if (!(e instanceof ParseError)) throw e;
  }
  return { chunk, diagnostics };
}
