// --eboot ist Pflicht; ISO lesen, --jobs, --progress json kommen noch (T13)
#include <algorithm>
#include <cstring>
#include <filesystem>

#include "recomp.h"

static void usage() {
    fprintf(stderr,
            "diva_recomp --game <games/spiel.toml> --eboot <entschluesselte EBOOT.BIN> --out <ordner>\n"
            "            [--analysis <ordner mit functions.json>] [--runtime-include <ordner>] [--check-only]\n");
}

int main(int argc, char** argv) {
    std::string game, eboot, out, analysis, rt_include;
    bool check_only = false;
    for (int i = 1; i < argc; i++) {
        auto arg = [&](const char* name, std::string& dst) {
            if (strcmp(argv[i], name) != 0) return false;
            if (i + 1 >= argc) { usage(); exit(EXIT_USAGE); }
            dst = argv[++i];
            return true;
        };
        if (arg("--game", game) || arg("--eboot", eboot) || arg("--out", out) || arg("--analysis", analysis) ||
            arg("--runtime-include", rt_include))
            continue;
        if (strcmp(argv[i], "--check-only") == 0) { check_only = true; continue; }
        fprintf(stderr, "unbekannter Parameter: %s\n", argv[i]);
        usage();
        return EXIT_USAGE;
    }
    if (game.empty() || eboot.empty() || (out.empty() && !check_only)) {
        usage();
        return EXIT_USAGE;
    }
    try {
        namespace fs = std::filesystem;
        Config cfg = load_config(game);
        if (rt_include.empty())
            rt_include = fs::absolute(fs::path(game).parent_path() / ".." / "runtime" / "include").lexically_normal().string();

        std::vector<u8> file = read_file(eboot);
        Module m = load_module(file, cfg.load_addr, cfg.elf_size);
        if (m.sha1 != cfg.elf_sha1)
            throw RecompError(EXIT_WRONG_HASH, strf("EBOOT passt nicht zur Konfiguration: SHA-1 %s, erwartet %s", m.sha1.c_str(),
                                                    cfg.elf_sha1.c_str()));
        if (cfg.exe_format == "elf-exec" && m.load_addr != cfg.load_addr)
            throw RecompError(EXIT_WRONG_HASH, strf("Ladeadresse 0x%08X, erwartet 0x%08X", m.load_addr, cfg.load_addr));
        fprintf(stderr, "%s: Modul %s, Code 0x%08X..0x%08X, %zu Importe, Einstieg 0x%08X\n", cfg.id.c_str(), m.name.c_str(),
                m.code_addr, m.code_addr + m.code_size, m.imports.size(), m.entry);
        if (check_only) return EXIT_OK;

        for (const auto& cr : cfg.code_ranges) {   // Code außerhalb von .text
            const u32 span_end = m.code_addr + m.code_size;
            if (cr.first < span_end && !m.text_ranges.empty() && cr.first < m.code_addr + m.text_size + m.stub_size)
                throw RecompError(EXIT_USAGE, strf("[[code_range]] 0x%08X überlappt .text", cr.first));
            if (!m.contains(cr.first, cr.second - cr.first))
                throw RecompError(EXIT_USAGE, strf("[[code_range]] 0x%08X..0x%08X liegt außerhalb des Abbilds", cr.first, cr.second));
            m.text_ranges.push_back(cr);
            if (cr.second > span_end) m.code_size = cr.second - m.code_addr;
        }
        std::sort(m.text_ranges.begin(), m.text_ranges.end());
        for (size_t k = 1; k < m.text_ranges.size(); k++)
            if (m.text_ranges[k].first < m.text_ranges[k - 1].second) throw RecompError(EXIT_USAGE, "[[code_range]]: Bereiche überlappen");

        for (const PatchCfg& p : cfg.patches) {
            if (!p.enabled) continue;
            for (size_t k = 0; k < p.words.size(); k++) {
                u32 a = p.addr + (u32)k * 4;
                if ((a & 3) || !m.in_text(a)) throw RecompError(EXIT_USAGE, strf("[[patch]] 0x%08X liegt nicht im Code", a));
                m.write32(a, p.words[k]);
            }
        }

        std::vector<u32> extra;
        if (!analysis.empty()) extra = load_ghidra_functions(analysis);
        Analysis an = analyze(m, cfg, extra);
        EmitStats st = emit_code(m, cfg, an, out, rt_include);

        u32 jt_targets = 0;
        for (const auto& j : an.jump_tables) jt_targets += (u32)j.second.size();
        fprintf(stderr, "Funktionen: %u (+%zu Import-Stubs), Befehle: %u, Dateien: %u\n", st.functions, m.imports.size(),
                st.instructions, st.files);
        fprintf(stderr, "Sprungtabellen: %zu (%u Ziele), jr <reg> ohne Tabelle: %u\n", an.jump_tables.size(), jt_targets,
                an.indirect_jr);
        fprintf(stderr, "Weiche Funktionsanfaenge: %zu\n", an.soft.size());
        fprintf(stderr, "Rueckwaertsspruenge ueber Funktionsgrenzen: %u\n", an.backward_cross);
        for (u32 a : an.backward_cross_examples) fprintf(stderr, "  0x%08X\n", a);
        fprintf(stderr, "Zeiger in den Code: %u aus Daten, %u im Code gebildet, davon verdaechtig: %zu\n", an.data_ptrs,
                an.code_ptrs, an.suspicious_ptrs.size());
        for (size_t k = 0; k < an.suspicious_ptrs.size() && k < 20; k++)
            fprintf(stderr, "  0x%08X -> 0x%08X\n", an.suspicious_ptrs[k].first, an.suspicious_ptrs[k].second);
        if (!an.code_like_ptrs.empty()) {
            fprintf(stderr, "WARNUNG: %zu Zeiger auf moeglichen Code ausserhalb der Codebereiche ([[code_range]] ergaenzen?):\n",
                    an.code_like_ptrs.size());
            for (size_t k = 0; k < an.code_like_ptrs.size() && k < 20; k++)
                fprintf(stderr, "  0x%08X -> 0x%08X\n", an.code_like_ptrs[k].first, an.code_like_ptrs[k].second);
        }
        fprintf(stderr, "Nicht uebersetzte Befehle: %u\n", st.unimplemented);
        {   // Bericht, enthält Spieladressen -> bleibt in generated/
            std::string rep = strf("{\n \"functions\": %u,\n \"import_stubs\": %zu,\n \"instructions\": %u,\n \"unimplemented\": %u,\n",
                                   st.functions, m.imports.size(), st.instructions, st.unimplemented);
            rep += " \"soft_starts\": [";
            for (u32 a : an.soft) rep += strf("\"0x%08X\",", a);
            if (rep.back() == ',') rep.pop_back();
            rep += "],\n \"indirect_jr\": [";
            for (const Insn& in : an.insns)
                if (in.op == Op::JR && in.rs != 31 && m.in_text(in.addr) && !an.jump_tables.count(in.addr)) rep += strf("\"0x%08X\",", in.addr);
            if (rep.back() == ',') rep.pop_back();
            rep += "],\n \"jump_tables\": {";
            for (const auto& j : an.jump_tables) rep += strf("\"0x%08X\": %zu,", j.first, j.second.size());
            if (rep.back() == ',') rep.pop_back();
            rep += strf("},\n \"data_pointers_into_code\": %u,\n \"code_pointers_into_code\": %u,\n \"suspicious_pointers\": [",
                        an.data_ptrs, an.code_ptrs);
            for (const auto& p : an.suspicious_ptrs) rep += strf("[\"0x%08X\",\"0x%08X\"],", p.first, p.second);
            if (rep.back() == ',') rep.pop_back();
            rep += "],\n \"code_like_pointers\": [";
            for (const auto& p : an.code_like_ptrs) rep += strf("[\"0x%08X\",\"0x%08X\"],", p.first, p.second);
            if (rep.back() == ',') rep.pop_back();
            rep += "],\n \"vfpu_by_name\": {";
            for (const auto& u : st.vfpu_by_name) rep += strf("\"%s\": %u,", u.first.c_str(), u.second);
            if (rep.back() == ',') rep.pop_back();
            rep += "},\n \"unimplemented_by_name\": {";
            for (const auto& u : st.unimplemented_by_name) rep += strf("\"%s\": %u,", u.first.c_str(), u.second);
            if (rep.back() == ',') rep.pop_back();
            rep += "}\n}\n";
            FILE* f = fopen((fs::path(out) / "report.json").string().c_str(), "wb");
            if (f) { fwrite(rep.data(), 1, rep.size(), f); fclose(f); }
        }
        std::map<std::string, u32> grouped;
        for (const auto& u : st.unimplemented_by_name) grouped[u.first.substr(0, u.first.find(':'))] += u.second;
        for (const auto& g : grouped) fprintf(stderr, "  %-22s %u\n", g.first.c_str(), g.second);
        return EXIT_OK;
    } catch (const RecompError& e) {
        fprintf(stderr, "Fehler: %s\n", e.what());
        return e.code;
    } catch (const std::exception& e) {
        fprintf(stderr, "Interner Fehler: %s\n", e.what());
        return EXIT_INTERNAL;
    }
}
