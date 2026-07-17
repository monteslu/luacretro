// luacretro parser - recursive descent, PICO-8-flavored Lua (shared
// front-end for gtlua / gbalua / mdlua).
//
// Dialect (PICO8.md): one-line `if (cond) stmt [else stmt]` / `while (cond)
// stmt` shorthand (parens required, newline ends the body), `\` floor
// division, `!=`, multiple assignment, bitwise operators. Cut Lua features
// fail here with the diagnostic the spec promises.

export function parse(tokens, file, sdkName = "luacretro") {
  let pos = 0;
  const diagnostics = [];

  const peek = (o = 0) => tokens[Math.min(pos + o, tokens.length - 1)];
  const at = (type) => peek().type === type;

  function error(msg, tok = peek()) {
    diagnostics.push({ file, line: tok.line, col: tok.col, severity: "error", message: msg });
  }

  function next() { return tokens[pos++]; }

  function expect(type, what) {
    if (at(type)) return next();
    error(`expected ${what ?? `'${type}'`} but found '${peek().value || peek().type}'`);
    return peek();
  }

  function sync(types) {
    while (!at("eof") && !types.includes(peek().type)) pos++;
  }

  // Consume tokens through the `end` that closes the CURRENT block, honoring
  // nesting (function/if/for/while/do all open blocks that close with `end`).
  // We're called sitting on the `function` keyword of an anonymous function we
  // can't parse; skip its whole body so recovery lands cleanly after `end`.
  function skipBalancedEnd() {
    let depth = 0;
    while (!at("eof")) {
      const t = peek().type;
      if (t === "function" || t === "if" || t === "for" || t === "while" || t === "do") depth++;
      else if (t === "end") { depth--; pos++; if (depth <= 0) return; continue; }
      pos++;
    }
  }

  // Consume through the `}` that closes the current table literal (we enter
  // sitting just after its `{`), honoring nested braces. Used to recover from a
  // table form the codegen can't represent without spraying downstream errors.
  function skipBalancedBrace() {
    let depth = 1;
    while (!at("eof")) {
      const t = peek().type;
      if (t === "{") depth++;
      else if (t === "}") { depth--; pos++; if (depth <= 0) return; continue; }
      pos++;
    }
  }

  // ---- statements ----------------------------------------------------------

  function block(enders) {
    const stmts = [];
    while (!at("eof") && !enders.includes(peek().type)) {
      const before = pos;
      const s = statement();
      if (s) stmts.push(s);
      if (pos === before) pos++;
    }
    return { kind: "block", stmts };
  }

  // statements until end-of-line `line` (for the one-line if/while shorthand)
  function lineBlock(line, extraEnders = []) {
    const stmts = [];
    while (!at("eof") && peek().line === line && !extraEnders.includes(peek().type)) {
      const before = pos;
      const s = statement();
      if (s) stmts.push(s);
      if (pos === before) pos++;
    }
    return { kind: "block", stmts };
  }

  function statement() {
    const tok = peek();
    switch (tok.type) {
      case ";": next(); return null;
      case "local": return localStmt();
      case "function": return functionStmt();
      case "if": return ifStmt();
      case "while": return whileStmt();
      case "for": return forStmt();
      case "repeat": return repeatStmt();
      case "return": {
        next();
        let value = null;
        if (!at("end") && !at("eof") && !at("else") && !at("elseif") && !at("until") &&
            peek().line === tok.line || (peek().line !== tok.line &&
            !at("end") && !at("eof") && !at("else") && !at("elseif") && !at("until") && !isStatementStart(peek()))) {
          if (!at("end") && !at("eof") && !at("else") && !at("elseif") && !at("until")) {
            value = expression();
            if (at(",")) {
              // multiple return: return a, b, c
              const values = [value];
              while (at(",")) { next(); values.push(expression()); }
              return { kind: "return", value, values, line: tok.line, col: tok.col };
            }
          }
        }
        return { kind: "return", value, line: tok.line, col: tok.col };
      }
      case "break": next(); return { kind: "break", line: tok.line, col: tok.col };
      case "do": {
        next();
        const body = block(["end"]);
        expect("end");
        return { kind: "do", body, line: tok.line, col: tok.col };
      }
      case "goto":
        error("goto is not supported (the runtime owns the main loop; use _draw())");
        // consume the target label name so it doesn't read as a bare statement
        next();
        if (at("name")) next();
        return null;
      case ":":
        // a `::label::` goto-label (lexed as : : name : :). goto is cut, so the
        // label is dead - skip it quietly to the closing `::` (no cascade).
        if (peek(1).type === ":") {
          next(); next();               // opening ::
          if (at("name")) next();       // label name
          if (at(":")) next();          // closing ::
          if (at(":")) next();
          return null;
        }
        return exprStatement();   // a lone ':' - let exprStatement report it
      default:
        return exprStatement();
    }
  }

  function isStatementStart(tok) {
    return ["local", "function", "if", "while", "for", "repeat", "return",
            "break", "do", "goto", "name", ";"].includes(tok.type);
  }

  function localStmt() {
    const tok = expect("local");
    if (at("function")) {
      next();
      return functionBody(expect("name", "function name"), tok);
    }
    const names = [expect("name", "variable name").value];
    while (at(",")) {
      next();
      names.push(expect("name", "variable name").value);
    }
    const inits = [];
    if (at("=")) {
      next();
      inits.push(expression());
      while (at(",")) { next(); inits.push(expression()); }
    }
    if (inits.length > names.length) {
      error(`${names.length} variable(s) but ${inits.length} value(s)`);
    }
    return { kind: "local", names, inits, line: tok.line, col: tok.col };
  }

  function functionStmt() {
    const tok = expect("function");
    const name = expect("name", "function name");
    if (at(".") || at(":")) {
      error("method definitions (function a.b / a:b) are not supported; use a plain function name");
      sync(["end", "eof"]);
      if (at("end")) next();
      return null;
    }
    return functionBody(name, tok);
  }

  function functionBody(nameTok, tok) {
    expect("(");
    const params = [];
    if (!at(")")) {
      for (;;) {
        if (at(".")) {
          error("variadic functions (...) are not supported");
          next(); if (at(".")) next(); if (at(".")) next();
        } else {
          params.push(expect("name", "parameter name").value);
        }
        if (at(",")) { next(); continue; }
        break;
      }
    }
    expect(")");
    const body = block(["end"]);
    expect("end");
    return { kind: "function", name: nameTok.value, params, body, line: tok.line, col: tok.col };
  }

  function ifStmt() {
    const tok = expect("if");
    const parenCond = at("(");
    const cond = expression();

    // Some PICO-8 minifiers emit `if cond do ... end` / `elseif cond do ...`
    // (PICO-8's parser tolerates `do` where `then` belongs). When the block form
    // uses `do`, skip the one-line-shorthand path and treat `do` as `then` below
    // (see expectThen) so a whole minified cart doesn't cascade off every `if`.

    // PICO-8 one-line shorthand: `if (cond) stmt [else stmt]` - parenthesized
    // condition, no `then`, body ends at end of line.
    if (parenCond && !at("then") && !at("do")) {
      if (peek().line !== tok.line || at("eof")) {
        error("expected 'then' (or a same-line statement for the `if (cond) stmt` shorthand)");
        return { kind: "if", clauses: [{ cond, body: { kind: "block", stmts: [] } }], elseBody: null, line: tok.line, col: tok.col };
      }
      const body = lineBlock(tok.line, ["else", "elseif", "end", "until"]);
      let elseBody = null;
      if (at("else") && peek().line === tok.line) {
        next();
        elseBody = lineBlock(tok.line, ["end", "until"]);
      }
      if (at("elseif") && peek().line === tok.line) {
        error("'elseif' is not allowed in the one-line if shorthand; use a full if/then/end");
      }
      if (body.stmts.length === 0 && !elseBody) {
        error("the one-line if shorthand needs a statement on the same line", tok);
      }
      return { kind: "if", clauses: [{ cond, body }], elseBody, line: tok.line, col: tok.col };
    }

    // accept `then` or the minifier's `do` after an if/elseif condition
    const expectThen = () => { if (at("do")) next(); else expect("then"); };

    const clauses = [];
    expectThen();
    let body = block(["elseif", "else", "end"]);
    clauses.push({ cond, body });
    let elseBody = null;
    for (;;) {
      if (at("elseif")) {
        next();
        const c = expression();
        expectThen();
        body = block(["elseif", "else", "end"]);
        clauses.push({ cond: c, body });
        continue;
      }
      if (at("else")) {
        next();
        elseBody = block(["end"]);
      }
      break;
    }
    expect("end");
    return { kind: "if", clauses, elseBody, line: tok.line, col: tok.col };
  }

  function whileStmt() {
    const tok = expect("while");
    const parenCond = at("(");
    const cond = expression();
    if (parenCond && !at("do")) {
      // one-line shorthand: `while (cond) stmt`
      if (peek().line !== tok.line || at("eof")) {
        error("expected 'do' (or a same-line statement for the `while (cond) stmt` shorthand)");
        return { kind: "while", cond, body: { kind: "block", stmts: [] }, line: tok.line, col: tok.col };
      }
      const body = lineBlock(tok.line, ["end", "until", "else", "elseif"]);
      return { kind: "while", cond, body, line: tok.line, col: tok.col };
    }
    expect("do");
    const body = block(["end"]);
    expect("end");
    return { kind: "while", cond, body, line: tok.line, col: tok.col };
  }

  function repeatStmt() {
    const tok = expect("repeat");
    const body = block(["until"]);
    expect("until");
    const cond = expression();
    return { kind: "repeat", body, cond, line: tok.line, col: tok.col };
  }

  function forStmt() {
    const tok = expect("for");
    const name = expect("name", "loop variable");
    if (at("in")) {
      next();
      const fn = expect("name", "'all'");
      if (fn.value !== "all") error("only 'for e in all(pool)' iteration is supported");
      expect("(");
      const poolExpr = expression();
      expect(")");
      expect("do");
      const body = block(["end"]);
      expect("end");
      return { kind: "forall", name: name.value, pool: poolExpr, body, line: tok.line, col: tok.col };
    }
    if (at(",")) {
      error("multiple loop variables are not supported; use 'for e in all(pool)'");
      sync(["end", "eof"]);
      if (at("end")) next();
      return null;
    }
    expect("=");
    const from = expression();
    expect(",");
    const to = expression();
    let step = null;
    if (at(",")) { next(); step = expression(); }
    expect("do");
    const body = block(["end"]);
    expect("end");
    return { kind: "fornum", name: name.value, from, to, step, body, line: tok.line, col: tok.col };
  }

  const ASSIGN_OPS = ["=", "+=", "-=", "*=", "/=", "\\=", "%=", "..=", "^="];

  function exprStatement() {
    const tok = peek();
    const target = expression();

    // multiple assignment: a, b = e1, e2
    if (at(",")) {
      const targets = [target];
      while (at(",")) {
        next();
        targets.push(expression());
      }
      const eq = expect("=", "'=' in multiple assignment");
      const values = [expression()];
      while (at(",")) { next(); values.push(expression()); }
      for (const t of targets) {
        // assignable targets: a plain name, a struct field (o.x), or an element
        // (a[i]) - the same forms single assignment accepts.
        if (t.kind !== "name" && t.kind !== "member" && t.kind !== "index") {
          error("cannot assign to this expression", eq);
        }
      }
      // a, b, c = f(...) : destructure a multi-return call. Allowed when the RHS
      // is a single call; check.js verifies the callee returns enough values.
      if (values.length === 1 && values[0].kind === "call" && targets.length > 1) {
        return { kind: "multiassign", targets, values, fromCall: true, line: tok.line, col: tok.col };
      }
      if (values.length !== targets.length) {
        error(`${targets.length} target(s) but ${values.length} value(s)`, eq);
      }
      return { kind: "multiassign", targets, values, line: tok.line, col: tok.col };
    }

    if (ASSIGN_OPS.includes(peek().type)) {
      const op = next();
      if (op.type === "^=") error("'^=' (exponent) is not supported");
      const value = expression();
      if (target.kind !== "name" && target.kind !== "index" && target.kind !== "member") {
        error("cannot assign to this expression", op);
      }
      return { kind: "assign", op: op.type, target, value, line: tok.line, col: tok.col };
    }
    if (target.kind === "call") {
      return { kind: "callstmt", call: target, line: tok.line, col: tok.col };
    }
    error("expected a statement (assignment or call)", tok);
    return null;
  }

  // ---- expressions (precedence climbing, Lua 5.3 ladder + P8 ops) ----------

  const BINARY = [
    { ops: ["or"] },
    { ops: ["and"] },
    { ops: ["<", ">", "<=", ">=", "~=", "=="] },
    { ops: ["|"] },
    { ops: ["^^"] },
    { ops: ["&"] },
    { ops: ["<<", ">>", ">>>"] },
    { ops: [".."] },
    { ops: ["+", "-"] },
    { ops: ["*", "/", "\\", "%"] },
  ];

  function expression(level = 0) {
    if (level >= BINARY.length) return unary();
    let left = expression(level + 1);
    while (BINARY[level].ops.includes(peek().type)) {
      const op = next();
      const right = expression(level + 1);
      left = { kind: "binop", op: op.type, left, right, line: op.line, col: op.col };
    }
    return left;
  }

  function unary() {
    const tok = peek();
    if (at("not")) { next(); return { kind: "not", expr: unary(), line: tok.line, col: tok.col }; }
    if (at("-")) { next(); return { kind: "neg", expr: unary(), line: tok.line, col: tok.col }; }
    if (at("~")) { next(); return { kind: "bnot", expr: unary(), line: tok.line, col: tok.col }; }
    if (at("#")) {
      next();
      return { kind: "len", expr: unary(), line: tok.line, col: tok.col };
    }
    if (at("@") || at("$")) {
      error(`'${tok.type}' (memory peek) is not supported`, tok);
      next();
      unary();
      return { kind: "number", value: 0, fixed: 0, isInt: true, line: tok.line, col: tok.col };
    }
    return power();
  }

  // a side-effect-free base can be safely duplicated for x^n -> x*x*... - names,
  // literals, field/index reads, and arithmetic over those (so (a-b)^2 works).
  // A call is NOT duplicable (could have side effects / be costly).
  function isDuplicable(e) {
    if (!e) return false;
    switch (e.kind) {
      case "name": case "number": return true;
      case "member": return isDuplicable(e.object);
      case "index": return isDuplicable(e.object) && isDuplicable(e.index);
      case "binop": return isDuplicable(e.left) && isDuplicable(e.right);
      case "neg": case "bnot": case "not": return isDuplicable(e.expr);
      default: return false;
    }
  }

  function power() {
    const base = suffixed();
    if (at("^")) {
      const caret = next();
      const exp = unary();
      // no float pow; expand a CONSTANT small integer exponent into
      // repeated multiplication (x^2 -> x*x). This is the only ^ real carts use
      // (distance-squared, etc). Non-constant or big exponents stay an error.
      if (exp.kind === "number" && exp.isInt && exp.value >= 1 && exp.value <= 8) {
        if (!isDuplicable(base)) {
          error("'^' needs a simple base (a variable or field) so it can expand to repeated multiplication; assign the base to a local first", caret);
          return base;
        }
        let acc = base;
        for (let k = 1; k < exp.value; k++) {
          acc = { kind: "binop", op: "*", left: acc, right: base, line: caret.line, col: caret.col };
        }
        return acc;
      }
      error("'^' (exponent) supports only a constant integer power 1..8 (expands to repeated multiplication); multiply explicitly or use shifts", caret);
    }
    return base;
  }

  function suffixed() {
    let expr = primary();
    for (;;) {
      if (at(".")) {
        const dot = next();
        const field = expect("name", "field name");
        expr = { kind: "member", object: expr, field: field.value, line: dot.line, col: dot.col };
        continue;
      }
      if (at("(")) {
        const paren = next();
        const args = [];
        if (!at(")")) {
          for (;;) {
            args.push(expression());
            if (at(",")) { next(); continue; }
            break;
          }
        }
        expect(")");
        expr = { kind: "call", callee: expr, args, line: paren.line, col: paren.col };
        continue;
      }
      // paren-less call with a single string or table argument: sfx"3",
      // print"hi", add{...}. PICO-8 idiom, "trivial grammar, heavily used"
      // (PICO8.md). Only a name/member is callable this way.
      if ((at("string") || at("{")) && (expr.kind === "name" || expr.kind === "member")) {
        const arg = primary();   // the string literal or table constructor
        expr = { kind: "call", callee: expr, args: [arg], line: expr.line, col: expr.col };
        continue;
      }
      if (at("[")) {
        const brk = next();
        const index = expression();
        expect("]");
        expr = { kind: "index", object: expr, index, line: brk.line, col: brk.col };
        continue;
      }
      // method calls a:b() need OOP the static model can't represent: `b` would
      // dispatch on the receiver's type (worldunder calls a:update/b:update on
      // different object types), and a pool element can't be passed as a `self`
      // whose fields are readable (pool fields are SoA arrays indexed by slot,
      // not a passable object). So this stays unsupported - see GTLUA_CORPUS_B.
      if (at(":")) {
        error(`method calls (a:b()) are not supported: ${sdkName} has no objects to dispatch on - rewrite b as a top-level function b(a, ...) and call it directly`);
        next();
        if (at("name")) next();
        continue;
      }
      break;
    }
    return expr;
  }

  function primary() {
    const tok = peek();
    switch (tok.type) {
      case "number":
        next();
        return { kind: "number", value: tok.value, fixed: tok.fixed, isInt: tok.isInt, line: tok.line, col: tok.col };
      case "true": next(); return { kind: "bool", value: true, line: tok.line, col: tok.col };
      case "false": next(); return { kind: "bool", value: false, line: tok.line, col: tok.col };
      case "nil":
        // no dynamic typing, but the nil-as-sentinel idiom
        // (x = nil / x == nil / x != nil, where nil marks "empty/inactive") is
        // extremely common and DOES compile: nil becomes a reserved sentinel
        // value. check.js allows it only in those sentinel positions and rejects
        // nil used as a real value elsewhere.
        next();
        return { kind: "nil", line: tok.line, col: tok.col };
      case "string":
        next();
        return { kind: "string", value: tok.value, line: tok.line, col: tok.col };
      case "name": next(); return { kind: "name", name: tok.value, line: tok.line, col: tok.col };
      case "(": {
        next();
        const e = expression();
        expect(")");
        e.parenthesized = true;
        return e;
      }
      case "{": {
        next();
        // Two table shapes understood:
        //   struct: named fields, {x=1, y=2} -> a struct with fixed fields.
        //   array:  positional values, {1, 2, 3} -> a fixed C array (const data
        //           at top level; check.js validates the elements are constant).
        // Computed-key ([i]=v) tables are still a different data model we can't
        // represent - reject those with one clear error and skip to `}`.
        if (at("[")) {
          error(`computed-key tables ([k]=v) are not supported; ${sdkName} tables are structs with named fields ({x=1, y=2}) or arrays ({1, 2, 3})`, tok);
          skipBalancedBrace();
          return { kind: "table", fields: [], line: tok.line, col: tok.col };
        }
        const firstIsStructField = at("name") && peek(1).type === "=";
        if (at("}") || firstIsStructField) {
          const fields = [];
          while (!at("}") && !at("eof")) {
            const fname = expect("name", "field name");
            expect("=");
            fields.push({ name: fname.value, expr: expression() });
            if (at(",")) { next(); continue; }
            break;
          }
          expect("}");
          return { kind: "table", fields, line: tok.line, col: tok.col };
        }
        // array-style: a comma-separated list of positional values
        const elements = [];
        while (!at("}") && !at("eof")) {
          elements.push(expression());
          if (at(",")) { next(); continue; }
          break;
        }
        expect("}");
        return { kind: "arraytable", elements, line: tok.line, col: tok.col };
      }
      case "function": {
        error("anonymous functions are not supported (no closures); define a named function at top level", tok);
        // Skip the whole function body, matching nested block openers to their
        // `end` so one closure yields ONE error, not a cascade off the wrong
        // `end` (function/if/for/while/do all close with `end`).
        skipBalancedEnd();
        return { kind: "number", value: 0, fixed: 0, isInt: true, line: tok.line, col: tok.col };
      }
      case "?":
        error("'?' print shorthand is not supported yet (print lands with strings)", tok);
        next();
        sync(["eof"]);
        return { kind: "number", value: 0, fixed: 0, isInt: true, line: tok.line, col: tok.col };
      default:
        error(`unexpected '${tok.value || tok.type}' in expression`, tok);
        next();
        return { kind: "number", value: 0, fixed: 0, isInt: true, line: tok.line, col: tok.col };
    }
  }

  const chunk = block(["eof"]);
  return { chunk, diagnostics };
}
