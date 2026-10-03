// justflow_xe's default mode: a tray app that engages frame generation automatically on the
// foreground window while its content runs at a steady 25-100 fps (118 with the output locked to
// the display rate). See xe_app.cpp.
#pragma once
#include <string>

int RunTrayApp(const std::wstring& exe_dir);
