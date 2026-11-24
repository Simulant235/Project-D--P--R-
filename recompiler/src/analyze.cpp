// .text linear dekodieren (jedes Wort = Befehl), Sprungtabellen, Funktionsanfänge.
// Funktion = von Anfang bis zum nächsten Anfang. Sprung über eine Grenze wird Call+Return, also
// lieber einen Anfang zu viel als einen zu wenig (der fehlt erst zur Laufzeit). Im Zweifel teilen.
#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>

#include "recomp.h"

std::vector<u32> load_ghidra_functions(const std::string& dir) {
    std::vector<u32> out;
    std::ifstream f(dir + "/functions.json", std::ios::binary);
    if (!f) throw RecompError(EXIT_NOT_FOUND, "functions.json nicht gefunden in " + dir);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    static const std::regex re("\"addr\"\\s*:\\s*\"0x([0-9A-Fa-f]{8})\"");
    for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it)
        out.push_back((u32)std::stoul((*it)[1].str(), nullptr, 16));
    return out;
}

namespace {

// Sprungtabelle zu "jr reg" suchen. Muster (Metrowerks):
//   sltiu x, idx, N ; beq x, zero, default ; sll i, idx, 2 ; lui t, HI ; addiu t, t, LO ; addu p, i, t ; lw r, 0(p) ; jr r
// Kandidaten: Konstanten davor (lui, lui+addiu/ori, plus lw-Offset); gültig, wenn dort N Codezeiger stehen.
std::vector<u32> find_jump_table(const Module& m, const std::vector<Insn>& insns, u32 jr_index) {
    const Insn& jr = insns[jr_index];
    const u32 first = jr_index > 24 ? jr_index - 24 : 0;

    // lw, das das Sprungregister lädt
    int lw_index = -1;
    for (u32 i = jr_index; i-- > first;) {
        const Insn& in = insns[i];
        if (in.op == Op::LW && in.rt == jr.rs) { lw_index = (int)i; break; }
        if (has_delay_slot(in.op)) break;   // nicht über Kontrollfluss suchen
    }
    if (lw_index < 0) return {};
    const s32 lw_off = insns[lw_index].imm;

    u32 count = 0;   // vom nächsten sltiu
    std::vector<u32> consts;
    u32 lui_val[32] = {};
    bool lui_set[32] = {};
    for (u32 i = first; i < (u32)lw_index; i++) {
        const Insn& in = insns[i];
        if (in.op == Op::LUI) {
            lui_val[in.rt] = in.uimm << 16;
            lui_set[in.rt] = true;
            consts.push_back(lui_val[in.rt]);
        } else if ((in.op == Op::ADDIU || in.op == Op::ORI) && lui_set[in.rs]) {
            u32 c = in.op == Op::ADDIU ? lui_val[in.rs] + (u32)in.imm : (lui_val[in.rs] | in.uimm);
            consts.push_back(c);
            if (in.rt != in.rs) lui_set[in.rt] = false;
        } else if (in.op == Op::SLTIU && in.imm > 0) {
            count = (u32)in.imm;
        }
    }
    for (auto it = consts.rbegin(); it != consts.rend(); ++it) {
        const u32 table = *it + (u32)lw_off;
        if ((table & 3) || !m.contains(table, 8) || m.is_code(table)) continue;
        std::vector<u32> targets;
        const u32 limit = count ? count : 512;
        for (u32 k = 0; k < limit && m.contains(table + k * 4); k++) {
            u32 t = m.read32(table + k * 4);
            bool ok = !(t & 3) && m.in_text(t);
            if (ok && !count) {   // ohne sltiu nur nahe Ziele
                u32 dist = t > jr.addr ? t - jr.addr : jr.addr - t;
                ok = dist < 0x4000;
            }
            if (!ok) break;
            targets.push_back(t);
        }
        if ((count && targets.size() == count && count >= 2) || (!count && targets.size() >= 3)) return targets;
    }
    return {};
}

} // namespace

Analysis analyze(const Module& m, const Config& cfg, const std::vector<u32>& extra_starts) {
    Analysis an;
    const u32 words = m.code_size / 4;
    an.insns.reserve(words);
    for (u32 i = 0; i < words; i++) {
        u32 addr = m.code_addr + i * 4;
        an.insns.push_back(decode(addr, m.read32(addr)));
    }
    // Schleifen nur über die Codebereiche, nicht über die Lücke dazwischen
    auto for_text = [&](auto&& fn) {
        for (const auto& r : m.text_ranges)
            for (u32 a = r.first; a < r.second; a += 4) fn(an.at(m, a));
    };

    // 1. Sprungtabellen
    for_text([&](const Insn& in) {
        if (in.op != Op::JR || in.rs == 31) return;
        std::vector<u32> t = find_jump_table(m, an.insns, (in.addr - m.code_addr) / 4);
        if (!t.empty()) an.jump_tables[in.addr] = t;
    });
    for (const JumpTableCfg& j : cfg.jump_tables) {
        if (!m.in_text(j.jr_addr) || an.at(m, j.jr_addr).op != Op::JR)
            throw RecompError(EXIT_USAGE, strf("[[jump_table]] jr_addr 0x%08X ist kein jr", j.jr_addr));
        std::vector<u32> t;
        for (u32 k = 0; k < j.count; k++) {
            if (!m.contains(j.table_addr + k * 4)) throw RecompError(EXIT_USAGE, "[[jump_table]] Tabelle außerhalb des Abbilds");
            u32 target = m.read32(j.table_addr + k * 4);
            if ((target & 3) || !m.in_text(target))
                throw RecompError(EXIT_USAGE, strf("[[jump_table]] 0x%08X: Eintrag %u zeigt nicht in den Code", j.jr_addr, k));
            t.push_back(target);
        }
        an.jump_tables[j.jr_addr] = t;
    }
    for_text([&](const Insn& in) {
        if (in.op == Op::JR && in.rs != 31 && !an.jump_tables.count(in.addr)) an.indirect_jr++;
    });

    // 2. Startmenge
    std::set<u32>& S = an.starts;
    auto seed = [&](u32 a) {
        if (!(a & 3) && m.in_text(a)) S.insert(a);
    };
    for (const auto& r : m.text_ranges) S.insert(r.first);   // jeder Bereich beginnt mit einer Funktion
    seed(m.entry);
    for (u32 a : m.export_funcs) seed(a);
    for (u32 a : cfg.functions) {
        if ((a & 3) || !m.in_text(a)) throw RecompError(EXIT_USAGE, strf("[[function]] addr 0x%08X liegt nicht im Code", a));
        S.insert(a);
    }
    for (u32 a : extra_starts) seed(a);
    for_text([&](const Insn& in) {
        if (in.op == Op::JAL) seed(in.target);
    });

    // 3. Funktionsgrenzen
    //    sicher (D): Startmenge + alles, was über eine sichere Grenze hinweg angesprungen wird
    //    vorläufig (T): Code hinter j/jr, auf den kein Vorwärtssprung zeigt
    //    Springt ein Sprung über einen vorläufigen Anfang (ohne sichere Grenze), ist der "weich": beendet die
    //    Funktion davor nicht (Schleifenrumpf hinter dem Return), bleibt aber als Einstieg erhalten.
    std::set<u32> T, soft;   // soft = von einem Sprung überquerte vorläufige Anfänge
    auto targets_of = [&](const Insn& in, std::vector<u32>& out) {
        out.clear();
        if (is_cond_branch(in.op) || in.op == Op::J) out.push_back(in.target);
        else if (in.op == Op::JR) {
            auto jt = an.jump_tables.find(in.addr);
            if (jt != an.jump_tables.end()) out = jt->second;
        }
    };
    std::vector<u32> tg;
    for (bool changed = true; changed;) {
        changed = false;
        T.clear();
        for (const auto& r : m.text_ranges) {
            const u32 range_end = r.second;
            u32 max_target = r.first;
            auto next_def = S.upper_bound(r.first);
            for (u32 a = r.first; a < range_end;) {
                while (next_def != S.end() && *next_def <= a) {   // sichere Grenze
                    max_target = *next_def;
                    ++next_def;
                }
                const u32 limit = next_def != S.end() && *next_def < range_end ? *next_def : range_end;
                const Insn& in = an.at(m, a);
                if (!has_delay_slot(in.op)) { a += 4; continue; }
                targets_of(in, tg);
                for (u32 t : tg)
                    if (!(t & 3) && t > a && t < limit) max_target = std::max(max_target, t);
                u32 next = a + 8;
                if (is_unconditional(in) && max_target < next) {
                    while (next < limit && an.at(m, next).op == Op::NOP) next += 4;   // Padding
                    if (next < range_end && !S.count(next)) T.insert(next);
                    max_target = next;
                }
                a = next;
            }
        }
        for_text([&](const Insn& in) {
            if (!has_delay_slot(in.op)) return;
            const u32 a = in.addr;
            targets_of(in, tg);
            for (u32 t : tg) {
                if ((t & 3) || !m.in_text(t)) continue;
                if (S.count(t)) continue;                     // Ziel ist sicherer Anfang
                const u32 lo = std::min(a, t), hi = std::max(a, t);
                auto it = S.upper_bound(lo);
                if (it != S.end() && *it <= hi) {
                    S.insert(t);                              // Sprung in andere Funktion -> Ziel wird Anfang
                    changed = true;
                } else {
                    for (auto q = T.upper_bound(lo); q != T.end() && *q <= hi; ++q) soft.insert(*q);
                }
            }
        });
        if (changed) soft.clear();
    }
    S.insert(T.begin(), T.end());

    // Ende der Funktion ab x: nächster harter Anfang, sonst Bereichsende
    auto hard_end = [&](u32 x) {
        auto it = S.upper_bound(x);
        while (it != S.end() && soft.count(*it)) ++it;
        const u32 re = m.range_end(x);
        return it != S.end() && *it < re ? *it : re;
    };

    // Weiche Anfänge: eigene Funktion bis zur nächsten harten Grenze. Springt sie rückwärts in die
    // umgebende, muss das Ziel ebenfalls (weicher) Anfang sein.
    for (bool changed = true; changed;) {
        changed = false;
        std::vector<u32> snapshot(soft.begin(), soft.end());
        for (u32 x : snapshot) {
            const u32 e = hard_end(x);
            auto hard_before = S.find(x);
            while (hard_before != S.begin() && soft.count(*hard_before)) --hard_before;
            const u32 hs = *hard_before;
            for (u32 a = x; a < e; a += 4) {
                const Insn& in = an.at(m, a);
                if (!has_delay_slot(in.op)) continue;
                targets_of(in, tg);
                for (u32 t : tg)
                    if (!(t & 3) && t >= hs && t < x && !S.count(t)) {
                        S.insert(t);
                        soft.insert(t);
                        changed = true;
                    }
            }
        }
    }
    an.soft = soft;

    // 3b. Zeigerprüfung: Codezeiger sollten auf Funktionsanfang/Tabellenziel zeigen, der Rest ist
    //     evtl. unbekanntes Sprungziel (oder Zufall). Außerdem: Datenzeiger außerhalb der Codebereiche auf
    //     etwas mit addiu sp,sp,-N -> wohl Code, der als [[code_range]] fehlt.
    {
        std::set<u32> table_targets;
        for (const auto& j : an.jump_tables) table_targets.insert(j.second.begin(), j.second.end());
        auto check = [&](u32 where, u32 v, u32& counter) {
            if (v & 3) return;
            if (m.in_text(v)) {
                counter++;
                if (!S.count(v) && !table_targets.count(v)) an.suspicious_ptrs.push_back({where, v});
            } else if (m.contains(v) && !m.is_code(v)) {
                const u32 w = m.read32(v);
                if ((w >> 16) == 0x27BD && (w & 0x8000)) an.code_like_ptrs.push_back({where, v});
            }
        };
        const u32 image_end = m.image_addr + (u32)m.image.size();
        for (u32 a = (m.image_addr + 3) & ~3u; a + 4 <= image_end; a += 4)
            if (!m.is_code(a)) check(a, m.read32(a), an.data_ptrs);
        u32 lui_val[32] = {};
        bool lui_set[32] = {};
        for_text([&](const Insn& in) {
            if (in.op == Op::LUI) {
                lui_val[in.rt] = in.uimm << 16;
                lui_set[in.rt] = in.rt != 0;
                return;
            }
            if ((in.op == Op::ADDIU || in.op == Op::ORI) && lui_set[in.rs])
                check(in.addr, in.op == Op::ADDIU ? lui_val[in.rs] + (u32)in.imm : (lui_val[in.rs] | in.uimm), an.code_ptrs);
            int d = dest_reg(in);
            if (d >= 0) lui_set[d] = false;
            if (in.op == Op::JAL || in.op == Op::JALR)   // nach Call sind flüchtige Regs unbekannt
                for (int r : {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25}) lui_set[r] = false;
        });
    }

    // 4. Statistik: bedingte Rückwärtssprünge über harte Grenzen (sollte es nicht geben)
    for (u32 s : S) {
        if (soft.count(s)) continue;
        const u32 e = hard_end(s);
        for (u32 a = s; a < e; a += 4) {
            const Insn& in = an.at(m, a);
            if (is_cond_branch(in.op) && m.in_text(in.target) && in.target < s) {
                an.backward_cross++;
                if (an.backward_cross_examples.size() < 20) an.backward_cross_examples.push_back(a);
            }
        }
    }
    return an;
}
