///|/ Profile sync dialog (fork feature): settings, status, version history.
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#ifndef slic3r_GUI_ProfileSyncDialog_hpp_
#define slic3r_GUI_ProfileSyncDialog_hpp_

#include "GUI_Utils.hpp"
#include "slic3r/Utils/ProfileSync.hpp"

#include <wx/wx.h>
#include <wx/listctrl.h>

namespace Slic3r {
namespace GUI {

class ProfileSyncDialog : public DPIDialog
{
public:
    ProfileSyncDialog(wxWindow *parent);

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    void build_settings(wxSizer *sizer);
    void build_status(wxSizer *sizer);
    void build_history(wxSizer *sizer);

    ProfileSync::Settings settings_from_ui() const;
    void apply_settings();
    void refresh_status();
    void refresh_history();
    void set_busy(bool busy, const wxString &what = wxEmptyString);
    void run(std::function<ProfileSync::SyncResult()> fn, const wxString &what);
    void on_restore();
    void on_show_changes();

    // settings
    wxCheckBox *m_enabled = nullptr;
    wxTextCtrl *m_remote = nullptr;
    wxTextCtrl *m_branch = nullptr;
    wxTextCtrl *m_machine = nullptr;
    wxCheckBox *m_auto_commit = nullptr;
    wxCheckBox *m_auto_push = nullptr;
    wxCheckBox *m_pull_on_launch = nullptr;
    wxCheckBox *m_track_physical = nullptr;
    wxCheckBox *m_track_vendor = nullptr;
    // status
    wxStaticText *m_status_text = nullptr;
    wxButton *m_btn_sync = nullptr;
    wxButton *m_btn_pull = nullptr;
    wxButton *m_btn_push = nullptr;
    wxButton *m_btn_commit = nullptr;
    wxButton *m_btn_force_push = nullptr;
    wxButton *m_btn_force_pull = nullptr;
    // history
    wxListCtrl *m_list = nullptr;
    wxTextCtrl *m_files = nullptr;
    wxButton *m_btn_restore = nullptr;
    wxButton *m_btn_changes = nullptr;
    std::vector<ProfileSync::Commit> m_commits;
    bool m_busy = false;
};

} // namespace GUI
} // namespace Slic3r

#endif
