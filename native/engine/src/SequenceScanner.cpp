#include "exrc/SequenceScanner.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <cstdio>
#include <regex>
#include <sstream>

namespace exrc {

static std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

bool hasExrExtension(const fs::path& p) { return toLower(p.extension().string()) == ".exr"; }

ParsedName parseFrameName(const std::string& filename) {
    ParsedName r;
    const auto dot = filename.find_last_of('.');
    const std::string stem = dot == std::string::npos ? filename : filename.substr(0, dot);
    const std::string ext = dot == std::string::npos ? std::string() : filename.substr(dot);

    size_t end = stem.size();
    while (end > 0 && !std::isdigit(static_cast<unsigned char>(stem[end - 1]))) --end;
    if (end == 0) {
        r.prefix = stem;
        r.suffix = ext;
        return r;
    }
    size_t begin = end;
    while (begin > 0 && std::isdigit(static_cast<unsigned char>(stem[begin - 1]))) --begin;

    r.hasFrame = (end - begin) <= 9;  // ignore absurd digit runs (hashes, timestamps)
    if (!r.hasFrame) {
        r.prefix = stem;
        r.suffix = ext;
        return r;
    }
    r.prefix = stem.substr(0, begin);
    r.digits = stem.substr(begin, end - begin);
    r.suffix = stem.substr(end) + ext;
    return r;
}

std::optional<fs::path> SequenceInfo::pathFor(int frame) const {
    auto it = std::lower_bound(files.begin(), files.end(), frame,
                               [](const auto& e, int f) { return e.first < f; });
    if (it != files.end() && it->first == frame) return it->second;
    return std::nullopt;
}

std::string SequenceInfo::pattern() const {
    if (files.size() == 1 && first == last && padding == 0 && prefix.empty())
        return files.front().second.filename().string();
    return prefix + std::string(std::max(padding, 1), '#') + suffix;
}

namespace {

struct Group {
    std::map<int, fs::path> frames;
    std::map<size_t, int> digitLengths;  // digit count -> occurrences
};

SequenceInfo buildInfo(const fs::path& dir, const std::string& prefix, const std::string& suffix, const Group& g) {
    SequenceInfo s;
    s.directory = dir;
    s.prefix = prefix;
    s.suffix = suffix;
    for (const auto& [f, p] : g.frames) s.files.emplace_back(f, p);
    s.first = s.files.front().first;
    s.last = s.files.back().first;
    // A fixed digit count across all names means a padded pattern (####); mixed lengths
    // (1, 2 ... 10) means unpadded frame numbers.
    if (g.digitLengths.size() == 1) s.padding = int(g.digitLengths.begin()->first);
    size_t idx = 0;
    for (int f = s.first; f <= s.last; ++f) {
        if (idx < s.files.size() && s.files[idx].first == f) {
            ++idx;
        } else {
            s.missing.push_back(f);
        }
    }
    return s;
}

}  // namespace

std::optional<SequenceInfo> scanSequence(const fs::path& input, std::string* error) {
    std::error_code ec;
    const bool isDir = fs::is_directory(input, ec);
    if (!isDir && !fs::exists(input, ec)) {
        if (error) *error = "Path does not exist: " + input.string();
        return std::nullopt;
    }
    const fs::path dir = isDir ? input : input.parent_path();

    ParsedName wanted;
    if (!isDir) {
        if (!hasExrExtension(input)) {
            if (error) *error = "Not an EXR file: " + input.filename().string();
            return std::nullopt;
        }
        wanted = parseFrameName(input.filename().string());
        if (!wanted.hasFrame) {
            // Single still image: a one-frame "sequence".
            SequenceInfo s;
            s.directory = dir;
            s.files.emplace_back(1, input);
            s.first = s.last = 1;
            return s;
        }
    }

    std::map<std::pair<std::string, std::string>, Group> groups;
    for (fs::directory_iterator it(dir.empty() ? fs::path(".") : dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || !hasExrExtension(it->path())) continue;
        const ParsedName pn = parseFrameName(it->path().filename().string());
        if (!pn.hasFrame) continue;
        if (!isDir && (pn.prefix != wanted.prefix || pn.suffix != wanted.suffix)) continue;
        Group& g = groups[{pn.prefix, pn.suffix}];
        const int frame = std::stoi(pn.digits);
        g.frames.emplace(frame, it->path());
        g.digitLengths[pn.digits.size()]++;
    }
    if (ec) {
        if (error) *error = "Cannot read directory: " + ec.message();
        return std::nullopt;
    }
    if (groups.empty()) {
        if (error) *error = "No EXR sequence found in " + dir.string();
        return std::nullopt;
    }
    auto best = std::max_element(groups.begin(), groups.end(), [](const auto& a, const auto& b) {
        return a.second.frames.size() < b.second.frames.size();
    });
    return buildInfo(dir, best->first.first, best->first.second, best->second);
}

}  // namespace exrc

namespace exrc {

namespace {
struct VersionMatch {
    std::string token;  // "v003"
    char letter = 'v';
    int number = 0;
    int digits = 0;
};

std::optional<VersionMatch> lastVersion(const std::string& text) {
    static const std::regex re(R"(([vV])(\d{1,5})(?!\d))");
    std::optional<VersionMatch> found;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), re); it != std::sregex_iterator(); ++it) {
        VersionMatch m;
        m.token = it->str(0);
        m.letter = it->str(1)[0];
        m.number = std::stoi(it->str(2));
        m.digits = int(it->str(2).size());
        found = m;
    }
    return found;
}

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    for (size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos; pos += to.size()) s.replace(pos, from.size(), to);
    return s;
}
}  // namespace

std::string versionToken(const SequenceInfo& seq) {
    const auto m = lastVersion((seq.directory / (seq.prefix + seq.suffix)).generic_string());
    return m ? m->token : std::string();
}

std::optional<fs::path> findVersionSibling(const SequenceInfo& seq, int delta) {
    const std::string dir = seq.directory.generic_string();
    const auto m = lastVersion(dir + "/" + seq.prefix + seq.suffix);
    if (!m || delta == 0) return std::nullopt;
    for (int k = 1; k <= 200; ++k) {
        const int candidate = m->number + (delta > 0 ? k : -k);
        if (candidate < 0) break;
        char digits[16];
        std::snprintf(digits, sizeof(digits), "%0*d", m->digits, candidate);
        const std::string token = std::string(1, m->letter) + digits;
        const fs::path newDir = fs::path(replaceAll(dir, m->token, token));
        const std::string newPrefix = replaceAll(seq.prefix, m->token, token);
        std::error_code ec;
        if (!fs::is_directory(newDir, ec)) continue;
        for (fs::directory_iterator it(newDir, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec) || !hasExrExtension(it->path())) continue;
            const ParsedName pn = parseFrameName(it->path().filename().string());
            if (seq.files.size() == 1 && seq.prefix.empty()) {
                // Single still: compare whole names.
                if (it->path().filename().string() == replaceAll(seq.files.front().second.filename().string(), m->token, token))
                    return it->path();
                continue;
            }
            if (pn.hasFrame && pn.prefix == newPrefix && pn.suffix == seq.suffix) return it->path();
        }
    }
    return std::nullopt;
}

}  // namespace exrc
