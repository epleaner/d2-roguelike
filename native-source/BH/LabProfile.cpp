#include "LabProfile.h"

#include <Windows.h>
#include <wincrypt.h>
#include <string>

#pragma comment(lib, "advapi32.lib")

namespace {
    // The launcher selects the copy; identity follows the loaded executable and
    // BH module, never a caller-supplied directory or the current working directory.
    std::wstring imageDirectory;
    struct Image { const wchar_t* name; const char* digest; };
    constexpr Image kImages[] = {
        {L"Game.exe", "74fe9c092a521f7710392548c82f81544e531107db2358617e83818874db40a2"},
        {L"D2Client.dll", "dd8bc6025de921216a97c17f97cd1a50fbb85926e838ec60e13451448836d906"},
        {L"D2Common.dll", "59fa5928522f566f2bf99675571206ad70df889c89d3d07fa87edf5083e06e10"},
        {L"D2Net.dll", "99af2f04a87fdbd91810be963df9c365cef529ae22e6a345193ba5018e9910c9"},
        {L"D2Win.dll", "ff5cbb956f1a419e0e76fc2df3be21065bbba894770b8de7cce8bc55ec453736"},
        {L"D2Launch.dll", "99cabc4751c95d035b5e77151b3605b809884fec2c10edf37d4c4e4e231e8479"},
        {L"D2Gfx.dll", "50284647a28e8839028e8acb021a6ebe48c006e431139969ccd0fe126e6e8811"},
        {L"D2Lang.dll", "206386a81f4046c693f2aca60b9ee19fffd05b1157bbb60992c0d4caad9b67ec"},
        {L"ProjectDiablo.dll", "538a77b7ccef3d5334e56c4e9e57a4d8fc69a1e27c46beb694c0dedfcfbf9cb3"}
    };

    bool ModulePath(HMODULE module, std::wstring& path) {
        wchar_t buffer[32768];
        const DWORD length = GetModuleFileNameW(module, buffer, static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])));
        if (!length || length >= sizeof(buffer) / sizeof(buffer[0])) return false;
        path.assign(buffer, length);
        return true;
    }

    bool HashFile(const std::wstring& path, std::string& digest) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        HCRYPTPROV provider = 0;
        HCRYPTHASH hash = 0;
        bool ok = CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
                  CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash);
        BYTE buffer[16384];
        DWORD count = 0;
        while (ok) {
            if (!ReadFile(file, buffer, sizeof(buffer), &count, nullptr)) { ok = false; break; }
            if (!count) break;
            ok = !!CryptHashData(hash, buffer, count, 0);
        }
        BYTE bytes[32];
        DWORD length = sizeof(bytes);
        ok = ok && CryptGetHashParam(hash, HP_HASHVAL, bytes, &length, 0) && length == sizeof(bytes);
        if (ok) {
            constexpr char hex[] = "0123456789abcdef";
            digest.resize(64);
            for (unsigned int i = 0; i < length; ++i) {
                digest[i * 2] = hex[bytes[i] >> 4];
                digest[i * 2 + 1] = hex[bytes[i] & 15];
            }
        }
        if (hash) CryptDestroyHash(hash);
        if (provider) CryptReleaseContext(provider, 0);
        CloseHandle(file);
        return ok;
    }

    bool VerifyImage(const wchar_t* name, const char* digest, std::string& error) {
        if (imageDirectory.empty()) { error = "Standalone executable not authenticated"; return false; }
        const std::wstring expected = imageDirectory + name;
        HMODULE module = GetModuleHandleW(name);
        std::wstring loaded;
        std::string actual;
        if (!module || !ModulePath(module, loaded) || _wcsicmp(loaded.c_str(), expected.c_str()) ||
            !HashFile(expected, actual) || actual != digest) {
            error = "Missing, relocated or unrecognized loaded image";
            return false;
        }
        return true;
    }
}

namespace LabProfile {
    bool LoadAndVerify(std::string& error) {
        imageDirectory.clear();
        std::wstring gamePath;
        if (!ModulePath(nullptr, gamePath)) { error = "Executable path unavailable"; return false; }
        const auto separator = gamePath.find_last_of(L"\\/");
        if (separator == std::wstring::npos ||
            _wcsicmp(gamePath.c_str() + separator + 1, L"Game.exe")) {
            error = "Executable is not ProjectD2/Game.exe"; return false;
        }
        const std::wstring directory = gamePath.substr(0, separator + 1);
        const auto parentEnd = directory.find_last_of(L"\\/", separator - 1);
        if (parentEnd == std::wstring::npos ||
            _wcsicmp(directory.substr(parentEnd + 1, separator - parentEnd - 1).c_str(), L"ProjectD2")) {
            error = "Executable is not in ProjectD2"; return false;
        }
        HMODULE self = nullptr;
        std::wstring selfPath;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&LoadAndVerify), &self) ||
            !ModulePath(self, selfPath) ||
            _wcsicmp(selfPath.c_str(), (directory + L"BH.dll").c_str())) {
            error = "Loaded BH.dll is not alongside Game.exe"; return false;
        }
        imageDirectory = directory;
        for (const auto& image : kImages) {
            if (!VerifyImage(image.name, image.digest, error)) {
                imageDirectory.clear();
                return false;
            }
        }
        return true;
    }

    bool VerifyClient(std::string& error) {
        return VerifyImage(L"D2Client.dll", kImages[1].digest, error);
    }
    bool VerifyCommon(std::string& error) {
        return VerifyImage(L"D2Common.dll", kImages[2].digest, error);
    }
    bool VerifyServer(std::string& error) {
        return VerifyImage(L"D2Game.dll",
            "24b293b86f95bea0228826c502ea04737ba21b91ca9832e52aac4b11fef14f53", error);
    }
    bool BackendFileIdentity(std::string& digest) {
        HMODULE module = nullptr;
        std::wstring path;
        if (imageDirectory.empty() ||
            !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&BackendFileIdentity), &module) ||
            !ModulePath(module, path) ||
            _wcsicmp(path.c_str(), (imageDirectory + L"BH.dll").c_str())) return false;
        return HashFile(path, digest);
    }
    bool ModFileIdentity(std::string& digest) {
        std::string error;
        digest.clear();
        if (!VerifyImage(L"ProjectDiablo.dll", kImages[8].digest, error)) return false;
        return HashFile(imageDirectory + L"ProjectDiablo.dll", digest);
    }
    // The standalone variant never loads a lab profile or starts its MCP listener.
    const std::string& Token() { static const std::string empty; return empty; }
    int Port() { return 0; }
}
