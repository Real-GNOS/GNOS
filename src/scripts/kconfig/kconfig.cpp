/*
 * kconfig.cpp — Kconfig parser + .config I/O.  C++20.
 */
#include "kconfig.h"
#include <algorithm>
#include <cctype>
#include <cstring>

namespace gnoscfg {

/* ------------------------------------------------------------------ */
/* OptValue                                                            */
/* ------------------------------------------------------------------ */

int OptValue::as_int() const {
    if (raw.starts_with("0x") || raw.starts_with("0X"))
        return (int)std::stoul(raw, nullptr, 16);
    return std::stoi(raw);
}

std::string OptValue::display() const {
    switch (type) {
    case OptType::Bool:
    case OptType::Tristate:
        if (raw == "y") return "<*>";
        if (raw == "m") return "<M>";
        return "< >";
    case OptType::Int:
    case OptType::Hex:
    case OptType::String:
        return "[" + raw + "]";
    }
    return raw;
}

void OptValue::toggle() {
    if (type == OptType::Bool) {
        raw = (raw == "y") ? "n" : "y";
    } else if (type == OptType::Tristate) {
        if      (raw == "y") raw = "m";
        else if (raw == "m") raw = "n";
        else                 raw = "y";
    }
    touched = true;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static std::string trim(std::string_view sv) {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t'))
        sv.remove_prefix(1);
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' ||
                           sv.back() == '\n' || sv.back() == '\r'))
        sv.remove_suffix(1);
    return std::string(sv);
}

static OptType parse_type(std::string_view s) {
    if (s == "bool")     return OptType::Bool;
    if (s == "tristate") return OptType::Tristate;
    if (s == "int")      return OptType::Int;
    if (s == "hex")      return OptType::Hex;
    if (s == "string")   return OptType::String;
    return OptType::Bool;
}

/* Extract a quoted string from a line. */
static std::string extract_quoted(std::string_view line) {
    auto q1 = line.find('"');
    if (q1 == std::string::npos) return "";
    auto q2 = line.rfind('"');
    if (q2 <= q1) return "";
    return std::string(line.substr(q1 + 1, q2 - q1 - 1));
}

/* ------------------------------------------------------------------ */
/* KconfigParser                                                       */
/* ------------------------------------------------------------------ */

ConfigEntry *KconfigParser::ensure(std::string_view name) {
    std::string key(name);
    auto it = m_entries.find(key);
    if (it != m_entries.end()) return it->second.get();

    auto e = std::make_unique<ConfigEntry>();
    e->name = key;
    ConfigEntry *p = e.get();
    m_entries[key] = std::move(e);
    return p;
}

std::unique_ptr<MenuNode> KconfigParser::parse(std::string_view path) {
    FILE *fp = fopen(std::string(path).c_str(), "r");
    if (!fp) return nullptr;

    auto root = std::make_unique<MenuNode>();
    root->kind = MenuNode::Submenu;
    root->title = "(root)";

    std::vector<std::string> dep_stack;
    parse_block(fp, root.get(), dep_stack);
    fclose(fp);

    /* Apply defaults */
    for (auto &[name, entry] : m_entries) {
        if (!entry->current.touched) {
            entry->current.type = entry->type;
            entry->current.raw  = entry->defval;
        }
    }
    return root;
}

void KconfigParser::parse_block(FILE *fp, MenuNode *parent,
                                 std::vector<std::string> &dep_stack) {
    char line[4096];
    ConfigEntry *cur_entry = nullptr;
    bool in_help = false;
    std::string pending_help;

    auto finish_help = [&]() {
        if (cur_entry && !pending_help.empty()) {
            while (!pending_help.empty() && pending_help.front() == '\n')
                pending_help.erase(pending_help.begin());
            while (!pending_help.empty() && pending_help.back() == '\n')
                pending_help.pop_back();
            cur_entry->help = pending_help;
            pending_help.clear();
        }
        cur_entry = nullptr;
        in_help = false;
    };

    while (fgets(line, sizeof(line), fp)) {
        std::string s = trim(line);

        /* blank */
        if (s.empty()) { if (in_help && cur_entry) pending_help += "\n"; continue; }

        /* comment */
        if (s[0] == '#') {
            if (in_help && cur_entry) pending_help += line;
            continue;
        }

        /* Indented help text */
        if (in_help && cur_entry) {
            if (line[0] == ' ' || line[0] == '\t') {
                pending_help += line;
                continue;
            }
            finish_help();
        }

        /* menu "..." */
        if (s.starts_with("menu ")) {
            finish_help();
            auto node = std::make_unique<MenuNode>();
            node->kind  = MenuNode::Submenu;
            node->title = extract_quoted(s);
            node->parent = parent;
            parent->children.push_back(std::move(node));

            dep_stack.push_back("");
            parse_block(fp, parent->children.back().get(), dep_stack);
            if (!dep_stack.empty()) dep_stack.pop_back();
            continue;
        }

        if (s == "endmenu") { finish_help(); return; }

        /* comment "..." */
        if (s.starts_with("comment ")) {
            finish_help();
            auto node = std::make_unique<MenuNode>();
            node->kind = MenuNode::Comment;
            node->comment_text = extract_quoted(s);
            node->parent = parent;
            parent->children.push_back(std::move(node));
            continue;
        }

        /* config NAME */
        if (s.starts_with("config ")) {
            finish_help();
            std::string name = s.substr(7);
            while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
                name.pop_back();

            auto *entry = ensure(name);
            for (auto &d : dep_stack)
                if (!d.empty()) entry->depends.push_back(d);

            auto node = std::make_unique<MenuNode>();
            node->kind = MenuNode::Config;
            node->config_name = name;
            node->entry = entry;
            node->parent = parent;
            parent->children.push_back(std::move(node));
            cur_entry = entry;
            continue;
        }

        /* Keywords inside a config block */
        if (cur_entry && !in_help) {
            auto sp = s.find(' ');
            std::string kw = (sp != std::string::npos) ? s.substr(0, sp) : s;

            if (kw == "bool" || kw == "tristate" || kw == "int" ||
                kw == "hex" || kw == "string") {
                cur_entry->type = parse_type(kw);
                cur_entry->current.type = cur_entry->type;
                std::string prompt = extract_quoted(s);
                if (!prompt.empty()) cur_entry->prompt = prompt;
                continue;
            }
            if (kw == "default") {
                std::string val = extract_quoted(s);
                if (val.empty()) {
                    auto eq = s.find(' ');
                    if (eq != std::string::npos) val = trim(s.substr(eq + 1));
                }
                cur_entry->defval = val;
                continue;
            }
            if (kw == "depends") {
                auto eq = s.find("on ");
                if (eq != std::string::npos) {
                    std::string dep = trim(s.substr(eq + 3));
                    cur_entry->depends.push_back(dep);
                    if (!dep_stack.empty()) dep_stack.back() = dep;
                }
                continue;
            }
            if (kw == "help" || kw == "---help---") {
                in_help = true;
                continue;
            }
        }
    }
    finish_help();
}

ConfigEntry *KconfigParser::find(std::string_view name) {
    auto it = m_entries.find(std::string(name));
    return (it != m_entries.end()) ? it->second.get() : nullptr;
}

/* ------------------------------------------------------------------ */
/* .config I/O                                                         */
/* ------------------------------------------------------------------ */

bool KconfigParser::load_config(std::string_view path) {
    FILE *fp = fopen(std::string(path).c_str(), "r");
    if (!fp) return false;

    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        std::string s = trim(line);
        if (s.empty() || s[0] == '#') continue;
        if (!s.starts_with("CONFIG_")) continue;

        auto eq = s.find('=');
        if (eq == std::string::npos) continue;

        std::string key = s.substr(7, eq - 7);
        std::string val = s.substr(eq + 1);

        auto *e = find(key);
        if (e) { e->current.raw = val; e->current.touched = true; }
    }
    fclose(fp);
    return true;
}

void KconfigParser::save_config(std::string_view path) const {
    FILE *fp = fopen(std::string(path).c_str(), "w");
    if (!fp) return;

    fprintf(fp, "#\n# GNOS 内核配置 — 由 gnoscfg (make config) 自动生成\n#\n");

    for (auto &[name, entry] : m_entries) {
        const char *desc = entry->prompt.empty() ? name.c_str()
                                                 : entry->prompt.c_str();
        fprintf(fp, "\n# %s\n", desc);
        fprintf(fp, "CONFIG_%s=%s\n", name.c_str(), entry->current.raw.c_str());
    }

    fclose(fp);
}

} /* namespace gnoscfg */
