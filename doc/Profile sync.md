# Profile sync (fork feature)

This fork adds git-native synchronisation of the user configuration directory
so profiles can be shared between machines and every change is versioned.

## What it does

* The PrusaSlicer data directory becomes a git work tree. A managed
  `.gitignore` tracks `print/`, `filament/`, `sla_print/`, `sla_material/`,
  `printer/`, `shapes/`, `physical_printer/` and (by default) `vendor/`.
  Everything machine-local (`PrusaSlicer.ini`, `cache/`, `snapshots/`, logs,
  lock files) stays out.
* **API keys never enter the repository.** `physical_printer/*.ini` go through
  a libgit2 clean/smudge filter (`prusaslicer-secrets`, enabled by a managed
  `.gitattributes`): on commit the values of `printhost_apikey` and
  `printhost_password` are blanked in the stored blob and remembered in the
  gitignored sidecar `profile_sync_secrets.ini`; on checkout/pull they are put
  back. Each machine keeps its own keys — enter them once per machine. Note
  that plain `git status` in the data directory (which lacks the filter) will
  show those files as modified; the app's own status is clean.
* Saving, renaming or deleting a preset records a commit a couple of seconds
  later (debounced), and pushes it if a remote is configured.
* On startup the remote branch is pulled and, if anything changed, presets are
  reloaded in place.
* When both machines changed the same profile, the remote version wins in
  place and the local one is kept as `<name> (<machine>).ini`, so nothing is
  ever lost and you can merge by hand in the UI.
* **Configuration → Profile Sync…** shows settings, status, the full history
  with per-version file lists and diffs, and a *Restore this version* button.
  **Configuration → Sync Profiles Now** does commit + pull + push in one go.
* When the merge policy is not what you want, the dialog has two explicit
  actions: *Overwrite remote with my profiles* (force-push this machine's
  state; other machines keep differing profiles as local copies on their
  next pull) and *Replace my profiles with remote* (your current profiles
  are recorded first, then the remote version is checked out and pushed).

## Setup

1. Create an empty repository on your git host (e.g. Gitea:
   `you/prusaslicer-profiles`, private).
2. On the first machine open *Configuration → Profile Sync…*, tick *Enable*,
   paste the SSH remote (`git@host:user/repo.git`), press *Sync now*.
3. On the second machine do the same. Both histories are merged; conflicting
   profiles are kept as local copies as described above.

SSH remotes use the system `ssh` binary (libgit2 is built with
`USE_SSH=exec`), so whatever works for `git push` in a terminal works here:
`~/.ssh/config`, agents, 1Password, Windows OpenSSH.

**1Password / prompting agents.** Every commit-and-push spawns a fresh `ssh`
process, and agents such as 1Password may ask for authorization each time.
On macOS/Linux, let ssh reuse one authenticated connection instead:

```
Host git.example.com
    Port 222
    User git
    ControlMaster auto
    ControlPath ~/.ssh/cm-%r@%h-%p
    ControlPersist 30m
```

The first connection prompts once; pushes within the next 30 minutes reuse
it silently. Windows OpenSSH has no ControlMaster; there, tick "remember"
in the agent prompt or use a key without an agent for this host
(`IdentityAgent none` + `IdentitiesOnly yes` in the host stanza).

## Implementation

* `src/slic3r/Utils/ProfileSync.{hpp,cpp}` — libgit2 wrapper (`Repo`) plus app
  glue: settings in `PrusaSlicer.ini` section `[profile_sync]`, debounced
  auto-commit, async runner, pull-on-launch.
* `src/slic3r/GUI/ProfileSyncDialog.{hpp,cpp}` — the dialog.
* Hooks: `Tab::save_preset`, `Tab::rename_preset`, `Tab::delete_preset`,
  `PhysicalPrinterDialog::OnOK` call `ProfileSync::notify_change()`.
  `GUI_App::reload_presets_from_disk()` re-reads presets after a pull (same
  path the configuration-snapshot restore uses).
* `deps/+libgit2/libgit2.cmake` builds libgit2 1.9.1 statically;
  `cmake/modules/Findlibgit2.cmake` finds it.

## CI

`.github/workflows/build.yml` builds macOS (x86_64 and arm64 DMGs), Windows
(zip) and Linux (AppImage) on GitHub-hosted runners and attaches them to a
GitHub release for tags `v*`. The GitHub repository is a push mirror of the
Gitea repository; `.gitea/workflows/track-upstream.yml` merges new upstream
`version_*` tags into `main` daily and opens an issue when that conflicts.
