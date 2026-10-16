// shim/shobjidl.h over NSOpenPanel: the IFileOpenDialog that OpenFlowsMac.mm (PickPaths) creates with
// CoCreateInstance. Modal, on the main thread.
#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "shobjidl.h"
#include "OpenFlowsMac.h"

const CLSID CLSID_FileOpenDialog = {0xdc1c5a9c, 0xe88a, 0x4dde, {0xa5, 0xa1, 0x60, 0xf8, 0x2a, 0x20, 0xae, 0xf7}};

namespace {

NSString* ToNS(const std::wstring& w) {
    return [[NSString alloc] initWithBytes:w.data() length:w.size() * sizeof(wchar_t) encoding:NSUTF32LittleEndianStringEncoding];
}
std::wstring ToW(NSString* s) {
    if (!s) return {};
    NSData* d = [s dataUsingEncoding:NSUTF32LittleEndianStringEncoding];
    return std::wstring(static_cast<const wchar_t*>(d.bytes), d.length / sizeof(wchar_t));
}
bool SameGuid(const GUID& a, const GUID& b) { return std::memcmp(&a, &b, sizeof(GUID)) == 0; }

/// The remembered directory per client GUID (the common dialog's per-GUID state), kept in the user defaults.
NSString* GuidKey(const GUID& g) {
    return [NSString stringWithFormat:@"OpenPanelDirectory.%08X-%04X-%04X", g.Data1, g.Data2, g.Data3];
}

template <class Base>
class RefCounted : public Base {
public:
    unsigned long AddRef() override { return ++refs_; }
    unsigned long Release() override {
        const unsigned long n = --refs_;
        if (n == 0) delete this;
        return n;
    }
private:
    std::atomic<unsigned long> refs_{1};
};

class ShellItem final : public RefCounted<IShellItem> {
public:
    explicit ShellItem(std::wstring path) : path_(std::move(path)) {}
    HRESULT GetDisplayName(SIGDN, PWSTR* name) override {
        if (!name) return E_FAIL;
        auto* p = static_cast<wchar_t*>(std::malloc((path_.size() + 1) * sizeof(wchar_t)));
        if (!p) return E_FAIL;
        path_.copy(p, path_.size());
        p[path_.size()] = 0;
        *name = p;
        return S_OK;
    }
private:
    std::wstring path_;
};

class ShellItemArray final : public RefCounted<IShellItemArray> {
public:
    explicit ShellItemArray(std::vector<std::wstring> paths) : paths_(std::move(paths)) {}
    HRESULT GetCount(DWORD* n) override { if (n) *n = (DWORD)paths_.size(); return S_OK; }
    HRESULT GetItemAt(DWORD i, IShellItem** item) override {
        if (!item || i >= paths_.size()) return E_FAIL;
        *item = new ShellItem(paths_[i]);
        return S_OK;
    }
private:
    std::vector<std::wstring> paths_;
};

class FileOpenDialog final : public RefCounted<IFileOpenDialog> {
public:
    HRESULT SetClientGuid(const GUID& g) override { guid_ = g; haveGuid_ = true; return S_OK; }
    HRESULT GetOptions(DWORD* o) override { if (o) *o = options_; return S_OK; }
    HRESULT SetOptions(DWORD o) override { options_ = o; return S_OK; }
    HRESULT SetTitle(LPCWSTR t) override { title_ = t ? t : L""; return S_OK; }
    HRESULT SetFileTypes(UINT n, const COMDLG_FILTERSPEC* specs) override {
        extensions_.clear();
        anyFile_ = n == 0;
        if (n > 0 && specs[0].pszSpec) {   // the first filter is the one Windows pre-selects
            std::wstring spec = specs[0].pszSpec;
            size_t start = 0;
            while (start <= spec.size()) {
                size_t end = spec.find(L';', start);
                if (end == std::wstring::npos) end = spec.size();
                std::wstring pat = spec.substr(start, end - start);
                while (!pat.empty() && pat.front() == L' ') pat.erase(pat.begin());
                if (pat == L"*.*" || pat == L"*") anyFile_ = true;
                else if (pat.rfind(L"*.", 0) == 0 && pat.size() > 2) extensions_.push_back(pat.substr(2));
                start = end + 1;
            }
        }
        return S_OK;
    }
    HRESULT Show(HWND owner) override {
        @autoreleasepool {
            NSOpenPanel* panel = [NSOpenPanel openPanel];
            const bool folders = (options_ & FOS_PICKFOLDERS) != 0;
            panel.canChooseDirectories = folders;
            panel.canChooseFiles = !folders;
            panel.allowsMultipleSelection = (options_ & FOS_ALLOWMULTISELECT) != 0;
            panel.resolvesAliases = YES;
            if (!title_.empty()) { panel.title = ToNS(title_); panel.message = ToNS(title_); }
            if (!folders && !anyFile_ && !extensions_.empty()) {
                NSMutableArray<UTType*>* types = [NSMutableArray array];
                for (const std::wstring& e : extensions_)
                    if (UTType* t = [UTType typeWithFilenameExtension:ToNS(e)]) [types addObject:t];
                if (types.count) panel.allowedContentTypes = types;
            }
            NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
            if (haveGuid_)
                if (NSString* dir = [defaults stringForKey:GuidKey(guid_)]) panel.directoryURL = [NSURL fileURLWithPath:dir isDirectory:YES];
            NSWindow* key = NSApp.keyWindow;
            app::RaiseDialogToOwner((__bridge void*)panel, owner ? (void*)owner : (__bridge void*)key);
            const NSModalResponse r = [panel runModal];
            [key makeKeyAndOrderFront:nil];
            if (r != NSModalResponseOK || panel.URLs.count == 0) return (HRESULT)0x800704C7;   // HRESULT_FROM_WIN32(ERROR_CANCELLED)
            results_.clear();
            for (NSURL* u in panel.URLs) results_.push_back(ToW(u.path));
            if (haveGuid_) {
                NSURL* dir = panel.URLs.firstObject.URLByDeletingLastPathComponent;   // the next panel opens next to the pick
                if (dir.path) [defaults setObject:dir.path forKey:GuidKey(guid_)];
            }
            return S_OK;
        }
    }
    HRESULT GetResults(IShellItemArray** items) override {
        if (!items || results_.empty()) return E_FAIL;
        *items = new ShellItemArray(results_);
        return S_OK;
    }
private:
    GUID guid_{};
    bool haveGuid_ = false;
    DWORD options_ = 0;
    std::wstring title_;
    std::vector<std::wstring> extensions_;
    bool anyFile_ = true;
    std::vector<std::wstring> results_;
};

}

HRESULT CoCreateInstance(const CLSID& clsid, void*, DWORD, const IID& iid, void** out) {
    if (!out) return E_FAIL;
    *out = nullptr;
    if (!SameGuid(clsid, CLSID_FileOpenDialog) || !SameGuid(iid, IFileOpenDialog::kIid)) return E_FAIL;
    *out = static_cast<IFileOpenDialog*>(new FileOpenDialog());
    return S_OK;
}

void CoTaskMemFree(void* p) { std::free(p); }
