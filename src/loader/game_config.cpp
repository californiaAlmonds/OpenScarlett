#include "openscarlett/loader/game_config.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

#if defined(_WIN32)
#  include <windows.h>
#endif

namespace openscarlett {

namespace {

bool IsXmlSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool IsXmlNameStart(char c) noexcept {
    const unsigned char uc = static_cast<unsigned char>(c);
    return std::isalpha(uc) != 0 || c == '_' || c == ':';
}

bool IsXmlNameChar(char c) noexcept {
    const unsigned char uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) != 0 || c == '_' || c == '-' || c == '.' || c == ':';
}

std::string ToLowerAscii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

std::string TrimAscii(std::string_view s) {
    size_t start = 0;
    while (start < s.size() && IsXmlSpace(s[start])) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && IsXmlSpace(s[end - 1])) {
        --end;
    }
    return std::string(s.substr(start, end - start));
}

bool AppendUtf8CodePoint(uint32_t cp, std::string& out) {
    if (cp == 0 || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
        return false;
    }
    if (cp <= 0x7Fu) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
    return true;
}

Status UnescapeXmlEntities(std::string_view raw, std::string& out) {
    out.clear();
    out.reserve(raw.size());
    size_t i = 0;
    while (i < raw.size()) {
        if (raw[i] != '&') {
            out.push_back(raw[i++]);
            continue;
        }
        const size_t semi = raw.find(';', i + 1);
        if (semi == std::string_view::npos) {
            return Status::Error(StatusCode::ParseError, "Unterminated XML entity reference");
        }
        const std::string_view ent = raw.substr(i + 1, semi - (i + 1));
        if (ent == "amp") {
            out.push_back('&');
        } else if (ent == "lt") {
            out.push_back('<');
        } else if (ent == "gt") {
            out.push_back('>');
        } else if (ent == "quot") {
            out.push_back('"');
        } else if (ent == "apos") {
            out.push_back('\'');
        } else if (ent.size() >= 2 && ent[0] == '#') {
            uint32_t cp = 0;
            bool hex = false;
            size_t digit_start = 1;
            if (ent[1] == 'x' || ent[1] == 'X') {
                hex = true;
                digit_start = 2;
            }
            if (digit_start >= ent.size()) {
                return Status::Error(StatusCode::ParseError, "Empty XML numeric character reference");
            }
            for (size_t d = digit_start; d < ent.size(); ++d) {
                char c = ent[d];
                uint32_t val = 0;
                if (c >= '0' && c <= '9') {
                    val = static_cast<uint32_t>(c - '0');
                } else if (hex && c >= 'a' && c <= 'f') {
                    val = static_cast<uint32_t>(10 + (c - 'a'));
                } else if (hex && c >= 'A' && c <= 'F') {
                    val = static_cast<uint32_t>(10 + (c - 'A'));
                } else {
                    return Status::Error(StatusCode::ParseError, "Invalid XML numeric character reference");
                }
                const uint32_t base = hex ? 16u : 10u;
                if (cp > (0x10FFFFu - val) / base) {
                    return Status::Error(StatusCode::ParseError, "XML numeric character reference overflow");
                }
                cp = cp * base + val;
            }
            if (!AppendUtf8CodePoint(cp, out)) {
                return Status::Error(StatusCode::ParseError, "Invalid Unicode code point in XML entity");
            }
        } else {
            return Status::Error(StatusCode::ParseError,
                                 "Unsupported XML entity: &" + std::string(ent) + ";");
        }
        i = semi + 1;
    }
    return Status::Ok();
}

struct XmlNode {
    std::string tag;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::string text;
    std::vector<XmlNode> children;

    const std::string* FindAttr(std::string_view name) const noexcept {
        for (const auto& kv : attrs) {
            if (EqualsIgnoreCase(kv.first, name)) {
                return &kv.second;
            }
        }
        return nullptr;
    }

    const XmlNode* FindChild(std::string_view name) const noexcept {
        for (const auto& child : children) {
            if (EqualsIgnoreCase(child.tag, name)) {
                return &child;
            }
        }
        return nullptr;
    }
};

class XmlParser {
public:
    explicit XmlParser(std::string_view input) : src_(input) {}

    Status ParseDocument(XmlNode& out_root) {
        // Strip UTF-8 BOM if present.
        if (src_.size() >= 3 &&
            static_cast<unsigned char>(src_[0]) == 0xEFu &&
            static_cast<unsigned char>(src_[1]) == 0xBBu &&
            static_cast<unsigned char>(src_[2]) == 0xBFu) {
            pos_ = 3;
        }

        auto st = SkipProlog();
        if (!st.ok()) {
            return st;
        }
        if (pos_ >= src_.size() || src_[pos_] != '<') {
            return Status::Error(StatusCode::ParseError, "Expected XML root element");
        }
        st = ParseElement(out_root, 0);
        if (!st.ok()) {
            return st;
        }
        st = SkipProlog();
        if (!st.ok()) {
            return st;
        }
        if (pos_ < src_.size()) {
            return Status::Error(StatusCode::ParseError, "Unexpected trailing content after XML root");
        }
        return Status::Ok();
    }

private:
    static constexpr int kMaxDepth = 64;

    bool StartsWith(std::string_view prefix) const noexcept {
        return src_.size() - pos_ >= prefix.size() &&
               src_.compare(pos_, prefix.size(), prefix) == 0;
    }

    void SkipWhitespace() noexcept {
        while (pos_ < src_.size() && IsXmlSpace(src_[pos_])) {
            ++pos_;
        }
    }

    Status SkipComment() {
        pos_ += 4;  // skip "<!--"
        const size_t end = src_.find("-->", pos_);
        if (end == std::string_view::npos) {
            return Status::Error(StatusCode::ParseError, "Unterminated XML comment");
        }
        pos_ = end + 3;
        return Status::Ok();
    }

    Status SkipProcessingInstruction() {
        pos_ += 2;  // skip "<?"
        const size_t end = src_.find("?>", pos_);
        if (end == std::string_view::npos) {
            return Status::Error(StatusCode::ParseError, "Unterminated XML processing instruction");
        }
        pos_ = end + 2;
        return Status::Ok();
    }

    Status SkipDoctype() {
        pos_ += 2;  // skip "<!"
        int bracket_depth = 0;
        while (pos_ < src_.size()) {
            char c = src_[pos_++];
            if (c == '[') {
                ++bracket_depth;
            } else if (c == ']' && bracket_depth > 0) {
                --bracket_depth;
            } else if (c == '>' && bracket_depth == 0) {
                return Status::Ok();
            }
        }
        return Status::Error(StatusCode::ParseError, "Unterminated XML DOCTYPE declaration");
    }

    Status SkipProlog() {
        for (;;) {
            SkipWhitespace();
            if (StartsWith("<!--")) {
                auto st = SkipComment();
                if (!st.ok()) return st;
            } else if (StartsWith("<?")) {
                auto st = SkipProcessingInstruction();
                if (!st.ok()) return st;
            } else if (StartsWith("<!DOCTYPE") || StartsWith("<!doctype")) {
                auto st = SkipDoctype();
                if (!st.ok()) return st;
            } else {
                break;
            }
        }
        return Status::Ok();
    }

    Status ParseName(std::string& out_name) {
        if (pos_ >= src_.size() || !IsXmlNameStart(src_[pos_])) {
            return Status::Error(StatusCode::ParseError, "Expected XML identifier");
        }
        const size_t start = pos_;
        ++pos_;
        while (pos_ < src_.size() && IsXmlNameChar(src_[pos_])) {
            ++pos_;
        }
        out_name.assign(src_.substr(start, pos_ - start));
        return Status::Ok();
    }

    Status ParseElement(XmlNode& node, int depth) {
        if (depth > kMaxDepth) {
            return Status::Error(StatusCode::ParseError, "XML nesting depth limit exceeded");
        }
        if (pos_ >= src_.size() || src_[pos_] != '<') {
            return Status::Error(StatusCode::ParseError, "Expected '<' to start XML element");
        }
        ++pos_;

        auto st = ParseName(node.tag);
        if (!st.ok()) {
            return st;
        }

        // Parse attributes until '>' or '/>'
        for (;;) {
            SkipWhitespace();
            if (pos_ >= src_.size()) {
                return Status::Error(StatusCode::ParseError,
                                     "Unexpected EOF inside tag <" + node.tag + ">");
            }
            if (StartsWith("/>")) {
                pos_ += 2;
                return Status::Ok();
            }
            if (src_[pos_] == '>') {
                ++pos_;
                break;
            }

            std::string attr_name;
            st = ParseName(attr_name);
            if (!st.ok()) {
                return st;
            }
            SkipWhitespace();
            if (pos_ >= src_.size() || src_[pos_] != '=') {
                return Status::Error(StatusCode::ParseError,
                                     "Expected '=' after attribute '" + attr_name + "'");
            }
            ++pos_;
            SkipWhitespace();
            if (pos_ >= src_.size() || (src_[pos_] != '"' && src_[pos_] != '\'')) {
                return Status::Error(StatusCode::ParseError,
                                     "Expected quoted value for attribute '" + attr_name + "'");
            }
            const char quote = src_[pos_++];
            const size_t val_start = pos_;
            const size_t val_end = src_.find(quote, val_start);
            if (val_end == std::string_view::npos) {
                return Status::Error(StatusCode::ParseError,
                                     "Unterminated quoted attribute '" + attr_name + "'");
            }
            std::string attr_val;
            st = UnescapeXmlEntities(src_.substr(val_start, val_end - val_start), attr_val);
            if (!st.ok()) {
                return st;
            }
            pos_ = val_end + 1;
            node.attrs.emplace_back(std::move(attr_name), std::move(attr_val));
        }

        // Parse element body until matching </tag>
        for (;;) {
            if (pos_ >= src_.size()) {
                return Status::Error(StatusCode::ParseError,
                                     "Unclosed XML element <" + node.tag + ">");
            }
            if (StartsWith("</")) {
                pos_ += 2;
                SkipWhitespace();
                std::string close_tag;
                st = ParseName(close_tag);
                if (!st.ok()) {
                    return st;
                }
                SkipWhitespace();
                if (pos_ >= src_.size() || src_[pos_] != '>') {
                    return Status::Error(StatusCode::ParseError,
                                         "Expected '>' to close </" + close_tag + ">");
                }
                ++pos_;
                if (close_tag != node.tag) {
                    return Status::Error(StatusCode::ParseError,
                                         "Mismatched closing tag </" + close_tag + "> for <" + node.tag + ">");
                }
                return Status::Ok();
            }
            if (StartsWith("<!--")) {
                st = SkipComment();
                if (!st.ok()) return st;
                continue;
            }
            if (StartsWith("<?")) {
                st = SkipProcessingInstruction();
                if (!st.ok()) return st;
                continue;
            }
            if (StartsWith("<![CDATA[")) {
                pos_ += 9;
                const size_t end = src_.find("]]>", pos_);
                if (end == std::string_view::npos) {
                    return Status::Error(StatusCode::ParseError, "Unterminated CDATA section");
                }
                node.text.append(src_.substr(pos_, end - pos_));
                pos_ = end + 3;
                continue;
            }
            if (src_[pos_] == '<') {
                XmlNode child;
                st = ParseElement(child, depth + 1);
                if (!st.ok()) return st;
                node.children.push_back(std::move(child));
                continue;
            }

            // Text content until next '<'
            const size_t text_start = pos_;
            const size_t next_lt = src_.find('<', text_start);
            const size_t text_end = (next_lt == std::string_view::npos) ? src_.size() : next_lt;
            std::string unescaped;
            st = UnescapeXmlEntities(src_.substr(text_start, text_end - text_start), unescaped);
            if (!st.ok()) {
                return st;
            }
            node.text.append(unescaped);
            pos_ = text_end;
        }
    }

    std::string_view src_;
    size_t pos_ = 0;
};

bool ParseHexTitleId(std::string_view raw, uint32_t& out_value) noexcept {
    if (raw.empty()) {
        return false;
    }
    if (raw.size() >= 2 && raw[0] == '0' && (raw[1] == 'x' || raw[1] == 'X')) {
        raw.remove_prefix(2);
    }
    if (raw.empty() || raw.size() > 8) {
        return false;
    }
    uint32_t acc = 0;
    for (char c : raw) {
        uint32_t digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<uint32_t>(10 + (c - 'a'));
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<uint32_t>(10 + (c - 'A'));
        } else {
            return false;
        }
        acc = (acc << 4) | digit;
    }
    out_value = acc;
    return true;
}

bool ParseXmlBool(std::string_view s) noexcept {
    return EqualsIgnoreCase(s, "true") || s == "1";
}

bool ParseXmlUint64(std::string_view raw, uint64_t& out_value) noexcept {
    const std::string trimmed = TrimAscii(raw);
    if (trimmed.empty()) {
        return false;
    }
    uint64_t acc = 0;
    for (char c : trimmed) {
        if (c < '0' || c > '9') {
            return false;
        }
        const uint64_t digit = static_cast<uint64_t>(c - '0');
        if (acc > (UINT64_MAX - digit) / 10ull) {
            return false;
        }
        acc = acc * 10ull + digit;
    }
    out_value = acc;
    return true;
}

std::string ReadEnvStringA(const char* name) {
#if defined(_WIN32)
    DWORD needed = ::GetEnvironmentVariableA(name, nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::string buf(needed, '\0');
    DWORD written = ::GetEnvironmentVariableA(name, buf.data(), needed);
    if (written == 0 || written >= needed) {
        return {};
    }
    buf.resize(written);
    return buf;
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string{};
#endif
}

bool FindConfigInDirectory(const std::filesystem::path& dir, std::filesystem::path& out_config) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return false;
    }
    std::filesystem::path fallback_mgc;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const std::string fname = entry.path().filename().string();
        if (EqualsIgnoreCase(fname, "MicrosoftGame.Config")) {
            out_config = entry.path();
            return true;
        }
        if (fallback_mgc.empty() && EqualsIgnoreCase(fname, "MicrosoftGameConfig.mgc")) {
            fallback_mgc = entry.path();
        }
    }
    if (!fallback_mgc.empty()) {
        out_config = std::move(fallback_mgc);
        return true;
    }
    return false;
}

bool IsConfigFilePath(const std::filesystem::path& p) {
    const std::string fname = p.filename().string();
    const std::string ext = p.extension().string();
    return EqualsIgnoreCase(fname, "MicrosoftGame.Config") ||
           EqualsIgnoreCase(ext, ".config") ||
           EqualsIgnoreCase(ext, ".mgc");
}

}  // namespace

const GameExecutableEntry* GameConfig::SelectPrimaryExecutable() const noexcept {
    if (executables.empty()) {
        return nullptr;
    }

    auto score_entry = [](const GameExecutableEntry& e) -> int {
        if (e.name.empty()) {
            return 100;
        }
        const std::string tdf = ToLowerAscii(e.target_device_family);
        const bool is_scarlett = (tdf.find("scarlett") != std::string::npos);
        const bool is_xbox = (!is_scarlett && tdf.find("xbox") != std::string::npos);
        const int base = e.is_dev_only ? 10 : 0;
        if (is_scarlett) return base + 0;
        if (is_xbox) return base + 1;
        return base + 2;
    };

    const GameExecutableEntry* best = &executables.front();
    int best_score = score_entry(*best);
    for (size_t i = 1; i < executables.size(); ++i) {
        const int sc = score_entry(executables[i]);
        if (sc < best_score) {
            best = &executables[i];
            best_score = sc;
        }
    }
    return best;
}

Status DecodeXmlEntities(std::string_view raw, std::string& out) {
    return UnescapeXmlEntities(raw, out);
}

Status ParseGameConfigXml(std::string_view xml_text,
                          const std::string& label,
                          GameConfig& out_config) {
    out_config = {};
    out_config.source_path = label;

    XmlNode root;
    XmlParser parser(xml_text);
    auto st = parser.ParseDocument(root);
    if (!st.ok()) {
        return st;
    }

    if (!EqualsIgnoreCase(root.tag, "Game")) {
        return Status::Error(StatusCode::ParseError,
                             "Expected <Game> root element, found <" + root.tag + ">");
    }

    if (const auto* ver = root.FindAttr("configVersion")) {
        out_config.config_version = *ver;
    }

    if (const auto* id_node = root.FindChild("Identity")) {
        if (const auto* v = id_node->FindAttr("Name")) out_config.identity.name = *v;
        if (const auto* v = id_node->FindAttr("Publisher")) out_config.identity.publisher = *v;
        if (const auto* v = id_node->FindAttr("Version")) out_config.identity.version = *v;
    }

    if (const auto* tid_node = root.FindChild("TitleId")) {
        out_config.title_id = TrimAscii(tid_node->text);
        if (!out_config.title_id.empty()) {
            uint32_t parsed = 0;
            if (!ParseHexTitleId(out_config.title_id, parsed)) {
                return Status::Error(StatusCode::ParseError,
                                     "Invalid hex <TitleId>: " + out_config.title_id);
            }
            out_config.title_id_value = parsed;
            out_config.has_title_id = true;
        }
    }

    if (const auto* msa_node = root.FindChild("MSAAppId")) {
        out_config.msa_app_id = TrimAscii(msa_node->text);
    }

    if (const auto* store_node = root.FindChild("StoreId")) {
        out_config.store_id = TrimAscii(store_node->text);
    }

    if (const auto* visuals = root.FindChild("ShellVisuals")) {
        if (const auto* v = visuals->FindAttr("DefaultDisplayName")) {
            out_config.display_name = *v;
        } else if (const auto* v_alt = visuals->FindAttr("DisplayName")) {
            out_config.display_name = *v_alt;
        }
    }

    if (const auto* pls_node = root.FindChild("PersistentLocalStorage")) {
        out_config.persistent_local_storage.has_config = true;
        if (const auto* sz_child = pls_node->FindChild("SizeMB")) {
            uint64_t val = 0;
            if (ParseXmlUint64(sz_child->text, val)) {
                out_config.persistent_local_storage.size_mb = val;
            }
        } else if (const auto* sz_attr = pls_node->FindAttr("SizeMB")) {
            uint64_t val = 0;
            if (ParseXmlUint64(*sz_attr, val)) {
                out_config.persistent_local_storage.size_mb = val;
            }
        }
        if (const auto* grow_child = pls_node->FindChild("GrowableToMB")) {
            uint64_t val = 0;
            if (ParseXmlUint64(grow_child->text, val)) {
                out_config.persistent_local_storage.growable_to_mb = val;
                out_config.persistent_local_storage.has_growable_to_mb = true;
            }
        } else if (const auto* grow_attr = pls_node->FindAttr("GrowableToMB")) {
            uint64_t val = 0;
            if (ParseXmlUint64(*grow_attr, val)) {
                out_config.persistent_local_storage.growable_to_mb = val;
                out_config.persistent_local_storage.has_growable_to_mb = true;
            }
        }
    }

    if (const auto* vm_node = root.FindChild("VirtualMachine")) {
        out_config.virtual_machine.has_config = true;
        if (const auto* v = vm_node->FindAttr("SMTEnabled")) {
            out_config.virtual_machine.smt_enabled = ParseXmlBool(TrimAscii(*v));
            out_config.virtual_machine.has_smt_enabled = true;
        } else if (const auto* c = vm_node->FindChild("SMTEnabled")) {
            out_config.virtual_machine.smt_enabled = ParseXmlBool(TrimAscii(c->text));
            out_config.virtual_machine.has_smt_enabled = true;
        }
        if (const auto* v = vm_node->FindAttr("XboxLockhartTitleMemory")) {
            uint64_t val = 0;
            if (ParseXmlUint64(*v, val)) {
                out_config.virtual_machine.xbox_lockhart_title_memory = val;
                out_config.virtual_machine.has_lockhart_title_memory = true;
            }
        } else if (const auto* c = vm_node->FindChild("XboxLockhartTitleMemory")) {
            uint64_t val = 0;
            if (ParseXmlUint64(c->text, val)) {
                out_config.virtual_machine.xbox_lockhart_title_memory = val;
                out_config.virtual_machine.has_lockhart_title_memory = true;
            }
        }
        if (const auto* v = vm_node->FindAttr("XboxAnacondaTitleMemory")) {
            uint64_t val = 0;
            if (ParseXmlUint64(*v, val)) {
                out_config.virtual_machine.xbox_anaconda_title_memory = val;
                out_config.virtual_machine.has_anaconda_title_memory = true;
            }
        } else if (const auto* c = vm_node->FindChild("XboxAnacondaTitleMemory")) {
            uint64_t val = 0;
            if (ParseXmlUint64(c->text, val)) {
                out_config.virtual_machine.xbox_anaconda_title_memory = val;
                out_config.virtual_machine.has_anaconda_title_memory = true;
            }
        }
    }

    if (const auto* res_node = root.FindChild("Resources")) {
        for (const auto& child : res_node->children) {
            if (!EqualsIgnoreCase(child.tag, "Resource")) {
                continue;
            }
            if (const auto* lang = child.FindAttr("Language")) {
                const std::string trimmed = TrimAscii(*lang);
                if (!trimmed.empty()) {
                    out_config.resources.push_back(trimmed);
                }
            }
        }
    }

    if (const auto* exe_list = root.FindChild("ExecutableList")) {
        for (const auto& child : exe_list->children) {
            if (!EqualsIgnoreCase(child.tag, "Executable")) {
                continue;
            }
            GameExecutableEntry entry{};
            if (const auto* v = child.FindAttr("Name")) entry.name = *v;
            if (const auto* v = child.FindAttr("Id")) entry.id = *v;
            if (const auto* v = child.FindAttr("TargetDeviceFamily")) {
                entry.target_device_family = *v;
            }
            if (const auto* v = child.FindAttr("OverrideDisplayName")) {
                entry.override_display_name = *v;
            }
            if (const auto* v = child.FindAttr("IsDevOnly")) {
                entry.is_dev_only = ParseXmlBool(*v);
            }
            out_config.executables.push_back(std::move(entry));
        }
    }

    return Status::Ok();
}

Status LoadGameConfigFile(const std::string& config_path, GameConfig& out_config) {
    std::ifstream ifs(config_path, std::ios::binary);
    if (!ifs) {
        return Status::Error(StatusCode::NotFound,
                             "Cannot open MicrosoftGame.Config: " + config_path);
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return ParseGameConfigXml(oss.str(), config_path, out_config);
}

Status ResolvePackageLaunchTarget(const std::string& input_path,
                                  ResolvedPackageTarget& out_target) {
    out_target = {};
    if (input_path.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "Launch target path is empty");
    }

    std::error_code ec;
    const std::filesystem::path abs_in =
        std::filesystem::absolute(std::filesystem::path(input_path), ec).lexically_normal();
    if (ec || !std::filesystem::exists(abs_in, ec)) {
        return Status::Error(StatusCode::NotFound, "Launch target not found: " + input_path);
    }

    auto resolve_from_config = [&](const std::filesystem::path& cfg_path,
                                   const std::filesystem::path& pkg_root) -> Status {
        auto st = LoadGameConfigFile(cfg_path.string(), out_target.config);
        if (!st.ok()) {
            return st;
        }
        const GameExecutableEntry* primary = out_target.config.SelectPrimaryExecutable();
        if (!primary || primary->name.empty()) {
            return Status::Error(StatusCode::ParseError,
                                 "MicrosoftGame.Config has no usable <Executable> entry: " +
                                     cfg_path.string());
        }
        const std::filesystem::path resolved_exe = (pkg_root / primary->name).lexically_normal();
        if (!std::filesystem::is_regular_file(resolved_exe, ec)) {
            return Status::Error(StatusCode::NotFound,
                                 "Executable declared in MicrosoftGame.Config not found: " +
                                     resolved_exe.string());
        }
        out_target.exe_path = resolved_exe.string();
        out_target.package_root = pkg_root.string();
        out_target.config_path = cfg_path.string();
        out_target.has_config = true;
        return Status::Ok();
    };

    if (std::filesystem::is_directory(abs_in, ec)) {
        std::filesystem::path cfg_path;
        if (!FindConfigInDirectory(abs_in, cfg_path)) {
            return Status::Error(StatusCode::NotFound,
                                 "No MicrosoftGame.Config found in directory: " + abs_in.string());
        }
        return resolve_from_config(cfg_path, abs_in);
    }

    if (IsConfigFilePath(abs_in)) {
        return resolve_from_config(abs_in, abs_in.parent_path());
    }

    if (!std::filesystem::is_regular_file(abs_in, ec)) {
        return Status::Error(StatusCode::InvalidArgument,
                             "Launch target is neither a file nor a package directory: " +
                                 abs_in.string());
    }

    // Direct .exe path: auto-detect sibling MicrosoftGame.Config if present.
    out_target.exe_path = abs_in.string();
    out_target.package_root = abs_in.parent_path().string();

    std::filesystem::path sibling_cfg;
    if (FindConfigInDirectory(abs_in.parent_path(), sibling_cfg)) {
        GameConfig cfg;
        auto st = LoadGameConfigFile(sibling_cfg.string(), cfg);
        if (st.ok()) {
            out_target.config = std::move(cfg);
            out_target.config_path = sibling_cfg.string();
            out_target.has_config = true;
        }
    }
    return Status::Ok();
}

std::string FormatTitleIdFolderName(const GameConfig& config) {
    if (config.has_title_id) {
        char buf[16]{};
        std::snprintf(buf, sizeof(buf), "%08X", config.title_id_value);
        return std::string(buf);
    }
    auto sanitize = [](std::string_view raw) -> std::string {
        std::string out;
        out.reserve(raw.size());
        for (char c : raw) {
            const unsigned char uc = static_cast<unsigned char>(c);
            if (std::isalnum(uc) != 0 || c == '.' || c == '-' || c == '_') {
                out.push_back(c);
            } else {
                out.push_back('_');
            }
        }
        while (!out.empty() && (out.back() == '.' || out.back() == ' ')) {
            out.pop_back();
        }
        return out;
    };
    if (!config.title_id.empty()) {
        return sanitize(config.title_id);
    }
    if (!config.identity.name.empty()) {
        return sanitize(config.identity.name);
    }
    return {};
}

std::string ResolvePreferredUserLocale(const GameConfig* config) {
    const std::string env_locale = TrimAscii(ReadEnvStringA("OPENSCARLETT_LOCALE"));
    if (!env_locale.empty()) {
        return env_locale;
    }
    if (config && !config->resources.empty() && !config->resources.front().empty()) {
        return config->resources.front();
    }
    return "en-US";
}

}  // namespace openscarlett
