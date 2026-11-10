#/bin/sh
/usr/bin/gtk-update-icon-cache %{_datadir}/icons/hicolor &>/dev/null || :
/usr/bin/xdg-mime default %{_datadir}/share/applications/omdscheme.desktop x-scheme-handler/omd
/usr/bin/update-desktop-database
