/* D3D11 devices of the presenter: adapter selection by LUID, the D3D11 debug layer and AVHWDeviceContext wrapping.
 * Included by d3d11.c after the IIDs. */

static const GUID pr_IID_IDXGIFactory1   = { 0x770aae78, 0xf26f, 0x4dba, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } };
static const GUID pr_IID_ID3D11InfoQueue = { 0x6543dbb6, 0x1b48, 0x42f5, { 0xab, 0x82, 0xe9, 0x7e, 0xc7, 0x43, 0x26, 0xf6 } };

typedef struct PrAdapter {
    char name[128];
    unsigned vendor_id, device_id;
    uint32_t luid_high, luid_low;
} PrAdapter;

static void pr_luid_text(char *buf, size_t n, uint32_t high, uint32_t low)
{
    snprintf(buf, n, "%08lx:%08lx", (unsigned long)high, (unsigned long)low);
}

static void pr_adapter_from_desc(const DXGI_ADAPTER_DESC *d, PrAdapter *a)
{
    memset(a, 0, sizeof(*a));
    WideCharToMultiByte(CP_UTF8, 0, d->Description, -1, a->name, sizeof(a->name) - 1, NULL, NULL);
    a->vendor_id = d->VendorId;
    a->device_id = d->DeviceId;
    a->luid_high = (uint32_t)d->AdapterLuid.HighPart;
    a->luid_low = d->AdapterLuid.LowPart;
}

/* a new DXGI factory every time: after a device loss the old one enumerates stale adapters */
static HRESULT pr_create_factory(IDXGIFactory1 **f)
{
    HMODULE m = LoadLibraryW(L"dxgi.dll");
    HRESULT (WINAPI *create)(const GUID *, void **);

    *f = NULL;
    if (!m)
        return E_FAIL;
    create = (HRESULT (WINAPI *)(const GUID *, void **))(void *)GetProcAddress(m, "CreateDXGIFactory1");
    return create ? create(&pr_IID_IDXGIFactory1, (void **)f) : E_FAIL;
}

/* The adapter with this LUID (new reference) and its DXGI enumeration index (what the "device" string of the d3d11va /
 * d3d12va hwcontexts means); DXGI_ERROR_NOT_FOUND if the machine has none. */
static HRESULT pr_find_adapter(uint32_t high, uint32_t low, IDXGIAdapter1 **out, int *index)
{
    IDXGIFactory1 *f = NULL;
    HRESULT hr = pr_create_factory(&f);
    UINT i;

    *out = NULL;
    if (FAILED(hr))
        return hr;
    hr = DXGI_ERROR_NOT_FOUND;
    for (i = 0; ; i++) {
        IDXGIAdapter1 *a = NULL;
        DXGI_ADAPTER_DESC1 d;

        if (FAILED(f->lpVtbl->EnumAdapters1(f, i, &a)))
            break;
        if (SUCCEEDED(a->lpVtbl->GetDesc1(a, &d)) && (uint32_t)d.AdapterLuid.HighPart == high &&
            d.AdapterLuid.LowPart == low) {
            *out = a;
            if (index)
                *index = (int)i;
            hr = S_OK;
            break;
        }
        a->lpVtbl->Release(a);
    }
    f->lpVtbl->Release(f);
    return hr;
}

static void pr_adapter_error(const PrConfig *cfg, HRESULT hr, char *err, size_t err_size)
{
    char luid[24];

    pr_luid_text(luid, sizeof(luid), cfg->luid_high, cfg->luid_low);
    if (hr == DXGI_ERROR_NOT_FOUND)
        snprintf(err, err_size, "adapter %s: this machine has no GPU with that LUID", luid);
    else
        snprintf(err, err_size, "adapter %s: DXGI adapter lookup failed (0x%08lx)", luid, (unsigned long)hr);
}

#define PR_DBG_IDS 128

typedef struct PrDebug {
    ID3D11InfoQueue *iq;
    CRITICAL_SECTION cs;
    volatile LONG messages;         /* every message seen, repeated ones included */
    int nids;
    int ids[PR_DBG_IDS];           /* distinct message IDs logged so far */
    unsigned counts[PR_DBG_IDS];
    int64_t last_us;
} PrDebug;

/* NULL when the device has no debug layer */
static PrDebug *pr_dbg_create(ID3D11Device *dev)
{
    ID3D11InfoQueue *iq = NULL;
    PrDebug *d;

    if (FAILED(dev->lpVtbl->QueryInterface(dev, &pr_IID_ID3D11InfoQueue, (void **)&iq)) || !iq)
        return NULL;
    d = av_mallocz(sizeof(*d));
    if (!d) {
        iq->lpVtbl->Release(iq);
        return NULL;
    }
    d->iq = iq;
    InitializeCriticalSection(&d->cs);
    return d;
}

static void pr_dbg_log(PrDebug *d, const D3D11_MESSAGE *m, const char *where)
{
    static const char *const sev[] = { "corruption", "error", "warning", "info", "message" };
    static const char *const cat[] = { "application", "misc", "initialization", "cleanup", "compilation",
                                       "state_creation", "state_setting", "state_getting", "resource",
                                       "execution", "shader" };
    int level, i;

    InterlockedIncrement(&d->messages);
    for (i = 0; i < d->nids; i++)
        if (d->ids[i] == (int)m->ID)
            break;
    if (i < d->nids) {
        d->counts[i]++;
        return;
    }
    if (d->nids < PR_DBG_IDS) {
        d->ids[d->nids] = (int)m->ID;
        d->counts[d->nids++] = 1;
    }
    switch (m->Severity) {
    case D3D11_MESSAGE_SEVERITY_CORRUPTION:
    case D3D11_MESSAGE_SEVERITY_ERROR:   level = AV_LOG_ERROR;   break;
    case D3D11_MESSAGE_SEVERITY_WARNING: level = AV_LOG_WARNING; break;
    case D3D11_MESSAGE_SEVERITY_INFO:    level = AV_LOG_INFO;    break;
    default:                             level = AV_LOG_VERBOSE; break;
    }
    av_log(NULL, level, "d3d11-debug: %s: [%s %s #%d] %s\n", where,
           (unsigned)m->Severity < FF_ARRAY_ELEMS(sev) ? sev[m->Severity] : "?",
           (unsigned)m->Category < FF_ARRAY_ELEMS(cat) ? cat[m->Category] : "?", (int)m->ID,
           m->pDescription ? m->pDescription : "");
}

/* (windows.h turns the GetMessage member of the vtable into GetMessageA / W: the same macro hits this call) */
static HRESULT pr_iq_get_message(ID3D11InfoQueue *iq, UINT64 index, D3D11_MESSAGE *m, SIZE_T *len)
{
    return iq->lpVtbl->GetMessage(iq, index, m, len);
}

/* Moves the stored messages of the device to av_log ("d3d11-debug:"), each message ID once. rate_limited: at most every
 * 250 ms (the per-frame call). */
static void pr_dbg_drain(PrDebug *d, const char *where, int rate_limited)
{
    UINT64 n, i;

    if (!d)
        return;
    if (rate_limited && av_gettime_relative() - d->last_us < 250000)
        return;
    if (!TryEnterCriticalSection(&d->cs))
        return;
    d->last_us = av_gettime_relative();
    n = d->iq->lpVtbl->GetNumStoredMessages(d->iq);
    for (i = 0; i < n; i++) {
        SIZE_T len = 0;
        D3D11_MESSAGE *m;

        if (FAILED(pr_iq_get_message(d->iq, i, NULL, &len)) || !len)
            continue;
        m = av_malloc(len);
        if (!m)
            continue;
        if (SUCCEEDED(pr_iq_get_message(d->iq, i, m, &len)))
            pr_dbg_log(d, m, where);
        av_free(m);
    }
    if (n)
        d->iq->lpVtbl->ClearStoredMessages(d->iq);
    LeaveCriticalSection(&d->cs);
}

static void pr_dbg_free(PrDebug *d)
{
    int i;

    if (!d)
        return;
    pr_dbg_drain(d, "close", 0);
    for (i = 0; i < d->nids; i++)
        if (d->counts[i] > 1)
            av_log(NULL, AV_LOG_INFO, "d3d11-debug: message #%d was repeated %u times\n", d->ids[i], d->counts[i]);
    d->iq->lpVtbl->Release(d->iq);
    DeleteCriticalSection(&d->cs);
    av_free(d);
}

/* The one place that creates ID3D11Devices. cfg selects the adapter (LUID) and the debug layer (cfg may be NULL). When
 * dbg is not NULL it receives the debug layer state of the device, NULL if the layer is off or not installed (the
 * device is then created without D3D11_CREATE_DEVICE_DEBUG). Returns DXGI_ERROR_NOT_FOUND for an unknown LUID. */
static HRESULT pr_create_device(const PrConfig *cfg, ID3D11Device **dev, ID3D11DeviceContext **ctx,
                                 PrDebug **dbg)
{
    static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    const UINT base = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    PFN_D3D11_CREATE_DEVICE fn;
    HMODULE m = LoadLibraryW(L"d3d11.dll");
    IDXGIAdapter1 *ad = NULL;
    ID3D10Multithread *mt = NULL;
    HRESULT hr = E_FAIL;
    int pass;

    if (dbg)
        *dbg = NULL;
    if (!m)
        return E_FAIL;
    fn = (PFN_D3D11_CREATE_DEVICE)(void *)GetProcAddress(m, "D3D11CreateDevice");
    if (!fn)
        return E_FAIL;
    if (cfg && cfg->adapter_set) {
        hr = pr_find_adapter(cfg->luid_high, cfg->luid_low, &ad, NULL);
        if (FAILED(hr))
            return hr;
    }
    for (pass = cfg && cfg->debug ? 1 : 0; pass >= 0; pass--) {
        UINT flags = base | (pass ? D3D11_CREATE_DEVICE_DEBUG : 0);

        hr = fn((IDXGIAdapter *)ad, ad ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, NULL, flags,
                levels, 2, D3D11_SDK_VERSION, dev, NULL, ctx);
        if (FAILED(hr))
            hr = fn((IDXGIAdapter *)ad, ad ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, NULL, flags,
                    levels + 1, 1, D3D11_SDK_VERSION, dev, NULL, ctx);
        if (SUCCEEDED(hr))
            break;
        if (pass)
            av_log(NULL, AV_LOG_WARNING, "d3d11-debug: the D3D11 debug layer is not available (0x%08lx, "
                   "DXGI_ERROR_SDK_COMPONENT_MISSING = 0x%08lx), creating the device without it\n",
                   (unsigned long)hr, (unsigned long)DXGI_ERROR_SDK_COMPONENT_MISSING);
    }
    PR_REL(ad);
    if (FAILED(hr))
        return hr;
    if (dbg && cfg && cfg->debug)
        *dbg = pr_dbg_create(*dev);
    if (SUCCEEDED((*ctx)->lpVtbl->QueryInterface(*ctx, &pr_IID_ID3D10Multithread, (void **)&mt)) && mt) {
        ((ID3D10Multithread *)mt)->lpVtbl->SetMultithreadProtected((ID3D10Multithread *)mt, TRUE);
        ((ID3D10Multithread *)mt)->lpVtbl->Release((ID3D10Multithread *)mt);
    }
    return S_OK;
}

static void pr_create_device_error(const PrConfig *cfg, HRESULT hr, char *err, size_t err_size)
{
    if (cfg && cfg->adapter_set && hr == DXGI_ERROR_NOT_FOUND)
        pr_adapter_error(cfg, hr, err, err_size);
    else
        snprintf(err, err_size, "D3D11CreateDevice failed (0x%08lx)", (unsigned long)hr);
}

static void pr_hwdev_free(AVHWDeviceContext *ctx)
{
    pr_dbg_free(ctx->user_opaque);
    ctx->user_opaque = NULL;
}

/* An AVHWDeviceContext (D3D11VA) around dev, like the one hwcontext_d3d11va creates itself: it takes its own reference
 * of dev and fills what only d3d11va_device_create fills (device_desc: libavcodec/dxva2.c reads VendorId for its Intel /
 * AMD workarounds; is_uma). On success the hwdevice owns dbg (final drain when it is freed). flags_nv12 / flags_p010:
 * AVD3D11VADeviceContext.decoder_bind_flags[_p010]. */
static int pr_wrap_hwdevice(ID3D11Device *dev, PrDebug *dbg, UINT flags_nv12, UINT flags_p010,
                             AVBufferRef **out, char *err, size_t err_size)
{
    AVBufferRef *ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    AVHWDeviceContext *hd;
    AVD3D11VADeviceContext *hw;
    IDXGIDevice *dxdev = NULL;
    IDXGIAdapter *ad = NULL;
    D3D11_FEATURE_DATA_D3D11_OPTIONS2 o2;
    int ret;

    *out = NULL;
    if (!ref) {
        snprintf(err, err_size, "av_hwdevice_ctx_alloc failed");
        return AVERROR(ENOMEM);
    }
    hd = (AVHWDeviceContext *)ref->data;
    hw = hd->hwctx;
    dev->lpVtbl->AddRef(dev);
    hw->device = dev;
    memset(&o2, 0, sizeof(o2));
    if (SUCCEEDED(dev->lpVtbl->CheckFeatureSupport(dev, D3D11_FEATURE_D3D11_OPTIONS2, &o2, sizeof(o2))))
        hw->is_uma = o2.UnifiedMemoryArchitecture;
    if (SUCCEEDED(dev->lpVtbl->QueryInterface(dev, &pr_IID_IDXGIDevice, (void **)&dxdev)) &&
        SUCCEEDED(dxdev->lpVtbl->GetAdapter(dxdev, &ad)))
        ad->lpVtbl->GetDesc(ad, &hw->device_desc);
    PR_REL(ad);
    PR_REL(dxdev);
    av_log(NULL, AV_LOG_VERBOSE, "d3d11 device: vendor %04x device %04x (%ls), UMA %d\n", (unsigned)hw->device_desc.VendorId,
           (unsigned)hw->device_desc.DeviceId, hw->device_desc.Description, hw->is_uma);
    hw->decoder_bind_flags = flags_nv12;
    hw->decoder_bind_flags_p010 = flags_p010;
    ret = av_hwdevice_ctx_init(ref);
    if (ret < 0) {
        snprintf(err, err_size, "av_hwdevice_ctx_init failed (%s)", av_err2str(ret));
        av_buffer_unref(&ref);
        return ret;
    }
    if (dbg) {
        hd->user_opaque = dbg;
        hd->free = pr_hwdev_free;
    }
    *out = ref;
    return 0;
}

int pr_create_d3d11_device(const PrConfig *cfg, AVBufferRef **out, char *err, size_t err_size)
{
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    PrDebug *dbg = NULL;
    HRESULT hr;
    int ret;

    *out = NULL;
    hr = pr_create_device(cfg, &dev, &ctx, &dbg);
    if (FAILED(hr)) {
        pr_create_device_error(cfg, hr, err, err_size);
        return AVERROR_EXTERNAL;
    }
    ret = pr_wrap_hwdevice(dev, dbg, 0, 0, out, err, err_size);
    PR_REL(ctx);
    PR_REL(dev);
    if (ret < 0) {
        pr_dbg_free(dbg);
        return ret;
    }
    pr_dbg_drain(dbg, "init", 0);
    return 0;
}
