// Wilds DualSense BT Rumble - REFramework plugin.
//
// The companion lua mod reads Capcom's own motor waveform inside the game and
// publishes the current levels to reframework/data/WildsDualSenseBTRumble.txt, in
// the same file it keeps its settings in. This plugin lives in the same process,
// picks those levels up, and writes DualSense Bluetooth
// output reports straight to the controller - the path Steam Input leaves silent
// because Wilds only ever drives the USB-only haptic actuators.
//
// Deliberately small: all the game-side logic stays in lua, where it is easy to
// change. This only moves bytes to the pad.

#include <windows.h>
#include <setupapi.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "reframework/API.h"

namespace {

// ---------------------------------------------------------------- logging
// Goes to REFramework's own log rather than a file of this mod's own: there is
// already one log everybody knows to open, and a mod that works should not leave
// a second one lying in the data folder. Only changes of state are written, so a
// healthy session says one line and then keeps quiet.

std::wstring gameDir() {
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return L"";
    std::wstring path(exePath);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L"";
    path.resize(slash + 1);
    return path;
}

void (*g_log)(const char*, ...) = nullptr;   // REFramework's, handed over at init

void logLine(const char* format, ...) {
    if (g_log == nullptr) return;

    char message[512];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    g_log("[WildsDualSenseBTRumble] %s", message);
}

// The pad is looked for once a second. Saying so once a second would be useless
// noise, so a missing pad is announced on the way in and not again until one has
// actually been found.
bool g_announcedMissing = false;

// ---------------------------------------------------------------- HID types
// Declared here so the build does not depend on where a toolchain keeps the DDK
// headers. Layouts are fixed by the Windows HID API.

struct DsHiddAttributes {
    ULONG  Size;
    USHORT VendorID;
    USHORT ProductID;
    USHORT VersionNumber;
};

struct DsHidpCaps {
    USHORT Usage;
    USHORT UsagePage;
    USHORT InputReportByteLength;
    USHORT OutputReportByteLength;
    USHORT FeatureReportByteLength;
    USHORT Reserved[17];
    USHORT NumberLinkCollectionNodes;
    USHORT NumberInputButtonCaps;
    USHORT NumberInputValueCaps;
    USHORT NumberInputDataIndices;
    USHORT NumberOutputButtonCaps;
    USHORT NumberOutputValueCaps;
    USHORT NumberOutputDataIndices;
    USHORT NumberFeatureButtonCaps;
    USHORT NumberFeatureValueCaps;
    USHORT NumberFeatureDataIndices;
};

using FnGetHidGuid        = void    (WINAPI*)(LPGUID);
using FnGetAttributes     = BOOLEAN (WINAPI*)(HANDLE, DsHiddAttributes*);
using FnGetPreparsedData  = BOOLEAN (WINAPI*)(HANDLE, PVOID*);
using FnFreePreparsedData = BOOLEAN (WINAPI*)(PVOID);
using FnGetCaps           = LONG    (WINAPI*)(PVOID, DsHidpCaps*);

struct HidApi {
    HMODULE module = nullptr;
    FnGetHidGuid        GetHidGuid        = nullptr;
    FnGetAttributes     GetAttributes     = nullptr;
    FnGetPreparsedData  GetPreparsedData  = nullptr;
    FnFreePreparsedData FreePreparsedData = nullptr;
    FnGetCaps           GetCaps           = nullptr;

    bool load() {
        module = LoadLibraryW(L"hid.dll");
        if (module == nullptr) return false;
        GetHidGuid        = (FnGetHidGuid)       GetProcAddress(module, "HidD_GetHidGuid");
        GetAttributes     = (FnGetAttributes)    GetProcAddress(module, "HidD_GetAttributes");
        GetPreparsedData  = (FnGetPreparsedData) GetProcAddress(module, "HidD_GetPreparsedData");
        FreePreparsedData = (FnFreePreparsedData)GetProcAddress(module, "HidD_FreePreparsedData");
        GetCaps           = (FnGetCaps)          GetProcAddress(module, "HidP_GetCaps");
        return GetHidGuid && GetAttributes && GetPreparsedData && FreePreparsedData && GetCaps;
    }
};

// ---------------------------------------------------------------- cfgmgr32
// Steam Input hides physical controllers from the game process by filtering HID
// enumeration. It hooks SetupAPI; cfgmgr32 is a separate path to the same device
// list, so we try that first and keep SetupAPI as a fallback.

using FnCmListSize = LONG (WINAPI*)(PULONG, LPGUID, wchar_t*, ULONG);
using FnCmList     = LONG (WINAPI*)(LPGUID, wchar_t*, wchar_t*, ULONG, ULONG);

struct CfgMgrApi {
    HMODULE      module = nullptr;
    FnCmListSize listSize = nullptr;
    FnCmList     list = nullptr;

    bool load() {
        module = LoadLibraryW(L"cfgmgr32.dll");
        if (module == nullptr) return false;
        listSize = (FnCmListSize)GetProcAddress(module, "CM_Get_Device_Interface_List_SizeW");
        list     = (FnCmList)    GetProcAddress(module, "CM_Get_Device_Interface_ListW");
        return listSize && list;
    }
};

// ---------------------------------------------------------------- native open
// Steam Input also blocks CreateFileW on controller devices inside the game
// process. It hooks the Win32 layer, so going straight to ntdll gets underneath
// that. Structures are declared locally to avoid pulling in winternl.h.

struct NtUnicodeString {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
};

struct NtObjectAttributes {
    ULONG            Length;
    HANDLE           RootDirectory;
    NtUnicodeString* ObjectName;
    ULONG            Attributes;
    PVOID            SecurityDescriptor;
    PVOID            SecurityQualityOfService;
};

struct NtIoStatusBlock {
    union { LONG Status; PVOID Pointer; };
    ULONG_PTR Information;
};

using FnNtCreateFile = LONG (NTAPI*)(PHANDLE, ACCESS_MASK, NtObjectAttributes*, NtIoStatusBlock*,
                                     PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);

FnNtCreateFile g_ntCreateFile = nullptr;

bool loadNtCreateFile() {
    if (g_ntCreateFile != nullptr) return true;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) return false;
    g_ntCreateFile = (FnNtCreateFile)GetProcAddress(ntdll, "NtCreateFile");
    return g_ntCreateFile != nullptr;
}

// Opens a device by its interface path without going through CreateFileW.
HANDLE nativeOpen(const std::wstring& win32Path) {
    if (!loadNtCreateFile()) return INVALID_HANDLE_VALUE;

    // \\?\foo is the Win32 spelling of the NT object path \??\foo
    std::wstring ntPath = win32Path;
    if (ntPath.compare(0, 4, L"\\\\?\\") == 0) ntPath = L"\\??\\" + ntPath.substr(4);
    else if (ntPath.compare(0, 4, L"\\\\.\\") == 0) ntPath = L"\\??\\" + ntPath.substr(4);
    else return INVALID_HANDLE_VALUE;

    NtUnicodeString name{};
    name.Length        = (USHORT)(ntPath.size() * sizeof(wchar_t));
    name.MaximumLength = (USHORT)((ntPath.size() + 1) * sizeof(wchar_t));
    name.Buffer        = const_cast<PWSTR>(ntPath.c_str());

    NtObjectAttributes attributes{};
    attributes.Length     = sizeof(attributes);
    attributes.ObjectName = &name;
    attributes.Attributes = 0x40;   // OBJ_CASE_INSENSITIVE

    NtIoStatusBlock status{};
    HANDLE handle = nullptr;

    const ACCESS_MASK access = GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE;
    const ULONG share        = FILE_SHARE_READ | FILE_SHARE_WRITE;
    const ULONG disposition  = 1;      // FILE_OPEN
    const ULONG options      = 0x20;   // FILE_SYNCHRONOUS_IO_NONALERT

    const LONG result = g_ntCreateFile(&handle, access, &attributes, &status, nullptr,
                                       FILE_ATTRIBUTE_NORMAL, share, disposition, options, nullptr, 0);
    if (result < 0 || handle == nullptr) return INVALID_HANDLE_VALUE;
    return handle;
}

// CreateFileW first, then the native path when it is being blocked.
HANDLE openDevice(const std::wstring& path, bool* usedNative) {
    if (usedNative) *usedNative = false;
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    if (handle != INVALID_HANDLE_VALUE) return handle;

    const DWORD error = GetLastError();
    handle = nativeOpen(path);
    if (handle != INVALID_HANDLE_VALUE) {
        if (usedNative) *usedNative = true;
        logLine("CreateFileW refused (err=%lu); NtCreateFile succeeded", error);
    }
    return handle;
}

// ---------------------------------------------------------------- the report

constexpr USHORT kSonyVid            = 0x054C;
constexpr USHORT kDualSensePid       = 0x0CE6;
constexpr USHORT kBluetoothInputLen  = 78;    // USB reports 64
constexpr USHORT kBluetoothOutputLen = 547;   // what Windows reports for a DualSense on Bluetooth
constexpr int    kReportMinLength    = 78;
constexpr BYTE   kReportId           = 0x31;
constexpr BYTE   kReportTag          = 0x10;
constexpr BYTE   kCrcSeed            = 0xA2;  // seed byte for output reports
constexpr BYTE   kValidFlag0         = 0x03;  // HAPTICS_SELECT | COMPATIBLE_VIBRATION

uint32_t crc32(uint32_t crc, const BYTE* data, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) {
            crc = (crc & 1) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return crc;
}

void buildReport(std::vector<BYTE>& out, BYTE motorLeft, BYTE motorRight, BYTE sequence, int totalLength) {
    const int length = totalLength > kReportMinLength ? totalLength : kReportMinLength;
    out.assign((size_t)length, 0);

    out[0]  = kReportId;
    out[1]  = (BYTE)((sequence & 0x0F) << 4);
    out[2]  = kReportTag;
    out[3]  = kValidFlag0;
    out[5]  = motorRight;
    out[6]  = motorLeft;

    const BYTE seed = kCrcSeed;
    uint32_t crc = crc32(0xFFFFFFFFu, &seed, 1);
    crc = ~crc32(crc, out.data(), kReportMinLength - 4);
    out[74] = (BYTE)(crc);
    out[75] = (BYTE)(crc >> 8);
    out[76] = (BYTE)(crc >> 16);
    out[77] = (BYTE)(crc >> 24);
}

// ---------------------------------------------------------------- controller

// True when the interface path itself names a DualSense, which survives Steam
// lying about a device's HID attributes.
std::wstring lowercased(const std::wstring& text) {
    std::wstring lower;
    lower.reserve(text.size());
    for (wchar_t c : text) lower.push_back((wchar_t)towlower(c));
    return lower;
}

bool pathLooksLikeDualSense(const std::wstring& path) {
    const std::wstring lower = lowercased(path);
    return lower.find(L"054c") != std::wstring::npos &&
           lower.find(L"0ce6") != std::wstring::npos;
}

// A Bluetooth HID interface path carries the Bluetooth HID service UUID. This is
// how the transport is identified when Steam blocks the HID capability queries.
bool pathIsBluetooth(const std::wstring& path) {
    return lowercased(path).find(L"00001124") != std::wstring::npos;
}

// Lists HID interface paths through cfgmgr32. Empty means it is unavailable or
// filtered, and the caller should fall back to SetupAPI.
std::vector<std::wstring> listHidPathsViaCfgMgr(CfgMgrApi& cm, GUID hidGuid) {
    std::vector<std::wstring> paths;
    if (cm.listSize == nullptr || cm.list == nullptr) return paths;

    ULONG length = 0;
    if (cm.listSize(&length, &hidGuid, nullptr, 0 /* PRESENT */) != 0 || length < 2) return paths;

    std::vector<wchar_t> buffer(length, 0);
    if (cm.list(&hidGuid, nullptr, buffer.data(), length, 0 /* PRESENT */) != 0) return paths;

    for (const wchar_t* cursor = buffer.data(); *cursor != L'\0'; cursor += wcslen(cursor) + 1) {
        paths.emplace_back(cursor);
    }
    return paths;
}

class PadLink {
public:
    bool connected() const { return handle_ != INVALID_HANDLE_VALUE; }
    bool bluetooth() const { return bluetooth_; }

    // Tries one interface path. Returns true when it is a Bluetooth DualSense we
    // managed to open for writing.
    bool tryPath(HidApi& hid, const std::wstring& path, bool trustAttributes) {
        const bool candidate = pathLooksLikeDualSense(path);
        bool usedNative = false;
        HANDLE probe = openDevice(path, &usedNative);
        if (probe == INVALID_HANDLE_VALUE) {
            if (candidate) {
                static int refusals = 0;
                if (refusals < 3 || (refusals % 60) == 0) {
                    logLine("DualSense path found but could not be opened, err=%lu", GetLastError());
                }
                ++refusals;
            }
            return false;
        }

        bool match = false;
        USHORT inLen = 0, outLen = 0;
        DsHiddAttributes attrs{};
        attrs.Size = sizeof(attrs);

        const bool gotAttributes = hid.GetAttributes(probe, &attrs) != FALSE;
        const bool attributesSay = gotAttributes &&
                                   attrs.VendorID == kSonyVid && attrs.ProductID == kDualSensePid;
        const bool pathSays = pathLooksLikeDualSense(path);

        // Only the first few candidates are traced. This goes into the log the
        // whole framework shares, so it has to stay out of everyone's way.
        static int traced = 0;
        const bool trace = candidate && traced < 3;
        if (candidate) ++traced;

        if (trace) {
            logLine("candidate opened (%s): attrsOk=%d vid=%04X pid=%04X",
                    usedNative ? "NtCreateFile" : "CreateFileW",
                    gotAttributes ? 1 : 0, (unsigned)attrs.VendorID, (unsigned)attrs.ProductID);
        }

        if (attributesSay || (!trustAttributes && pathSays)) {
            PVOID preparsed = nullptr;
            const bool gotPreparsed = hid.GetPreparsedData(probe, &preparsed) != FALSE;
            if (trace) logLine("  GetPreparsedData=%d (err=%lu)", gotPreparsed ? 1 : 0, GetLastError());
            if (gotPreparsed) {
                DsHidpCaps caps{};
                const LONG capsResult = hid.GetCaps(preparsed, &caps);
                hid.FreePreparsedData(preparsed);
                inLen  = caps.InputReportByteLength;
                outLen = caps.OutputReportByteLength;
                match = true;
                if (trace) logLine("  caps result=%ld inputLen=%u outputLen=%u",
                                   (long)capsResult, (unsigned)inLen, (unsigned)outLen);
            } else if (pathSays) {
                // Steam Input hides controllers by making the HID queries fail, not
                // by blocking the open: the handle is still a perfectly good file
                // handle to write reports to. The path tells us the transport, and
                // the report length is fixed for a DualSense, so carry on without
                // the capabilities. write() confirms the length on first use.
                inLen  = pathIsBluetooth(path) ? kBluetoothInputLen : 64;
                outLen = kBluetoothOutputLen;
                match  = true;
                capsUnknown_ = true;
                if (trace) logLine("  capability query blocked; assuming %s report sizes",
                                   pathIsBluetooth(path) ? "Bluetooth" : "USB");
            }
        } else if (trace) {
            logLine("  rejected: attributesSay=%d pathSays=%d trustAttributes=%d",
                    attributesSay ? 1 : 0, pathSays ? 1 : 0, trustAttributes ? 1 : 0);
        }
        CloseHandle(probe);
        if (!match) return false;

        bool usedNativeAgain = false;
        HANDLE opened = openDevice(path, &usedNativeAgain);
        if (opened == INVALID_HANDLE_VALUE) return false;

        handle_       = opened;
        outputLength_ = outLen;
        bluetooth_    = (inLen == kBluetoothInputLen);
        sequence_     = 0;
        g_announcedMissing = false;
        logLine("pad opened (%s, matched by %s): inputLen=%u outputLen=%u bluetooth=%s",
                usedNativeAgain ? "NtCreateFile" : "CreateFileW",
                attributesSay ? "attributes" : "path",
                (unsigned)inLen, (unsigned)outLen, bluetooth_ ? "yes" : "no");
        return true;
    }

    bool connectViaCfgMgr(HidApi& hid, CfgMgrApi& cm) {
        disconnect();
        GUID hidGuid{};
        hid.GetHidGuid(&hidGuid);

        const std::vector<std::wstring> paths = listHidPathsViaCfgMgr(cm, hidGuid);
        for (const std::wstring& path : paths) {
            if (tryPath(hid, path, /*trustAttributes=*/false)) return true;
        }
        // Silent: the SetupAPI pass runs next and reports for both of them.
        return false;
    }

    bool connect(HidApi& hid) {
        disconnect();

        GUID hidGuid{};
        hid.GetHidGuid(&hidGuid);

        HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (set == INVALID_HANDLE_VALUE) return false;

        bool found = false;
        int seen = 0, opened = 0, sony = 0;
        SP_DEVICE_INTERFACE_DATA iface{};
        iface.cbSize = sizeof(iface);

        for (DWORD index = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, index, &iface); ++index) {
            ++seen;
            DWORD needed = 0;
            SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &needed, nullptr);
            if (needed == 0) continue;

            std::vector<BYTE> buffer(needed);
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, needed, &needed, nullptr)) continue;

            HANDLE probe = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                       OPEN_EXISTING, 0, nullptr);
            if (probe == INVALID_HANDLE_VALUE) continue;
            ++opened;

            DsHiddAttributes attrs{};
            attrs.Size = sizeof(attrs);
            bool match = false;
            USHORT inLen = 0, outLen = 0;

            // Counted, not logged: the summary below carries the number, and this
            // runs once a second.
            if (hid.GetAttributes(probe, &attrs) && attrs.VendorID == kSonyVid) ++sony;

            if (hid.GetAttributes(probe, &attrs) &&
                attrs.VendorID == kSonyVid && attrs.ProductID == kDualSensePid) {
                PVOID preparsed = nullptr;
                if (hid.GetPreparsedData(probe, &preparsed)) {
                    DsHidpCaps caps{};
                    hid.GetCaps(preparsed, &caps);
                    hid.FreePreparsedData(preparsed);
                    inLen  = caps.InputReportByteLength;
                    outLen = caps.OutputReportByteLength;
                    match = true;
                }
            }
            CloseHandle(probe);
            if (!match) continue;

            HANDLE opened = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                        OPEN_EXISTING, 0, nullptr);
            if (opened == INVALID_HANDLE_VALUE) continue;

            handle_      = opened;
            outputLength_ = outLen;
            bluetooth_   = (inLen == kBluetoothInputLen);
            sequence_    = 0;
            found = true;
            g_announcedMissing = false;
            logLine("pad opened: inputLen=%u outputLen=%u bluetooth=%s",
                    (unsigned)inLen, (unsigned)outLen, bluetooth_ ? "yes" : "no");
            break;
        }

        SetupDiDestroyDeviceInfoList(set);
        if (!found && !g_announcedMissing) {
            // Steam Input hides physical controllers from the game process, so a
            // low count here means enumeration itself is being filtered.
            g_announcedMissing = true;
            logLine("no DualSense found: enumerated=%d opened=%d sonyVid=%d", seen, opened, sony);
        }
        return found;
    }

    void disconnect() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
        bluetooth_ = false;
        outputLength_ = 0;
        capsUnknown_ = false;
        triedAlternateLength_ = false;
    }

    bool write(BYTE left, BYTE right) {
        if (!connected()) return false;

        buildReport(scratch_, left, right, sequence_, outputLength_);
        sequence_ = (BYTE)((sequence_ + 1) & 0x0F);
        DWORD written = 0;
        if (WriteFile(handle_, scratch_.data(), (DWORD)scratch_.size(), &written, nullptr) != FALSE) {
            return true;
        }

        // The report length was assumed rather than queried, so a rejection here
        // most likely means this pad wants the other one. Try it once.
        if (capsUnknown_ && !triedAlternateLength_) {
            triedAlternateLength_ = true;
            const USHORT alternate = (outputLength_ == kBluetoothOutputLen)
                                   ? (USHORT)kReportMinLength : kBluetoothOutputLen;
            logLine("write rejected at length %u (err=%lu); retrying at %u",
                    (unsigned)outputLength_, GetLastError(), (unsigned)alternate);
            outputLength_ = alternate;
            buildReport(scratch_, left, right, sequence_, outputLength_);
            sequence_ = (BYTE)((sequence_ + 1) & 0x0F);
            return WriteFile(handle_, scratch_.data(), (DWORD)scratch_.size(), &written, nullptr) != FALSE;
        }
        return false;
    }

    // Sent several times so it cannot be lost in a race with Steam.
    void silence() { for (int i = 0; i < 3 && connected(); ++i) write(0, 0); }

    ~PadLink() { silence(); disconnect(); }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    bool   bluetooth_ = false;
    USHORT outputLength_ = 0;
    BYTE   sequence_ = 0;
    bool   capsUnknown_ = false;
    bool   triedAlternateLength_ = false;
    std::vector<BYTE> scratch_;
};

// ---------------------------------------------------------------- the signal

struct Levels {
    int    sequence = -1;
    double low = 0.0;
    double high = 0.0;
};

// Reads "name=value" out of a line. The name has to start a word, so looking for
// "low" does not match the "lowScale" setting sitting a few lines above it.
bool readField(const std::string& line, const char* name, double& value) {
    const size_t nameLength = strlen(name);
    size_t at = 0;
    while ((at = line.find(name, at)) != std::string::npos) {
        const size_t after = at + nameLength;
        const bool startsWord = (at == 0) || line[at - 1] == ' ' || line[at - 1] == '\t';
        if (startsWord && after < line.size() && line[after] == '=') {
            return sscanf(line.c_str() + after + 1, "%lf", &value) == 1;
        }
        at = after;
    }
    return false;
}

bool parseLine(const char* begin, const char* end, Levels& out) {
    if (begin >= end) return false;
    std::string line(begin, end);
    auto clamp01 = [](double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); };

    // Every value on the state line is named, so a settings line - or half of a
    // line caught mid-rewrite - fails here instead of being read as levels.
    double sequence = 0.0, low = 0.0, high = 0.0;
    if (readField(line, "sequence", sequence) &&
        readField(line, "low", low) &&
        readField(line, "high", high)) {
        out.sequence = (int)sequence;
        out.low  = clamp01(low);
        out.high = clamp01(high);
        return true;
    }

    // v1.0 wrote five bare numbers and nothing else. Still accepted, so a
    // half-updated install goes on working rather than falling silent.
    int legacySequence = 0;
    double values[4] = {0, 0, 0, 0};
    if (sscanf(line.c_str(), "%d %lf %lf %lf %lf",
               &legacySequence, &values[0], &values[1], &values[2], &values[3]) == 5) {
        out.sequence = legacySequence;
        out.low  = clamp01(values[0]);
        out.high = clamp01(values[1]);
        return true;
    }
    return false;
}

// Takes the newest complete line, skipping a half-written one at the end: the
// game writes this file underneath us.
bool readSignal(const std::wstring& path, Levels& out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    // Only the tail is interesting - the state line is last - and reading from
    // the end keeps this correct however long the settings block above it grows.
    char buffer[1024];
    const DWORD size = GetFileSize(file, nullptr);
    if (size != INVALID_FILE_SIZE && size > sizeof(buffer) - 1) {
        SetFilePointer(file, (LONG)(size - (sizeof(buffer) - 1)), nullptr, FILE_BEGIN);
    }

    DWORD read = 0;
    const BOOL ok = ReadFile(file, buffer, sizeof(buffer) - 1, &read, nullptr);
    CloseHandle(file);
    if (!ok || read == 0) return false;
    buffer[read] = '\0';

    const char* const start = buffer;
    const char* cursor = buffer + read;
    while (cursor > start) {
        const char* lineEnd = cursor;
        while (cursor > start && *(cursor - 1) != '\n') --cursor;
        if (parseLine(cursor, lineEnd, out)) return true;
        if (cursor > start) --cursor;   // step over the newline and try the line before
    }
    return false;
}

std::wstring signalPath() {
    const std::wstring dir = gameDir();
    if (dir.empty()) return L"";
    return dir + L"reframework\\data\\WildsDualSenseBTRumble.txt";
}

// ---------------------------------------------------------------- the worker

std::atomic<bool> g_running{false};
std::thread       g_worker;

void workerMain() {
    HidApi hid;
    if (!hid.load()) { logLine("no rumble: hid.dll entry points missing"); return; }

    CfgMgrApi cm;
    cm.load();

    const std::wstring path = signalPath();
    if (path.empty()) { logLine("no rumble: could not resolve the state file path"); return; }

    PadLink pad;
    int   writeFailures = 0;
    bool  saidDormant = false;
    Levels current;
    int  lastSequence = -1;
    int  quietTicks   = 0;
    const int tickMs  = 16;                 // ~60 Hz
    const int staleTicks = 2000 / tickMs;   // give up on the levels after 2s of silence

    while (g_running.load()) {
        if (!pad.connected()) {
            // cfgmgr32 first: SetupAPI is the one Steam Input filters.
            if (!pad.connectViaCfgMgr(hid, cm) && !pad.connect(hid)) {
                Sleep(1000);
                continue;
            }
        }

        // On USB the game's own haptics already work; stay out of the way.
        if (!pad.bluetooth()) {
            if (!saidDormant) { saidDormant = true; logLine("pad is on USB - dormant"); }
            pad.disconnect();      // re-enumerate, in case it moves to Bluetooth
            Sleep(1000);
            continue;
        }
        saidDormant = false;

        Levels fresh;
        if (readSignal(path, fresh)) {
            if (fresh.sequence != lastSequence) {
                lastSequence = fresh.sequence;
                current = fresh;
                quietTicks = 0;
            } else if (quietTicks < staleTicks) {
                ++quietTicks;
            }
        } else if (quietTicks < staleTicks) {
            ++quietTicks;
        }

        double low = current.low, high = current.high;
        if (quietTicks >= staleTicks) { low = 0.0; high = 0.0; }

        const BYTE left  = (BYTE)(low  * 255.0 + 0.5);
        const BYTE right = (BYTE)(high * 255.0 + 0.5);

        if (!pad.write(left, right)) {
            if (++writeFailures <= 3) logLine("write failed, err=%lu", GetLastError());
            pad.disconnect();      // controller went away; re-enumerate next tick
        }

        Sleep(tickMs);
    }

    pad.silence();
}

} // namespace

// ---------------------------------------------------------------- exports

extern "C" __declspec(dllexport)
void reframework_plugin_required_version(REFrameworkPluginVersion* version) {
    version->major = REFRAMEWORK_PLUGIN_VERSION_MAJOR;
    version->minor = REFRAMEWORK_PLUGIN_VERSION_MINOR;
    version->patch = REFRAMEWORK_PLUGIN_VERSION_PATCH;
    // game_name is left alone on purpose: this plugin does not need REFramework
    // to gate it on a particular game.
}

extern "C" __declspec(dllexport)
bool reframework_plugin_initialize(const REFrameworkPluginInitializeParam* param) {
    if (param != nullptr && param->functions != nullptr) g_log = param->functions->log_info;
    if (g_running.exchange(true)) return true;   // already started
    g_worker = std::thread(workerMain);
    g_worker.detach();
    return true;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH) {
        g_running.store(false);   // best effort; the worker silences on its way out
    }
    return TRUE;
}
