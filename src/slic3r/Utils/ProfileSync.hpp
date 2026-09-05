///|/ Git-native profile sync for PrusaSlicer (fork feature).
///|/
///|/ Keeps the user configuration directory (print/filament/printer presets,
///|/ vendor bundles, custom shapes) in a git repository and syncs it with a
///|/ remote so profiles can be shared between machines and versioned.
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#ifndef slic3r_ProfileSync_hpp_
#define slic3r_ProfileSync_hpp_

#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <memory>
#include <ctime>

namespace Slic3r {

class AppConfig;

namespace ProfileSync {

// Settings live in PrusaSlicer.ini under [profile_sync].
struct Settings
{
    bool        enabled              = false;
    std::string remote_url;                    // e.g. git@git.example.com:you/prusaslicer-profiles.git
    std::string branch               = "main";
    std::string machine_name;                  // used as commit author; defaults to hostname
    bool        auto_commit          = true;   // commit after every preset save/rename/delete
    bool        auto_push            = true;   // push after each auto commit
    bool        pull_on_launch       = true;
    bool        track_physical_printers = true;  // API keys/passwords are scrubbed by a git filter and kept in a local sidecar
    bool        track_vendor         = true;   // vendor/*.ini system bundles

    static Settings load(const AppConfig &cfg);
    void            save(AppConfig &cfg) const;
};

struct FileChange
{
    std::string path;
    char        status; // 'A' added, 'M' modified, 'D' deleted, 'R' renamed
};

struct Commit
{
    std::string  id;          // full sha
    std::string  short_id;
    std::time_t  time = 0;
    std::string  author;
    std::string  summary;     // first line of the message
    std::vector<FileChange> files;
};

struct Status
{
    bool        repo_exists   = false;
    bool        has_remote    = false;
    std::string head_id;
    std::string branch;
    int         uncommitted   = 0;   // dirty tracked + untracked files
    int         ahead         = 0;   // local commits not on remote-tracking branch
    int         behind        = 0;   // remote commits not merged locally
    std::string last_error;
};

struct SyncResult
{
    bool        ok            = false;
    bool        pulled_changes = false; // worktree changed -> presets must be reloaded
    int         merged_conflicts = 0;   // files where both sides changed; local copies kept as "<name> (<machine>)"
    std::string message;               // human-readable summary or error
};

// Thread-safe facade over libgit2. All methods may block on network I/O and
// must not be called on the GUI thread except through the async helpers below.
class Repo
{
public:
    explicit Repo(const std::string &data_dir);
    ~Repo();

    // Creates the repository (and .gitignore) if it does not exist yet.
    void        ensure_initialized(const Settings &s);
    bool        exists() const;

    Status      status(const Settings &s);
    // Stages everything (respecting .gitignore) and commits if there is anything to commit.
    // Returns true if a commit was created.
    bool        commit_all(const Settings &s, const std::string &message);
    // fetch + merge (fast-forward when possible, merge commit otherwise, conflicts auto-resolved).
    SyncResult  pull(const Settings &s);
    SyncResult  push(const Settings &s);
    // commit_all + pull + push
    SyncResult  sync(const Settings &s, const std::string &commit_message);
    // Make the remote match this machine: commit local state and force-push it, discarding
    // whatever the remote branch had that is not in local history. Other machines will see
    // this as remote changes and keep their own versions of conflicting profiles as copies.
    SyncResult  overwrite_remote(const Settings &s);
    // Make this machine match the remote: local state is recorded first (so it stays in the
    // history), then the remote version is checked out and committed on top, and pushed.
    SyncResult  replace_local_with_remote(const Settings &s);

    std::vector<Commit> log(size_t max_count = 200);
    std::string         file_at(const std::string &commit_id, const std::string &path);
    // Restores the whole tracked tree to the given commit and records a new commit for it.
    SyncResult          restore_commit(const Settings &s, const std::string &commit_id);
    // Restores a single file from the given commit (does not commit).
    void                restore_file(const std::string &commit_id, const std::string &path);
    // Unified diff of a commit against its first parent.
    std::string         diff_text(const std::string &commit_id);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Application-level glue -----------------------------------------------------

// Singleton bound to the current data_dir().
Repo&       repo();
Settings    settings();
void        set_settings(const Settings &s);
bool        is_enabled();

// Called by the GUI after the user saved/renamed/deleted a preset. Batches
// changes for a couple of seconds, then commits (and pushes) in the background.
void        notify_change(const std::string &what);
// Commit anything still pending synchronously (used on shutdown).
void        flush_pending();

// Runs `fn` on a worker thread, then `done` on the GUI thread.
void        run_async(std::function<SyncResult()> fn, std::function<void(SyncResult)> done);

// Pull on startup; reloads presets on the GUI thread if anything changed.
void        pull_on_launch();

std::string default_machine_name();

} // namespace ProfileSync
} // namespace Slic3r

#endif // slic3r_ProfileSync_hpp_
