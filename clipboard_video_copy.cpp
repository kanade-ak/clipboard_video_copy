#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "plugin2.h"
#include "logger2.h"
#include "detours.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")

namespace {

constexpr UINT kCopyVideoIdBase = 0x7f40;
constexpr wchar_t kPluginName[] = L"Clipboard Video Copy";
constexpr wchar_t kMenuTextCopyVideo[] =
    L"\u30AF\u30EA\u30C3\u30D7\u30DC\u30FC\u30C9\u306B\u52D5\u753B\u3092\u30B3\u30D4\u30FC";
constexpr wchar_t kMenuTextCopyVideoAlpha[] =
    L"\u30AF\u30EA\u30C3\u30D7\u30DC\u30FC\u30C9\u306B\u52D5\u753B\u3092\u30B3\u30D4\u30FC"
    L"(\u80CC\u666F\u900F\u904E)";
constexpr wchar_t kMenuNeedleClipboard[] =
    L"\u30AF\u30EA\u30C3\u30D7\u30DC\u30FC\u30C9";
constexpr wchar_t kMenuNeedleImage[] =
    L"\u30A4\u30E1\u30FC\u30B8";
constexpr wchar_t kMenuNeedleAlpha[] =
    L"\u80CC\u666F\u900F\u904E";

HMODULE g_module = nullptr;
EDIT_HANDLE* g_edit = nullptr;
LOG_HANDLE* g_logger = nullptr;
std::atomic_bool g_copying = false;
std::mutex g_clipboard_mutex;
IDataObject* g_clipboard_object = nullptr;
bool g_ole_initialized = false;
DWORD g_ole_thread_id = 0;

using TrackPopupMenuPtr = BOOL(WINAPI*)(HMENU, UINT, int, int, int, HWND, const RECT*);
using TrackPopupMenuExPtr = BOOL(WINAPI*)(HMENU, UINT, int, int, HWND, LPTPMPARAMS);

TrackPopupMenuPtr TrueTrackPopupMenu = TrackPopupMenu;
TrackPopupMenuExPtr TrueTrackPopupMenuEx = TrackPopupMenuEx;

COMMON_PLUGIN_TABLE g_common_plugin_table = {
    kPluginName,
    L"Clipboard Video Copy version 0.2"
};

std::wstring FormatWin32Error(DWORD error)
{
    wchar_t* buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD len = FormatMessageW(flags, nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring message = len ? buffer : L"unknown error";
    if (buffer) {
        LocalFree(buffer);
    }
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
        message.pop_back();
    }
    return message;
}

std::wstring FormatHresult(HRESULT hr)
{
    wchar_t buffer[16]{};
    swprintf_s(buffer, L"0x%08X", static_cast<unsigned int>(hr));
    return buffer;
}

std::wstring Utf8ToWide(const std::vector<std::uint8_t>& bytes)
{
    if (bytes.empty()) {
        return {};
    }
    const char* data = reinterpret_cast<const char*>(bytes.data());
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, static_cast<int>(bytes.size()), nullptr, 0);
    UINT code_page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (len <= 0) {
        code_page = CP_ACP;
        flags = 0;
        len = MultiByteToWideChar(code_page, flags, data, static_cast<int>(bytes.size()), nullptr, 0);
    }
    if (len <= 0) {
        return L"(failed to decode ffmpeg output)";
    }
    std::wstring result(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(code_page, flags, data, static_cast<int>(bytes.size()), result.data(), len);
    return result;
}

void LogInfo(const std::wstring& message)
{
    if (g_logger && g_logger->info) {
        g_logger->info(g_logger, message.c_str());
    }
}

void LogWarn(const std::wstring& message)
{
    if (g_logger && g_logger->warn) {
        g_logger->warn(g_logger, message.c_str());
    }
}

void LogError(const std::wstring& message)
{
    if (g_logger && g_logger->error) {
        g_logger->error(g_logger, message.c_str());
    }
}

HWND HostWindow()
{
    if (g_edit && g_edit->get_host_app_window) {
        HWND hwnd = g_edit->get_host_app_window();
        if (hwnd) {
            return hwnd;
        }
    }
    return GetActiveWindow();
}

void ShowMessage(UINT icon, const std::wstring& message)
{
    MessageBoxW(HostWindow(), message.c_str(), kPluginName, MB_OK | icon);
}

bool ContainsMenuText(LPCWSTR text)
{
    if (!text || IS_INTRESOURCE(text)) {
        return false;
    }
    return wcsstr(text, kMenuNeedleClipboard) &&
           wcsstr(text, kMenuNeedleImage) &&
           wcsstr(text, kMenuNeedleAlpha);
}

bool GetMenuText(HMENU menu, int position, std::wstring* text)
{
    wchar_t buffer[512]{};
    const int len = GetMenuStringW(menu, static_cast<UINT>(position), buffer, static_cast<int>(_countof(buffer)), MF_BYPOSITION);
    if (len <= 0) {
        text->clear();
        return false;
    }
    *text = buffer;
    return true;
}

bool IsOurMenuText(const std::wstring& text)
{
    return text == kMenuTextCopyVideo || text == kMenuTextCopyVideoAlpha;
}

bool MenuAlreadyHasVideoItems(HMENU menu)
{
    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i) {
        std::wstring text;
        if (GetMenuText(menu, i, &text) && IsOurMenuText(text)) {
            return true;
        }
    }
    return false;
}

int FindInsertAfterImageAlphaMenu(HMENU menu)
{
    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i) {
        std::wstring text;
        if (GetMenuText(menu, i, &text) && ContainsMenuText(text.c_str())) {
            return i;
        }
    }
    return -1;
}

bool MenuUsesCommandId(HMENU menu, UINT id)
{
    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i) {
        if (GetMenuItemID(menu, i) == id) {
            return true;
        }
    }
    return false;
}

UINT FindUnusedMenuId(HMENU menu, UINT start)
{
    UINT id = start;
    while (id < 0xff00 && MenuUsesCommandId(menu, id)) {
        ++id;
    }
    return id;
}

void InsertStringMenuItem(HMENU menu, int position, UINT_PTR id, LPCWSTR text)
{
    MENUITEMINFOW item{};
    item.cbSize = sizeof(item);
    item.fMask = MIIM_FTYPE | MIIM_ID | MIIM_STRING;
    item.fType = MFT_STRING;
    item.wID = static_cast<UINT>(id);
    item.dwTypeData = const_cast<LPWSTR>(text);
    InsertMenuItemW(menu, static_cast<UINT>(position), TRUE, &item);
}

void EnsureVideoMenuItems(HMENU menu)
{
    if (!menu || !g_edit || MenuAlreadyHasVideoItems(menu)) {
        return;
    }

    const int target = FindInsertAfterImageAlphaMenu(menu);
    if (target < 0) {
        return;
    }

    const UINT video_id = FindUnusedMenuId(menu, kCopyVideoIdBase);
    const UINT alpha_id = FindUnusedMenuId(menu, video_id + 1);
    InsertStringMenuItem(menu, target + 1, video_id, kMenuTextCopyVideo);
    InsertStringMenuItem(menu, target + 2, alpha_id, kMenuTextCopyVideoAlpha);
}

bool IsVideoMenuCommand(HMENU menu, BOOL command, bool* alpha)
{
    if (!menu || command == 0) {
        return false;
    }

    wchar_t buffer[512]{};
    const int len = GetMenuStringW(menu,
                                   static_cast<UINT>(command),
                                   buffer,
                                   static_cast<int>(_countof(buffer)),
                                   MF_BYCOMMAND);
    if (len <= 0) {
        return false;
    }

    const std::wstring text = buffer;
    if (text == kMenuTextCopyVideo) {
        *alpha = false;
        return true;
    }
    if (text == kMenuTextCopyVideoAlpha) {
        *alpha = true;
        return true;
    }
    return false;
}

std::wstring QuoteArg(const std::wstring& value)
{
    if (value.empty()) {
        return L"\"\"";
    }
    const bool needs_quote = value.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if (!needs_quote) {
        return value;
    }

    std::wstring result = L"\"";
    size_t backslashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') {
            ++backslashes;
        } else if (ch == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(ch);
            backslashes = 0;
        } else {
            result.append(backslashes, L'\\');
            backslashes = 0;
            result.push_back(ch);
        }
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

std::wstring ModuleDirectory()
{
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(g_module, path, MAX_PATH)) {
        return {};
    }
    std::wstring result(path);
    const size_t pos = result.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        result.resize(pos);
    }
    return result;
}

std::wstring FindFfmpeg()
{
    wchar_t found[MAX_PATH]{};
    if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, found, nullptr) > 0) {
        return found;
    }

    std::vector<std::wstring> candidates;
    const std::wstring module_dir = ModuleDirectory();
    if (!module_dir.empty()) {
        candidates.push_back(module_dir + L"\\ffmpeg.exe");
    }
    candidates.push_back(L"C:\\ffmpeg\\bin\\ffmpeg.exe");

    for (const auto& candidate : candidates) {
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return candidate;
        }
    }
    return {};
}

HGLOBAL CopyToHGlobal(const void* data, SIZE_T size)
{
    HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
    if (!global) {
        return nullptr;
    }
    void* dest = GlobalLock(global);
    if (!dest) {
        GlobalFree(global);
        return nullptr;
    }
    if (size != 0) {
        std::memcpy(dest, data, size);
    }
    GlobalUnlock(global);
    return global;
}

class FormatEtcEnumerator final : public IEnumFORMATETC {
public:
    explicit FormatEtcEnumerator(std::vector<FORMATETC> formats)
        : formats_(std::move(formats))
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
    {
        if (!ppvObject) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IEnumFORMATETC) {
            *ppvObject = static_cast<IEnumFORMATETC*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++ref_count_;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG refs = --ref_count_;
        if (refs == 0) {
            delete this;
        }
        return refs;
    }

    HRESULT STDMETHODCALLTYPE Next(ULONG celt, FORMATETC* rgelt, ULONG* pceltFetched) override
    {
        if (!rgelt || (celt > 1 && !pceltFetched)) {
            return E_POINTER;
        }
        ULONG fetched = 0;
        while (fetched < celt && index_ < formats_.size()) {
            rgelt[fetched++] = formats_[index_++];
        }
        if (pceltFetched) {
            *pceltFetched = fetched;
        }
        return fetched == celt ? S_OK : S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE Skip(ULONG celt) override
    {
        index_ = std::min(index_ + static_cast<size_t>(celt), formats_.size());
        return index_ < formats_.size() ? S_OK : S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE Reset() override
    {
        index_ = 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Clone(IEnumFORMATETC** ppenum) override
    {
        if (!ppenum) {
            return E_POINTER;
        }
        auto* clone = new FormatEtcEnumerator(formats_);
        clone->index_ = index_;
        *ppenum = clone;
        return S_OK;
    }

private:
    std::atomic_ulong ref_count_{1};
    std::vector<FORMATETC> formats_;
    size_t index_ = 0;
};

class VirtualFileDataObject final : public IDataObject {
public:
    VirtualFileDataObject(std::wstring file_name, std::vector<std::uint8_t> data)
        : file_name_(std::move(file_name)), data_(std::move(data))
    {
        file_descriptor_format_ = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileGroupDescriptorW"));
        file_contents_format_ = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileContents"));
        preferred_drop_format_ = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"Preferred DropEffect"));
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
    {
        if (!ppvObject) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IDataObject) {
            *ppvObject = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++ref_count_;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG refs = --ref_count_;
        if (refs == 0) {
            delete this;
        }
        return refs;
    }

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* pformatetcIn, STGMEDIUM* pmedium) override
    {
        if (!pformatetcIn || !pmedium) {
            return E_POINTER;
        }
        std::memset(pmedium, 0, sizeof(*pmedium));

        if (pformatetcIn->cfFormat == file_descriptor_format_ &&
            (pformatetcIn->tymed & TYMED_HGLOBAL)) {
            FILEGROUPDESCRIPTORW descriptor{};
            descriptor.cItems = 1;
            FILEDESCRIPTORW& file = descriptor.fgd[0];
            file.dwFlags = FD_ATTRIBUTES | FD_FILESIZE | FD_WRITESTIME;
            file.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
            file.nFileSizeHigh = static_cast<DWORD>((static_cast<unsigned long long>(data_.size()) >> 32) & 0xffffffffu);
            file.nFileSizeLow = static_cast<DWORD>(data_.size() & 0xffffffffu);
            GetSystemTimeAsFileTime(&file.ftLastWriteTime);
            wcsncpy_s(file.cFileName, file_name_.c_str(), _TRUNCATE);

            HGLOBAL global = CopyToHGlobal(&descriptor, sizeof(descriptor));
            if (!global) {
                return STG_E_MEDIUMFULL;
            }
            pmedium->tymed = TYMED_HGLOBAL;
            pmedium->hGlobal = global;
            return S_OK;
        }

        if (pformatetcIn->cfFormat == file_contents_format_ &&
            (pformatetcIn->lindex == 0 || pformatetcIn->lindex == -1)) {
            HGLOBAL global = CopyToHGlobal(data_.data(), data_.size());
            if (!global) {
                return STG_E_MEDIUMFULL;
            }

            if (pformatetcIn->tymed & TYMED_ISTREAM) {
                IStream* stream = nullptr;
                HRESULT hr = CreateStreamOnHGlobal(global, TRUE, &stream);
                if (FAILED(hr)) {
                    GlobalFree(global);
                    return hr;
                }
                pmedium->tymed = TYMED_ISTREAM;
                pmedium->pstm = stream;
                return S_OK;
            }

            if (pformatetcIn->tymed & TYMED_HGLOBAL) {
                pmedium->tymed = TYMED_HGLOBAL;
                pmedium->hGlobal = global;
                return S_OK;
            }

            GlobalFree(global);
            return DV_E_TYMED;
        }

        if (pformatetcIn->cfFormat == preferred_drop_format_ &&
            (pformatetcIn->tymed & TYMED_HGLOBAL)) {
            const DWORD effect = DROPEFFECT_COPY;
            HGLOBAL global = CopyToHGlobal(&effect, sizeof(effect));
            if (!global) {
                return STG_E_MEDIUMFULL;
            }
            pmedium->tymed = TYMED_HGLOBAL;
            pmedium->hGlobal = global;
            return S_OK;
        }

        return DV_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override
    {
        return DATA_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* pformatetc) override
    {
        if (!pformatetc) {
            return E_POINTER;
        }
        if (pformatetc->cfFormat == file_descriptor_format_ && (pformatetc->tymed & TYMED_HGLOBAL)) {
            return S_OK;
        }
        if (pformatetc->cfFormat == file_contents_format_ &&
            (pformatetc->tymed & (TYMED_ISTREAM | TYMED_HGLOBAL)) &&
            (pformatetc->lindex == 0 || pformatetc->lindex == -1)) {
            return S_OK;
        }
        if (pformatetc->cfFormat == preferred_drop_format_ && (pformatetc->tymed & TYMED_HGLOBAL)) {
            return S_OK;
        }
        return DV_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* pformatetcOut) override
    {
        if (!pformatetcOut) {
            return E_POINTER;
        }
        pformatetcOut->ptd = nullptr;
        return DATA_S_SAMEFORMATETC;
    }

    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override
    {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD dwDirection, IEnumFORMATETC** ppenumFormatEtc) override
    {
        if (!ppenumFormatEtc) {
            return E_POINTER;
        }
        if (dwDirection != DATADIR_GET) {
            return E_NOTIMPL;
        }

        std::vector<FORMATETC> formats;
        formats.push_back({file_descriptor_format_, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
        formats.push_back({file_contents_format_, nullptr, DVASPECT_CONTENT, 0, TYMED_ISTREAM});
        formats.push_back({file_contents_format_, nullptr, DVASPECT_CONTENT, 0, TYMED_HGLOBAL});
        formats.push_back({preferred_drop_format_, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
        *ppenumFormatEtc = new FormatEtcEnumerator(std::move(formats));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }

private:
    std::atomic_ulong ref_count_{1};
    std::wstring file_name_;
    std::vector<std::uint8_t> data_;
    CLIPFORMAT file_descriptor_format_ = 0;
    CLIPFORMAT file_contents_format_ = 0;
    CLIPFORMAT preferred_drop_format_ = 0;
};

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { reset(); }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    Handle(Handle&& other) noexcept : value_(other.release()) {}
    Handle& operator=(Handle&& other) noexcept
    {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    HANDLE get() const { return value_; }
    HANDLE* put()
    {
        reset();
        return &value_;
    }
    HANDLE release()
    {
        HANDLE value = value_;
        value_ = nullptr;
        return value;
    }
    void reset(HANDLE value = nullptr)
    {
        if (value_ && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
        value_ = value;
    }
    explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE value_ = nullptr;
};

void ReadPipe(HANDLE handle, std::vector<std::uint8_t>* output)
{
    std::uint8_t buffer[32768];
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(handle, buffer, sizeof(buffer), &read, nullptr) || read == 0) {
            break;
        }
        output->insert(output->end(), buffer, buffer + read);
    }
}

bool WriteAll(HANDLE handle, const void* data, size_t size)
{
    const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data);
    size_t offset = 0;
    while (offset < size) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size - offset, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(handle, bytes + offset, chunk, &written, nullptr)) {
            return false;
        }
        offset += written;
    }
    return true;
}

struct VideoFrameContext {
    std::vector<std::uint8_t> pixels;
    int width = 0;
    int height = 0;
    bool done = false;
};

void OnVideoFrame(void* param, int, const void* buffer, int width, int height, int pitch)
{
    auto* context = static_cast<VideoFrameContext*>(param);
    context->width = width;
    context->height = height;
    context->pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);

    const auto* source = static_cast<const std::uint8_t*>(buffer);
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = pitch >= 0
            ? source + static_cast<size_t>(y) * static_cast<size_t>(pitch)
            : source + static_cast<size_t>(height - 1 - y) * static_cast<size_t>(-pitch);
        std::memcpy(context->pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 4,
                    row,
                    static_cast<size_t>(width) * 4);
    }
    context->done = true;
}

bool RenderFrame(int frame, int expected_width, int expected_height, std::vector<std::uint8_t>* pixels)
{
    VideoFrameContext context;
    if (!g_edit->rendering_scene_video(frame, &context, OnVideoFrame)) {
        return false;
    }
    g_edit->wait_rendering_task();
    if (!context.done || context.width != expected_width || context.height != expected_height) {
        return false;
    }
    *pixels = std::move(context.pixels);
    return true;
}

void OpaqueAlpha(std::vector<std::uint8_t>* pixels)
{
    for (size_t i = 3; i < pixels->size(); i += 4) {
        (*pixels)[i] = 255;
    }
}

struct AudioFrameContext {
    std::vector<float> samples;
    int sample_num = 0;
    bool done = false;
};

void OnAudioFrame(void* param, int, const float* buffer0, const float* buffer1, int sample_num)
{
    auto* context = static_cast<AudioFrameContext*>(param);
    context->sample_num = sample_num;
    context->samples.resize(static_cast<size_t>(std::max(0, sample_num)) * 2);
    for (int i = 0; i < sample_num; ++i) {
        context->samples[static_cast<size_t>(i) * 2] = buffer0 ? buffer0[i] : 0.0f;
        context->samples[static_cast<size_t>(i) * 2 + 1] = buffer1 ? buffer1[i] : 0.0f;
    }
    context->done = true;
}

bool RenderAudioFrame(int frame, std::vector<float>* samples)
{
    AudioFrameContext context;
    if (!g_edit->rendering_scene_audio(frame, &context, OnAudioFrame)) {
        return false;
    }
    g_edit->wait_rendering_task();
    if (!context.done) {
        return false;
    }
    *samples = std::move(context.samples);
    return true;
}

std::wstring BuildFfmpegCommand(const std::wstring& ffmpeg,
                                const EDIT_INFO& info,
                                bool alpha,
                                const std::wstring& audio_path,
                                const std::wstring& output_path)
{
    const std::wstring size = std::to_wstring(info.width) + L"x" + std::to_wstring(info.height);
    const std::wstring rate = std::to_wstring(info.rate) + L"/" + std::to_wstring(info.scale);
    const bool has_audio = !audio_path.empty();

    std::vector<std::wstring> args = {
        ffmpeg,
        L"-y",
        L"-hide_banner",
        L"-loglevel", L"error",
        L"-f", L"rawvideo",
        L"-pix_fmt", L"rgba",
        L"-video_size", size,
        L"-framerate", rate,
        L"-i", L"pipe:0",
    };

    if (has_audio) {
        args.insert(args.end(), {
            L"-f", L"f32le",
            L"-ar", std::to_wstring(info.sample_rate),
            L"-ac", L"2",
            L"-i", audio_path,
            L"-shortest"
        });
    } else {
        args.push_back(L"-an");
    }

    if (alpha) {
        args.insert(args.end(), {
            L"-map", L"0:v:0",
        });
        if (has_audio) {
            args.insert(args.end(), {
                L"-map", L"1:a:0",
            });
        }
        args.insert(args.end(), {
            L"-c:v", L"qtrle",
            L"-pix_fmt", L"argb",
        });
        if (has_audio) {
            args.insert(args.end(), {
                L"-c:a", L"pcm_s16le",
            });
        }
        args.insert(args.end(), {
            L"-f", L"mov",
            output_path
        });
    } else {
        args.insert(args.end(), {
            L"-map", L"0:v:0",
        });
        if (has_audio) {
            args.insert(args.end(), {
                L"-map", L"1:a:0",
            });
        }
        args.insert(args.end(), {
            L"-c:v", L"libx264",
            L"-preset", L"veryfast",
            L"-crf", L"18",
            L"-pix_fmt", L"yuv420p",
            L"-movflags", L"+faststart",
        });
        if (has_audio) {
            args.insert(args.end(), {
                L"-c:a", L"aac",
                L"-b:a", L"192k",
            });
        }
        args.insert(args.end(), {
            L"-f", L"mp4",
            output_path
        });
    }

    std::wstring command;
    for (const auto& arg : args) {
        if (!command.empty()) {
            command.push_back(L' ');
        }
        command += QuoteArg(arg);
    }
    return command;
}

bool EncodeVideoToFile(const EDIT_INFO& info,
                       int start_frame,
                       int end_frame,
                       bool alpha,
                       const std::wstring& audio_path,
                       const std::wstring& output_path,
                       std::wstring* error)
{
    const std::wstring ffmpeg = FindFfmpeg();
    if (ffmpeg.empty()) {
        *error = L"ffmpeg.exe が見つかりません。PATH、プラグインフォルダ、または C:\\ffmpeg\\bin に配置してください";
        return false;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    Handle stdin_read;
    Handle stdin_write;
    Handle stderr_read;
    Handle stderr_write;

    if (!CreatePipe(stdin_read.put(), stdin_write.put(), &sa, 0) ||
        !CreatePipe(stderr_read.put(), stderr_write.put(), &sa, 0)) {
        *error = L"CreatePipe に失敗しました: " + FormatWin32Error(GetLastError());
        return false;
    }

    SetHandleInformation(stdin_write.get(), HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_read.get(), HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdin_read.get();
    startup.hStdOutput = stderr_write.get();
    startup.hStdError = stderr_write.get();

    PROCESS_INFORMATION process{};
    std::wstring command = BuildFfmpegCommand(ffmpeg, info, alpha, audio_path, output_path);
    if (!CreateProcessW(nullptr,
                        command.data(),
                        nullptr,
                        nullptr,
                        TRUE,
                        CREATE_NO_WINDOW,
                        nullptr,
                        nullptr,
                        &startup,
                        &process)) {
        *error = L"ffmpeg.exe の起動に失敗しました: " + FormatWin32Error(GetLastError());
        return false;
    }

    Handle process_handle(process.hProcess);
    Handle thread_handle(process.hThread);
    stdin_read.reset();
    stderr_write.reset();

    std::vector<std::uint8_t> stderr_bytes;
    std::thread stderr_thread(ReadPipe, stderr_read.get(), &stderr_bytes);

    bool write_ok = true;
    std::vector<std::uint8_t> frame_pixels;
    for (int frame = start_frame; frame <= end_frame; ++frame) {
        if (!RenderFrame(frame, info.width, info.height, &frame_pixels)) {
            write_ok = false;
            *error = L"フレーム " + std::to_wstring(frame) + L" のレンダリングに失敗しました";
            break;
        }
        if (!alpha) {
            OpaqueAlpha(&frame_pixels);
        }
        if (!WriteAll(stdin_write.get(), frame_pixels.data(), frame_pixels.size())) {
            write_ok = false;
            *error = L"ffmpeg.exe への書き込みに失敗しました: " + FormatWin32Error(GetLastError());
            break;
        }
    }

    stdin_write.reset();
    WaitForSingleObject(process_handle.get(), INFINITE);

    DWORD exit_code = 1;
    GetExitCodeProcess(process_handle.get(), &exit_code);

    stderr_thread.join();

    if (!write_ok) {
        return false;
    }
    LARGE_INTEGER output_size{};
    Handle output_file(CreateFileW(output_path.c_str(),
                                   GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr,
                                   OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL,
                                   nullptr));
    const bool output_ok = output_file && GetFileSizeEx(output_file.get(), &output_size) && output_size.QuadPart > 0;
    if (exit_code != 0 || !output_ok) {
        std::wstring ffmpeg_error = Utf8ToWide(stderr_bytes);
        if (ffmpeg_error.empty()) {
            ffmpeg_error = L"(ffmpeg returned no error text)";
        }
        *error = L"ffmpeg.exe が失敗しました: " + ffmpeg_error;
        return false;
    }
    return true;
}

bool EnsureOleInitialized(std::wstring* error)
{
    const DWORD current_thread = GetCurrentThreadId();
    if (g_ole_initialized) {
        if (g_ole_thread_id == current_thread) {
            return true;
        }
        *error = L"クリップボードのCOM初期化スレッドが一致しません。AviUtl2を再起動してください";
        return false;
    }

    const HRESULT hr = OleInitialize(nullptr);
    if (FAILED(hr)) {
        *error = L"OleInitialize に失敗しました。HRESULT=" + FormatHresult(hr);
        return false;
    }

    g_ole_initialized = true;
    g_ole_thread_id = current_thread;
    return true;
}

bool VerifyVirtualFileDataObject(IDataObject* object, std::wstring* error)
{
    if (!object) {
        *error = L"クリップボードデータの作成に失敗しました";
        return false;
    }

    const CLIPFORMAT file_descriptor_format =
        static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileGroupDescriptorW"));
    const CLIPFORMAT file_contents_format =
        static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileContents"));

    FORMATETC descriptor{file_descriptor_format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    HRESULT hr = object->QueryGetData(&descriptor);
    if (FAILED(hr)) {
        *error = L"FileGroupDescriptorW をクリップボードから取得できません。HRESULT=" + FormatHresult(hr);
        return false;
    }

    STGMEDIUM descriptor_medium{};
    hr = object->GetData(&descriptor, &descriptor_medium);
    if (FAILED(hr)) {
        *error = L"FileGroupDescriptorW のデータ取得に失敗しました。HRESULT=" + FormatHresult(hr);
        return false;
    }
    ReleaseStgMedium(&descriptor_medium);

    FORMATETC contents_stream{file_contents_format, nullptr, DVASPECT_CONTENT, 0, TYMED_ISTREAM};
    hr = object->QueryGetData(&contents_stream);
    if (FAILED(hr)) {
        FORMATETC contents_hglobal{file_contents_format, nullptr, DVASPECT_CONTENT, 0, TYMED_HGLOBAL};
        hr = object->QueryGetData(&contents_hglobal);
    }
    if (FAILED(hr)) {
        *error = L"FileContents をクリップボードから取得できません。HRESULT=" + FormatHresult(hr);
        return false;
    }

    STGMEDIUM contents_medium{};
    hr = object->GetData(&contents_stream, &contents_medium);
    if (FAILED(hr)) {
        FORMATETC contents_hglobal{file_contents_format, nullptr, DVASPECT_CONTENT, 0, TYMED_HGLOBAL};
        hr = object->GetData(&contents_hglobal, &contents_medium);
    }
    if (FAILED(hr)) {
        *error = L"FileContents のデータ取得に失敗しました。HRESULT=" + FormatHresult(hr);
        return false;
    }
    ReleaseStgMedium(&contents_medium);

    return true;
}

bool SetVirtualFileClipboard(const std::wstring& file_name,
                             std::vector<std::uint8_t> data,
                             std::wstring* error)
{
    std::lock_guard<std::mutex> lock(g_clipboard_mutex);
    if (!EnsureOleInitialized(error)) {
        return false;
    }

    auto* object = new VirtualFileDataObject(file_name, std::move(data));
    if (!VerifyVirtualFileDataObject(object, error)) {
        object->Release();
        return false;
    }

    const HRESULT hr = OleSetClipboard(object);
    if (FAILED(hr)) {
        object->Release();
        *error = L"クリップボードへの設定に失敗しました。HRESULT=" + FormatHresult(hr);
        return false;
    }

    IDataObject* old_object = g_clipboard_object;
    g_clipboard_object = object;
    if (old_object) {
        old_object->Release();
    }
    return true;
}

void ReleaseClipboardResources()
{
    std::lock_guard<std::mutex> lock(g_clipboard_mutex);
    if (g_ole_initialized && g_ole_thread_id == GetCurrentThreadId() && g_clipboard_object) {
        if (OleIsCurrentClipboard(g_clipboard_object) == S_OK) {
            OleFlushClipboard();
        }
    }

    if (g_clipboard_object) {
        g_clipboard_object->Release();
        g_clipboard_object = nullptr;
    }

    if (g_ole_initialized && g_ole_thread_id == GetCurrentThreadId()) {
        OleUninitialize();
        g_ole_initialized = false;
        g_ole_thread_id = 0;
    }
}

bool EnsureDirectory(const std::wstring& path, std::wstring* error)
{
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
            return true;
        }
        *error = L"temp の作成先に同名ファイルがあります: " + path;
        return false;
    }

    if (CreateDirectoryW(path.c_str(), nullptr)) {
        return true;
    }
    const DWORD last_error = GetLastError();
    if (last_error == ERROR_ALREADY_EXISTS) {
        return true;
    }
    *error = L"temp フォルダを作成できません: " + path + L"\n" + FormatWin32Error(last_error);
    return false;
}

std::wstring BuildTempFilePath(const wchar_t* label, const wchar_t* extension, std::wstring* error)
{
    const std::wstring plugin_dir = ModuleDirectory();
    if (plugin_dir.empty()) {
        *error = L"プラグインフォルダを取得できません。";
        return {};
    }

    const std::wstring temp_dir = plugin_dir + L"\\temp";
    if (!EnsureDirectory(temp_dir, error)) {
        return {};
    }

    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t file_name[160]{};
    swprintf_s(file_name,
               L"aviutl2_clip_%s_%04u%02u%02u_%02u%02u%02u_%03u_%lu.%s",
               label,
               now.wYear,
               now.wMonth,
               now.wDay,
               now.wHour,
               now.wMinute,
               now.wSecond,
               now.wMilliseconds,
               static_cast<unsigned long>(GetTickCount()),
               extension);
    return temp_dir + L"\\" + file_name;
}

std::wstring BuildTempVideoPath(bool alpha, std::wstring* error)
{
    return BuildTempFilePath(alpha ? L"alpha" : L"video", alpha ? L"mov" : L"mp4", error);
}

std::wstring BuildTempAudioPath(std::wstring* error)
{
    return BuildTempFilePath(L"audio", L"f32le", error);
}

bool WriteBytesToFile(const std::wstring& path, const std::vector<std::uint8_t>& data, std::wstring* error)
{
    Handle file(CreateFileW(path.c_str(),
                            GENERIC_WRITE,
                            0,
                            nullptr,
                            CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL,
                            nullptr));
    if (!file) {
        *error = L"動画ファイルを作成できません: " + path + L"\n" + FormatWin32Error(GetLastError());
        return false;
    }

    size_t offset = 0;
    while (offset < data.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(data.size() - offset, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(file.get(), data.data() + offset, chunk, &written, nullptr) || written == 0) {
            *error = L"動画ファイルへの書き込みに失敗しました: " + path + L"\n" + FormatWin32Error(GetLastError());
            return false;
        }
        offset += written;
    }

    FlushFileBuffers(file.get());
    return true;
}

bool WriteAudioRangeToFile(const EDIT_INFO& info,
                           int start_frame,
                           int end_frame,
                           const std::wstring& path,
                           bool* has_audio,
                           std::wstring* error)
{
    *has_audio = false;
    if (!g_edit->rendering_scene_audio || info.sample_rate <= 0) {
        return true;
    }

    Handle file(CreateFileW(path.c_str(),
                            GENERIC_WRITE,
                            0,
                            nullptr,
                            CREATE_ALWAYS,
                            FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
                            nullptr));
    if (!file) {
        *error = L"音声一時ファイルを作成できません: " + path + L"\n" + FormatWin32Error(GetLastError());
        return false;
    }

    long long total_samples = 0;
    std::vector<float> samples;
    for (int frame = start_frame; frame <= end_frame; ++frame) {
        if (!RenderAudioFrame(frame, &samples)) {
            *error = L"フレーム " + std::to_wstring(frame) + L" の音声レンダリングに失敗しました";
            return false;
        }
        if (!samples.empty()) {
            if (!WriteAll(file.get(), samples.data(), samples.size() * sizeof(float))) {
                *error = L"音声一時ファイルへの書き込みに失敗しました: " + path + L"\n" + FormatWin32Error(GetLastError());
                return false;
            }
            total_samples += static_cast<long long>(samples.size() / 2);
        }
    }

    FlushFileBuffers(file.get());
    *has_audio = total_samples > 0;
    return true;
}

HGLOBAL CreateHDropForFile(const std::wstring& path)
{
    const SIZE_T bytes = sizeof(DROPFILES) + (path.size() + 2) * sizeof(wchar_t);
    HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!global) {
        return nullptr;
    }

    auto* drop_files = static_cast<DROPFILES*>(GlobalLock(global));
    if (!drop_files) {
        GlobalFree(global);
        return nullptr;
    }

    drop_files->pFiles = sizeof(DROPFILES);
    drop_files->fWide = TRUE;
    auto* file_list = reinterpret_cast<wchar_t*>(reinterpret_cast<std::uint8_t*>(drop_files) + sizeof(DROPFILES));
    std::memcpy(file_list, path.c_str(), (path.size() + 1) * sizeof(wchar_t));
    file_list[path.size() + 1] = L'\0';
    GlobalUnlock(global);
    return global;
}

bool OpenClipboardWithRetry(HWND owner)
{
    for (int i = 0; i < 20; ++i) {
        if (OpenClipboard(owner)) {
            return true;
        }
        Sleep(25);
    }
    return false;
}

bool SetFileClipboard(const std::wstring& path, std::wstring* error)
{
    std::lock_guard<std::mutex> lock(g_clipboard_mutex);

    if (g_clipboard_object) {
        g_clipboard_object->Release();
        g_clipboard_object = nullptr;
    }

    if (!OpenClipboardWithRetry(HostWindow())) {
        *error = L"クリップボードを開けません: " + FormatWin32Error(GetLastError());
        return false;
    }

    if (!EmptyClipboard()) {
        const DWORD last_error = GetLastError();
        CloseClipboard();
        *error = L"クリップボードを初期化できません: " + FormatWin32Error(last_error);
        return false;
    }

    HGLOBAL hdrop = CreateHDropForFile(path);
    if (!hdrop) {
        CloseClipboard();
        *error = L"CF_HDROP データを作成できません";
        return false;
    }
    if (!SetClipboardData(CF_HDROP, hdrop)) {
        const DWORD last_error = GetLastError();
        GlobalFree(hdrop);
        CloseClipboard();
        *error = L"CF_HDROP をクリップボードへ設定できません: " + FormatWin32Error(last_error);
        return false;
    }

    const CLIPFORMAT preferred_drop_format =
        static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"Preferred DropEffect"));
    const DWORD effect = DROPEFFECT_COPY;
    HGLOBAL preferred_drop = CopyToHGlobal(&effect, sizeof(effect));
    if (preferred_drop) {
        if (!SetClipboardData(preferred_drop_format, preferred_drop)) {
            GlobalFree(preferred_drop);
        }
    }

    CloseClipboard();
    return true;
}

void CopySelectedRangeVideo(bool alpha)
{
    bool expected = false;
    if (!g_copying.compare_exchange_strong(expected, true)) {
        ShowMessage(MB_ICONINFORMATION, L"現在コピー処理中です");
        return;
    }

    struct CopyingGuard {
        ~CopyingGuard() { g_copying = false; }
    } guard;

    if (!g_edit) {
        ShowMessage(MB_ICONERROR, L"編集ハンドルが初期化されていません");
        return;
    }

    if (g_edit->get_edit_state && g_edit->get_edit_state() != EDIT_HANDLE::EDIT_STATE_EDIT) {
        ShowMessage(MB_ICONWARNING, L"編集中の状態で実行してください");
        return;
    }

    EDIT_INFO info{};
    g_edit->get_edit_info(&info, sizeof(info));
    if (info.width <= 0 || info.height <= 0 || info.rate <= 0 || info.scale <= 0) {
        ShowMessage(MB_ICONERROR, L"シーン情報を取得できませんでした");
        return;
    }

    int start = info.select_range_start;
    int end = info.select_range_end;
    if (start < 0 || end < 0) {
        ShowMessage(MB_ICONWARNING, L"フレーム範囲を選択してから実行してください");
        return;
    }
    if (start > end) {
        std::swap(start, end);
    }
    start = std::max(0, start);
    end = std::min(end, info.frame_max);
    if (start > end) {
        ShowMessage(MB_ICONWARNING, L"有効なフレーム範囲がありません");
        return;
    }

    const int frame_count = end - start + 1;
    LogInfo(L"Clipboard Video Copy: encoding " + std::to_wstring(frame_count) + L" frames");

    std::wstring error;
    std::wstring file_path = BuildTempVideoPath(alpha, &error);
    if (file_path.empty()) {
        LogError(error);
        ShowMessage(MB_ICONERROR, error);
        return;
    }

    std::wstring audio_path = BuildTempAudioPath(&error);
    if (audio_path.empty()) {
        LogError(error);
        ShowMessage(MB_ICONERROR, error);
        return;
    }

    bool has_audio = false;
    if (!WriteAudioRangeToFile(info, start, end, audio_path, &has_audio, &error)) {
        DeleteFileW(audio_path.c_str());
        LogError(error);
        ShowMessage(MB_ICONERROR, error);
        return;
    }
    if (!has_audio) {
        DeleteFileW(audio_path.c_str());
        audio_path.clear();
    }

    if (!EncodeVideoToFile(info, start, end, alpha, audio_path, file_path, &error)) {
        if (!audio_path.empty()) {
            DeleteFileW(audio_path.c_str());
        }
        DeleteFileW(file_path.c_str());
        LogError(error);
        ShowMessage(MB_ICONERROR, error);
        return;
    }
    if (!audio_path.empty()) {
        DeleteFileW(audio_path.c_str());
    }

    if (!SetFileClipboard(file_path, &error)) {
        LogError(error);
        ShowMessage(MB_ICONERROR, error);
        return;
    }

    ShowMessage(MB_ICONINFORMATION,
                alpha
                    ? L"選択範囲の透過動画をクリップボードにコピーしました\n" + file_path
                    : L"選択範囲の動画をクリップボードにコピーしました\n" + file_path);
}

BOOL WINAPI HookTrackPopupMenu(HMENU menu,
                              UINT flags,
                              int x,
                              int y,
                              int reserved,
                              HWND hwnd,
                              const RECT* rect)
{
    EnsureVideoMenuItems(menu);
    BOOL command = TrueTrackPopupMenu(menu, flags, x, y, reserved, hwnd, rect);
    bool alpha = false;
    if (IsVideoMenuCommand(menu, command, &alpha)) {
        CopySelectedRangeVideo(alpha);
        return 0;
    }
    return command;
}

BOOL WINAPI HookTrackPopupMenuEx(HMENU menu, UINT flags, int x, int y, HWND hwnd, LPTPMPARAMS params)
{
    EnsureVideoMenuItems(menu);
    BOOL command = TrueTrackPopupMenuEx(menu, flags, x, y, hwnd, params);
    bool alpha = false;
    if (IsVideoMenuCommand(menu, command, &alpha)) {
        CopySelectedRangeVideo(alpha);
        return 0;
    }
    return command;
}

bool AttachHooks()
{
    DetourRestoreAfterWith();
    if (DetourTransactionBegin() != NO_ERROR) {
        return false;
    }
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(reinterpret_cast<PVOID*>(&TrueTrackPopupMenu), HookTrackPopupMenu);
    DetourAttach(reinterpret_cast<PVOID*>(&TrueTrackPopupMenuEx), HookTrackPopupMenuEx);
    const LONG error = DetourTransactionCommit();
    if (error != NO_ERROR) {
        LogError(L"Clipboard Video Copy: Detour attach failed: " + std::to_wstring(error));
        return false;
    }
    return true;
}

void DetachHooks()
{
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(reinterpret_cast<PVOID*>(&TrueTrackPopupMenu), HookTrackPopupMenu);
    DetourDetach(reinterpret_cast<PVOID*>(&TrueTrackPopupMenuEx), HookTrackPopupMenuEx);
    DetourTransactionCommit();
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (DetourIsHelperProcess()) {
        return TRUE;
    }
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

EXTERN_C __declspec(dllexport) DWORD RequiredVersion()
{
    return 2003300;
}

EXTERN_C __declspec(dllexport) void InitializeLogger(LOG_HANDLE* logger)
{
    g_logger = logger;
}

EXTERN_C __declspec(dllexport) bool InitializePlugin(DWORD)
{
    return AttachHooks();
}

EXTERN_C __declspec(dllexport) void UninitializePlugin()
{
    DetachHooks();
    ReleaseClipboardResources();
}

EXTERN_C __declspec(dllexport) COMMON_PLUGIN_TABLE* GetCommonPluginTable()
{
    return &g_common_plugin_table;
}

EXTERN_C __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE* host)
{
    if (host && host->create_edit_handle) {
        g_edit = host->create_edit_handle();
    }
}
