#pragma once

namespace archivefetch_diag {

using InstallerLifecycle = int (*)();
using InstallAll = int (*)(void*);
using InstallTitleDir = int (*)(const char*, const char*, void*);

struct InstallerExports {
    InstallerLifecycle initialize;
    InstallAll install_all;
    InstallTitleDir title_dir_by_symbol;
    InstallTitleDir title_dir_by_nid;
    InstallTitleDir title_dir_by_handle_symbol;
    int module_handle_result;
    unsigned module_handle;
};

// Looks up AppInstUtil entry points without calling any of them; logs each lookup.
InstallerExports resolve_installer();

// Writes /user/app/<title>/sce_sys/{param.json,icon0.png}; unchanged files are left alone.
bool stage_shortcut();

// Always attempts registration, even when staging found nothing to change.
bool register_shortcut(const InstallerExports& exports);

}
