#include "shader_source.h"

#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>

namespace shadersrc {

const char* const kSharedInclude = "common.glsl";

static std::string readFile(const std::string& path, bool* ok) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { *ok = false; return std::string(); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string s(static_cast<size_t>(n < 0 ? 0 : n), ' ');
    size_t got = fread(&s[0], 1, s.size(), f);
    s.resize(got);
    fclose(f);
    *ok = true;
    return s;
}

std::string dir() {
    // An override for running a build that was moved away from its source
    // tree -- the shaders are data, not part of the executable.
    const char* env = std::getenv("PAINTIFY_ROOT");
    if (env && *env) return std::string(env) + "/shaders/";
    return std::string(SBR_ROOT_DIR) + "/shaders/";
}

int64_t mtime(const std::string& file) {
    const std::string path = dir() + file;
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(path.c_str(), &st) != 0) return 0;
#else
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0;
#endif
    return static_cast<int64_t>(st.st_mtime);
}

std::vector<int64_t> mtimes(const std::vector<std::string>& files) {
    std::vector<int64_t> out;
    for (const std::string& f : files) out.push_back(mtime(f));
    out.push_back(mtime(kSharedInclude));
    return out;
}

std::string load(const std::string& file, std::string* err) {
    bool ok = false;
    std::string src = readFile(dir() + file, &ok);
    if (!ok) { if (err) *err += "cannot open " + file; return std::string(); }

    // Single-level include splice. Deliberately not recursive: common.glsl is
    // the only include, and keeping it flat keeps #line numbering trivial.
    std::string out;
    out.reserve(src.size() + 8192);
    size_t pos = 0;
    int lineNo = 1;
    while (pos <= src.size()) {
        size_t eol = src.find('\n', pos);
        bool last = (eol == std::string::npos);
        if (last) eol = src.size();
        std::string line = src.substr(pos, eol - pos);

        size_t inc = line.find("#include");
        size_t firstNonWs = line.find_first_not_of(" \t");
        if (inc != std::string::npos && firstNonWs == inc) {
            size_t a = line.find('"', inc);
            size_t b = (a == std::string::npos) ? std::string::npos : line.find('"', a + 1);
            if (a != std::string::npos && b != std::string::npos) {
                std::string incName = line.substr(a + 1, b - a - 1);
                bool iok = false;
                std::string incSrc = readFile(dir() + incName, &iok);
                if (!iok) { if (err) *err += "cannot open include " + incName; return std::string(); }
                out += "#line 1\n";
                out += incSrc;
                if (!incSrc.empty() && incSrc[incSrc.size() - 1] != '\n') out += "\n";
                out += "#line " + std::to_string(lineNo + 1) + "\n";
                pos = eol + 1;
                ++lineNo;
                if (last) break;
                continue;
            }
        }
        out += line;
        out += '\n';
        pos = eol + 1;
        ++lineNo;
        if (last) break;
    }
    return out;
}

} // namespace shadersrc
