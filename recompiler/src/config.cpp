// kleiner TOML-Parser für games/<spiel>.toml: [a.b], [[a]], Werte = String, Ganzzahl (dez/hex), Float, bool, [Liste]
#include <cstdarg>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "recomp.h"

std::string strf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return buf;
}

namespace {

struct Value {
    enum Kind { STRING, INT, FLOAT, BOOL, ARRAY } kind = INT;
    std::string s;
    long long i = 0;
    double d = 0;
    bool b = false;
    std::vector<Value> arr;
};

struct Table {
    std::string name;                 // z.B. "game.executable" oder "jump_table"
    std::map<std::string, Value> kv;
    int line = 0;
};

[[noreturn]] void fail(const std::string& path, int line, const std::string& msg) {
    throw RecompError(EXIT_USAGE, strf("%s:%d: %s", path.c_str(), line, msg.c_str()));
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Kommentar abschneiden, Strings beachten
std::string strip_comment(const std::string& line) {
    bool in_str = false;
    for (size_t i = 0; i < line.size(); i++) {
        if (line[i] == '"' && (i == 0 || line[i - 1] != '\\')) in_str = !in_str;
        if (line[i] == '#' && !in_str) return line.substr(0, i);
    }
    return line;
}

Value parse_value(const std::string& path, int line, const std::string& text) {
    std::string t = trim(text);
    Value v;
    if (t.empty()) fail(path, line, "Wert fehlt");
    if (t[0] == '"') {
        if (t.size() < 2 || t.back() != '"') fail(path, line, "Zeichenkette nicht geschlossen");
        v.kind = Value::STRING;
        for (size_t i = 1; i + 1 < t.size(); i++) {
            if (t[i] == '\\' && i + 2 < t.size()) {
                char c = t[++i];
                v.s += c == 'n' ? '\n' : c == 't' ? '\t' : c;
            } else {
                v.s += t[i];
            }
        }
        return v;
    }
    if (t[0] == '[') {
        if (t.back() != ']') fail(path, line, "Liste nicht geschlossen (Listen müssen in einer Zeile stehen)");
        v.kind = Value::ARRAY;
        std::string inner = t.substr(1, t.size() - 2), item;
        bool in_str = false;
        for (char c : inner + ",") {
            if (c == '"') in_str = !in_str;
            if (c == ',' && !in_str) {
                if (!trim(item).empty()) v.arr.push_back(parse_value(path, line, item));
                item.clear();
            } else {
                item += c;
            }
        }
        return v;
    }
    if (t == "true" || t == "false") {
        v.kind = Value::BOOL;
        v.b = t == "true";
        return v;
    }
    std::string num;
    for (char c : t)
        if (c != '_') num += c;
    char* end = nullptr;
    if (num.rfind("0x", 0) == 0 || num.rfind("0X", 0) == 0) {
        v.kind = Value::INT;
        v.i = std::strtoll(num.c_str() + 2, &end, 16);
    } else if (num.find_first_of(".eE") != std::string::npos) {
        v.kind = Value::FLOAT;
        v.d = std::strtod(num.c_str(), &end);
    } else {
        v.kind = Value::INT;
        v.i = std::strtoll(num.c_str(), &end, 10);
    }
    if (!end || *end) fail(path, line, "ungültiger Wert: " + t);
    return v;
}

std::vector<Table> parse_toml(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw RecompError(EXIT_USAGE, "Konfiguration nicht gefunden: " + path);
    std::vector<Table> tables;
    tables.push_back(Table{});
    std::string raw;
    int line = 0;
    while (std::getline(f, raw)) {
        line++;
        if (line == 1 && raw.rfind("\xEF\xBB\xBF", 0) == 0) raw = raw.substr(3);
        std::string t = trim(strip_comment(raw));
        if (t.empty()) continue;
        if (t[0] == '[') {
            bool arr = t.rfind("[[", 0) == 0;
            size_t n = arr ? 2 : 1;
            if (t.size() < 2 * n + 1 || t.substr(t.size() - n) != (arr ? "]]" : "]")) fail(path, line, "ungültiger Tabellenkopf");
            Table tb;
            tb.name = trim(t.substr(n, t.size() - 2 * n));
            tb.line = line;
            if (!arr)
                for (const Table& o : tables)
                    if (o.name == tb.name) fail(path, line, "Tabelle doppelt: " + tb.name);
            tables.push_back(tb);
            continue;
        }
        size_t eq = t.find('=');
        if (eq == std::string::npos) fail(path, line, "erwartet: schluessel = wert");
        std::string key = trim(t.substr(0, eq));
        if (tables.back().kv.count(key)) fail(path, line, "Schlüssel doppelt: " + key);
        tables.back().kv[key] = parse_value(path, line, t.substr(eq + 1));
    }
    return tables;
}

struct Reader {
    const std::string& path;
    const Table& t;
    std::set<std::string> used;

    const Value* get(const char* key, Value::Kind kind, bool required) {
        used.insert(key);
        auto it = t.kv.find(key);
        if (it == t.kv.end()) {
            if (required) fail(path, t.line, strf("[%s]: Schlüssel '%s' fehlt", t.name.c_str(), key));
            return nullptr;
        }
        if (it->second.kind != kind) fail(path, t.line, strf("[%s]: '%s' hat den falschen Typ", t.name.c_str(), key));
        return &it->second;
    }
    std::string str(const char* key, bool required = true, const std::string& def = "") {
        const Value* v = get(key, Value::STRING, required);
        return v ? v->s : def;
    }
    u32 num(const char* key, bool required = true, u32 def = 0) {
        const Value* v = get(key, Value::INT, required);
        return v ? (u32)v->i : def;
    }
    bool flag(const char* key, bool def) {
        const Value* v = get(key, Value::BOOL, false);
        return v ? v->b : def;
    }
    void ignore(const char* key) { used.insert(key); }
    void done() {
        for (const auto& kv : t.kv)
            if (!used.count(kv.first))
                fail(path, t.line, strf("[%s]: unbekannter Schlüssel '%s'", t.name.c_str(), kv.first.c_str()));
    }
};

} // namespace

Config load_config(const std::string& path) {
    Config c;
    bool have_game = false, have_exe = false;
    for (const Table& t : parse_toml(path)) {
        Reader r{path, t, {}};
        if (t.name.empty()) {
            // vor der ersten Tabelle keine Schlüssel
        } else if (t.name == "game") {
            have_game = true;
            c.id = r.str("id");
            c.title = r.str("title");
            c.disc_id = r.str("disc_id");
            c.disc_version = r.str("disc_version");
        } else if (t.name == "game.executable") {
            have_exe = true;
            c.exe_path = r.str("path");
            c.exe_format = r.str("format");
            c.module_name = r.str("module", false);
            c.load_addr = r.num("load_addr");
            c.elf_size = r.num("elf_size", false);
            c.elf_sha1 = r.str("elf_sha1");
            if (c.exe_format != "elf-exec" && c.exe_format != "prx")
                fail(path, t.line, "format muss \"elf-exec\" oder \"prx\" sein");
        } else if (t.name == "game.iso") {
            for (const char* k : {"note", "size", "crc32", "md5", "sha1"}) r.ignore(k);
        } else if (t.name == "module") {
            fail(path, t.line, "[[module]] (zusätzliche .prx) wird noch nicht unterstützt (T11)");
        } else if (t.name == "memory") {
            for (const char* k : {"user_ram_size", "host_stack_size"}) r.ignore(k);   // nur Runtime
        } else if (t.name == "recompiler") {
            c.safepoints = r.flag("safepoints", true);
            c.functions_per_file = r.num("functions_per_file", false, 500);
            if (c.functions_per_file == 0) fail(path, t.line, "functions_per_file muss > 0 sein");
        } else if (t.name == "runtime") {
            r.ignore("vblank_hz");
            c.exe_name = r.str("exe_name", false);
        } else if (t.name == "function") {
            c.functions.push_back(r.num("addr"));
            r.ignore("name");
            r.ignore("size");
        } else if (t.name == "jump_table") {
            JumpTableCfg j;
            j.jr_addr = r.num("jr_addr");
            j.table_addr = r.num("table_addr");
            j.count = r.num("count");
            r.ignore("index_reg");
            c.jump_tables.push_back(j);
        } else if (t.name == "code_range") {
            const u32 start = r.num("start"), end = r.num("end");
            r.ignore("reason");
            if ((start & 3) || (end & 3) || end <= start) fail(path, t.line, "[[code_range]]: start/end ungültig");
            c.code_ranges.push_back({start, end});
        } else if (t.name == "hook") {
            HookCfg h;
            h.addr = r.num("addr");
            h.symbol = r.str("symbol");
            r.ignore("reason");
            c.hooks.push_back(h);
        } else if (t.name == "patch") {
            PatchCfg p;
            p.addr = r.num("addr");
            const Value* w = r.get("words", Value::ARRAY, true);
            for (const Value& v : w->arr) {
                if (v.kind != Value::INT) fail(path, t.line, "[[patch]] words: nur Ganzzahlen");
                p.words.push_back((u32)v.i);
            }
            p.enabled = r.flag("enabled", true);
            r.ignore("reason");
            c.patches.push_back(p);
        } else {
            fail(path, t.line, "unbekannte Tabelle [" + t.name + "]");
        }
        r.done();
    }
    if (!have_game || !have_exe) throw RecompError(EXIT_USAGE, path + ": [game] oder [game.executable] fehlt");
    if (c.exe_name.empty()) c.exe_name = c.id;
    return c;
}
