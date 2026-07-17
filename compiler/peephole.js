// luacretro peephole - 6502/cc65 .s rewriter (tail-call fusion, redundant
// reload elimination). Only the GameTank build pipeline calls it.
//
// cc65's own optimizer (-Osr) leaves classic slack on the table; this pass
// runs between cc65 and ca65 on EVERY compiled C unit (game code and the SDK
// alike), so wins apply to all carts with zero source changes. Rules are
// deliberately conservative: each one preserves machine state exactly, or
// only discards flag effects it can prove nothing consumes.
//
// Rule 1 - tail-call fusion:      jsr FOO / rts        ->  jmp FOO
//   Saves 9 cycles + 3 stack bytes per dynamic hit. Safe when no label sits
//   between the jsr and the rts (nothing else can enter at the rts) - the
//   rts becomes unreachable and is dropped.
//
// Rule 2 - redundant reload:      sta X / ... / lda X  ->  sta X / ...
//   After `sta X`, A already holds X's value, so a later `lda X` (with only
//   flag-neutral stores between, and no label/branch in between) is purely a
//   flags operation. It is dropped only when the flags it would set are
//   provably dead: scanning forward, an NZ-writing instruction or a jsr/rts
//   boundary is reached before any branch/flag-consumer. Indexed, indirect,
//   and immediate operands are excluded; hardware I/O in this codebase is
//   reached via pointers (indirect), which the operand filter excludes.
//
// The pass returns rewrite counts so the build can report what it did.

const NZ_WRITERS = new Set([
  "lda", "ldx", "ldy", "adc", "sbc", "and", "ora", "eor",
  "asl", "lsr", "rol", "ror", "inx", "iny", "dex", "dey",
  "inc", "dec", "pla", "cmp", "cpx", "cpy", "txa", "tya",
  "tax", "tay", "tsx",
]);
const FLAG_NEUTRAL = new Set(["sta", "stx", "sty"]);
const FLAG_CONSUMERS_PREFIX = ["b"]; // bcc/bcs/beq/bne/bmi/bpl/bvc/bvs (+bra: reads none, but keep simple)

function parseLine(line) {
  // instruction lines in cc65 output are TAB-indented: "\tlda     _gtl_px"
  const m = line.match(/^\t([a-z]{3})(?:\s+(.*?))?\s*$/);
  if (!m) return null;
  return { op: m[1], operand: (m[2] ?? "").trim() };
}

function isLabelOrDirective(line) {
  // labels ("L0023:", "@rej:", "_gtl_foo:") and directives (".segment", ...)
  return /^[^\s;]/.test(line) || /^\s*\./.test(line);
}

// operand safe for the sta/lda rule: plain symbol or symbol+offset - no
// immediate (#), no indirect "(", no ",x"/",y" indexing (the index register
// could differ between the two references).
function plainOperand(operand) {
  return operand.length > 0 &&
    !operand.includes("#") && !operand.includes("(") && !operand.includes(",");
}

export function peephole(text) {
  const lines = text.split("\n");
  const stats = { tailCalls: 0, reloads: 0 };

  // ---- Rule 1: jsr/rts -> jmp --------------------------------------------
  for (let i = 0; i < lines.length; i++) {
    const a = parseLine(lines[i]);
    if (!a || a.op !== "jsr") continue;
    // find the next significant line; only comments/blank may intervene
    let j = i + 1;
    while (j < lines.length && (/^\s*$/.test(lines[j]) || /^\s*;/.test(lines[j]))) j++;
    if (j >= lines.length || isLabelOrDirective(lines[j])) continue;
    const b = parseLine(lines[j]);
    if (!b || b.op !== "rts") continue;
    lines[i] = `\tjmp     ${a.operand}`;
    lines.splice(j, 1);
    stats.tailCalls++;
  }

  // ---- Rule 2: sta X / [stores] / lda X (dead flags) ---------------------
  for (let i = 0; i < lines.length; i++) {
    const a = parseLine(lines[i]);
    if (!a || a.op !== "sta" || !plainOperand(a.operand)) continue;
    // walk forward over flag-neutral stores looking for the matching lda
    let j = i + 1, ok = true;
    while (j < lines.length) {
      if (/^\s*$/.test(lines[j]) || /^\s*;/.test(lines[j])) { j++; continue; }
      if (isLabelOrDirective(lines[j])) { ok = false; break; }
      const ins = parseLine(lines[j]);
      if (!ins) { ok = false; break; }
      if (ins.op === "lda" && ins.operand === a.operand) break;   // candidate
      if (FLAG_NEUTRAL.has(ins.op) && plainOperand(ins.operand) &&
          ins.operand !== a.operand) { j++; continue; }           // A untouched
      ok = false; break;
    }
    if (!ok || j >= lines.length) continue;
    const ldaIdx = j;
    // flags-dead scan: from after the lda, must reach an NZ writer or a
    // jsr/rts (cc65 never carries flags across calls) before any branch,
    // flag consumer, label, or anything unrecognized.
    let k = ldaIdx + 1, dead = false;
    while (k < lines.length) {
      if (/^\s*$/.test(lines[k]) || /^\s*;/.test(lines[k])) { k++; continue; }
      if (isLabelOrDirective(lines[k])) break;
      const ins = parseLine(lines[k]);
      if (!ins) break;
      if (NZ_WRITERS.has(ins.op)) { dead = true; break; }
      if (ins.op === "jsr" || ins.op === "rts") { dead = true; break; }
      if (FLAG_NEUTRAL.has(ins.op)) { k++; continue; }
      if (FLAG_CONSUMERS_PREFIX.some((p) => ins.op.startsWith(p))) break;
      break; // anything else: assume flags may matter
    }
    if (!dead) continue;
    lines.splice(ldaIdx, 1);
    stats.reloads++;
    i--; // rescan: another lda of the same operand may follow
  }

  return { text: lines.join("\n"), stats };
}
