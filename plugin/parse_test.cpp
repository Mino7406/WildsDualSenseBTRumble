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

// Exactly what lua's writeState() produces: the settings object, then the state
// object alone on its line, then the closing brace.
static std::string build(int seq, double low, double high, double lt, double rt) {
    char s[1024];
    snprintf(s, sizeof(s),
        "{\n"
        "  \"settings\": {\n"
        "    \"enabled\": true,\n"
        "    \"strength\": 0.4,\n"
        "    \"gamma\": 1.9,\n"
        "    \"gate\": 0.15,\n"
        "    \"lowScale\": 1,\n"
        "    \"highScale\": 1,\n"
        "    \"minOutput\": 0.004\n"
        "  },\n"
        "  \"state\": {\"sequence\": %d, \"low\": %.4f, \"high\": %.4f, "
        "\"ltrigger\": %.4f, \"rtrigger\": %.4f}\n"
        "}\n",
        seq, low, high, lt, rt);
    return s;
}

int main() {
    Levels out;

    // 1. the ordinary case
    auto p = writeTemp(build(752, 0.0, 0.0, 0.0, 0.0), L"t1.json");
    bool ok = readSignal(p, out);
    check("idle file parses", ok && out.sequence == 752 && out.low == 0.0 && out.high == 0.0);

    // 2. real levels, and the right field goes to the right motor
    p = writeTemp(build(41, 0.3125, 0.7500, 0.1, 0.2), L"t2.json");
    out = Levels{};
    ok = readSignal(p, out);
    check("levels land on the right motors", ok && out.sequence == 41 &&
          out.low > 0.3124 && out.low < 0.3126 && out.high > 0.7499 && out.high < 0.7501);

    // 3. "lowScale" must not be picked up when looking for "low"
    std::string settingsOnly =
        "{\n  \"settings\": {\n    \"lowScale\": 1,\n    \"highScale\": 1\n  }\n}\n";
    p = writeTemp(settingsOnly, L"t3.json");
    out = Levels{};
    check("settings alone are not read as levels", !readSignal(p, out));

    // 3b. the same trap on one line, where a substring search would be fooled
    p = writeTemp("{\"lowScale\": 0.9, \"highScale\": 0.8, \"sequence\": 5}\n", L"t3b.json");
    out = Levels{};
    check("lowScale/highScale are not read as low/high", !readSignal(p, out));

    // 4. torn write. lua truncates then writes, so a read can only ever catch a
    // prefix of the new file. Cut before "high" there is nothing usable, and the
    // plugin has to hold its last value rather than invent one.
    std::string torn = build(99, 0.5, 0.5, 0.0, 0.0);
    p = writeTemp(torn.substr(0, torn.find("\"high\"")), L"t4.json");
    out = Levels{};
    check("state cut before \"high\" is rejected", !readSignal(p, out));

    // Cut after it, low and high are both there and both in range.
    p = writeTemp(torn.substr(0, torn.find("\"high\"") + 14), L"t4b.json");
    out = Levels{};
    ok = readSignal(p, out);
    check("state cut after \"high\" stays in range", ok && out.sequence == 99 &&
          out.low >= 0.0 && out.low <= 1.0 && out.high >= 0.0 && out.high <= 1.0);

    // 5. a settings block long enough to push the state past the read buffer
    std::string big = build(7, 0.9, 0.1, 0.0, 0.0);
    std::string pad(4000, '\0');
    for (size_t i = 0; i < pad.size(); ++i) pad[i] = (i % 40 == 39) ? '\n' : '#';
    big = pad + big;
    p = writeTemp(big, L"t5.json");
    out = Levels{};
    ok = readSignal(p, out);
    check("state still found in an oversized file", ok && out.sequence == 7 &&
          out.high > 0.0999 && out.high < 0.1001);

    // 6. out-of-range values are clamped rather than passed through
    p = writeTemp("{\"state\": {\"sequence\": 5, \"low\": 9.0, \"high\": -2.0}}\n", L"t6.json");
    out = Levels{};
    ok = readSignal(p, out);
    check("levels are clamped to 0..1", ok && out.low == 1.0 && out.high == 0.0);

    // 7. a v1.0 settings file, which has no state at all, must not parse
    p = writeTemp("{\n  \"enabled\": true,\n  \"strength\": 0.4,\n  \"gamma\": 1.9\n}\n", L"t7.json");
    out = Levels{};
    check("a v1.0 settings file yields no levels", !readSignal(p, out));

    printf("\n%s\n", failures == 0 ? "all checks passed" : "CHECKS FAILED");
    return failures;
}
