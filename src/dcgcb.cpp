// ============================================================================
//  dcGCB - Direct-Compile Garbage-Collected Build  (v0.1.0)
//  A tiny, fast, single-file alternative to CMake-style generators.
//
//    1. reads a minimal `build.dcg` config (line-based, no script VM)
//    2. GARBAGE-COLLECTS stale/derived build artifacts (precise keep-set)
//    3. COLLECTS sources into batches and emits COMBINED (unity) chunks
//       so N files become N/batch compile processes
//    4. emits build.ninja; the Ninja engine runs it in parallel
//
//  Verbs:   dcg init|generate|build|gc|clean   (see README.md)
//  Build:   see build.ps1  (PowerShell + one-line compiler call)
// ============================================================================

#define _CRT_SECURE_NO_WARNINGS
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static const char* KVersion = "0.1.0";
static const char* KTool = "dcGCB";

// ---------------------------------------------------------------------------
// small utilities
// ---------------------------------------------------------------------------
static std::string lowerStr(std::string s) {
    for (char& c : s) c = (char)::tolower((unsigned char)c);
    return s;
}

static std::string trimStr(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n\v\f");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n\v\f");
    return s.substr(a, b - a + 1);
}

static std::string stripQuotes(const std::string& s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return s.substr(1, s.size() - 2);
    return s;
}

static void replaceChars(std::string& s, char from, char to) {
    for (char& c : s)
        if (c == from) c = to;
}

static std::string firstLineOf(const std::string& s) {
    size_t p = s.find('\n');
    return trimStr(p == std::string::npos ? s : s.substr(0, p));
}

static int parseIntSafe(const std::string& s, int fallback) {
    try {
        size_t used = 0;
        int v = std::stoi(s, &used);
        if (used == s.size() && !s.empty()) return v;
    } catch (...) {
    }
    return fallback;
}

static std::string humanBytes(uintmax_t bytes) {
    char buf[64];
    if (bytes >= 1024ull * 1024ull)
        std::snprintf(buf, sizeof buf, "%.1f MB", (double)bytes / 1048576.0);
    else if (bytes >= 1024ull)
        std::snprintf(buf, sizeof buf, "%.1f KB", (double)bytes / 1024.0);
    else
        std::snprintf(buf, sizeof buf, "%llu B", (unsigned long long)bytes);
    return buf;
}

static std::string msString(double ms) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(1) << ms << " ms";
    return o.str();
}

// ninja build-line path escaping ($$  $:  $ )
static std::string npath(const std::string& p) {
    std::string r;
    r.reserve(p.size() + 16);
    for (char c : p) {
        switch (c) {
            case '$': r += "$$"; break;
            case ':': r += "$:"; break;
            case ' ': r += "$ "; break;
            default: r += c; break;
        }
    }
    return r;
}

static std::string quoteIfNeeded(const std::string& s) {
    if (s.find(' ') != std::string::npos || s.find('"') != std::string::npos)
        return "\"" + s + "\"";
    return s;
}

static std::string joinStr(const std::vector<std::string>& v, const std::string& sep) {
    std::string r;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) r += sep;
        r += v[i];
    }
    return r;
}

static std::string captureCmd(const std::string& cmd) {
#ifdef _WIN32
    FILE* f = _popen(cmd.c_str(), "r");
#else
    FILE* f = popen(cmd.c_str(), "r");
#endif
    if (!f) return std::string();
    std::string out;
    char buf[4096];
    while (fgets(buf, sizeof buf, f)) {
        out += buf;
        if (out.size() > 16384) break;
    }
#ifdef _WIN32
    _pclose(f);
#else
    pclose(f);
#endif
    return out;
}

static bool exeExistsIn(const fs::path& dir, const std::string& exe) {
#ifdef _WIN32
    fs::path f = dir / (exe + ".exe");
#else
    fs::path f = dir / exe;
#endif
    std::error_code ec;
    return fs::is_regular_file(f, ec) && !ec;
}

// pure PATH scan - no process spawn (a `where` subprocess can cost seconds
// on paths flooded by AV/heavy PATH setups)
static bool haveOnPath(const std::string& exe) {
#ifdef _WIN32
    const char* env = std::getenv("PATH");
    if (!env) return false;
    std::string pathEnv(env);
    std::error_code ec;
    if (exeExistsIn(fs::current_path(ec), exe)) return true;
    size_t start = 0;
    while (start <= pathEnv.size()) {
        size_t end = pathEnv.find(';', start);
        std::string part = pathEnv.substr(start, (end == std::string::npos) ? std::string::npos : end - start);
        if (part.size() >= 2 && part.front() == '"' && part.back() == '"')
            part = part.substr(1, part.size() - 2);
        if (!part.empty()) {
            fs::path dir = fs::path(part);
            if (exeExistsIn(dir, exe)) return true;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
#else
    return std::system(("command -v " + exe + " >/dev/null 2>/dev/null").c_str()) == 0;
#endif
}

static bool readFileRaw(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

static bool writeFileRaw(const fs::path& p, const std::string& data) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << data;
    return f.good();
}

static bool writeIfChanged(const fs::path& p, const std::string& data, bool* wrote) {
    std::string old;
    if (readFileRaw(p, old) && old == data) {
        if (wrote) *wrote = false;
        return true;
    }
    if (!writeFileRaw(p, data)) return false;
    if (wrote) *wrote = true;
    return true;
}

// ---------------------------------------------------------------------------
// paths
// ---------------------------------------------------------------------------
static std::string absPosix(const fs::path& p) {
    std::error_code ec;
    fs::path a = fs::weakly_canonical(p, ec);
    if (ec || a.empty()) a = fs::absolute(p, ec);
    return a.generic_string();
}

static std::string relPosix(const fs::path& p, const fs::path& base) {
    std::error_code ec;
    fs::path r = fs::relative(p, base, ec);
    if (ec || r.empty()) return absPosix(p);
    return r.generic_string();
}

static fs::path makeAbsPath(const fs::path& base, const std::string& maybeRel) {
    fs::path p(maybeRel);
    if (p.is_relative()) p = base / p;
    std::error_code ec;
    fs::path a = fs::weakly_canonical(p, ec);
    if (ec || a.empty()) a = p;
    return a;
}

static fs::path findConfigFrom(const fs::path& start) {
    std::error_code ec;
    fs::path dir = fs::weakly_canonical(start, ec);
    if (ec || dir.empty()) dir = fs::absolute(start, ec);
    for (;;) {
        std::error_code e2;
        fs::path cand = dir / "build.dcg";
        if (fs::is_regular_file(cand, e2) && !e2) return cand;
        fs::path parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return fs::path();
}

// ---------------------------------------------------------------------------
// glob  (* ? per segment, ** recursive)
// ---------------------------------------------------------------------------
static bool isWildcardPart(const std::string& part) {
    return part.find('*') != std::string::npos || part.find('?') != std::string::npos;
}

static bool sameNameCI(const std::string& a, const std::string& b) {
#ifdef _WIN32
    return lowerStr(a) == lowerStr(b);
#else
    return a == b;
#endif
}

static bool wcMatchPart(const std::string& patIn, const std::string& nameIn) {
#ifdef _WIN32
    std::string pat = lowerStr(patIn);
    std::string name = lowerStr(nameIn);
#else
    const std::string& pat = patIn;
    const std::string& name = nameIn;
#endif
    size_t pi = 0, ni = 0;
    size_t starP = std::string::npos, starN = 0;
    while (ni < name.size()) {
        if (pi < pat.size() && (pat[pi] == '?' || pat[pi] == name[ni])) {
            ++pi;
            ++ni;
        } else if (pi < pat.size() && pat[pi] == '*') {
            starP = pi++;
            starN = ni;
        } else if (starP != std::string::npos) {
            pi = starP + 1;
            ni = ++starN;
        } else {
            return false;
        }
    }
    while (pi < pat.size() && pat[pi] == '*') ++pi;
    return pi == pat.size();
}

static void globWalk(const fs::path& dir, size_t idx, const std::vector<std::string>& segs,
                     std::set<std::string>& out) {
    if (idx >= segs.size()) {
        std::error_code ec;
        if (fs::is_regular_file(dir, ec)) out.insert(absPosix(dir));
        return;
    }
    const std::string& part = segs[idx];
    if (part == "**") {
        globWalk(dir, idx + 1, segs, out);  // '**' can swallow nothing
        std::error_code ec;
        for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            fs::path child = it->path();
            std::error_code e2;
            bool isDir = it->is_directory(e2);
            if (!e2 && isDir) globWalk(child, idx, segs, out);
        }
        return;
    }
    bool literal = !isWildcardPart(part);
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        std::string nm = it->path().filename().generic_string();
        bool m = literal ? sameNameCI(part, nm) : wcMatchPart(part, nm);
        if (!m) continue;
        std::error_code e2;
        if (idx + 1 == segs.size()) {
            bool isFile = it->is_regular_file(e2);
            if (!e2 && isFile) out.insert(absPosix(it->path()));
        } else {
            bool isDir = it->is_directory(e2);
            if (!e2 && isDir) globWalk(it->path(), idx + 1, segs, out);
        }
    }
}

static std::set<std::string> expandPatterns(const fs::path& base, const std::vector<std::string>& pats) {
    std::set<std::string> out;
    for (const std::string& pat0 : pats) {
        std::string pat = pat0;
        replaceChars(pat, '\\', '/');
        std::vector<std::string> segs;
        {
            std::string cur;
            for (char c : pat) {
                if (c == '/') {
                    segs.push_back(cur);
                    cur.clear();
                } else {
                    cur += c;
                }
            }
            segs.push_back(cur);
        }
        std::vector<std::string> kept;
        for (size_t i = 0; i < segs.size(); ++i) {
            const std::string& s = segs[i];
            if (s.empty()) {
                if (i == 0 && segs.size() > 1) kept.push_back(s);
            } else {
                kept.push_back(s);
            }
        }
        segs.swap(kept);
        fs::path start = base;
        size_t i = 0;
        if (!segs.empty() && segs[i].size() >= 2 && segs[i][1] == ':') {
            start = fs::path(segs[i] + "/");
            ++i;
        } else if (!segs.empty() && segs[i].empty()) {
            start = fs::path("/");
            ++i;
        }
        while (i < segs.size() && !isWildcardPart(segs[i]) && segs[i] != "**") {
            start /= segs[i];
            ++i;
        }
        globWalk(start, i, segs, out);
    }
    return out;
}

// ---------------------------------------------------------------------------
// build.dcg config
// ---------------------------------------------------------------------------
struct Config {
    std::string name = "App";
    std::string type = "exe";            // exe | static | dll
    std::string cc = "auto";             // auto | cl | gcc | clang
    std::string stdCxx = "c++20";
    std::string stdC = "c99";
    std::string opt = "O2";
    std::string flagsRaw;                // extra compile flags (verbatim)
    std::string ldflagsRaw;              // extra link flags (verbatim)
    std::vector<std::string> defines;
    std::vector<std::string> incs;
    std::vector<std::string> srcPatterns;
    std::vector<std::string> libdirs;
    std::vector<std::string> linkTokens; // libs passed to the linker
    std::string outdir = "build";
    std::string shell = "direct";        // direct | powershell
    bool combine = true;                 // garbage-collected combined (unity) compile
    int batch = 32;                      // sources per combined chunk
    bool gc = true;
    bool staticLink = true;              // gcc: -static-libgcc/-static-libstdc++
};

static std::vector<std::string> tokenizeLine(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool inQ = false;
    for (char c : s) {
        if (c == '"') {
            inQ = !inQ;
            continue;
        }
        if ((c == ' ' || c == '\t') && !inQ) {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static std::string restAfterFirst(const std::string& line) {
    size_t i = 0;
    bool inQ = false;
    while (i < line.size()) {
        char c = line[i];
        if (c == '"') {
            inQ = !inQ;
        } else if ((c == ' ' || c == '\t') && !inQ) {
            break;
        }
        ++i;
    }
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    return line.substr(i);
}

static bool parseOnOff(const std::string& t, bool dflt) {
    std::string v = lowerStr(stripQuotes(t));
    if (v == "on" || v == "true" || v == "yes") return true;
    if (v == "off" || v == "false" || v == "no") return false;
    return dflt;
}

static bool loadConfigFile(const fs::path& file, Config& c, std::string& err) {
    std::ifstream f(file);
    if (!f) {
        err = "cannot open " + file.generic_string();
        return false;
    }
    std::string line;
    int ln = 0;
    while (std::getline(f, line)) {
        ++ln;
        std::string t = trimStr(line);
        if (t.empty() || t[0] == '#') continue;
        std::vector<std::string> tok = tokenizeLine(t);
        if (tok.empty()) continue;
        std::string key = lowerStr(tok[0]);
        std::ostringstream eo;
        auto need = [&](size_t n) -> bool {
            if (tok.size() < n) {
                eo << "line " << ln << ": '" << key << "' expects " << (n - 1) << " value(s)";
                err = eo.str();
                return false;
            }
            return true;
        };
        auto bad = [&](const std::string& msg) {
            err = "line " + std::to_string(ln) + ": " + msg;
            return false;
        };
        if (key == "project" || key == "name" || key == "target") {
            if (!need(2)) return false;
            c.name = stripQuotes(tok[1]);
            if (c.name.empty() || c.name.find(' ') != std::string::npos ||
                c.name.find('$') != std::string::npos || c.name.find(':') != std::string::npos)
                return bad("invalid project name (no spaces/$/: allowed)");
        } else if (key == "type") {
            if (!need(2)) return false;
            std::string tt = lowerStr(stripQuotes(tok[1]));
            if (tt == "lib") tt = "static";
            if (tt == "dll" || tt == "shlib" || tt == "shared") tt = "dll";
            if (tt != "exe" && tt != "static" && tt != "dll") return bad("type must be exe|static|dll");
            c.type = tt;
        } else if (key == "cc" || key == "compiler") {
            if (!need(2)) return false;
            std::string w = lowerStr(stripQuotes(tok[1]));
            if (w != "auto" && w != "cl" && w != "gcc" && w != "clang")
                return bad("cc must be auto|cl|gcc|clang");
            c.cc = w;
        } else if (key == "std") {
            if (!need(2)) return false;
            c.stdCxx = stripQuotes(tok[1]);
        } else if (key == "cstd") {
            if (!need(2)) return false;
            c.stdC = stripQuotes(tok[1]);
        } else if (key == "opt") {
            if (!need(2)) return false;
            c.opt = stripQuotes(tok[1]);
        } else if (key == "def" || key == "define") {
            if (!need(2)) return false;
            c.defines.push_back(stripQuotes(tok[1]));
        } else if (key == "inc" || key == "include") {
            if (!need(2)) return false;
            c.incs.push_back(stripQuotes(tok[1]));
        } else if (key == "flag" || key == "flags") {
            std::string r = trimStr(restAfterFirst(t));
            if (!r.empty()) c.flagsRaw = c.flagsRaw.empty() ? r : c.flagsRaw + " " + r;
        } else if (key == "ldflags" || key == "ldflag") {
            std::string r = trimStr(restAfterFirst(t));
            if (!r.empty()) c.ldflagsRaw = c.ldflagsRaw.empty() ? r : c.ldflagsRaw + " " + r;
        } else if (key == "link" || key == "links") {
            for (size_t i = 1; i < tok.size(); ++i) c.linkTokens.push_back(stripQuotes(tok[i]));
        } else if (key == "libdir") {
            if (!need(2)) return false;
            c.libdirs.push_back(stripQuotes(tok[1]));
        } else if (key == "src" || key == "srcs" || key == "sources") {
            for (size_t i = 1; i < tok.size(); ++i) c.srcPatterns.push_back(stripQuotes(tok[i]));
        } else if (key == "out" || key == "outdir" || key == "builddir") {
            if (!need(2)) return false;
            c.outdir = stripQuotes(tok[1]);
        } else if (key == "shell") {
            if (!need(2)) return false;
            std::string sh = lowerStr(stripQuotes(tok[1]));
            if (sh != "direct" && sh != "powershell") return bad("shell must be direct|powershell");
            c.shell = sh;
        } else if (key == "gc") {
            if (!need(2)) return false;
            c.gc = parseOnOff(tok[1], true);
        } else if (key == "static") {
            if (!need(2)) return false;
            c.staticLink = parseOnOff(tok[1], true);
        } else if (key == "combine") {
            if (!need(2)) return false;
            std::string v = lowerStr(stripQuotes(tok[1]));
            int n = parseIntSafe(v, 0);
            if (n > 0) c.combine = true;
            else c.combine = parseOnOff(tok[1], true);
            if (tok.size() >= 3) c.batch = parseIntSafe(stripQuotes(tok[2]), c.batch);
        } else if (key == "batch") {
            if (!need(2)) return false;
            c.batch = parseIntSafe(stripQuotes(tok[1]), c.batch);
        } else {
            return bad("unknown directive '" + key + "'");
        }
    }
    if (c.batch < 1) c.batch = 1;
    if (c.batch > 4096) c.batch = 4096;
    return true;
}

// ---------------------------------------------------------------------------
// toolchain
// ---------------------------------------------------------------------------
struct Toolchain {
    std::string family;  // cl | gcc | clang
    std::string cxxExe;
    std::string cExe;
    std::string version;
    bool ok = false;
};

static Toolchain detectToolchain(const std::string& wanted, bool wantVersion, std::string& err) {
    Toolchain tc;
    std::vector<std::string> names;
    if (wanted == "cl") names = {"cl"};
    else if (wanted == "gcc") names = {"g++", "gcc"};
    else if (wanted == "clang") names = {"clang++", "clang"};
    else names = {"cl", "g++", "clang++"};
    std::string best;
    for (const std::string& n : names) {
        if (haveOnPath(n)) {
            best = n;
            break;
        }
    }
    if (best.empty()) {
        err = "no C/C++ compiler found in PATH (tried: " + joinStr(names, ", ") +
              "). Install WinLibs g++ (winget install BrechtSanders.WinLibs.POSIX.UCRT) or MSVC Build Tools.";
        return tc;
    }
    if (best == "cl") {
        tc.family = "cl";
        tc.cxxExe = tc.cExe = "cl";
        tc.version = "Microsoft (R) C/C++ (msvc)";
    } else if (best == "g++" || best == "gcc") {
        tc.family = "gcc";
        tc.cxxExe = best;
        tc.cExe = (best == "g++") ? "gcc" : best;
        if (wantVersion) {
            std::string v = captureCmd(best + " --version");
            tc.version = v.empty() ? best : firstLineOf(v);
        } else {
            tc.version = best + " (--verbose shows version)";
        }
    } else {
        tc.family = "clang";
        tc.cxxExe = (best == "clang++") ? "clang++" : "clang";
        tc.cExe = (best == "clang++") ? "clang" : "clang";
        if (wantVersion) {
            std::string v = captureCmd(best + " --version");
            tc.version = v.empty() ? best : firstLineOf(v);
        } else {
            tc.version = best + " (--verbose shows version)";
        }
    }
    tc.ok = true;
    return tc;
}

// ---------------------------------------------------------------------------
// source collection, naming, ninja emission
// ---------------------------------------------------------------------------
struct LangSplit {
    std::vector<std::string> cxx;   // abs posix
    std::vector<std::string> c;     // abs posix
    size_t skipped = 0;
};

static LangSplit splitByLang(const std::set<std::string>& files) {
    LangSplit s;
    for (const std::string& f : files) {
        size_t dot = f.find_last_of('.');
        std::string ext = dot == std::string::npos ? std::string() : lowerStr(f.substr(dot));
        if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".c++")
            s.cxx.push_back(f);
        else if (ext == ".c")
            s.c.push_back(f);
        else
            ++s.skipped;
    }
    return s;
}

static std::string outputArtifact(const Config& c, const Toolchain& tc) {
#ifdef _WIN32
    if (c.type == "exe") return c.name + ".exe";
    if (c.type == "dll") return c.name + ".dll";
    if (c.type == "static") return (tc.family == "cl") ? c.name + ".lib" : "lib" + c.name + ".a";
#else
    if (c.type == "exe") return c.name;
    if (c.type == "dll") return "lib" + c.name + ".so";
    if (c.type == "static") return "lib" + c.name + ".a";
#endif
    return c.name + ".out";
}

static std::string objExtFor(const std::string& family) {
    return (family == "cl") ? ".obj" : ".o";
}

// per-source object name (non-combine mode), collision-safe
static std::string objForSource(const std::string& relSrc, const std::string& objExt,
                                std::map<std::string, int>& seen) {
    std::string out;
    out.reserve(relSrc.size() + 8);
    for (char ch : relSrc) {
        if (ch == '/' || ch == '\\' || ch == '.' || ch == ' ' || ch == '-') out += '_';
        else out += ch;
    }
    int& n = seen[out];
    ++n;
    if (n > 1) out += "_" + std::to_string(n);
    return ".dcgc/obj/" + out + objExt;
}

// ---------------------------------------------------------------------------
// flag / command composition
// ---------------------------------------------------------------------------
static std::string optFlagFor(const std::string& family, const std::string& opt) {
    if (opt.empty()) return std::string();
    if (opt[0] == '-' || opt[0] == '/') return opt;
    return (family == "cl" ? "/" : "-") + opt;
}

static std::string stdFlagFor(const std::string& family, const std::string& spec) {
    if (spec.empty()) return std::string();
    if (family == "cl") return "/std:" + spec;
    return "-std=" + spec;
}

static std::string wrapPowerShell(const std::string& cmd) {
    return "powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -Command \"& { " + cmd + " } \"";
}

// linker token for a lib name / path
static std::string linkArgFor(const std::string& l) {
    if (l.empty()) return l;
#ifdef _WIN32
    if (l[0] == '/' || l[0] == '-') return l;
    if (l.find('.') != std::string::npos) return l;             // explicit lib name/path
    if (l.find('/') != std::string::npos || l.find('\\') != std::string::npos ||
        l.find(':') != std::string::npos)
        return l;                                               // path form
    return l + ".lib";
#else
    if (l[0] == '-' || l[0] == '/') return l;
    if (l.find('/') != std::string::npos || l.find('\\') != std::string::npos ||
        l.find(':') != std::string::npos)
        return l;
    return "-l" + l;
#endif
}

static bool flagsContain(const std::string& flags, const char* needle) {
    return flags.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// ninja emission (writes the combined-compile chunk files too)
// ---------------------------------------------------------------------------
struct Chunk {
    std::string genRel;    // .dcgc/gen/combine_cxx_0.cpp
    std::string objRel;    // .dcgc/obj/combine_cxx_0.o
    size_t count = 0;
};

struct Emit {
    std::string text;
    std::set<std::string> keep;   // rel posix of emitted files + outputs
    std::vector<std::string> objs;   // rel posix, link inputs
    std::string outTarget;           // rel posix
    bool hasCxx = false;
    bool hasC = false;
    size_t units = 0;
};

// writes chunk files (only when content differs; keeps incremental obj mtimes)
static std::vector<Chunk> makeChunks(const Config& c, const std::string& lang,
                                     const std::string& genExt, const std::vector<std::string>& srcs,
                                     const std::string& objExt, const fs::path& buildDir,
                                     std::set<std::string>& keep, bool& anyChanged) {
    std::vector<Chunk> chunks;
    size_t idx = 0;
    for (size_t off = 0; off < srcs.size(); ++idx) {
        size_t n = std::min((size_t)c.batch, srcs.size() - off);
        std::string base = "combine_" + lang + "_" + std::to_string(idx);
        std::string genRel = ".dcgc/gen/" + base + "." + genExt;
        std::string body;
        body += "// dcGCB generated combined-compile unit -- DO NOT EDIT\n";
        body += "// regenerated by: dcgcb generate   (bucket " + lang +
                ", chunk " + std::to_string(idx) + ", " + std::to_string(n) + " sources)\n";
        for (size_t k = 0; k < n; ++k) body += "#include \"" + srcs[off + k] + "\"\n";
        fs::path genPath = buildDir / fs::path(genRel);
        bool changed = false;
        if (!writeIfChanged(genPath, body, &changed)) {
            std::cout << "[dcg] error: cannot write " << genRel << "\n";
            std::exit(1);
        }
        if (changed) anyChanged = true;
        Chunk ch;
        ch.genRel = genRel;
        ch.objRel = ".dcgc/obj/" + base + objExt;
        ch.count = n;
        chunks.push_back(ch);
        keep.insert(genRel);
        off += n;
    }
    return chunks;
}

static Emit emitBuild(const Config& c, const Toolchain& tc, const fs::path& configDir,
                      const fs::path& buildDir, const LangSplit& lang, bool combine,
                      bool& anyWrite) {
    Emit em;
    em.hasCxx = !lang.cxx.empty();
    em.hasC = !lang.c.empty();
    std::ostringstream o;
    const bool isCl = (tc.family == "cl");
    const std::string objExt = objExtFor(tc.family);
    const bool gccDeps = !isCl;

    // commands run inside buildDir, so all paths are relative to it
    auto relOf = [&](const fs::path& p) { return relPosix(p, buildDir); };

    auto commonFlags = [&](bool forC) {
        std::string r;
        std::string stdf = stdFlagFor(tc.family, forC ? c.stdC : c.stdCxx);
        if (!stdf.empty()) r += " " + stdf;
        std::string optf = optFlagFor(tc.family, c.opt);
        if (!optf.empty()) r += " " + optf;
        bool debugOpt = (c.opt == "Od" || c.opt == "-O0" || c.opt == "/Od");
        if (!debugOpt) r += isCl ? " /DNDEBUG" : " -DNDEBUG";
        for (const std::string& d : c.defines) {
            std::string t = quoteIfNeeded(d);
            r += isCl ? " /D" + t : " -D" + t;
        }
        for (const std::string& inc : c.incs) {
            std::string rel = relOf(makeAbsPath(configDir, inc));
            if (rel.empty()) continue;
            r += isCl ? " /I" + quoteIfNeeded(rel) : " -I" + quoteIfNeeded(rel);
        }
        if (!c.flagsRaw.empty()) r += " " + c.flagsRaw;
        return r;
    };
    auto shellWrap = [&](const std::string& cmd) {
        return (lowerStr(c.shell) == "powershell") ? wrapPowerShell(cmd) : cmd;
    };

    // ---- compile rules
    std::string cxxCmd, cCmd;
    if (isCl) {
        std::string eh = flagsContain(c.flagsRaw, "/EH") ? std::string() : " /EHsc";
        cxxCmd = shellWrap(tc.cxxExe + " /nologo" + eh + commonFlags(false) +
                           " /showIncludes /c $in /Fo$out /FS");
        cCmd = shellWrap(tc.cExe + " /nologo" + commonFlags(true) +
                         " /showIncludes /c $in /Fo$out /FS");
    } else {
        cxxCmd = shellWrap(tc.cxxExe + commonFlags(false) + " -MP -MD -MF $out.d -c $in -o $out");
        cCmd = shellWrap(tc.cExe + commonFlags(true) + " -MP -MD -MF $out.d -c $in -o $out");
    }
    o << "# generated by dcGCB v" << KVersion << " -- edit build.dcg, then run: dcgcb generate\n";
    o << "ninja_required_version = 1.3\n\n";
    if (isCl) o << "msvc_deps_prefix = Note: including file: \n";
    o << "rule dcg_cxx\n  deps = " << (isCl ? "msvc" : "gcc") << "\n";
    if (gccDeps) o << "  depfile = $out.d\n";
    o << "  command = " << cxxCmd << "\n  description = dcGCB compile $dcg_info\n\n";
    if (em.hasC) {
        o << "rule dcg_c\n  deps = " << (isCl ? "msvc" : "gcc") << "\n";
        if (gccDeps) o << "  depfile = $out.d\n";
        o << "  command = " << cCmd << "\n  description = dcGCB compile $dcg_info\n\n";
    }

    em.keep.insert("build.ninja");

    // ---- compile edges & objects
    std::map<std::string, int> seenObj;
    auto emitCompileEdge = [&](const std::string& rule, const std::string& objRel,
                               const std::string& inRel, const std::string& info) {
        o << "build " << npath(objRel) << ": " << rule << " " << npath(inRel) << "\n";
        o << "  dcg_info = " << info << "\n";
        em.objs.push_back(objRel);
        em.keep.insert(objRel);
        if (gccDeps) em.keep.insert(objRel + ".d");
    };
    if (combine) {
        if (em.hasCxx) {
            auto chunks = makeChunks(c, "cxx", "cpp", lang.cxx, objExt, buildDir, em.keep, anyWrite);
            em.units += chunks.size();
            for (size_t i = 0; i < chunks.size(); ++i) {
                const Chunk& ch = chunks[i];
                std::string info = "C++ combined chunk " + std::to_string(i) + " (" +
                                   std::to_string(ch.count) + " sources)";
                emitCompileEdge("dcg_cxx", ch.objRel, ch.genRel, info);
            }
        }
        if (em.hasC) {
            auto chunks = makeChunks(c, "c", "c", lang.c, objExt, buildDir, em.keep, anyWrite);
            em.units += chunks.size();
            for (size_t i = 0; i < chunks.size(); ++i) {
                const Chunk& ch = chunks[i];
                std::string info = "C combined chunk " + std::to_string(i) + " (" +
                                   std::to_string(ch.count) + " sources)";
                emitCompileEdge("dcg_c", ch.objRel, ch.genRel, info);
            }
        }
    } else {
        for (const std::string& s : lang.cxx) {
            std::string relSrc = relOf(fs::path(s));
            std::string obj = objForSource(relSrc, objExt, seenObj);
            emitCompileEdge("dcg_cxx", obj, relSrc, "C++ " + relSrc);
        }
        for (const std::string& s : lang.c) {
            std::string relSrc = relOf(fs::path(s));
            std::string obj = objForSource(relSrc, objExt, seenObj);
            emitCompileEdge("dcg_c", obj, relSrc, "C " + relSrc);
        }
    }
    if (em.objs.empty()) {
        std::cout << "[dcg] error: no sources matched the src patterns in build.dcg\n";
        std::exit(1);
    }

    // ---- link rule
    std::string outTarget = outputArtifact(c, tc);
    em.outTarget = outTarget;
    std::string libdirFlags;
    for (const std::string& d : c.libdirs) {
        std::string relLib = relOf(makeAbsPath(configDir, d));
#ifdef _WIN32
        libdirFlags += isCl ? " /LIBPATH:" + quoteIfNeeded(relLib) : " -L" + quoteIfNeeded(relLib);
#else
        libdirFlags += " -L" + quoteIfNeeded(relLib);
#endif
    }
    std::string linkTokens;
    for (const std::string& l : c.linkTokens) linkTokens += " " + linkArgFor(l);
    std::string objsJoined = joinStr(em.objs, " ");
    std::string linkCmd, ruleName;
    if (c.type == "exe") {
        ruleName = "dcg_link";
        if (isCl) {
            linkCmd = shellWrap("cl /nologo $in /Fe$out /link" + libdirFlags + linkTokens +
                                (c.ldflagsRaw.empty() ? std::string() : " " + c.ldflagsRaw));
        } else {
            bool useStatic = c.staticLink && tc.family == "gcc";
            std::string staticFlags = useStatic ? " -static-libgcc -static-libstdc++" : std::string();
            std::string ldr = em.hasCxx ? tc.cxxExe : tc.cExe;
            linkCmd = shellWrap(ldr + " $in -o $out" + staticFlags + libdirFlags + linkTokens +
                                (c.ldflagsRaw.empty() ? std::string() : " " + c.ldflagsRaw));
        }
    } else if (c.type == "dll") {
        ruleName = "dcg_sh";
        if (isCl)
            linkCmd = shellWrap("cl /nologo /LD $in /Fe$out /link" + libdirFlags + linkTokens +
                                (c.ldflagsRaw.empty() ? std::string() : " " + c.ldflagsRaw));
        else
            linkCmd = shellWrap((em.hasCxx ? tc.cxxExe : tc.cExe) + " -shared $in -o $out" +
                                libdirFlags + linkTokens +
                                (c.ldflagsRaw.empty() ? std::string() : " " + c.ldflagsRaw));
    } else {  // static
        ruleName = "dcg_ar";
#ifdef _WIN32
        linkCmd = isCl ? shellWrap("lib /nologo /OUT:$out $in") : shellWrap("ar rcs $out $in");
#else
        linkCmd = shellWrap("ar rcs $out $in");
#endif
    }
    o << "rule " << ruleName << "\n  command = " << linkCmd
      << "\n  description = dcGCB output $out\n\n";
    o << "build " << npath(outTarget) << ": " << ruleName << " " << objsJoined << "\n";
    o << "default " << npath(outTarget) << "\n";
    em.keep.insert(outTarget);
    em.text = o.str();
    return em;
}

// ---------------------------------------------------------------------------
// garbage collection: collect what the fresh emit no longer references
// ---------------------------------------------------------------------------
static void pruneEmptyDirs(const fs::path& dir) {
    std::vector<fs::path> children;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        children.push_back(it->path());
    }
    for (const fs::path& child : children) {
        std::error_code e2;
        if (!fs::is_directory(child, e2) || e2) continue;
        pruneEmptyDirs(child);
        std::error_code e3;
        fs::directory_iterator probe(child, e3);
        if (!e3 && probe == fs::directory_iterator()) {
            std::error_code e4;
            fs::remove(child, e4);
        }
    }
}

struct GcStats {
    size_t files = 0;
    uintmax_t bytes = 0;
    std::vector<std::string> removed;
};

static GcStats collectGarbage(const fs::path& buildDir, const std::set<std::string>& keep, bool verbose) {
    static const char* kArtifacts[] = {".obj", ".o", ".d", ".pdb", ".ilk", ".idb", ".exp", ".res"};
    GcStats g;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(buildDir, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code e2;
        if (!it->is_regular_file(e2) || e2) continue;
        fs::path p = it->path();
        std::string rel = relPosix(p, buildDir);
        if (keep.count(rel)) continue;
        std::string ext = lowerStr(p.extension().generic_string());
        std::string sub = rel.substr(0, 6);
        bool derived = (sub == ".dcgc/");
        bool artifact = false;
        for (const char* a : kArtifacts)
            if (ext == a) artifact = true;
        if (!derived && !artifact) continue;
        uintmax_t sz = fs::file_size(p, e2);
        if (fs::remove(p, e2)) {
            g.files++;
            g.bytes += sz;
            g.removed.push_back(rel);
        }
        if (ec) break;
    }
    pruneEmptyDirs(buildDir / ".dcgc");
    if (verbose)
        for (const std::string& f : g.removed) std::cout << "[dcg] gc: - " << f << "\n";
    return g;
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------
struct Options {
    std::string command;      // generate|build|gc|clean|init|version|help
    fs::path dirArg;
    fs::path configPath;
    std::string ccOverride;
    int jobs = 0;
    bool noGc = false;
    bool noCombine = false;
    bool verbose = false;
    std::vector<std::string> ninjaPassthrough;
};

static void printHelp() {
    std::cout
        << "dcGCB v" << KVersion << " — fast CMake-like build generator (garbage-collected + ninja)\n\n"
        << "usage:\n"
        << "  dcgcb init [dir]              scaffold a new project\n"
        << "  dcgcb generate [dir]          collect + emit build.ninja (default)\n"
        << "  dcgcb build [dir]             generate + run ninja\n"
        << "  dcgcb gc [dir]                alias of generate (with garbage collection)\n"
        << "  dcgcb clean [dir]             remove the out dir\n"
        << "  dcgcb version | help\n\n"
        << "flags:\n"
        << "  --config=PATH            use an explicit build.dcg\n"
        << "  --cc=auto|cl|gcc|clang   override toolchain family\n"
        << "  --jobs=N                 ninja parallelism\n"
        << "  --no-combine             one object per source (no unity chunks)\n"
        << "  --no-gc                  skip the garbage-collection pass\n"
        << "  --verbose | -v           chatty output\n"
        << "  -- args...               forwarded to ninja (e.g. dcgcb build -- -d explain)\n\n"
        << "config directives (build.dcg):\n"
        << "  project NAME | type exe|static|dll | cc auto|cl|gcc|clang\n"
        << "  std c++20 | cstd c99 | opt O2 | inc DIR | def NAME\n"
        << "  flags ... | ldflags ... | link LIB... | libdir DIR\n"
        << "  src PATTERN... | out DIR | shell direct|powershell\n"
        << "  combine on|off [N] | batch N | gc on|off | static on|off\n"
        << "project docs: see README.md next to this tool's source\n";
}

// ---------------------------------------------------------------------------
// commands
// ---------------------------------------------------------------------------
static fs::path buildDirFor(const Config& c, const fs::path& configDir) {
    fs::path od(c.outdir);
    fs::path buildDir = od.is_relative() ? (configDir / od) : od;
    return makeAbsPath(configDir, buildDir.generic_string());
}

static void printBanner() {
    std::cout << KTool << " v" << KVersion
              << " - direct-compile garbage-collected build (config -> .dcgc chunks -> build.ninja)\n";
}

static fs::path resolveConfig(const Options& opt, std::string& where) {
    if (!opt.configPath.empty()) return makeAbsPath(fs::current_path(), opt.configPath.generic_string());
    fs::path start = opt.dirArg.empty() ? fs::current_path() : fs::path(opt.dirArg);
    where = start.generic_string();
    fs::path cfg = findConfigFrom(start);
    return cfg;
}

static int ledgerNinja(const Options& opt, const fs::path& buildDir) {
    if (!haveOnPath("ninja")) {
        std::cout << "[dcg] error: ninja not found in PATH. Install it:\n"
                  << "        winget install Ninja-build.Ninja\n";
        return 2;
    }
    std::string args = "ninja -C \"" + buildDir.generic_string() + "\"";
    if (opt.jobs > 0) args += " -j" + std::to_string(opt.jobs);
    if (opt.verbose) args += " -v";
    for (const std::string& a : opt.ninjaPassthrough) args += " " + a;
    std::cout << "[dcg] running ninja..." << std::endl;  // flush before spawning
    return std::system(args.c_str());
}

static int cmdGenerate(const Options& opt) {
    auto t0 = std::chrono::steady_clock::now();
    printBanner();
    std::string where;
    fs::path cfgPath = resolveConfig(opt, where);
    if (cfgPath.empty() || !fs::exists(cfgPath)) {
        std::cout << "[dcg] error: no build.dcg found in " << where << " or its parents.\n"
                  << "      run `dcgcb init <dir>` to scaffold a project.\n";
        return 2;
    }
    Config c;
    std::string err;
    if (!loadConfigFile(cfgPath, c, err)) {
        std::cout << "[dcg] config error: " << err << "\n";
        return 2;
    }
    if (!opt.ccOverride.empty()) c.cc = opt.ccOverride;
    if (opt.noCombine) c.combine = false;
    fs::path configDir = cfgPath.parent_path();
    fs::path buildDir = buildDirFor(c, configDir);
    std::cout << "[dcg] config:    " << cfgPath.generic_string() << "\n";

    Toolchain tc;
    std::string terr;
    tc = detectToolchain(c.cc, opt.verbose, terr);
    if (!tc.ok) {
        std::cout << "[dcg] error: " << terr << "\n";
        return 2;
    }
    std::cout << "[dcg] toolchain: " << tc.family << " (" << tc.version << ")\n";

    std::set<std::string> files = expandPatterns(configDir, c.srcPatterns);
    LangSplit lang = splitByLang(files);
    size_t matched = lang.cxx.size() + lang.c.size();
    if (matched == 0) {
        std::cout << "[dcg] error: no sources matched the src patterns in build.dcg\n";
        return 1;
    }
    std::cout << "[dcg] scanned:   " << matched << " sources (C++ " << lang.cxx.size()
              << ", C " << lang.c.size() << ")"
              << (lang.skipped ? " (skipped unknown ext: " + std::to_string(lang.skipped) + ")" : "")
              << "\n";

    fs::create_directories(buildDir / ".dcgc/gen");
    fs::create_directories(buildDir / ".dcgc/obj");
    bool anyWrite = false;
    Emit em = emitBuild(c, tc, configDir, buildDir, lang, c.combine, anyWrite);
    fs::path ninjaPath = buildDir / "build.ninja";
    bool changed = false;
    if (!writeIfChanged(ninjaPath, em.text, &changed)) {
        std::cout << "[dcg] error: cannot write " << ninjaPath.generic_string() << "\n";
        return 1;
    }
    if (changed) anyWrite = true;

    size_t gcFiles = 0;
    uintmax_t gcBytes = 0;
    if (c.gc && !opt.noGc) {
        GcStats g = collectGarbage(buildDir, em.keep, opt.verbose);
        gcFiles = g.files;
        gcBytes = g.bytes;
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "[dcg] collect:   " << (c.combine ? "combined" : "per-source")
              << " compile -> " << em.units << " chunk(s) with batch " << c.batch << "\n";
    std::cout << "[dcg] gc:        collected " << gcFiles << " stale file(s), " << humanBytes(gcBytes) << " freed\n";
    std::cout << "[dcg] emit:      " << (anyWrite ? "regenerated" : "unchanged") << " build.ninja in "
              << msString(ms) << " -> " << ninjaPath.generic_string() << "\n";
    std::cout << "[dcg] target:    " << em.outTarget << " (" << c.type << ", " << c.shell << " shell)" << std::endl;
    return 0;
}

static int cmdBuild(const Options& opt) {
    auto t0 = std::chrono::steady_clock::now();
    int rc = cmdGenerate(opt);
    if (rc != 0) return rc;
    std::string where;
    fs::path cfgPath = resolveConfig(opt, where);
    Config c;
    std::string err;
    loadConfigFile(cfgPath, c, err);
    fs::path configDir = cfgPath.parent_path();
    fs::path buildDir = buildDirFor(c, configDir);
    int nrc = ledgerNinja(opt, buildDir);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (nrc != 0) {
        std::cout << "[dcg] build FAILED after " << msString(ms) << "\n";
        return nrc;
    }
    std::cout << "[dcg] build OK in " << msString(ms) << "\n";
    return 0;
}

static int cmdClean(const Options& opt) {
    printBanner();
    std::string where;
    fs::path cfgPath = resolveConfig(opt, where);
    if (cfgPath.empty() || !fs::exists(cfgPath)) {
        std::cout << "[dcg] error: no build.dcg found (" << where << ")\n";
        return 2;
    }
    Config c;
    std::string err;
    if (!loadConfigFile(cfgPath, c, err)) {
        std::cout << "[dcg] config error: " << err << "\n";
        return 2;
    }
    fs::path configDir = cfgPath.parent_path();
    fs::path buildDir = buildDirFor(c, configDir);
    std::error_code ec;
    if (!fs::exists(buildDir, ec)) {
        std::cout << "[dcg] clean: nothing to remove (" << buildDir.generic_string() << ")\n";
        return 0;
    }
    std::string before = buildDir.generic_string();
    fs::remove_all(buildDir, ec);
    std::cout << "[dcg] clean: removed " << before << "\n";
    return ec ? 1 : 0;
}

static int cmdInit(const Options& opt) {
    printBanner();
    fs::path dir = opt.dirArg.empty() ? fs::current_path() : fs::path(opt.dirArg);
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path cfg = dir / "build.dcg";
    if (fs::exists(cfg, ec)) {
        std::cout << "[dcg] init: " << cfg.generic_string() << " already exists\n";
        return 2;
    }
    const char* cfgText =
        "# dcGCB project config -- full directive list in ../README.md\n"
        "project NewApp\n"
        "type exe\n"
        "cc auto\n"
        "std c++20\n"
        "opt O2\n"
        "inc include\n"
        "src src/**/*.cpp\n"
        "out build\n"
        "combine on 32\n"
        "gc on\n";
    writeIfChanged(cfg, cfgText, nullptr);
    fs::create_directories(dir / "include");
    fs::path mainp = dir / "src" / "main.cpp";
    fs::create_directories(mainp.parent_path());
    if (!fs::exists(mainp)) {
        const char* mainText =
            "#include <cstdio>\n"
            "int main() {\n"
            "    std::puts(\"dcGCB: hello from NewApp\");\n"
            "    return 0;\n"
            "}\n";
        writeIfChanged(mainp, mainText, nullptr);
    }
    std::cout << "[dcg] init: scaffolded " << dir.generic_string() << "\n"
              << "      cd into it and run: dcgcb build\n";
    return 0;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    try {
        Options opt;
        std::vector<std::string> pos;
        bool afterDash = false;
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (afterDash) {
                opt.ninjaPassthrough.push_back(a);
                continue;
            }
            if (a == "--") {
                afterDash = true;
            } else if (a.rfind("--", 0) == 0) {
                std::string body = a.substr(2);
                size_t eq = body.find('=');
                std::string key = eq == std::string::npos ? body : body.substr(0, eq);
                std::string val = eq == std::string::npos ? std::string() : body.substr(eq + 1);
                if (key == "help") opt.command = "help";
                else if (key == "version" && val.empty()) opt.command = "version";
                else if (key == "config") opt.configPath = val;
                else if (key == "cc") opt.ccOverride = lowerStr(val);
                else if (key == "jobs") opt.jobs = parseIntSafe(val, 0);
                else if (key == "no-gc") opt.noGc = true;
                else if (key == "no-combine") opt.noCombine = true;
                else if (key == "verbose") opt.verbose = true;
                else {
                    std::cout << "[dcg] error: unknown flag '" << a << "' (see: dcgcb help)\n";
                    return 2;
                }
            } else if (a == "-v") {
                opt.verbose = true;
            } else if (a == "-h") {
                opt.command = "help";
            } else if (a == "-f") {
                if (i + 1 >= argc) {
                    std::cout << "[dcg] error: -f needs a path\n";
                    return 2;
                }
                opt.configPath = argv[++i];
            } else {
                pos.push_back(a);
            }
        }
        for (const std::string& tok : pos) {
            std::string t = lowerStr(tok);
            if (t == "generate" || t == "gen" || t == "build" || t == "gc" || t == "clean" ||
                t == "init" || t == "version" || t == "help") {
                opt.command = (t == "gen") ? "generate" : t;
            } else if (opt.dirArg.empty()) {
                opt.dirArg = fs::path(tok);
            } else {
                std::cout << "[dcg] error: unexpected argument '" << tok << "' (see: dcgcb help)\n";
                return 2;
            }
        }
        if (opt.command.empty()) opt.command = "generate";

        if (opt.command == "help") {
            printHelp();
            return 0;
        }
        if (opt.command == "version") {
            printBanner();
            return 0;
        }
        if (opt.command == "init") return cmdInit(opt);
        if (opt.command == "clean") return cmdClean(opt);
        if (opt.command == "build") return cmdBuild(opt);
        // generate + gc share the collector pipeline
        return cmdGenerate(opt);
    } catch (const std::exception& e) {
        std::cout << "[dcg] error: " << e.what() << "\n";
        return 1;
    }
}





