// Standalone tray smoke test: creates the icon, feeds profiles/hotkeys/state, prints events for 10 s.
// cl /std:c++17 /EHsc /MD /W3 /permissive- /utf-8 tools\tray_test.cpp src\tray.cpp user32.lib shell32.lib gdi32.lib comctl32.lib
#include "../src/tray.h"
#include <cstdio>

int main()
{
    Tray* t = TrayCreate(L"JustFlow (test)", nullptr);
    if (!t) { puts("TrayCreate failed"); return 1; }
    const wchar_t* profiles[] = { L"WoW", L"Generic", L"Cinematic" };
    TraySetProfiles(t, profiles, 3);
    const wchar_t* hk[] = { L"F9", L"F10", L"F11", L"F8", L"Ctrl+F12" };
    TraySetHotkeys(t, hk);
    TrayState s; s.nr_on = true; s.fg_on = true; s.fg_multiplier = 3; s.wipe_mode = 0; s.profile_index = 0;
    s.status = L"WoW  90->180 fps  age 12 ms";
    TraySetState(t, s);
    TrayNotify(t, L"JustFlow", L"Profile switched: WoW");
    puts("tray up; 10 s of polling (right-click the icon)");
    for (int i = 0; i < 100; ++i)
    {
        TrayEvent ev; int arg;
        while (TrayPoll(t, ev, arg))
        {
            printf("event %d arg %d\n", (int)ev, arg);
            if (ev == TrayHotkeys) { wchar_t h[5][32]; TrayGetHotkeys(t, h); for (auto& x : h) wprintf(L"  hk %s\n", x); }
            if (ev == TrayQuit) i = 100;
        }
        Sleep(100);
    }
    TrayDestroy(t);
    puts("destroyed cleanly");
    return 0;
}
