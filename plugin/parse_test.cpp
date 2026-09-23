// Checks the plugin's reader against the exact bytes the lua writes.
// Compiled against plugin.cpp itself, so it cannot drift from the real parser.

#define main plugin_main_unused
#include "plugin.cpp"
#undef main

#include <cstdio>
#include <string>

static int failures = 0;

static std::wstring writeTemp(const std::string& body, const wchar_t* name) {
    std::wstring path = std::wstring(L"./") + name;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD w = 0;
    WriteFile(f, body.data(), (DWORD)body.size(), &w, nullptr);
    CloseHandle(f);
    return path;
}

static void check(const char* what, bool ok) {
    printf("%-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

// Exactly what lua's writeState() produces: FILE_HEADER, then "%-9s = %s" per
// setting in SETTING_ORDER, then a blank line and the named state line.
static std::string build(int seq, double low, double high, double lt, double rt) {
    char s[1024];
    snprintf(s, sizeof(s),
        "# Wilds DualSense BT Rumble\n"
        "#\n"
        "# Written by the mod - the settings below are what the in-game menu saved,\n"
        "# and the state line at the bottom is the live motor level the plugin reads.\n"
        "# Editing the settings by hand works while the game is closed.\n"
        "\n"
        "enabled   = 1\n"
        "strength  = 0.4\n"
        "gamma     = 1.9\n"
        "gate      = 0.15\n"
        "lowScale  = 1.0\n"
        "highScale = 1.0\n"
        "minOutput = 0.004\n"
        "\nstate sequence=%d low=%.4f high=%.4f ltrigger=%.4f rtrigger=%.4f\n",
        seq, low, high, lt, rt);
    return s;
}

int main() {
    Levels out;

    // 1. the ordinary case
    auto p = writeTemp(build(752, 0.0, 0.0, 0.0, 0.0), L"t1.txt");
    bool ok = readSignal(p, out);
    check("idle file parses", ok && out.sequence == 752 && out.low == 0.0 && out.high == 0.0);

    // 2. real levels, and the right field goes to the right motor
    p = writeTemp(build(41, 0.3125, 0.7500, 0.1, 0.2), L"t2.txt");
    out = Levels{};
    ok = readSignal(p, out);
    check("levels land on the right motors", ok && out.sequence == 41 &&
          out.low > 0.3124 && out.low < 0.3126 && out.high > 0.7499 && out.high < 0.7501);

    // 3. "lowScale" must not be picked up when looking for "low"
    std::string settingsOnly =
        "enabled   = 1\nlowScale  = 1.0\nhighScale = 1.0\nminOutput = 0.004\n";
    p = writeTemp(settingsOnly, L"t3.txt");
    out = Levels{};
    check("settings alone are not read as levels", !readSignal(p, out));

    // 4. torn write. lua truncates then writes, so a read can only ever catch a
    // prefix of the new file. Cut before "high=" there is nothing usable, and the
    // plugin has to hold its last value rather than invent one.
    std::string torn = build(99, 0.5, 0.5, 0.0, 0.0);
    p = writeTemp(torn.substr(0, torn.find("high=")), L"t4.txt");
    out = Levels{};
    check("state line cut before high= is rejected", !readSignal(p, out));

    // Cut after it, low and high are both there and both in range - at worst a
    // number lost a trailing digit, which is one frame slightly off.
    p = writeTemp(torn.substr(0, torn.find("high=") + 8), L"t4b.txt");
    out = Levels{};
    ok = readSignal(p, out);
    check("state line cut after high= stays in range", ok && out.sequence == 99 &&
          out.low >= 0.0 && out.low <= 1.0 && out.high >= 0.0 && out.high <= 1.0);

    // 5. v1.0 format still works, for a half-updated install
    p = writeTemp("13 0.2500 0.6000 0.0000 0.0000\n", L"t5.txt");
    out = Levels{};
    ok = readSignal(p, out);
    check("v1.0 bare-number line still parses", ok && out.sequence == 13 &&
          out.low > 0.2499 && out.low < 0.2501);
    check("v1.0 line is not mistaken for a v1.1 one", ok && !out.named);

    // The named flag gates deleting the v1.0 json, so it has to be right.
    p = writeTemp(build(1, 0.0, 0.0, 0.0, 0.0), L"t5b.txt");
    out = Levels{};
    ok = readSignal(p, out);
    check("v1.1 line is recognised as named", ok && out.named);

    // 6. a settings block long enough to push the state line past the buffer
    std::string big = build(7, 0.9, 0.1, 0.0, 0.0);
    std::string pad(4000, '\0');
    for (size_t i = 0; i < pad.size(); ++i) pad[i] = (i % 40 == 39) ? '\n' : '#';
    big = pad + big;
    p = writeTemp(big, L"t6.txt");
    out = Levels{};
    ok = readSignal(p, out);
    check("state line still found in an oversized file", ok && out.sequence == 7 &&
          out.high > 0.0999 && out.high < 0.1001);

    // 7. out-of-range values are clamped rather than passed through
    p = writeTemp("\nstate sequence=5 low=9.0 high=-2.0 ltrigger=0 rtrigger=0\n", L"t7.txt");
    out = Levels{};
    ok = readSignal(p, out);
    check("levels are clamped to 0..1", ok && out.low == 1.0 && out.high == 0.0);

    printf("\n%s\n", failures == 0 ? "all checks passed" : "CHECKS FAILED");
    return failures;
}
