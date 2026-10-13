#pragma once
// NOT the Windows Shell: a tiny COM-shaped IFileOpenDialog over NSOpenPanel, for OpenFlowsMac.mm (PickPaths).
// Only the members used by the file-opening flow exist. CoCreateInstance(CLSID_FileOpenDialog) creates it;
// Show runs the panel modally (attached to nothing) and returns S_OK or a failure HRESULT when cancelled.
// File types: the FIRST filter's patterns restrict the panel (Windows pre-selects the first filter);
// "*.*" means no restriction. Implementation: src/FileDialogMac.mm.
#include "windows.h"
#include "wrl/client.h"

struct GUID {
    std::uint32_t Data1;
    unsigned short Data2, Data3;
    unsigned char Data4[8];
};
typedef GUID IID;
typedef GUID CLSID;

#define CLSCTX_INPROC_SERVER 0x1u

enum : DWORD {
    FOS_PICKFOLDERS = 0x20,
    FOS_FORCEFILESYSTEM = 0x40,
    FOS_ALLOWMULTISELECT = 0x200,
    FOS_PATHMUSTEXIST = 0x800,
    FOS_FILEMUSTEXIST = 0x1000,
};
enum SIGDN : int { SIGDN_FILESYSPATH = (int)0x80058000 };

struct COMDLG_FILTERSPEC {
    LPCWSTR pszName;
    LPCWSTR pszSpec;
};

struct IUnknown {
    virtual unsigned long AddRef() = 0;
    virtual unsigned long Release() = 0;
protected:
    virtual ~IUnknown() = default;
};

struct IShellItem : IUnknown {
    static constexpr GUID kIid = {0x43826d1e, 0xe718, 0x42ee, {0xbc, 0x55, 0xa1, 0xe2, 0x61, 0xc3, 0x7b, 0xfe}};
    /// SIGDN_FILESYSPATH: the POSIX path (free with CoTaskMemFree).
    virtual HRESULT GetDisplayName(SIGDN kind, PWSTR* name) = 0;
};

struct IShellItemArray : IUnknown {
    static constexpr GUID kIid = {0xb63ea76d, 0x1f85, 0x456f, {0xa1, 0x9c, 0x48, 0x15, 0x9e, 0xfa, 0x85, 0x8b}};
    virtual HRESULT GetCount(DWORD* count) = 0;
    virtual HRESULT GetItemAt(DWORD index, IShellItem** item) = 0;
};

struct IFileOpenDialog : IUnknown {
    static constexpr GUID kIid = {0xd57c7288, 0xd4ad, 0x4768, {0xbe, 0x02, 0x9d, 0x96, 0x95, 0x32, 0xd9, 0x60}};
    virtual HRESULT SetClientGuid(const GUID& guid) = 0;   // keys the remembered directory
    virtual HRESULT GetOptions(DWORD* options) = 0;
    virtual HRESULT SetOptions(DWORD options) = 0;
    virtual HRESULT SetTitle(LPCWSTR title) = 0;
    virtual HRESULT SetFileTypes(UINT count, const COMDLG_FILTERSPEC* specs) = 0;
    virtual HRESULT Show(HWND owner) = 0;
    virtual HRESULT GetResults(IShellItemArray** items) = 0;
};

extern const CLSID CLSID_FileOpenDialog;

namespace shimcom {
template <class T> const IID& IidOf(T**) { return T::kIid; }
}
#define IID_PPV_ARGS(pp) ::shimcom::IidOf(pp), reinterpret_cast<void**>(pp)

/// CLSID_FileOpenDialog + IFileOpenDialog's IID: a new dialog in *out (one reference). E_FAIL otherwise.
HRESULT CoCreateInstance(const CLSID& clsid, void* outer, DWORD context, const IID& iid, void** out);
void CoTaskMemFree(void* p);
