// rename_sanitize.cpp
// C++17 - Parcours récursif, remplace ['_', '-', '.'] par ' '
// SAUF le dernier '.' (extension) pour les fichiers.
// + Supprime des tags "release/téléchargement" (bluray, dvdrip, vff, etc.)
// Affiche une ligne par modification et, à la fin, un timer 5s puis "All done" + total.

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
	// buf peut contenir des '\0' en fin
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

// "Phrase case" : tout en minuscule, puis 1ère lettre alphabétique en majuscule
template <typename StringT>
static StringT phrase_case(StringT s) {
	using C = typename StringT::value_type;

	// tout en minuscule
	for (auto& ch : s) ch = to_lower_char(ch);

	// 1ère lettre alphabétique en majuscule
	for (size_t i = 0; i < s.size(); ++i) {
		if (is_space(s[i])) continue;
		if (is_alpha_char(s[i])) {
			s[i] = to_upper_char(s[i]);
			break;
		}
		// si c'est un chiffre/punct, on continue jusqu'à trouver une lettre
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

// Supprime ponctuation / symboles en bord (ex: "[BluRay]" -> "BluRay")
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
			// ' et apostrophe typographique ’ (U+2019) en wchar_t
			return ch == C('\'') || ch == static_cast<C>(0x2019);
		}
		};

	StringT out;
	out.reserve(s.size());

	for (size_t i = 0; i < s.size(); ++i) {
		C ch = s[i];

		if (is_apostrophe(ch)) {
			// On garde seulement si entourée par 2 alphanums (ex: D'Après)
			bool keep = false;
			if (i > 0 && i + 1 < s.size()) {
				if (is_alnum_char(s[i - 1]) && is_alnum_char(s[i + 1])) {
					keep = true;
				}
			}
			if (keep) out.push_back(ch);
			continue; // sinon on la supprime
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
		// sources / qualités
		"bluray","blu-ray","bdrip","bd-rip","brrip",
		"dvdrip","dvd-rip","dvrip","tvrip","hdrip","hd-rip",
		"webrip","web-rip","webdl","web-dl","web",
		"hdtv","cam","ts","telesync","tc","telecine","scr","screener",
		"remux",

		// langues / sous-titres
		"vf","vff","vfi","truefrench","true-french","french",
		"vostfr","vost","subfrench","subs","sub","stfr",
		"multi", "pophd","tyhd",
		"amzn","nf","dsnp","hmax","atvp","hulu",

		// codecs / encodes
		"x264","x265","h264","h265","hevc","av1","xvid","divx",

		// audio
		"aac","ac3","eac3","ddp","dd","dts","dtshd","truehd","atmos","flac","mp3",

		// divers tags courants
		"hdr","sdr","10bit","8bit","proper","repack","limited","unrated","extended","internal","readnfo",

		// “download” / sites
		"download","telecharger","ddl","dl",

		"serqph","qtz","notag", "4klight","hdlight", "he",
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
		// variantes éclatées par sanitize: "Blu-Ray" => "Blu Ray"
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
		// exemple si tu veux : "multi audio fr" etc (à compléter si besoin)
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

// Nettoie les tags type BluRay / DVDRip / VFF / 1080p / WEB DL etc.
template <typename StringT>
static StringT remove_release_tags(const StringT& sanitized) {
	using C = typename StringT::value_type;
	const auto& singles = single_tags<C>();
	const auto& p2 = phrase2_tags<C>();
	const auto& p3 = phrase3_tags<C>();

	auto tokens_raw = split_spaces(sanitized);

	// normalise tokens (strip punctuation edges) tout en gardant la version “affichage”
	std::vector<StringT> tokens;
	tokens.reserve(tokens_raw.size());
	for (auto& t : tokens_raw) {
		auto stripped = strip_non_alnum_edges(t);
		if (!stripped.empty()) tokens.push_back(stripped);
	}

	// helpers locaux
	auto starts_with = [](const StringT& s, const StringT& prefix) {
		return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
		};
	auto is_one_digit = [&](const StringT& low) {
		return all_digits(low) && low.size() == 1; // "0".."9"
		};
	auto is_audio_base = [&](const StringT& low) {
		// codecs qui apparaissent souvent avec "2.0", "5.1", "7.1" etc
		// (après sanitize: "2 0", "5 1", ...)
		return (low == StringT{ C('d'),C('d'),C('p') } ||
			low == StringT{ C('e'),C('a'),C('c'),C('3') } ||
			low == StringT{ C('a'),C('c'),C('3') } ||
			low == StringT{ C('d'),C('t'),C('s') } ||
			low == StringT{ C('a'),C('a'),C('c') });
		};

	std::vector<StringT> kept;
	kept.reserve(tokens.size());

	for (size_t i = 0; i < tokens.size(); ) {
		// on compare en lower
		StringT t0_low = to_lower_copy(tokens[i]);

		// match phrase 3 mots
		if (i + 2 < tokens.size()) {
			StringT a = to_lower_copy(tokens[i]);
			StringT b = to_lower_copy(tokens[i + 1]);
			StringT c = to_lower_copy(tokens[i + 2]);
			StringT phrase3 = a + StringT{ C(' ') } + b + StringT{ C(' ') } + c;
			if (p3.find(phrase3) != p3.end()) { i += 3; continue; }
		}

		// match phrase 2 mots
		if (i + 1 < tokens.size()) {
			StringT a = to_lower_copy(tokens[i]);
			StringT b = to_lower_copy(tokens[i + 1]);
			StringT phrase2 = a + StringT{ C(' ') } + b;
			if (p2.find(phrase2) != p2.end()) { i += 2; continue; }
		}

		// --- NEW: "DDP2.0" => "ddp2 0", "DDP5.1" => "ddp5 1", "DTS7.1" => "dts7 1" ---
		// token courant = ddpX / dtsX (X = digits), on saute aussi le token suivant si "0".."9"
		if (starts_with(t0_low, StringT{ C('d'),C('d'),C('p') }) && t0_low.size() > 3 && all_digits(t0_low.substr(3))) {
			++i; // skip "ddpX"
			if (i < tokens.size()) {
				StringT nxt = to_lower_copy(tokens[i]);
				if (is_one_digit(nxt)) ++i; // skip "0" ou "1"
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

		// --- NEW: "EAC3 5.1" => "eac3 5 1" / "DDP 2.0" => "ddp 2 0" ---
		// si on rencontre un codec audio "base", et qu'il est suivi de 1 ou 2 tokens digit, on les supprime aussi
		if (is_audio_base(t0_low)) {
			++i; // skip le codec (eac3/ac3/ddp/dts/aac)
			// saute 1 à 2 digits (2 0 / 5 1 / 7 1)
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

		// channels (2ch, 6ch, 8ch...)
		if (t0_low.size() > 2 &&
			t0_low[t0_low.size() - 2] == C('c') &&
			t0_low[t0_low.size() - 1] == C('h')) {
			StringT n = t0_low.substr(0, t0_low.size() - 2);
			if (all_digits(n)) {
				++i;
				continue;
			}
		}

		// tokens type 1080p / 1920x1080 / 4k
		if (is_resolution_token(t0_low)) { ++i; continue; }

		// single tag blacklist
		if (singles.find(t0_low) != singles.end()) { ++i; continue; }

		kept.push_back(tokens[i]);
		++i;
	}

	// rejoin
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

	// collapse espaces + trim
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

	// évite fin problématique (Windows) + noms vides
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

	// re-collapse/trim au cas où la suppression laisse des espaces bizarres
	s = sanitize_all(s, /*replace_dots=*/false);

	// Conserve ' seulement si entre 2 alphanums (D'Après), sinon supprime
	s = keep_inner_apostrophes_only(std::move(s));

	// IMPORTANT: recollapse après suppression d'apostrophes isolées
	s = sanitize_all(s, /*replace_dots=*/false);

	// Première lettre majuscule, reste minuscule
	s = phrase_case(std::move(s));

	return s;
}

static fs::path make_unique_target(const fs::path& desired) {
	if (!fs::exists(desired)) return desired;

	fs::path parent = desired.parent_path();

	// Insère un suffixe avant extension si présent
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

	// ex: "Nom.__tmp__.mkv"
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

	// ex: "Nom.__tmp__.mkv"
	std::string base = stem + ".__tmp__" + ext;
	fs::path cand = parent / base;
	if (!fs::exists(cand, ec) && !ec) return cand;

	for (int i = 1; i < 100000; ++i) {
		fs::path c = parent / (stem + ".__tmp__ (" + std::to_string(i) + ")" + ext);
		if (!fs::exists(c, ec) && !ec) return c;
	}
#endif

	// fallback
	return parent / fs::path("__tmp__");
}

static void rename_entry(const fs::path& p, Stats& st) {
	std::error_code ec;

	// Ne jamais renommer l'exécutable en cours
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
		// fallback simple (au cas où equivalent échoue)
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
		// Dossier : on remplace tous les '.' aussi
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
		// Fichier : on remplace tout dans le stem, extension inchangée (dernier '.')
		fs::path stem = p.stem();      // ex: "archive.tar"
		fs::path ext = p.extension(); // ex: ".gz"
#ifdef _WIN32
		std::wstring newStem = sanitize_and_clean(stem.wstring(), /*replace_dots=*/true);
		desired = p.parent_path() / fs::path(newStem + ext.wstring());
#else
		std::string newStem = sanitize_and_clean(stem.string(), /*replace_dots=*/true);
		desired = p.parent_path() / fs::path(newStem + ext.string());
#endif
	}

	if (desired == p) { st.skipped++; return; }

	// Si "desired" existe mais correspond au même fichier (cas Windows: changement de casse),
	// on fait un rename en 2 étapes pour éviter le "(1)".
	std::error_code ecExist;
	bool existsDesired = fs::exists(desired, ecExist) && !ecExist;

	bool sameFile = false;
	if (existsDesired) {
		std::error_code ecEq;
		sameFile = fs::equivalent(p, desired, ecEq) && !ecEq;
	}

	fs::path target;

	if (sameFile) {
		// rename en 2 temps: p -> temp -> desired (permet de changer la casse sans suffixe)
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
			// tentative de rollback
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

	// Cas normal: collision réelle éventuelle => suffixe si besoin
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

// Post-order : renomme le contenu puis le dossier (évite de casser le parcours)
static void process_dir(const fs::path& dir, Stats& st, bool is_root = false) {
	std::error_code ec;

	std::vector<fs::path> children;
	for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
		it != end && !ec; it.increment(ec)) {
		children.push_back(it->path());
	}
	if (ec) {
		std::cout << "[ERR] Lecture dossier: " << dir.string()
			<< " | " << ec.message() << "\n";
		st.errors++;
		return;
	}

	for (const auto& child : children) {
		std::error_code ec2;
		bool is_dir = fs::is_directory(child, ec2);
		bool is_symlink = fs::is_symlink(child, ec2);

		if (!ec2 && is_dir && !is_symlink) {
			process_dir(child, st, false); // renomme aussi ce dossier à la fin
		}
		else {
			rename_entry(child, st);
		}
	}

	// Par sécurité, on ne renomme pas la racine
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
			std::cout << "Usage: " << argv[0] << " [chemin]\n";
			return 0;
		}
		root = fs::path(a);
	}

	if (!fs::exists(root) || !fs::is_directory(root)) {
		std::cerr << "Chemin invalide: " << root.string() << "\n";
		return 1;
	}

	std::cout << "Racine: " << root.string() << "\n\n";

	Stats st;
	process_dir(root, st, true);

	std::cout << "\n--- Resume ---\n"
		<< "Renommes : " << st.renamed << "\n"
		<< "Ignores  : " << st.skipped << "\n"
		<< "Erreurs  : " << st.errors << "\n\n";

	// Timer 5 secondes (compte à rebours)
	for (int i = 5; i >= 1; --i) {
		std::cout << "Fin dans " << i << "...\n";
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}

	std::cout << "All done. Total: " << st.renamed << "\n";
	return st.errors ? 2 : 0;
}