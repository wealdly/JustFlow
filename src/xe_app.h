// justflow_xe's default mode: a tray app that engages frame generation automatically on the
// foreground window while its content runs at a steady rate worth generating for. See xe_app.cpp.
#pragma once
#include <string>

int RunTrayApp(const std::wstring& exe_dir);
