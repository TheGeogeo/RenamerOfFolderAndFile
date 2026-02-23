// CleanMediaRenamer
// rename_sanitize.cpp
// C++17 - Recursive traversal, replaces ['_', '-', '.'] with ' '
// EXCEPT the last '.' (extension) for files.
// + Removes common "release/download" tags (bluray, dvdrip, vff, etc.).
// Prints one line per rename and, at the end, a 5s countdown then "All done" + total.

#include <filesystem>
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <unordered_set>
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <type_traits>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <limits.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace fs = std::filesystem;

static fs::path g_self_exe;

static fs::path get_self_executable_path() {
#ifdef _WIN32
	std::wstring buf(32768, L'\0');
	DWORD len = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
	if (len == 0) return {};
	buf.resize(len);
	return fs::path(buf);
#elif defined(__APPLE__)
	uint32_t size = 0;
	_NSGetExecutablePath(nullptr, &size);
	std::string buf(size, '\0');
	if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
	// buf may contain trailing '\0'
	return fs::path(buf.c_str());
#else
	// Linux: /proc/self/exe
	std::vector<char> buf(4096, '\0');
	ssize_t len = ::readlink("/proc/self/exe", buf.data(), buf.size() - 1);
	if (len <= 0) return {};
	buf[(size_t)len] = '\0';
	return fs::path(buf.data());
#endif
}

static fs::path normalize_path(fs::path p) {
	std::error_code ec;
	p = fs::absolute(p, ec);
	if (ec) return p.lexically_normal();
	return p.lexically_normal();
}

template <typename CharT>
static bool is_space(CharT c) {
	return c == CharT(' ') || c == CharT('\t') || c == CharT('\n') ||
		c == CharT('\r') || c == CharT('\f') || c == CharT('\v');
}

template <typename CharT>
static bool is_alnum_char(CharT c) {
	if constexpr (std::is_same_v<CharT, char>) {
		return std::isalnum(static_cast<unsigned char>(c)) != 0;
	}
	else {
		return std::iswalnum(static_cast<wint_t>(c)) != 0;
	}
}

template <typename CharT>
static CharT to_lower_char(CharT c) {
	if constexpr (std::is_same_v<CharT, char>) {
		return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	else {
		return static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c)));
	}
}

template <typename CharT>
static CharT to_upper_char(CharT c) {
	if constexpr (std::is_same_v<CharT, char>) {
		return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	}
	else {
		return static_cast<wchar_t>(std::towupper(static_cast<wint_t>(c)));
	}
}

template <typename CharT>
static bool is_alpha_char(CharT c) {
	if constexpr (std::is_same_v<CharT, char>) {
		return std::isalpha(static_cast<unsigned char>(c)) != 0;
	}
	else {
		return std::iswalpha(static_cast<wint_t>(c)) != 0;
	}
}

// "Sentence case": lowercase everything, then uppercase the first alphabetic letter.
template <typename StringT>
static StringT phrase_case(StringT s) {
	using C = typename StringT::value_type;

	// Lowercase everything
	for (auto& ch : s) ch = to_lower_char(ch);

	// Uppercase the first alphabetic letter
	for (size_t i = 0; i < s.size(); ++i) {
		if (is_space(s[i])) continue;
		if (is_alpha_char(s[i])) {
			s[i] = to_upper_char(s[i]);
			break;
		}
		// If it's a digit/punctuation, keep searching for the first letter
	}
	return s;
}

template <typename StringT>
static StringT to_lower_copy(const StringT& s) {
	StringT out;
	out.reserve(s.size());
	for (auto ch : s) out.push_back(to_lower_char(ch));
	return out;
}

// Strip punctuation/symbols at the edges (e.g., "[BluRay]" -> "BluRay")
template <typename StringT>
static StringT strip_non_alnum_edges(const StringT& s) {
	using C = typename StringT::value_type;
	size_t b = 0;
	size_t e = s.size();
	while (b < e && !is_alnum_char<C>(s[b])) ++b;
	while (e > b && !is_alnum_char<C>(s[e - 1])) --e;
	return (b < e) ? s.substr(b, e - b) : StringT{};
}

template <typename StringT>
static StringT keep_inner_apostrophes_only(StringT s) {
	using C = typename StringT::value_type;

	auto is_apostrophe = [](C ch) -> bool {
		if constexpr (std::is_same_v<C, char>) {
			return ch == C('\'');
		}
		else {
			// ' and typographic apostrophe ’ (U+2019) in wchar_t
			return ch == C('\'') || ch == static_cast<C>(0x2019);
		}
		};

	StringT out;
	out.reserve(s.size());

	for (size_t i = 0; i < s.size(); ++i) {
		C ch = s[i];

		if (is_apostrophe(ch)) {
			// Keep only if surrounded by two alphanumerics (e.g., D'Après)
			bool keep = false;
			if (i > 0 && i + 1 < s.size()) {
				if (is_alnum_char(s[i - 1]) && is_alnum_char(s[i + 1])) {
					keep = true;
				}
			}
			if (keep) out.push_back(ch);
			continue; // otherwise drop it
		}

		out.push_back(ch);
	}

	return out;
}

template <typename StringT>
static bool all_digits(const StringT& s) {
	using C = typename StringT::value_type;
	if (s.empty()) return false;
	for (auto ch : s) {
		if constexpr (std::is_same_v<C, char>) {
			if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
		}
		else {
			if (!std::iswdigit(static_cast<wint_t>(ch))) return false;
		}
	}
	return true;
}

template <typename StringT>
static bool is_resolution_token(const StringT& low) {
	using C = typename StringT::value_type;

	// 720p / 1080p / 2160p
	if (low.size() >= 2 && low.back() == C('p')) {
		StringT n = low.substr(0, low.size() - 1);
		if (all_digits(n)) return true;
	}

	// 1920x1080
	auto pos = low.find(C('x'));
	if (pos != StringT::npos && pos > 0 && pos + 1 < low.size()) {
		StringT a = low.substr(0, pos);
		StringT b = low.substr(pos + 1);
		if (all_digits(a) && all_digits(b)) return true;
	}

	// 4k / uhd
	if (low == StringT{ C('4'),C('k') } || low == StringT{ C('u'),C('h'),C('d') }) return true;

	return false;
}

template <typename CharT>
static const std::unordered_set<std::basic_string<CharT>>& single_tags() {
	using S = std::basic_string<CharT>;
	static const std::vector<const char*> kSingleTags = {
		// sources / quality
		"bluray","blu-ray","bdrip","bd-rip","brrip",
		"dvdrip","dvd-rip","dvrip","tvrip","hdrip","hd-rip",
		"webrip","web-rip","webdl","web-dl","web",
		"hdtv","cam","ts","telesync","tc","telecine","scr","screener",
		"remux",

		// languages / subtitles
		"vf","vff","vfi","truefrench","true-french","french",
		"vostfr","vost","subfrench","subs","sub","stfr",
		"multi","pophd","tyhd",
		"amzn","nf","dsnp","hmax","atvp","hulu",

		// codecs / encodes
		"x264","x265","h264","h265","hevc","av1","xvid","divx",

		// audio
		"aac","ac3","eac3","ddp","dd","dts","dtshd","truehd","atmos","flac","mp3",

		// misc common tags
		"hdr","sdr","10bit","8bit","proper","repack","limited","unrated","extended","internal","readnfo",

		// download / sites
		"download","telecharger","ddl","dl",

		// custom teams / misc
		"serqph","qtz","notag","4klight","hdlight","he",
	};

	static const std::unordered_set<S> set = [] {
		std::unordered_set<S> tmp;
		tmp.reserve(kSingleTags.size() * 2);
		for (auto* cstr : kSingleTags) {
			S s;
			for (const char* p = cstr; *p; ++p) s.push_back(static_cast<CharT>(*p));
			tmp.insert(s);
		}
		return tmp;
		}();

	return set;
}

template <typename CharT>
static const std::unordered_set<std::basic_string<CharT>>& phrase2_tags() {
	using S = std::basic_string<CharT>;
	static const std::vector<const char*> kPhrase2 = {
		// variants split by sanitize: "Blu-Ray" => "Blu Ray"
		"blu ray",
		"web dl",
		"web rip",
		"dvd rip",
		"bd rip",
		"hd rip",
		"true french",
		"dts hd",
		"4k light",
		"hd light",
	};

	static const std::unordered_set<S> set = [] {
		std::unordered_set<S> tmp;
		tmp.reserve(kPhrase2.size() * 2);
		for (auto* cstr : kPhrase2) {
			S s;
			for (const char* p = cstr; *p; ++p) s.push_back(static_cast<CharT>(*p));
			tmp.insert(s);
		}
		return tmp;
		}();

	return set;
}

template <typename CharT>
static const std::unordered_set<std::basic_string<CharT>>& phrase3_tags() {
	using S = std::basic_string<CharT>;
	static const std::vector<const char*> kPhrase3 = {
		// Example if needed later: "multi audio fr"
		// "multi audio fr"
	};

	static const std::unordered_set<S> set = [] {
		std::unordered_set<S> tmp;
		tmp.reserve(kPhrase3.size() * 2);
		for (auto* cstr : kPhrase3) {
			S s;
			for (const char* p = cstr; *p; ++p) s.push_back(static_cast<CharT>(*p));
			tmp.insert(s);
		}
		return tmp;
		}();

	return set;
}

template <typename StringT>
static std::vector<StringT> split_spaces(const StringT& s) {
	using C = typename StringT::value_type;
	std::vector<StringT> out;
	StringT cur;

	for (auto ch : s) {
		if (is_space(ch)) {
			if (!cur.empty()) { out.push_back(cur); cur.clear(); }
		}
		else {
			cur.push_back(ch);
		}
	}
	if (!cur.empty()) out.push_back(cur);
	return out;
}

// Removes tags like BluRay / DVDRip / VFF / 1080p / WEB DL etc.
template <typename StringT>
static StringT remove_release_tags(const StringT& sanitized) {
	using C = typename StringT::value_type;
	const auto& singles = single_tags<C>();
	const auto& p2 = phrase2_tags<C>();
	const auto& p3 = phrase3_tags<C>();

	auto tokens_raw = split_spaces(sanitized);

	// Normalize tokens (strip edge punctuation) while keeping display-friendly tokens
	std::vector<StringT> tokens;
	tokens.reserve(tokens_raw.size());
	for (auto& t : tokens_raw) {
		auto stripped = strip_non_alnum_edges(t);
		if (!stripped.empty()) tokens.push_back(stripped);
	}

	// local helpers
	auto starts_with = [](const StringT& s, const StringT& prefix) {
		return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
		};
	auto is_one_digit = [&](const StringT& low) {
		return all_digits(low) && low.size() == 1; // "0".."9"
		};
	auto is_audio_base = [&](const StringT& low) {
		// codecs often followed by "2.0", "5.1", "7.1", etc.
		// (after sanitize: "2 0", "5 1", ...)
		return (low == StringT{ C('d'),C('d'),C('p') } ||
			low == StringT{ C('e'),C('a'),C('c'),C('3') } ||
			low == StringT{ C('a'),C('c'),C('3') } ||
			low == StringT{ C('d'),C('t'),C('s') } ||
			low == StringT{ C('a'),C('a'),C('c') });
		};

	std::vector<StringT> kept;
	kept.reserve(tokens.size());

	for (size_t i = 0; i < tokens.size(); ) {
		// compare in lowercase
		StringT t0_low = to_lower_copy(tokens[i]);

		// match 3-word phrases
		if (i + 2 < tokens.size()) {
			StringT a = to_lower_copy(tokens[i]);
			StringT b = to_lower_copy(tokens[i + 1]);
			StringT c = to_lower_copy(tokens[i + 2]);
			StringT phrase3 = a + StringT{ C(' ') } + b + StringT{ C(' ') } + c;
			if (p3.find(phrase3) != p3.end()) { i += 3; continue; }
		}

		// match 2-word phrases
		if (i + 1 < tokens.size()) {
			StringT a = to_lower_copy(tokens[i]);
			StringT b = to_lower_copy(tokens[i + 1]);
			StringT phrase2 = a + StringT{ C(' ') } + b;
			if (p2.find(phrase2) != p2.end()) { i += 2; continue; }
		}

		// "DDP2.0" => "ddp2 0", "DDP5.1" => "ddp5 1", "DTS7.1" => "dts7 1"
		// Current token = ddpX / dtsX (X = digits), also skip next token if it's "0".."9"
		if (starts_with(t0_low, StringT{ C('d'),C('d'),C('p') }) && t0_low.size() > 3 && all_digits(t0_low.substr(3))) {
			++i; // skip "ddpX"
			if (i < tokens.size()) {
				StringT nxt = to_lower_copy(tokens[i]);
				if (is_one_digit(nxt)) ++i; // skip "0" or "1"
			}
			continue;
		}
		if (starts_with(t0_low, StringT{ C('d'),C('t'),C('s') }) && t0_low.size() > 3 && all_digits(t0_low.substr(3))) {
			++i; // skip "dtsX"
			if (i < tokens.size()) {
				StringT nxt = to_lower_copy(tokens[i]);
				if (is_one_digit(nxt)) ++i;
			}
			continue;
		}

		// "EAC3 5.1" => "eac3 5 1" / "DDP 2.0" => "ddp 2 0"
		// If we encounter an audio codec "base" and it's followed by 1-2 digit tokens, skip them too
		if (is_audio_base(t0_low)) {
			++i; // skip codec (eac3/ac3/ddp/dts/aac)
			// skip 1 to 2 digits (2 0 / 5 1 / 7 1)
			if (i < tokens.size()) {
				StringT d1 = to_lower_copy(tokens[i]);
				if (is_one_digit(d1)) {
					++i;
					if (i < tokens.size()) {
						StringT d2 = to_lower_copy(tokens[i]);
						if (is_one_digit(d2)) ++i;
					}
				}
			}
			continue;
		}

		// VF + index (vf2, vf3, vf4...)
		if (t0_low.size() > 2 &&
			t0_low[0] == C('v') && t0_low[1] == C('f') &&
			all_digits(t0_low.substr(2))) {
			++i;
			continue;
		}

		// Channels (2ch, 6ch, 8ch...)
		if (t0_low.size() > 2 &&
			t0_low[t0_low.size() - 2] == C('c') &&
			t0_low[t0_low.size() - 1] == C('h')) {
			StringT n = t0_low.substr(0, t0_low.size() - 2);
			if (all_digits(n)) {
				++i;
				continue;
			}
		}

		// Tokens like 1080p / 1920x1080 / 4k
		if (is_resolution_token(t0_low)) { ++i; continue; }

		// Single-token blacklist
		if (singles.find(t0_low) != singles.end()) { ++i; continue; }

		kept.push_back(tokens[i]);
		++i;
	}

	// Re-join
	StringT joined;
	for (size_t i = 0; i < kept.size(); ++i) {
		if (i) joined.push_back(C(' '));
		joined += kept[i];
	}

	return joined;
}

template <typename StringT>
static StringT sanitize_all(const StringT& in, bool replace_dots) {
	using C = typename StringT::value_type;
	StringT tmp;
	tmp.reserve(in.size());

	for (auto ch : in) {
		if (ch == C('_') || ch == C('-') || (replace_dots && ch == C('.'))) {
			tmp.push_back(C(' '));
		}
		else {
			tmp.push_back(ch);
		}
	}

	// Collapse spaces + trim
	StringT out;
	out.reserve(tmp.size());

	bool prev_space = true;
	for (auto ch : tmp) {
		bool sp = is_space(ch);
		if (sp) {
			if (!prev_space) out.push_back(C(' '));
			prev_space = true;
		}
		else {
			out.push_back(ch);
			prev_space = false;
		}
	}
	while (!out.empty() && out.back() == C(' ')) out.pop_back();

	// Avoid problematic endings (Windows) + empty names
	while (!out.empty() && (out.back() == C(' ') || out.back() == C('.'))) out.pop_back();
	if (out.empty()) {
		out = StringT{ C('u'),C('n'),C('n'),C('a'),C('m'),C('e'),C('d') };
	}
	return out;
}

template <typename StringT>
static StringT sanitize_and_clean(const StringT& in, bool replace_dots) {
	StringT s = sanitize_all(in, replace_dots);
	s = remove_release_tags(s);

	// Re-collapse/trim in case tag removal introduced weird spacing
	s = sanitize_all(s, /*replace_dots=*/false);

	// Keep apostrophe only if between two alphanumerics (D'Après), otherwise remove it
	s = keep_inner_apostrophes_only(std::move(s));

	// IMPORTANT: re-collapse after removing isolated apostrophes
	s = sanitize_all(s, /*replace_dots=*/false);

	// Sentence case: first letter uppercase, rest lowercase
	s = phrase_case(std::move(s));

	return s;
}

static fs::path make_unique_target(const fs::path& desired) {
	if (!fs::exists(desired)) return desired;

	fs::path parent = desired.parent_path();

	// Insert a suffix before extension if present
	if (desired.has_extension()) {
#ifdef _WIN32
		std::wstring stem = desired.stem().wstring();
		std::wstring ext = desired.extension().wstring();
		for (int i = 1; i < 100000; ++i) {
			fs::path cand = parent / fs::path(stem + L" (" + std::to_wstring(i) + L")" + ext);
			if (!fs::exists(cand)) return cand;
		}
#else
		std::string stem = desired.stem().string();
		std::string ext = desired.extension().string();
		for (int i = 1; i < 100000; ++i) {
			fs::path cand = parent / (stem + " (" + std::to_string(i) + ")" + ext);
			if (!fs::exists(cand)) return cand;
		}
#endif
	}
	else {
#ifdef _WIN32
		std::wstring base = desired.filename().wstring();
		for (int i = 1; i < 100000; ++i) {
			fs::path cand = parent / fs::path(base + L" (" + std::to_wstring(i) + L")");
			if (!fs::exists(cand)) return cand;
		}
#else
		std::string base = desired.filename().string();
		for (int i = 1; i < 100000; ++i) {
			fs::path cand = parent / (base + " (" + std::to_string(i) + ")");
			if (!fs::exists(cand)) return cand;
		}
#endif
	}

	return desired;
}

struct Stats {
	std::uint64_t renamed = 0;
	std::uint64_t skipped = 0;
	std::uint64_t errors = 0;
};

static fs::path make_temp_name(const fs::path& /*src*/, const fs::path& desired) {
	std::error_code ec;
	fs::path parent = desired.parent_path();

#ifdef _WIN32
	std::wstring stem = desired.has_extension()
		? desired.stem().wstring()
		: desired.filename().wstring();
	std::wstring ext = desired.has_extension()
		? desired.extension().wstring()
		: L"";

	// e.g., "Name.__tmp__.mkv"
	std::wstring base = stem + L".__tmp__" + ext;
	fs::path cand = parent / fs::path(base);
	if (!fs::exists(cand, ec) && !ec) return cand;

	for (int i = 1; i < 100000; ++i) {
		fs::path c = parent / fs::path(stem + L".__tmp__ (" + std::to_wstring(i) + L")" + ext);
		if (!fs::exists(c, ec) && !ec) return c;
	}
#else
	std::string stem = desired.has_extension()
		? desired.stem().string()
		: desired.filename().string();
	std::string ext = desired.has_extension()
		? desired.extension().string()
		: "";

	// e.g., "Name.__tmp__.mkv"
	std::string base = stem + ".__tmp__" + ext;
	fs::path cand = parent / base;
	if (!fs::exists(cand, ec) && !ec) return cand;

	for (int i = 1; i < 100000; ++i) {
		fs::path c = parent / (stem + ".__tmp__ (" + std::to_string(i) + ")" + ext);
		if (!fs::exists(c, ec) && !ec) return c;
	}
#endif

	// Fallback
	return parent / fs::path("__tmp__");
}

static void rename_entry(const fs::path& p, Stats& st) {
	std::error_code ec;

	// Never rename the currently running executable
	if (!g_self_exe.empty()) {
		std::error_code ecAbs;
		fs::path pAbs = normalize_path(p);

		std::error_code ecEq;
		if (!ecEq && fs::exists(pAbs, ecAbs) && !ecAbs && fs::exists(g_self_exe, ecAbs) && !ecAbs) {
			if (fs::equivalent(pAbs, g_self_exe, ecEq) && !ecEq) {
				st.skipped++;
				return;
			}
		}
		// Simple fallback (if equivalent() fails)
		if (pAbs == g_self_exe) {
			st.skipped++;
			return;
		}
	}

	if (!fs::exists(p, ec) || ec) { st.skipped++; return; }

	bool is_dir = fs::is_directory(p, ec);
	if (ec) { st.errors++; return; }

	fs::path desired;

	if (is_dir) {
		// Folder: also replace all '.' characters
#ifdef _WIN32
		std::wstring oldName = p.filename().wstring();
		std::wstring newName = sanitize_and_clean(oldName, /*replace_dots=*/true);
		desired = p.parent_path() / fs::path(newName);
#else
		std::string oldName = p.filename().string();
		std::string newName = sanitize_and_clean(oldName, /*replace_dots=*/true);
		desired = p.parent_path() / fs::path(newName);
#endif
	}
	else {
		// File: sanitize the stem; keep extension unchanged (final '.')
		fs::path stem = p.stem();      // e.g., "archive.tar"
		fs::path ext = p.extension();  // e.g., ".gz"
#ifdef _WIN32
		std::wstring newStem = sanitize_and_clean(stem.wstring(), /*replace_dots=*/true);
		desired = p.parent_path() / fs::path(newStem + ext.wstring());
#else
		std::string newStem = sanitize_and_clean(stem.string(), /*replace_dots=*/true);
		desired = p.parent_path() / fs::path(newStem + ext.string());
#endif
	}

	if (desired == p) { st.skipped++; return; }

	// If "desired" exists but points to the same file (Windows case-only rename),
	// do a 2-step rename to avoid adding "(1)".
	std::error_code ecExist;
	bool existsDesired = fs::exists(desired, ecExist) && !ecExist;

	bool sameFile = false;
	if (existsDesired) {
		std::error_code ecEq;
		sameFile = fs::equivalent(p, desired, ecEq) && !ecEq;
	}

	fs::path target;

	if (sameFile) {
		// Two-step rename: p -> temp -> desired (allows case changes without suffix)
		fs::path tmp = make_temp_name(p, desired);

		std::error_code ec1;
		fs::rename(p, tmp, ec1);
		if (ec1) {
			std::cout << "[ERR] " << p.string() << "  ->  " << tmp.string()
				<< " | " << ec1.message() << "\n";
			st.errors++;
			return;
		}

		std::error_code ec2;
		fs::rename(tmp, desired, ec2);
		if (ec2) {
			// Rollback attempt
			std::error_code ecBack;
			fs::rename(tmp, p, ecBack);

			std::cout << "[ERR] " << tmp.string() << "  ->  " << desired.string()
				<< " | " << ec2.message() << "\n";
			st.errors++;
			return;
		}

		std::cout << "[OK ] " << p.string() << "  ->  " << desired.string() << "\n";
		st.renamed++;
		return;
	}

	// Normal case: real collision possible -> use suffix if needed
	target = make_unique_target(desired);

	fs::rename(p, target, ec);
	if (ec) {
		std::cout << "[ERR] " << p.string() << "  ->  " << target.string()
			<< " | " << ec.message() << "\n";
		st.errors++;
	}
	else {
		std::cout << "[OK ] " << p.string() << "  ->  " << target.string() << "\n";
		st.renamed++;
	}
}

// Post-order: rename children first, then the folder (avoids breaking traversal)
static void process_dir(const fs::path& dir, Stats& st, bool is_root = false) {
	std::error_code ec;

	std::vector<fs::path> children;
	for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
		it != end && !ec; it.increment(ec)) {
		children.push_back(it->path());
	}
	if (ec) {
		std::cout << "[ERR] Reading folder: " << dir.string()
			<< " | " << ec.message() << "\n";
		st.errors++;
		return;
	}

	for (const auto& child : children) {
		std::error_code ec2;
		bool is_dir = fs::is_directory(child, ec2);
		bool is_symlink = fs::is_symlink(child, ec2);

		if (!ec2 && is_dir && !is_symlink) {
			process_dir(child, st, false); // rename this folder at the end
		}
		else {
			rename_entry(child, st);
		}
	}

	// Safety: never rename the root directory itself
	if (!is_root) {
		rename_entry(dir, st);
	}
}

int main(int argc, char** argv) {
	auto self = get_self_executable_path();
	if (!self.empty()) {
		g_self_exe = normalize_path(self);
	}

	fs::path root = fs::current_path();
	if (argc >= 2) {
		std::string a = argv[1];
		if (a == "--help" || a == "-h") {
			std::cout << "Usage: " << argv[0] << " [path]\n";
			return 0;
		}
		root = fs::path(a);
	}

	if (!fs::exists(root) || !fs::is_directory(root)) {
		std::cerr << "Invalid path: " << root.string() << "\n";
		return 1;
	}

	std::cout << "Root: " << root.string() << "\n\n";

	Stats st;
	process_dir(root, st, true);

	std::cout << "\n--- Summary ---\n"
		<< "Renamed : " << st.renamed << "\n"
		<< "Skipped : " << st.skipped << "\n"
		<< "Errors  : " << st.errors << "\n\n";

	// 5-second countdown
	for (int i = 5; i >= 1; --i) {
		std::cout << "Finishing in " << i << "...\n";
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}

	std::cout << "All done. Total: " << st.renamed << "\n";
	return st.errors ? 2 : 0;
}