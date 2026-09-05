///|/ Profile sync dialog (fork feature).
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#include "ProfileSyncDialog.hpp"

#include "GUI_App.hpp"
#include "libslic3r/AppConfig.hpp"
#include "GUI.hpp"
#include "I18N.hpp"
#include "format.hpp"
#include "MsgDialog.hpp"
#include "wxExtensions.hpp"

#include <wx/statline.h>
#include <wx/datetime.h>

namespace Slic3r {
namespace GUI {

using namespace ProfileSync;

ProfileSyncDialog::ProfileSyncDialog(wxWindow *parent)
    : DPIDialog(parent, wxID_ANY, _L("Profile Sync"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    SetFont(wxGetApp().normal_font());
    wxBoxSizer *top = new wxBoxSizer(wxVERTICAL);

    wxStaticText *intro = new wxStaticText(this, wxID_ANY,
        _L("Keeps your profiles (print, filament and printer presets) in a git repository and syncs them with a remote, "
           "so you can use the same profiles on several computers and go back to earlier versions."));
    intro->Wrap(wxGetApp().em_unit() * 70);
    top->Add(intro, 0, wxALL | wxEXPAND, 10);

    build_settings(top);
    top->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 10);
    build_status(top);
    top->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 10);
    build_history(top);

    wxStdDialogButtonSizer *btns = CreateStdDialogButtonSizer(wxCLOSE);
    top->Add(btns, 0, wxEXPAND | wxALL, 10);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { apply_settings(); EndModal(wxID_CLOSE); }, wxID_CLOSE);
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent &) { apply_settings(); EndModal(wxID_CLOSE); });

    SetSizer(top);
    SetMinSize(wxSize(wxGetApp().em_unit() * 75, wxGetApp().em_unit() * 60));
    Fit();
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);

    refresh_status();
    refresh_history();
}

void ProfileSyncDialog::build_settings(wxSizer *sizer)
{
    Settings s = settings();
    wxStaticBoxSizer *box = new wxStaticBoxSizer(wxVERTICAL, this, _L("Settings"));
    wxFlexGridSizer *grid = new wxFlexGridSizer(2, 5, 10);
    grid->AddGrowableCol(1);

    m_enabled = new wxCheckBox(this, wxID_ANY, _L("Enable profile sync"));
    m_enabled->SetValue(s.enabled);
    box->Add(m_enabled, 0, wxALL, 5);

    auto add_row = [&](const wxString &label, wxWindow *ctrl, const wxString &tip) {
        grid->Add(new wxStaticText(this, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
        ctrl->SetToolTip(tip);
        grid->Add(ctrl, 1, wxEXPAND);
    };
    m_remote = new wxTextCtrl(this, wxID_ANY, from_u8(s.remote_url));
    add_row(_L("Remote URL:"), m_remote, _L("Git remote, e.g. git@git.example.com:you/prusaslicer-profiles.git or ssh://git@host:port/you/repo.git. SSH remotes use your system ssh keys and agent."));
    // Pull/Push/etc. depend on having a remote; react as the user types instead of only on reopen.
    m_remote->Bind(wxEVT_TEXT, [this](wxCommandEvent &) { if (m_status_text) refresh_status(); });
    m_enabled->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { if (m_status_text) refresh_status(); });
    m_branch = new wxTextCtrl(this, wxID_ANY, from_u8(s.branch));
    add_row(_L("Branch:"), m_branch, _L("Branch to sync (default: main)."));
    m_machine = new wxTextCtrl(this, wxID_ANY, from_u8(s.machine_name));
    add_row(_L("This machine:"), m_machine, _L("Name used as commit author and for local copies of conflicting profiles."));
    box->Add(grid, 0, wxEXPAND | wxALL, 5);

    wxFlexGridSizer *checks = new wxFlexGridSizer(2, 2, 10);
    m_auto_commit = new wxCheckBox(this, wxID_ANY, _L("Record a version whenever a profile is saved"));
    m_auto_commit->SetValue(s.auto_commit);
    m_auto_push = new wxCheckBox(this, wxID_ANY, _L("Push to remote after recording"));
    m_auto_push->SetValue(s.auto_push);
    m_pull_on_launch = new wxCheckBox(this, wxID_ANY, _L("Pull from remote on startup"));
    m_pull_on_launch->SetValue(s.pull_on_launch);
    m_track_vendor = new wxCheckBox(this, wxID_ANY, _L("Include vendor (system) bundles"));
    m_track_vendor->SetValue(s.track_vendor);
    m_track_physical = new wxCheckBox(this, wxID_ANY, _L("Include physical printers (API keys stay on this machine)"));
    m_track_physical->SetValue(s.track_physical_printers);
    m_track_physical->SetToolTip(_L("Physical printer profiles are synced, but print-host API keys and passwords are stripped before they enter the repository and kept in a local file (profile_sync_secrets.ini). Enter the key once on each machine."));
    for (wxCheckBox *c : { m_auto_commit, m_auto_push, m_pull_on_launch, m_track_vendor, m_track_physical })
        checks->Add(c, 0, wxALL, 2);
    box->Add(checks, 0, wxEXPAND | wxALL, 5);

    sizer->Add(box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
}

void ProfileSyncDialog::build_status(wxSizer *sizer)
{
    wxBoxSizer *row = new wxBoxSizer(wxHORIZONTAL);
    m_status_text = new wxStaticText(this, wxID_ANY, "");
    row->Add(m_status_text, 1, wxALIGN_CENTER_VERTICAL | wxALL, 5);

    m_btn_commit = new wxButton(this, wxID_ANY, _L("Record version"));
    m_btn_pull   = new wxButton(this, wxID_ANY, _L("Pull"));
    m_btn_push   = new wxButton(this, wxID_ANY, _L("Push"));
    m_btn_sync   = new wxButton(this, wxID_ANY, _L("Sync now"));
    for (wxButton *b : { m_btn_commit, m_btn_pull, m_btn_push, m_btn_sync }) row->Add(b, 0, wxALL, 3);

    // Explicit "make one side win" actions, for when the merge policy is not what you want.
    wxBoxSizer *row2 = new wxBoxSizer(wxHORIZONTAL);
    row2->AddStretchSpacer();
    m_btn_force_push = new wxButton(this, wxID_ANY, _L("Overwrite remote with my profiles"));
    m_btn_force_push->SetToolTip(_L("Force the remote to match this machine. Other machines keep their differing profiles as local copies on their next pull."));
    m_btn_force_pull = new wxButton(this, wxID_ANY, _L("Replace my profiles with remote"));
    m_btn_force_pull->SetToolTip(_L("Make this machine match the remote. Your current profiles are recorded first, so this can be undone from the history."));
    row2->Add(m_btn_force_push, 0, wxALL, 3);
    row2->Add(m_btn_force_pull, 0, wxALL, 3);
    m_btn_force_push->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        MessageDialog dlg(this, _L("Overwrite the remote with this machine's profiles?\n\nProfiles that only exist on the remote will be removed from it. Other machines keep local copies of anything that differs."), _L("Overwrite remote"), wxICON_QUESTION | wxYES_NO);
        if (dlg.ShowModal() != wxID_YES) return;
        Settings s = settings_from_ui();
        run([s]() -> SyncResult { repo().ensure_initialized(s); return repo().overwrite_remote(s); }, _L("Overwriting remote"));
    });
    m_btn_force_pull->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        MessageDialog dlg(this, _L("Replace this machine's profiles with the remote version?\n\nYour current profiles are recorded in the history first, so you can restore them later."), _L("Replace local profiles"), wxICON_QUESTION | wxYES_NO);
        if (dlg.ShowModal() != wxID_YES) return;
        Settings s = settings_from_ui();
        run([s]() -> SyncResult { repo().ensure_initialized(s); return repo().replace_local_with_remote(s); }, _L("Replacing local profiles"));
    });

    m_btn_commit->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        wxTextEntryDialog dlg(this, _L("Describe this version"), _L("Record version"), _L("Manual snapshot"));
        wxGetApp().UpdateDlgDarkUI(&dlg);
        if (dlg.ShowModal() != wxID_OK) return;
        std::string msg = into_u8(dlg.GetValue());
        Settings s = settings_from_ui();
        run([s, msg] { repo().ensure_initialized(s); SyncResult r; r.ok = true; r.message = repo().commit_all(s, msg) ? "Version recorded." : "No changes to record."; return r; }, _L("Recording version"));
    });
    m_btn_pull->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { Settings s = settings_from_ui(); run([s] { repo().ensure_initialized(s); return repo().pull(s); }, _L("Pulling")); });
    m_btn_push->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { Settings s = settings_from_ui(); run([s] { repo().ensure_initialized(s); repo().commit_all(s, "Changes on " + s.machine_name); return repo().push(s); }, _L("Pushing")); });
    m_btn_sync->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { Settings s = settings_from_ui(); run([s] { repo().ensure_initialized(s); return repo().sync(s, "Sync from " + s.machine_name); }, _L("Syncing")); });

    sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);
    sizer->Add(row2, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);
}

void ProfileSyncDialog::build_history(wxSizer *sizer)
{
    wxStaticBoxSizer *box = new wxStaticBoxSizer(wxVERTICAL, this, _L("Version history"));
    m_list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(-1, wxGetApp().em_unit() * 18), wxLC_REPORT | wxLC_SINGLE_SEL);
    m_list->AppendColumn(_L("Date"),    wxLIST_FORMAT_LEFT, wxGetApp().em_unit() * 14);
    m_list->AppendColumn(_L("Machine"), wxLIST_FORMAT_LEFT, wxGetApp().em_unit() * 10);
    m_list->AppendColumn(_L("Description"), wxLIST_FORMAT_LEFT, wxGetApp().em_unit() * 36);
    m_list->AppendColumn(_L("Files"),   wxLIST_FORMAT_RIGHT, wxGetApp().em_unit() * 5);
    box->Add(m_list, 1, wxEXPAND | wxALL, 5);

    m_files = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(-1, wxGetApp().em_unit() * 8), wxTE_MULTILINE | wxTE_READONLY);
    box->Add(m_files, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);

    wxBoxSizer *row = new wxBoxSizer(wxHORIZONTAL);
    m_btn_changes = new wxButton(this, wxID_ANY, _L("Show changes"));
    m_btn_restore = new wxButton(this, wxID_ANY, _L("Restore this version"));
    row->AddStretchSpacer();
    row->Add(m_btn_changes, 0, wxALL, 3);
    row->Add(m_btn_restore, 0, wxALL, 3);
    box->Add(row, 0, wxEXPAND);

    m_list->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent &e) {
        long i = e.GetIndex();
        if (i < 0 || size_t(i) >= m_commits.size()) return;
        wxString t;
        for (const FileChange &f : m_commits[i].files) t += wxString::Format("%c  %s\n", f.status, from_u8(f.path));
        m_files->SetValue(t);
        m_btn_restore->Enable(i > 0 && !m_busy);
        m_btn_changes->Enable(!m_busy);
    });
    m_btn_changes->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_show_changes(); });
    m_btn_restore->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_restore(); });
    m_btn_restore->Disable(); m_btn_changes->Disable();

    sizer->Add(box, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
}

Settings ProfileSyncDialog::settings_from_ui() const
{
    Settings s = settings();
    s.enabled        = m_enabled->GetValue();
    s.remote_url     = into_u8(m_remote->GetValue()); boost::trim(s.remote_url);
    s.branch         = into_u8(m_branch->GetValue()); boost::trim(s.branch); if (s.branch.empty()) s.branch = "main";
    s.machine_name   = into_u8(m_machine->GetValue()); boost::trim(s.machine_name); if (s.machine_name.empty()) s.machine_name = default_machine_name();
    s.auto_commit    = m_auto_commit->GetValue();
    s.auto_push      = m_auto_push->GetValue();
    s.pull_on_launch = m_pull_on_launch->GetValue();
    s.track_vendor   = m_track_vendor->GetValue();
    s.track_physical_printers = m_track_physical->GetValue();
    return s;
}

void ProfileSyncDialog::apply_settings()
{
    Settings s = settings_from_ui();
    set_settings(s);
    wxGetApp().app_config->save();
}

void ProfileSyncDialog::refresh_status()
{
    Settings s = settings_from_ui();
    Status st = repo().status(s);
    wxString text;
    if (!st.repo_exists)         text = _L("Not initialized yet. Enable sync and press \"Sync now\" or \"Record version\".");
    else if (!st.last_error.empty()) text = from_u8(st.last_error);
    else {
        text = format_wxstr(_L("Version %1%"), st.head_id.substr(0, 8));
        if (st.uncommitted) text += format_wxstr(_L(", %1% unrecorded changes"), st.uncommitted);
        if (st.has_remote)  text += format_wxstr(_L(", %1% ahead / %2% behind remote"), st.ahead, st.behind);
        else                text += _L(", no remote configured");
    }
    m_status_text->SetLabel(text);
    bool has_remote = !s.remote_url.empty();
    m_btn_pull->Enable(has_remote && !m_busy);
    m_btn_push->Enable(has_remote && !m_busy);
    m_btn_force_push->Enable(has_remote && !m_busy);
    m_btn_force_pull->Enable(has_remote && !m_busy);
    m_btn_sync->Enable(!m_busy);
    m_btn_commit->Enable(!m_busy);
    Layout();
}

void ProfileSyncDialog::refresh_history()
{
    m_list->DeleteAllItems();
    m_files->Clear();
    m_commits.clear();
    try { m_commits = repo().log(300); } catch (const std::exception &e) { m_files->SetValue(from_u8(e.what())); }
    long i = 0;
    for (const Commit &c : m_commits) {
        wxDateTime dt(static_cast<time_t>(c.time));
        long row = m_list->InsertItem(i++, dt.Format("%Y-%m-%d %H:%M"));
        m_list->SetItem(row, 1, from_u8(c.author));
        m_list->SetItem(row, 2, from_u8(c.summary));
        m_list->SetItem(row, 3, wxString::Format("%zu", c.files.size()));
    }
}

void ProfileSyncDialog::set_busy(bool busy, const wxString &what)
{
    m_busy = busy;
    if (busy) m_status_text->SetLabel(what + "...");
    for (wxButton *b : { m_btn_commit, m_btn_pull, m_btn_push, m_btn_sync, m_btn_force_push, m_btn_force_pull, m_btn_restore, m_btn_changes }) b->Enable(!busy);
    if (!busy) { refresh_status(); refresh_history(); }
}

void ProfileSyncDialog::run(std::function<SyncResult()> fn, const wxString &what)
{
    apply_settings();
    set_busy(true, what);
    run_async(fn, [this](SyncResult r) {
        set_busy(false);
        if (!r.ok) { show_error(this, _L("Profile sync failed:") + "\n" + from_u8(r.message)); return; }
        if (r.pulled_changes) wxGetApp().reload_presets_from_disk();
        wxString msg = from_u8(r.message);
        if (r.merged_conflicts) msg += "\n" + format_wxstr(_L("%1% profiles changed on both sides; the local versions were kept as copies named \"... (%2%)\"."), r.merged_conflicts, from_u8(settings().machine_name));
        m_status_text->SetLabel(msg);
    });
}

void ProfileSyncDialog::on_restore()
{
    long i = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (i < 0 || size_t(i) >= m_commits.size()) return;
    const Commit &c = m_commits[i];
    wxDateTime dt(static_cast<time_t>(c.time));
    MessageDialog dlg(this, format_wxstr(_L("Restore all profiles to the version from %1% (\"%2%\")?\n\nYour current profiles are recorded first, so this can be undone from the history."), dt.Format("%Y-%m-%d %H:%M"), from_u8(c.summary)),
                      _L("Restore version"), wxICON_QUESTION | wxYES_NO);
    if (dlg.ShowModal() != wxID_YES) return;
    Settings s = settings_from_ui();
    std::string id = c.id;
    run([s, id]() -> SyncResult { return repo().restore_commit(s, id); }, _L("Restoring"));
}

void ProfileSyncDialog::on_show_changes()
{
    long i = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (i < 0 || size_t(i) >= m_commits.size()) return;
    std::string text;
    try { text = repo().diff_text(m_commits[i].id); } catch (const std::exception &e) { text = e.what(); }
    if (text.empty()) text = "(no textual changes)";
    wxDialog dlg(this, wxID_ANY, _L("Changes in version ") + from_u8(m_commits[i].short_id), wxDefaultPosition, wxSize(wxGetApp().em_unit() * 80, wxGetApp().em_unit() * 50), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    wxBoxSizer *sz = new wxBoxSizer(wxVERTICAL);
    wxTextCtrl *tc = new wxTextCtrl(&dlg, wxID_ANY, from_u8(text), wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
    tc->SetFont(wxGetApp().code_font());
    sz->Add(tc, 1, wxEXPAND | wxALL, 5);
    sz->Add(dlg.CreateStdDialogButtonSizer(wxOK), 0, wxEXPAND | wxALL, 5);
    dlg.SetSizer(sz);
    wxGetApp().UpdateDlgDarkUI(&dlg);
    dlg.ShowModal();
}

void ProfileSyncDialog::on_dpi_changed(const wxRect &)
{
    const int em = wxGetApp().em_unit();
    SetMinSize(wxSize(em * 75, em * 60));
    Fit();
    Refresh();
}

} // namespace GUI
} // namespace Slic3r
