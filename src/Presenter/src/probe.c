/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * Decode capabilities of an adapter: the D3D11 video decoder profiles and output formats, and the adapter identity of a
 * D3D11VA device.
 */

#include <stdio.h>
#include <string.h>

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>

#include "presenter_core.h"

static const GUID pr_g_h264   = { 0x1b81be68, 0xa0c7, 0x11d3, { 0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5 } };
static const GUID pr_g_hevc   = { 0x5b11d51b, 0x2f4c, 0x4452, { 0xbc, 0xc3, 0x09, 0xf2, 0xa1, 0x16, 0x0c, 0xc0 } };
static const GUID pr_g_hevc10 = { 0x107af0e0, 0xef1a, 0x4d19, { 0xab, 0xa8, 0x67, 0xa1, 0x63, 0x07, 0x3d, 0x13 } };
static const GUID pr_g_vp9_0  = { 0x463707f8, 0xa1d0, 0x4585, { 0x87, 0x6d, 0x83, 0xaa, 0x6d, 0x60, 0xb8, 0x9e } };
static const GUID pr_g_vp9_2  = { 0xa4c749ef, 0x6ecf, 0x48aa, { 0x84, 0x48, 0x50, 0xa7, 0xa1, 0x16, 0x5f, 0xf7 } };
static const GUID pr_g_av1    = { 0xb8be4ccb, 0xcf53, 0x46ba, { 0x8d, 0x59, 0xd6, 0xb8, 0xa6, 0xda, 0x5d, 0x2a } };
static const GUID pr_g_mpeg2  = { 0xee27417f, 0x5e28, 0x4e65, { 0xbe, 0xea, 0x1d, 0x26, 0xb5, 0x08, 0xad, 0xc9 } };
static const GUID pr_g_vc1    = { 0x1b81bea3, 0xa0c7, 0x11d3, { 0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5 } };
static const GUID pr_g_vc1d   = { 0x1b81bea4, 0xa0c7, 0x11d3, { 0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5 } };
static const GUID pr_iid_dxgidevice = { 0x54ec77fa, 0x1377, 0x44e6, { 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c } };

static const struct {
    const char *codec;
    const GUID *g8, *g10;           /* decoder profile for 8 bit (NV12) / 10 bit (P010), NULL = none */
    const GUID *g8b;                /* another 8-bit profile of the codec */
} pr_codecs[] = {
    { "h264",       &pr_g_h264,  NULL,         NULL      },
    { "hevc",       &pr_g_hevc,  &pr_g_hevc10, NULL      },
    { "vp9",        &pr_g_vp9_0, &pr_g_vp9_2,  NULL      },
    { "av1",        &pr_g_av1,   &pr_g_av1,    NULL      },
    { "mpeg2video", &pr_g_mpeg2, NULL,         NULL      },
    { "vc1",        &pr_g_vc1d,  NULL,         &pr_g_vc1 },
};

static int pr_codec_index(const char *codec)
{
    int i;

    for (i = 0; i < (int)(sizeof(pr_codecs) / sizeof(pr_codecs[0])); i++)
        if (!strcmp(pr_codecs[i].codec, codec))
            return i;
    return -1;
}

/* D3D11 video decoder profile + output format supported by the device's GPU? */
static int pr_profile_supported(ID3D11VideoDevice *vd, const GUID *profile, DXGI_FORMAT fmt)
{
    UINT n, i;
    BOOL ok = FALSE;

    n = vd->lpVtbl->GetVideoDecoderProfileCount(vd);
    for (i = 0; i < n; i++) {
        GUID g;
        if (SUCCEEDED(vd->lpVtbl->GetVideoDecoderProfile(vd, i, &g)) && !memcmp(&g, profile, sizeof(g)))
            return SUCCEEDED(vd->lpVtbl->CheckVideoDecoderFormat(vd, profile, fmt, &ok)) && ok;
    }
    return 0;
}

int pr_d3d11_decoder_depths(AVBufferRef *d3d11, const char *codec, int depths[2])
{
    AVD3D11VADeviceContext *hw = ((AVHWDeviceContext *)d3d11->data)->hwctx;
    ID3D11VideoDevice *vd = hw ? hw->video_device : NULL;
    int c = pr_codec_index(codec), n = 0;

    if (!vd || c < 0)
        return 0;
    if ((pr_codecs[c].g8 && pr_profile_supported(vd, pr_codecs[c].g8, DXGI_FORMAT_NV12)) ||
        (pr_codecs[c].g8b && pr_profile_supported(vd, pr_codecs[c].g8b, DXGI_FORMAT_NV12)))
        depths[n++] = 8;
    if (pr_codecs[c].g10 && pr_profile_supported(vd, pr_codecs[c].g10, DXGI_FORMAT_P010))
        depths[n++] = 10;
    return n;
}

void pr_d3d11_adapter_info(AVBufferRef *d3d11, char *luid, size_t luid_size, char *name, size_t name_size,
                           unsigned *vendor, char *driver, size_t driver_size)
{
    AVD3D11VADeviceContext *hw = ((AVHWDeviceContext *)d3d11->data)->hwctx;
    IDXGIDevice *dxdev = NULL;
    IDXGIAdapter *adapter = NULL;
    DXGI_ADAPTER_DESC desc;
    LARGE_INTEGER umd;

    luid[0] = name[0] = driver[0] = 0;
    *vendor = 0;
    if (!hw || !hw->device ||
        FAILED(hw->device->lpVtbl->QueryInterface(hw->device, &pr_iid_dxgidevice, (void **)&dxdev)))
        return;
    if (SUCCEEDED(dxdev->lpVtbl->GetAdapter(dxdev, &adapter)) && SUCCEEDED(adapter->lpVtbl->GetDesc(adapter, &desc))) {
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, (int)name_size - 1, NULL, NULL);
        name[name_size - 1] = 0;
        snprintf(luid, luid_size, "%08lx:%08lx", (unsigned long)desc.AdapterLuid.HighPart,
                 (unsigned long)desc.AdapterLuid.LowPart);
        *vendor = desc.VendorId;
        umd.QuadPart = 0;
        if (SUCCEEDED(adapter->lpVtbl->CheckInterfaceSupport(adapter, &pr_iid_dxgidevice, &umd)))
            snprintf(driver, driver_size, "%u.%u.%u.%u", (unsigned)HIWORD(umd.HighPart), (unsigned)LOWORD(umd.HighPart),
                     (unsigned)HIWORD(umd.LowPart), (unsigned)LOWORD(umd.LowPart));
    }
    if (adapter)
        adapter->lpVtbl->Release(adapter);
    dxdev->lpVtbl->Release(dxdev);
}
