///|/ Git-native profile sync for PrusaSlicer (fork feature).
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#include "ProfileSync.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <git2.h>
#include <git2/sys/filter.h>

#include <regex>
#include <map>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>

#include <wx/timer.h>
#include <wx/event.h>

#include <thread>
#include <atomic>
#include <sstream>
#include <iomanip>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <unistd.h>
#endif

namespace Slic3r {
namespace ProfileSync {

static const char *SECTION = "profile_sync";

// ---------------------------------------------------------------- Settings

static bool cfg_bool(const AppConfig &cfg, const char *key, bool def)
{
    std::string v = cfg.get(SECTION, key);
    return v.empty() ? def : v == "1";
}

Settings Settings::load(const AppConfig &cfg)
{
    Settings s;
    s.enabled        = cfg_bool(cfg, "enabled", false);
    s.remote_url     = cfg.get(SECTION, "remote_url");
    s.branch         = cfg.get(SECTION, "branch");
    if (s.branch.empty()) s.branch = "main";
    s.machine_name   = cfg.get(SECTION, "machine_name");
    if (s.machine_name.empty()) s.machine_name = default_machine_name();
    s.auto_commit    = cfg_bool(cfg, "auto_commit", true);
    s.auto_push      = cfg_bool(cfg, "auto_push", true);
    s.pull_on_launch = cfg_bool(cfg, "pull_on_launch", true);
    s.track_physical_printers = cfg_bool(cfg, "track_physical_printers", true);
    s.track_vendor   = cfg_bool(cfg, "track_vendor", true);
    return s;
}

void Settings::save(AppConfig &cfg) const
{
    cfg.set(SECTION, "enabled",        enabled ? "1" : "0");
    cfg.set(SECTION, "remote_url",     remote_url);
    cfg.set(SECTION, "branch",         branch);
    cfg.set(SECTION, "machine_name",   machine_name);
    cfg.set(SECTION, "auto_commit",    auto_commit ? "1" : "0");
    cfg.set(SECTION, "auto_push",      auto_push ? "1" : "0");
    cfg.set(SECTION, "pull_on_launch", pull_on_launch ? "1" : "0");
    cfg.set(SECTION, "track_physical_printers", track_physical_printers ? "1" : "0");
    cfg.set(SECTION, "track_vendor",   track_vendor ? "1" : "0");
}

std::string default_machine_name()
{
    char buf[256] = {0};
#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    gethostname(buf, sizeof(buf) - 1);
    WSACleanup();
#else
    gethostname(buf, sizeof(buf) - 1);
#endif
    std::string name(buf);
    if (auto dot = name.find('.'); dot != std::string::npos) name.erase(dot);
    return name.empty() ? std::string("prusaslicer") : name;
}

// ---------------------------------------------------------------- libgit2 helpers

namespace {

struct GitError : std::runtime_error { using std::runtime_error::runtime_error; };

[[noreturn]] void throw_git(const char *what)
{
    const git_error *e = git_error_last();
    std::string msg = std::string(what) + ": " + (e && e->message ? e->message : "unknown libgit2 error");
    throw GitError(msg);
}

inline void check(int rc, const char *what) { if (rc < 0) throw_git(what); }

template<typename T, void (*Free)(T*)>
struct Ptr {
    T *p = nullptr;
    Ptr() = default;
    ~Ptr() { if (p) Free(p); }
    Ptr(const Ptr&) = delete; Ptr& operator=(const Ptr&) = delete;
    Ptr(Ptr &&o) noexcept : p(o.p) { o.p = nullptr; }
    Ptr& operator=(Ptr &&o) noexcept { if (this != &o) { reset(); p = o.p; o.p = nullptr; } return *this; }
    T** operator&() { return &p; }
    operator T*() const { return p; }
    T* get() const { return p; }
    void reset() { if (p) Free(p); p = nullptr; }
};

using RepoPtr      = Ptr<git_repository, git_repository_free>;
using IndexPtr     = Ptr<git_index, git_index_free>;
using TreePtr      = Ptr<git_tree, git_tree_free>;
using CommitPtr    = Ptr<git_commit, git_commit_free>;
using SigPtr       = Ptr<git_signature, git_signature_free>;
using RefPtr       = Ptr<git_reference, git_reference_free>;
using RemotePtr    = Ptr<git_remote, git_remote_free>;
using AnnotPtr     = Ptr<git_annotated_commit, git_annotated_commit_free>;
using StatusPtr    = Ptr<git_status_list, git_status_list_free>;
using WalkPtr      = Ptr<git_revwalk, git_revwalk_free>;
using DiffPtr      = Ptr<git_diff, git_diff_free>;
using ObjPtr       = Ptr<git_object, git_object_free>;
using BlobPtr      = Ptr<git_blob, git_blob_free>;
using ConfigPtr    = Ptr<git_config, git_config_free>;
using ConflictItPtr = Ptr<git_index_conflict_iterator, git_index_conflict_iterator_free>;


// ---------------------------------------------------------------- local secrets
//
// physical_printer/*.ini hold print-host API keys/passwords in plain text. A
// libgit2 clean/smudge filter ("prusaslicer-secrets", enabled via a managed
// .gitattributes) strips those values on the way into the repository and puts
// them back on the way out, storing the local values in a gitignored sidecar
// file (profile_sync_secrets.ini). So the repo never contains a key, and each
// machine keeps its own.
const char *SECRETS_FILE = "profile_sync_secrets.ini";
const char *SECRET_FILTER_NAME = "prusaslicer-secrets";
const std::regex SECRET_LINE_RE("^(printhost_apikey|printhost_password)[ \\t]*=[ \\t]*(.*?)[ \\t]*\\r?$");

struct SecretStore
{
    std::string dir;
    std::mutex  mtx;
    // (repo-relative path, option key) -> value
    std::map<std::pair<std::string, std::string>, std::string> values;
    bool loaded = false;

    boost::filesystem::path file() const { return boost::filesystem::path(dir) / SECRETS_FILE; }

    void load_locked()
    {
        if (loaded) return;
        loaded = true;
        values.clear();
        boost::nowide::ifstream in(file().string());
        std::string line, section;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            if (line.front() == '[' && line.back() == ']') { section = line.substr(1, line.size() - 2); continue; }
            auto eq = line.find('=');
            if (eq == std::string::npos || section.empty()) continue;
            std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            boost::trim(k); boost::trim(v);
            values[{section, k}] = v;
        }
    }
    void save_locked()
    {
        boost::nowide::ofstream out(file().string(), std::ios::binary);
        out << "# Local copies of secrets that PrusaSlicer profile sync keeps out of the git repository.\n"
               "# This file is machine-local and gitignored. Do not share it.\n";
        std::string section;
        for (auto &kv : values) {
            if (kv.first.first != section) { section = kv.first.first; out << "\n[" << section << "]\n"; }
            out << kv.first.second << " = " << kv.second << "\n";
        }
    }
    void remember(const std::string &path, const std::string &key, const std::string &value)
    {
        std::lock_guard<std::mutex> l(mtx); load_locked();
        auto it = values.find({path, key});
        if (it != values.end() && it->second == value) return;
        values[{path, key}] = value;
        save_locked();
    }
    bool lookup(const std::string &path, const std::string &key, std::string &value)
    {
        std::lock_guard<std::mutex> l(mtx); load_locked();
        auto it = values.find({path, key});
        if (it == values.end()) return false;
        value = it->second; return true;
    }
};
std::unique_ptr<SecretStore> g_secrets;
std::mutex g_secrets_mtx;

SecretStore& secrets_for(const std::string &dir)
{
    std::lock_guard<std::mutex> l(g_secrets_mtx);
    if (!g_secrets || g_secrets->dir != dir) { g_secrets.reset(new SecretStore); g_secrets->dir = dir; }
    return *g_secrets;
}

// Worktree -> repository: strip secret values, remembering them locally.
std::string scrub_secrets(const std::string &dir, const std::string &rel_path, const std::string &content)
{
    SecretStore &store = secrets_for(dir);
    std::string out; out.reserve(content.size());
    std::istringstream in(content);
    std::string line;
    while (std::getline(in, line)) {
        bool crlf = !line.empty() && line.back() == '\r';
        std::smatch m;
        if (std::regex_match(line, m, SECRET_LINE_RE)) {
            std::string key = m[1], value = m[2];
            if (!value.empty()) store.remember(rel_path, key, value);
            line = key + " = " + (crlf ? "\r" : "");
        }
        out += line;
        if (!in.eof() || content.back() == '\n') out += '\n';
    }
    return out;
}

// Repository -> worktree: put the locally remembered values back.
std::string restore_secrets(const std::string &dir, const std::string &rel_path, const std::string &content)
{
    SecretStore &store = secrets_for(dir);
    std::string out; out.reserve(content.size() + 64);
    std::istringstream in(content);
    std::string line;
    while (std::getline(in, line)) {
        bool crlf = !line.empty() && line.back() == '\r';
        std::smatch m;
        if (std::regex_match(line, m, SECRET_LINE_RE) && std::string(m[2]).empty()) {
            std::string key = m[1], value;
            if (store.lookup(rel_path, key, value))
                line = key + " = " + value + (crlf ? "\r" : "");
        }
        out += line;
        if (!in.eof() || content.back() == '\n') out += '\n';
    }
    return out;
}

int secret_filter_check(git_filter*, void**, const git_filter_source *src, const char**)
{
    return git_filter_source_path(src) ? 0 : GIT_PASSTHROUGH;
}

int secret_filter_apply(git_filter*, void**, git_buf *to, const git_buf *from, const git_filter_source *src)
{
    std::string dir = git_repository_workdir(git_filter_source_repo(src));
    if (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) dir.pop_back();
    std::string rel = git_filter_source_path(src);
    std::string in(from->ptr, from->size);
    std::string out = git_filter_source_mode(src) == GIT_FILTER_TO_ODB ? scrub_secrets(dir, rel, in) : restore_secrets(dir, rel, in);
    if (out == in) return GIT_PASSTHROUGH;
    return git_buf_set(to, out.data(), out.size());
}

git_filter g_secret_filter = GIT_FILTER_INIT;

void register_secret_filter()
{
    g_secret_filter.attributes = "filter=prusaslicer-secrets";
    g_secret_filter.check = secret_filter_check;
    g_secret_filter.apply = secret_filter_apply;
    git_filter_register(SECRET_FILTER_NAME, &g_secret_filter, GIT_FILTER_DRIVER_PRIORITY);
}

const char *GITATTRIBUTES_TEXT =
    "# Managed by PrusaSlicer profile sync: keep print-host API keys out of the repository.\n"
    "physical_printer/*.ini filter=prusaslicer-secrets\n";

std::once_flag g_git_init;
void git_init_once() { std::call_once(g_git_init, [] { git_libgit2_init(); register_secret_filter(); }); }

std::string oid_str(const git_oid *oid) { char b[GIT_OID_SHA1_HEXSIZE + 1]; git_oid_tostr(b, sizeof(b), oid); return b; }

// Credentials: SSH goes through the system `ssh` binary (USE_SSH=exec), so
// keys and agent come from the user's normal ssh setup. We still answer the
// username query and offer agent keys for completeness.
int cred_cb(git_credential **out, const char *url, const char *username_from_url, unsigned int allowed, void *)
{
    const char *user = username_from_url ? username_from_url : "git";
    if (allowed & GIT_CREDENTIAL_USERNAME)
        return git_credential_username_new(out, user);
    if (allowed & GIT_CREDENTIAL_SSH_KEY)
        return git_credential_ssh_key_from_agent(out, user);
    if (allowed & GIT_CREDENTIAL_DEFAULT)
        return git_credential_default_new(out);
    // No usable credential type offered (HTTPS with password). Use an SSH remote with keys in the agent.
    return GIT_EAUTH;
}

git_remote_callbacks make_callbacks()
{
    git_remote_callbacks cb = GIT_REMOTE_CALLBACKS_INIT;
    cb.credentials = cred_cb;
    return cb;
}

std::string gitignore_text(const Settings &s)
{
    std::string t =
        "# Managed by PrusaSlicer profile sync. Machine-local state is excluded.\n"
        "*\n"
        "!.gitignore\n!.gitattributes\n"
        "!print/\n!print/**\n"
        "!sla_print/\n!sla_print/**\n"
        "!filament/\n!filament/**\n"
        "!sla_material/\n!sla_material/**\n"
        "!printer/\n!printer/**\n"
        "!shapes/\n!shapes/**\n";
    if (s.track_physical_printers) t += "!physical_printer/\n!physical_printer/**\n";
    if (s.track_vendor)            t += "!vendor/\n!vendor/**\n";
    t += std::string(SECRETS_FILE) + "\n*.lock\n*.log\n*.tmp\n*.bak\n.DS_Store\n";
    return t;
}

} // namespace

// ---------------------------------------------------------------- Repo

struct Repo::Impl
{
    std::string dir;
    std::mutex  mtx;
    RepoPtr     repo;

    void open()
    {
        if (repo) return;
        git_init_once();
        check(git_repository_open(&repo, dir.c_str()), "open repository");
    }

    SigPtr signature(const Settings &s)
    {
        SigPtr sig;
        std::string email = s.machine_name + "@prusaslicer.sync";
        check(git_signature_now(&sig, s.machine_name.c_str(), email.c_str()), "signature");
        return sig;
    }

    bool head_commit(CommitPtr &out)
    {
        git_oid oid;
        int rc = git_reference_name_to_id(&oid, repo, "HEAD");
        if (rc == GIT_ENOTFOUND || rc == GIT_EUNBORNBRANCH) return false;
        check(rc, "resolve HEAD");
        check(git_commit_lookup(&out, repo, &oid), "lookup HEAD");
        return true;
    }

    void write_gitignore(const Settings &s)
    {
        boost::filesystem::path p = boost::filesystem::path(dir) / ".gitignore";
        std::string want = gitignore_text(s);
        std::string have;
        if (boost::filesystem::exists(p)) {
            boost::nowide::ifstream in(p.string()); std::stringstream ss; ss << in.rdbuf(); have = ss.str();
        }
        if (have != want) { boost::nowide::ofstream out(p.string(), std::ios::binary); out << want; }
        boost::filesystem::path a = boost::filesystem::path(dir) / ".gitattributes";
        std::string have_a;
        if (boost::filesystem::exists(a)) { boost::nowide::ifstream in(a.string()); std::stringstream ss; ss << in.rdbuf(); have_a = ss.str(); }
        if (have_a != GITATTRIBUTES_TEXT) { boost::nowide::ofstream out(a.string(), std::ios::binary); out << GITATTRIBUTES_TEXT; }
    }

    // libgit2's exec SSH transport honours core.sshcommand. ssh's stdio is piped, so an
    // interactive host-key question would hang invisibly: accept new host keys (TOFU,
    // like a first `git clone` would after the user says yes) and bound the connect time.
    // Only set if the user has not configured their own.
    void ensure_ssh_command()
    {
        ConfigPtr cfg;
        if (git_repository_config(&cfg, repo) != 0) return;
        git_buf existing = GIT_BUF_INIT;
        if (git_config_get_string_buf(&existing, cfg, "core.sshcommand") == 0) { git_buf_dispose(&existing); return; }
        git_config_set_string(cfg, "core.sshcommand", "ssh -o StrictHostKeyChecking=accept-new -o ConnectTimeout=30");
    }

    void ensure_remote(const Settings &s)
    {
        ensure_ssh_command();
        if (s.remote_url.empty()) return;
        RemotePtr remote;
        int rc = git_remote_lookup(&remote, repo, "origin");
        if (rc == GIT_ENOTFOUND) {
            check(git_remote_create(&remote, repo, "origin", s.remote_url.c_str()), "create remote");
        } else {
            check(rc, "lookup remote");
            if (s.remote_url != git_remote_url(remote))
                check(git_remote_set_url(repo, "origin", s.remote_url.c_str()), "set remote url");
        }
    }

    // git does not track empty directories, so a checkout can remove e.g. sla_print/ when it
    // has no presets; PresetBundle::load_presets() iterates those directories and throws if
    // one is missing. Recreate the standard layout after every checkout.
    void ensure_layout()
    {
        for (const char *sub : { "print", "sla_print", "filament", "sla_material", "printer", "physical_printer", "vendor", "shapes" })
            boost::filesystem::create_directories(boost::filesystem::path(dir) / sub);
    }

    std::string local_ref(const Settings &s)  { return "refs/heads/" + s.branch; }
    std::string remote_ref(const Settings &s) { return "refs/remotes/origin/" + s.branch; }

    bool commit_all(const Settings &s, const std::string &message)
    {
        write_gitignore(s);
        IndexPtr index;
        check(git_repository_index(&index, repo), "index");
        // git add -A
        check(git_index_add_all(index, nullptr, GIT_INDEX_ADD_DEFAULT, nullptr, nullptr), "add all");
        check(git_index_update_all(index, nullptr, nullptr, nullptr), "update all");
        check(git_index_write(index), "write index");

        git_oid tree_oid;
        check(git_index_write_tree(&tree_oid, index), "write tree");

        CommitPtr parent;
        bool has_parent = head_commit(parent);
        if (has_parent) {
            // Nothing to commit if the tree is identical to HEAD's.
            if (git_oid_equal(&tree_oid, git_commit_tree_id(parent))) return false;
        }
        TreePtr tree;
        check(git_tree_lookup(&tree, repo, &tree_oid), "lookup tree");
        SigPtr sig = signature(s);
        git_oid commit_oid;
        const git_commit *parents[1] = { parent.get() };
        check(git_commit_create(&commit_oid, repo, "HEAD", sig, sig, "UTF-8", message.c_str(), tree,
                                has_parent ? 1 : 0, has_parent ? parents : nullptr), "create commit");
        BOOST_LOG_TRIVIAL(info) << "ProfileSync: committed " << oid_str(&commit_oid) << " \"" << message << "\"";
        return true;
    }

    void fetch(const Settings &s)
    {
        ensure_remote(s);
        RemotePtr remote;
        check(git_remote_lookup(&remote, repo, "origin"), "lookup remote");
        git_fetch_options fo = GIT_FETCH_OPTIONS_INIT;
        fo.callbacks = make_callbacks();
        fo.prune = GIT_FETCH_PRUNE;
        std::string spec = "+" + local_ref(s) + ":" + remote_ref(s);
        const char *specs[1] = { spec.c_str() };
        git_strarray arr = { const_cast<char**>(specs), 1 };
        check(git_remote_fetch(remote, &arr, &fo, "profile sync fetch"), "fetch");
    }

    // Auto-resolve conflicts: remote wins in place, local copy survives as
    // "<name> (<machine>).ini" next to it so nothing is lost.
    int resolve_conflicts(const Settings &s, git_index *index)
    {
        int n = 0;
        ConflictItPtr it;
        check(git_index_conflict_iterator_new(&it, index), "conflict iterator");
        struct Item { std::string path; git_oid ours, theirs; bool has_ours, has_theirs; };
        std::vector<Item> items;
        const git_index_entry *anc, *ours, *theirs;
        while (git_index_conflict_next(&anc, &ours, &theirs, it) == 0) {
            Item i;
            i.path = ours ? ours->path : (theirs ? theirs->path : anc->path);
            i.has_ours = ours != nullptr;  if (ours)   i.ours = ours->id;
            i.has_theirs = theirs != nullptr; if (theirs) i.theirs = theirs->id;
            items.push_back(i);
        }
        it.reset();
        for (const Item &i : items) {
            boost::filesystem::path p = boost::filesystem::path(dir) / i.path;
            auto write_blob = [&](const git_oid &oid, const boost::filesystem::path &to) {
                BlobPtr blob; check(git_blob_lookup(&blob, repo, &oid), "lookup blob");
                boost::filesystem::create_directories(to.parent_path());
                std::string content(static_cast<const char*>(git_blob_rawcontent(blob)), git_blob_rawsize(blob));
                content = restore_secrets(dir, i.path, content);
                boost::nowide::ofstream out(to.string(), std::ios::binary);
                out.write(content.data(), content.size());
            };
            if (i.has_theirs) write_blob(i.theirs, p);
            else if (boost::filesystem::exists(p)) boost::filesystem::remove(p);
            if (i.has_ours && i.has_theirs) {
                boost::filesystem::path alt = p.parent_path() / (p.stem().string() + " (" + s.machine_name + ")" + p.extension().string());
                write_blob(i.ours, alt);
            }
            check(git_index_conflict_remove(index, i.path.c_str()), "conflict remove");
            ++n;
        }
        // Re-stage the resolved worktree.
        check(git_index_add_all(index, nullptr, GIT_INDEX_ADD_DEFAULT, nullptr, nullptr), "add all");
        check(git_index_update_all(index, nullptr, nullptr, nullptr), "update all");
        check(git_index_write(index), "write index");
        return n;
    }

    SyncResult merge_remote(const Settings &s)
    {
        SyncResult r; r.ok = true;
        RefPtr remote_branch;
        int rc = git_reference_lookup(&remote_branch, repo, remote_ref(s).c_str());
        if (rc == GIT_ENOTFOUND) { r.message = "Remote branch does not exist yet."; return r; }
        check(rc, "lookup remote branch");
        AnnotPtr their;
        check(git_annotated_commit_from_ref(&their, repo, remote_branch), "annotated commit");
        git_merge_analysis_t analysis; git_merge_preference_t pref;
        const git_annotated_commit *heads[1] = { their.get() };
        check(git_merge_analysis(&analysis, &pref, repo, heads, 1), "merge analysis");

        git_checkout_options co = GIT_CHECKOUT_OPTIONS_INIT;
        co.checkout_strategy = GIT_CHECKOUT_FORCE; // worktree was committed just before, nothing to lose

        if (analysis & GIT_MERGE_ANALYSIS_UP_TO_DATE) { r.message = "Already up to date."; return r; }

        if ((analysis & GIT_MERGE_ANALYSIS_UNBORN) || (analysis & GIT_MERGE_ANALYSIS_FASTFORWARD)) {
            const git_oid *target = git_annotated_commit_id(their);
            ObjPtr obj; check(git_object_lookup(&obj, repo, target, GIT_OBJECT_COMMIT), "lookup target");
            check(git_checkout_tree(repo, obj, &co), "checkout");
            ensure_layout();
            RefPtr newref;
            check(git_reference_create(&newref, repo, local_ref(s).c_str(), target, 1, "profile sync fast-forward"), "update branch");
            check(git_repository_set_head(repo, local_ref(s).c_str()), "set head");
            r.pulled_changes = true; r.message = "Fast-forwarded to remote.";
            return r;
        }

        if (analysis & GIT_MERGE_ANALYSIS_NORMAL) {
            git_merge_options mo = GIT_MERGE_OPTIONS_INIT;
            mo.file_favor = GIT_MERGE_FILE_FAVOR_NORMAL;
            check(git_merge(repo, heads, 1, &mo, &co), "merge");
            IndexPtr index; check(git_repository_index(&index, repo), "index");
            if (git_index_has_conflicts(index))
                r.merged_conflicts = resolve_conflicts(s, index);
            git_oid tree_oid; check(git_index_write_tree(&tree_oid, index), "write tree");
            TreePtr tree; check(git_tree_lookup(&tree, repo, &tree_oid), "lookup tree");
            CommitPtr ours; head_commit(ours);
            CommitPtr theirs_c; check(git_commit_lookup(&theirs_c, repo, git_annotated_commit_id(their)), "lookup theirs");
            const git_commit *parents[2] = { ours.get(), theirs_c.get() };
            SigPtr sig = signature(s);
            std::string msg = "Merge remote changes into " + s.machine_name;
            if (r.merged_conflicts) msg += " (" + std::to_string(r.merged_conflicts) + " conflicting files kept as local copies)";
            git_oid cid;
            check(git_commit_create(&cid, repo, "HEAD", sig, sig, "UTF-8", msg.c_str(), tree, 2, parents), "merge commit");
            git_repository_state_cleanup(repo);
            ensure_layout();
            r.pulled_changes = true; r.message = msg;
            return r;
        }
        r.ok = false; r.message = "Unexpected merge state.";
        return r;
    }

    SyncResult push(const Settings &s, bool force = false)
    {
        SyncResult r;
        ensure_remote(s);
        RemotePtr remote;
        check(git_remote_lookup(&remote, repo, "origin"), "lookup remote");
        git_push_options po = GIT_PUSH_OPTIONS_INIT;
        po.callbacks = make_callbacks();
        std::string spec = (force ? "+" : "") + local_ref(s) + ":" + local_ref(s);
        const char *specs[1] = { spec.c_str() };
        git_strarray arr = { const_cast<char**>(specs), 1 };
        check(git_remote_push(remote, &arr, &po), "push");
        // Track what we just pushed.
        git_oid head; if (git_reference_name_to_id(&head, repo, "HEAD") == 0) {
            RefPtr rr; git_reference_create(&rr, repo, remote_ref(s).c_str(), &head, 1, "profile sync push");
        }
        r.ok = true; r.message = force ? "Remote overwritten with this machine's profiles." : "Pushed.";
        return r;
    }

    void ahead_behind(const Settings &s, int &ahead, int &behind)
    {
        ahead = behind = 0;
        git_oid local, remote;
        if (git_reference_name_to_id(&local, repo, "HEAD") != 0) return;
        if (git_reference_name_to_id(&remote, repo, remote_ref(s).c_str()) != 0) { return; }
        size_t a = 0, b = 0;
        if (git_graph_ahead_behind(&a, &b, repo, &local, &remote) == 0) { ahead = int(a); behind = int(b); }
    }
};

Repo::Repo(const std::string &data_dir) : m_impl(new Impl) { m_impl->dir = data_dir; }
Repo::~Repo() = default;

bool Repo::exists() const { return boost::filesystem::exists(boost::filesystem::path(m_impl->dir) / ".git"); }

void Repo::ensure_initialized(const Settings &s)
{
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    git_init_once();
    if (!exists()) {
        git_repository_init_options o = GIT_REPOSITORY_INIT_OPTIONS_INIT;
        o.flags = GIT_REPOSITORY_INIT_MKPATH | GIT_REPOSITORY_INIT_NO_REINIT;
        std::string head = "refs/heads/" + s.branch;
        o.initial_head = head.c_str();
        check(git_repository_init_ext(&m_impl->repo, m_impl->dir.c_str(), &o), "init repository");
    }
    m_impl->open();
    m_impl->ensure_layout();
    m_impl->write_gitignore(s);
    m_impl->ensure_remote(s);
}

Status Repo::status(const Settings &s)
{
    Status st;
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    try {
        st.repo_exists = exists();
        if (!st.repo_exists) return st;
        m_impl->open();
        m_impl->write_gitignore(s);
        RemotePtr remote;
        st.has_remote = git_remote_lookup(&remote, m_impl->repo, "origin") == 0;
        git_oid head;
        if (git_reference_name_to_id(&head, m_impl->repo, "HEAD") == 0) st.head_id = oid_str(&head);
        st.branch = s.branch;
        git_status_options so = GIT_STATUS_OPTIONS_INIT;
        so.show  = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
        so.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS | GIT_STATUS_OPT_EXCLUDE_SUBMODULES;
        StatusPtr list;
        check(git_status_list_new(&list, m_impl->repo, &so), "status");
        st.uncommitted = int(git_status_list_entrycount(list));
        m_impl->ahead_behind(s, st.ahead, st.behind);
    } catch (const std::exception &e) { st.last_error = e.what(); }
    return st;
}

bool Repo::commit_all(const Settings &s, const std::string &message)
{
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    m_impl->open();
    return m_impl->commit_all(s, message);
}

SyncResult Repo::pull(const Settings &s)
{
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    SyncResult r;
    try {
        m_impl->open();
        m_impl->commit_all(s, "Local changes on " + s.machine_name + " before sync");
        m_impl->fetch(s);
        r = m_impl->merge_remote(s);
    } catch (const std::exception &e) { r.ok = false; r.message = e.what(); }
    return r;
}

SyncResult Repo::push(const Settings &s)
{
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    SyncResult r;
    try { m_impl->open(); r = m_impl->push(s); }
    catch (const std::exception &e) { r.ok = false; r.message = e.what(); }
    return r;
}

SyncResult Repo::sync(const Settings &s, const std::string &commit_message)
{
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    SyncResult r;
    try {
        m_impl->open();
        bool committed = m_impl->commit_all(s, commit_message);
        if (!s.remote_url.empty()) {
            m_impl->fetch(s);
            r = m_impl->merge_remote(s);
            if (!r.ok) return r;
            int ahead, behind; m_impl->ahead_behind(s, ahead, behind);
            if (ahead > 0 || committed) {
                SyncResult p = m_impl->push(s);
                if (!p.ok) return p;
                r.message += (r.message.empty() ? "" : " ") + p.message;
            }
        } else {
            r.ok = true; r.message = committed ? "Committed locally (no remote configured)." : "Nothing to commit.";
        }
    } catch (const std::exception &e) { r.ok = false; r.message = e.what(); }
    return r;
}

SyncResult Repo::overwrite_remote(const Settings &s)
{
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    SyncResult r;
    try {
        m_impl->open();
        m_impl->commit_all(s, "Overwrite remote with profiles from " + s.machine_name);
        m_impl->fetch(s); // so the remote-tracking ref is current and history stays visible
        r = m_impl->push(s, true);
    } catch (const std::exception &e) { r.ok = false; r.message = e.what(); }
    return r;
}

SyncResult Repo::replace_local_with_remote(const Settings &s)
{
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    SyncResult r;
    try {
        m_impl->open();
        m_impl->commit_all(s, "Local profiles on " + s.machine_name + " before replacing them with the remote");
        m_impl->fetch(s);
        git_oid remote_oid;
        if (git_reference_name_to_id(&remote_oid, m_impl->repo, m_impl->remote_ref(s).c_str()) != 0) {
            r.ok = false; r.message = "The remote branch does not exist yet."; return r;
        }
        // 1. Join the histories (fast-forward or merge; conflicts auto-resolved) so the
        //    result is a descendant of both sides and can be pushed without force.
        SyncResult m = m_impl->merge_remote(s);
        if (!m.ok) return m;
        // 2. Then make the tree exactly the remote's, discarding whatever the merge kept
        //    from this machine, and record that as a new version.
        CommitPtr c; check(git_commit_lookup(&c, m_impl->repo, &remote_oid), "lookup remote commit");
        TreePtr tree; check(git_commit_tree(&tree, c), "tree");
        git_checkout_options co = GIT_CHECKOUT_OPTIONS_INIT;
        co.checkout_strategy = GIT_CHECKOUT_FORCE | GIT_CHECKOUT_REMOVE_UNTRACKED;
        check(git_checkout_tree(m_impl->repo, reinterpret_cast<git_object*>(tree.get()), &co), "checkout remote tree");
        m_impl->ensure_layout();
        m_impl->commit_all(s, "Replace profiles on " + s.machine_name + " with the remote version");
        SyncResult p = m_impl->push(s);
        if (!p.ok) return p;
        r.ok = true; r.pulled_changes = true;
        r.message = "This machine's profiles now match the remote.";
    } catch (const std::exception &e) { r.ok = false; r.message = e.what(); }
    return r;
}

std::vector<Commit> Repo::log(size_t max_count)
{
    std::vector<Commit> out;
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    if (!exists()) return out;
    m_impl->open();
    git_repository *repo = m_impl->repo;
    WalkPtr walk;
    check(git_revwalk_new(&walk, repo), "revwalk");
    git_revwalk_sorting(walk, GIT_SORT_TIME);
    if (git_revwalk_push_head(walk) != 0) return out; // unborn
    git_oid oid;
    while (out.size() < max_count && git_revwalk_next(&oid, walk) == 0) {
        CommitPtr c; check(git_commit_lookup(&c, repo, &oid), "lookup commit");
        Commit e;
        e.id = oid_str(&oid); e.short_id = e.id.substr(0, 8);
        e.time = git_commit_time(c);
        e.author = git_commit_author(c)->name;
        e.summary = git_commit_summary(c) ? git_commit_summary(c) : "";
        TreePtr tree; check(git_commit_tree(&tree, c), "commit tree");
        TreePtr parent_tree;
        if (git_commit_parentcount(c) > 0) {
            CommitPtr p; check(git_commit_parent(&p, c, 0), "parent");
            check(git_commit_tree(&parent_tree, p), "parent tree");
        }
        DiffPtr diff;
        git_diff_options dopts = GIT_DIFF_OPTIONS_INIT;
        check(git_diff_tree_to_tree(&diff, repo, parent_tree, tree, &dopts), "diff");
        git_diff_find_options fopts = GIT_DIFF_FIND_OPTIONS_INIT;
        git_diff_find_similar(diff, &fopts);
        size_t n = git_diff_num_deltas(diff);
        for (size_t i = 0; i < n; ++i) {
            const git_diff_delta *d = git_diff_get_delta(diff, i);
            FileChange fc; fc.path = d->new_file.path ? d->new_file.path : d->old_file.path;
            switch (d->status) {
            case GIT_DELTA_ADDED: fc.status = 'A'; break;
            case GIT_DELTA_DELETED: fc.status = 'D'; fc.path = d->old_file.path; break;
            case GIT_DELTA_RENAMED: fc.status = 'R'; break;
            default: fc.status = 'M';
            }
            e.files.push_back(fc);
        }
        out.push_back(std::move(e));
    }
    return out;
}

std::string Repo::file_at(const std::string &commit_id, const std::string &path)
{
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    m_impl->open();
    git_oid oid; check(git_oid_fromstr(&oid, commit_id.c_str()), "parse id");
    CommitPtr c; check(git_commit_lookup(&c, m_impl->repo, &oid), "lookup commit");
    TreePtr tree; check(git_commit_tree(&tree, c), "tree");
    git_tree_entry *entry = nullptr;
    check(git_tree_entry_bypath(&entry, tree, path.c_str()), "entry");
    BlobPtr blob; int rc = git_blob_lookup(&blob, m_impl->repo, git_tree_entry_id(entry));
    git_tree_entry_free(entry);
    check(rc, "blob");
    return std::string(static_cast<const char*>(git_blob_rawcontent(blob)), git_blob_rawsize(blob));
}

void Repo::restore_file(const std::string &commit_id, const std::string &path)
{
    std::string content = restore_secrets(m_impl->dir, path, file_at(commit_id, path));
    boost::filesystem::path p = boost::filesystem::path(m_impl->dir) / path;
    boost::filesystem::create_directories(p.parent_path());
    boost::nowide::ofstream out(p.string(), std::ios::binary); out << content;
}

SyncResult Repo::restore_commit(const Settings &s, const std::string &commit_id)
{
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    SyncResult r;
    try {
        m_impl->open();
        m_impl->commit_all(s, "Local changes on " + s.machine_name + " before restoring an older version");
        git_oid oid; check(git_oid_fromstr(&oid, commit_id.c_str()), "parse id");
        CommitPtr c; check(git_commit_lookup(&c, m_impl->repo, &oid), "lookup commit");
        TreePtr tree; check(git_commit_tree(&tree, c), "tree");
        git_checkout_options co = GIT_CHECKOUT_OPTIONS_INIT;
        co.checkout_strategy = GIT_CHECKOUT_FORCE | GIT_CHECKOUT_REMOVE_UNTRACKED;
        check(git_checkout_tree(m_impl->repo, reinterpret_cast<git_object*>(tree.get()), &co), "checkout tree");
        m_impl->ensure_layout();
        bool committed = m_impl->commit_all(s, "Restore profiles to version " + commit_id.substr(0, 8) + " on " + s.machine_name);
        r.ok = true; r.pulled_changes = true;
        r.message = committed ? "Restored and recorded as a new version." : "Already at that version.";
    } catch (const std::exception &e) { r.ok = false; r.message = e.what(); }
    return r;
}

std::string Repo::diff_text(const std::string &commit_id)
{
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    m_impl->open();
    git_oid oid; check(git_oid_fromstr(&oid, commit_id.c_str()), "parse id");
    CommitPtr c; check(git_commit_lookup(&c, m_impl->repo, &oid), "lookup commit");
    TreePtr tree; check(git_commit_tree(&tree, c), "tree");
    TreePtr parent_tree;
    if (git_commit_parentcount(c) > 0) { CommitPtr p; check(git_commit_parent(&p, c, 0), "parent"); check(git_commit_tree(&parent_tree, p), "ptree"); }
    DiffPtr diff; git_diff_options dopts = GIT_DIFF_OPTIONS_INIT; dopts.context_lines = 2;
    check(git_diff_tree_to_tree(&diff, m_impl->repo, parent_tree, tree, &dopts), "diff");
    std::string out;
    git_diff_print(diff, GIT_DIFF_FORMAT_PATCH, [](const git_diff_delta*, const git_diff_hunk*, const git_diff_line *line, void *payload) {
        std::string &o = *static_cast<std::string*>(payload);
        if (line->origin == GIT_DIFF_LINE_CONTEXT || line->origin == GIT_DIFF_LINE_ADDITION || line->origin == GIT_DIFF_LINE_DELETION)
            o += line->origin;
        o.append(line->content, line->content_len);
        return 0;
    }, &out);
    return out;
}

// ---------------------------------------------------------------- app glue

namespace {

std::unique_ptr<Repo> g_repo;
std::mutex g_pending_mtx;
std::vector<std::string> g_pending;   // reasons since last commit
std::atomic<bool> g_busy{false};

class Debouncer : public wxEvtHandler
{
public:
    wxTimer timer;
    Debouncer() : timer(this) { Bind(wxEVT_TIMER, [this](wxTimerEvent&) { fire(); }); }
    void kick() { timer.StartOnce(2500); }
    void fire();
};
Debouncer *g_debouncer = nullptr;

std::string pending_message()
{
    std::lock_guard<std::mutex> lock(g_pending_mtx);
    if (g_pending.empty()) return {};
    std::string msg = g_pending.size() == 1 ? g_pending.front() : g_pending.front() + " (+" + std::to_string(g_pending.size() - 1) + " more)";
    if (g_pending.size() > 1) { msg += "\n\n"; for (auto &p : g_pending) msg += "- " + p + "\n"; }
    g_pending.clear();
    return msg;
}

void notify_gui(const std::string &text, bool error)
{
    if (auto *plater = GUI::wxGetApp().plater(); plater != nullptr) {
        auto *nm = plater->get_notification_manager();
        if (error) nm->push_notification(GUI::NotificationType::CustomNotification, GUI::NotificationManager::NotificationLevel::ErrorNotificationLevel, "Profile sync: " + text);
        else       nm->push_notification(GUI::NotificationType::CustomNotification, GUI::NotificationManager::NotificationLevel::RegularNotificationLevel, "Profile sync: " + text);
    }
}

void reload_presets_after_pull()
{
    GUI::wxGetApp().reload_presets_from_disk();
}

void Debouncer::fire()
{
    Settings s = settings();
    if (!s.enabled || !s.auto_commit) return;
    std::string msg = pending_message();
    if (msg.empty()) return;
    run_async([s, msg] {
        SyncResult r;
        try {
            repo().ensure_initialized(s);
            if (s.auto_push && !s.remote_url.empty()) r = repo().sync(s, msg);
            else { r.ok = true; r.message = repo().commit_all(s, msg) ? "Committed." : "Nothing to commit."; }
        } catch (const std::exception &e) { r.ok = false; r.message = e.what(); }
        return r;
    }, [](SyncResult r) {
        if (!r.ok) notify_gui(r.message, true);
        else if (r.pulled_changes) { notify_gui(r.message, false); reload_presets_after_pull(); }
        if (r.merged_conflicts) notify_gui(GUI::format("%1% conflicting profiles were kept as local copies", r.merged_conflicts), false);
    });
}

} // namespace

Repo& repo()
{
    if (!g_repo) g_repo.reset(new Repo(data_dir()));
    return *g_repo;
}

Settings settings() { return Settings::load(*GUI::wxGetApp().app_config); }
void set_settings(const Settings &s) { s.save(*GUI::wxGetApp().app_config); }
bool is_enabled() { return settings().enabled; }

void notify_change(const std::string &what)
{
    if (!is_enabled()) return;
    { std::lock_guard<std::mutex> lock(g_pending_mtx); g_pending.push_back(what); }
    if (!g_debouncer) g_debouncer = new Debouncer();
    g_debouncer->kick();
}

void flush_pending()
{
    if (g_debouncer) g_debouncer->timer.Stop();
    Settings s = settings();
    if (!s.enabled || !s.auto_commit) return;
    std::string msg = pending_message();
    if (msg.empty()) return;
    try {
        repo().ensure_initialized(s);
        if (s.auto_push && !s.remote_url.empty()) repo().sync(s, msg);
        else repo().commit_all(s, msg);
    } catch (const std::exception &e) { BOOST_LOG_TRIVIAL(error) << "ProfileSync flush failed: " << e.what(); }
}

void run_async(std::function<SyncResult()> fn, std::function<void(SyncResult)> done)
{
    std::thread([fn, done] {
        SyncResult r;
        try { r = fn(); } catch (const std::exception &e) { r.ok = false; r.message = e.what(); }
        GUI::wxGetApp().CallAfter([done, r] { done(r); });
    }).detach();
}

void pull_on_launch()
{
    Settings s = settings();
    if (!s.enabled || !s.pull_on_launch || s.remote_url.empty()) return;
    run_async([s] { repo().ensure_initialized(s); return repo().pull(s); },
              [](SyncResult r) {
                  if (!r.ok) { notify_gui(r.message, true); return; }
                  if (r.pulled_changes) { notify_gui("Profiles updated from remote. " + r.message, false); reload_presets_after_pull(); }
                  if (r.merged_conflicts) notify_gui(GUI::format("%1% conflicting profiles were kept as local copies", r.merged_conflicts), false);
              });
}

} // namespace ProfileSync
} // namespace Slic3r
