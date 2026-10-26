#include "shell/uwp/disc_file.h"

#include <windows.h>
#include <fileapifromapp.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>

namespace xbrt {

namespace {

std::string win32_error(const char *call, DWORD error)
{
    char buf[96];
    snprintf(buf, sizeof buf, "%s error %lu", call, (unsigned long)error);
    return buf;
}

class FromAppFile final : public Source {
public:
    FromAppFile(HANDLE file, uint64_t size) : file_(file), size_(size) {}
    ~FromAppFile() override { CloseHandle(file_); }
    FromAppFile(const FromAppFile &) = delete;
    FromAppFile &operator=(const FromAppFile &) = delete;

    bool read_at(uint64_t offset, std::span<uint8_t> out) const override
    {
        while (!out.empty()) {
            OVERLAPPED at = {};
            at.Offset = DWORD(offset);
            at.OffsetHigh = DWORD(offset >> 32);
            DWORD want = DWORD(std::min<size_t>(out.size(), size_t(1) << 30));
            DWORD got = 0;
            if (!ReadFile(file_, out.data(), want, &got, &at) || got == 0) return false;
            offset += got;
            out = out.subspan(got);
        }
        return true;
    }

    uint64_t size() const override { return size_; }

private:
    HANDLE file_;
    uint64_t size_;
};

}  // namespace

std::unique_ptr<Source> open_disc_file(std::u16string_view path, std::string &error)
{
    std::wstring wide(reinterpret_cast<const wchar_t *>(path.data()), path.size());
    HANDLE file = CreateFile2FromAppW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = win32_error("CreateFile2FromAppW", GetLastError());
        return nullptr;
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size)) {
        error = win32_error("GetFileSizeEx", GetLastError());
        CloseHandle(file);
        return nullptr;
    }
    return std::make_unique<FromAppFile>(file, uint64_t(size.QuadPart));
}

std::vector<std::u16string> list_disc_files(std::u16string_view folder, std::string &error)
{
    std::vector<std::u16string> out;
    std::wstring pattern(reinterpret_cast<const wchar_t *>(folder.data()), folder.size());
    if (!pattern.empty() && pattern.back() != L'\\') pattern += L'\\';
    pattern += L"*";
    WIN32_FIND_DATAW found;
    HANDLE h = FindFirstFileExFromAppW(pattern.c_str(), FindExInfoBasic, &found, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) {
        error = win32_error("FindFirstFileExFromAppW", GetLastError());
        return out;
    }
    do {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = found.cFileName;
        size_t dot = name.find_last_of(L'.');
        std::wstring ext = dot == std::wstring::npos ? L"" : name.substr(dot);
        for (wchar_t &c : ext) c = wchar_t(std::towlower(c));
        if (ext != L".iso" && ext != L".xiso") continue;
        std::wstring full = pattern.substr(0, pattern.size() - 1) + name;
        out.emplace_back(reinterpret_cast<const char16_t *>(full.data()), full.size());
    } while (FindNextFileW(h, &found));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace xbrt
