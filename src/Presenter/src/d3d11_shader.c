/* D3D11 decoder / upload surface -> the YUV pixel shader.
 * Included by d3d11.c after pr_get_input and pr_make_tex. */
static int pr_d3d11_shader_render(PrD3D11 *p, const PrInput *source, const AVFrame *f,
                                  ID3D11RenderTargetView *rtv,
                                  int tw, int th, const RECT *dst, int *copied,
                                  char *err, size_t err_size)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd;
    PrShaderInput in = { 0 };
    PrShaderGeometry geo = { 0 };
    ID3D11Texture2D *copy = NULL;
    ID3D11ShaderResourceView *y = NULL, *uv = NULL;
    DXGI_FORMAT formats[2];
    HRESULT hr;
    int i, result = -1;
    if (!p->shader && !(p->shader = pr_shader_create(p->dev, err, err_size))) {
        p->shader_failed = 1;
        return -1;
    }
    source->tex->lpVtbl->GetDesc(source->tex, &td);
    if (source->slice < 0 || (UINT)source->slice >= td.ArraySize) {
        pr_set_err(err, err_size, "invalid D3D11 array slice", E_INVALIDARG);
        return -1;
    }
    *copied = 0;
    /* Decoder pools are created with DECODER|SHADER_RESOURCE. A pool the driver rejected that with (or created without
     * it because the probe said so) and foreign pools lack the SRV flag: one device-local slice copy is sufficient. */
    if (!(td.BindFlags & D3D11_BIND_SHADER_RESOURCE)) {
        pr_record_fallback(p, "decoder_pool_no_srv");
        D3D11_TEXTURE2D_DESC cd = td;
        cd.ArraySize = 1;
        cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        cd.MiscFlags = 0;
        cd.CPUAccessFlags = 0;
        cd.Usage = D3D11_USAGE_DEFAULT;
        hr = p->dev->lpVtbl->CreateTexture2D(p->dev, &cd, NULL, &copy);
        if (FAILED(hr)) { pr_set_err(err, err_size, "shader fallback texture", hr); goto done; }
        p->ctx->lpVtbl->CopySubresourceRegion(p->ctx, (ID3D11Resource *)copy, 0, 0, 0, 0,
                    (ID3D11Resource *)source->tex,
                    (UINT)source->slice * td.MipLevels, NULL);
        td = cd;
        *copied = 1;
    }
    in.storage_width = td.Width;
    in.storage_height = td.Height;
    in.p010 = td.Format == DXGI_FORMAT_P010;
    geo.target_width = tw;
    geo.target_height = th;
    geo.destination = dst;
    if (td.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
        memset(&vd, 0, sizeof(vd));
        vd.Format = td.Format;
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        vd.Texture2DArray.MipLevels = 1;
        vd.Texture2DArray.ArraySize = 1;
        vd.Texture2DArray.FirstArraySlice = copy ? 0 : source->slice;
        hr = p->dev->lpVtbl->CreateShaderResourceView(p->dev,
                         (ID3D11Resource *)(copy ? copy : source->tex), &vd, &y);
        if (FAILED(hr)) { pr_set_err(err, err_size, "BGRA shader view", hr); goto done; }
        in.rgb = y;
    } else if (td.Format == DXGI_FORMAT_NV12 || td.Format == DXGI_FORMAT_P010) {
        formats[0] = in.p010 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
        formats[1] = in.p010 ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
        for (i = 0; i < 2; i++) {
            memset(&vd, 0, sizeof(vd));
            vd.Format = formats[i];
            vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            vd.Texture2DArray.MipLevels = 1;
            vd.Texture2DArray.FirstArraySlice = copy ? 0 : source->slice;
            vd.Texture2DArray.ArraySize = 1;
            hr = p->dev->lpVtbl->CreateShaderResourceView(p->dev,
                        (ID3D11Resource *)(copy ? copy : source->tex), &vd, i ? &uv : &y);
            if (FAILED(hr)) { pr_set_err(err, err_size, "YUV plane shader view", hr); goto done; }
        }
        in.ay = y;
        in.auv = uv;
        in.slice = 0;
    } else {
        pr_set_err(err, err_size, "unsupported shader texture format", E_INVALIDARG);
        goto done;
    }
    result = pr_shader_render(p->shader, p->ctx, rtv, &in, f, &geo, err, err_size);
done:
    PR_REL(uv);
    PR_REL(y);
    PR_REL(copy);
    return result;
}

static int pr_probe_d3d11_shader(const PrConfig *cfg)
{
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    ID3D11Texture2D *nv = NULL, *target = NULL, *staging = NULL;
    ID3D11ShaderResourceView *yview = NULL, *uvview = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    PrShader *shader = NULL;
    PrShaderInput in = { 0 };
    PrShaderGeometry geo = { 0 };
    AVFrame frame = { 0 };
    D3D11_TEXTURE2D_DESC td = { 0 };
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = { 0 };
    D3D11_MAPPED_SUBRESOURCE mapped;
    D3D11_SUBRESOURCE_DATA data = { 0 };
    uint8_t sample[4 * 4 + 4 * 2];
    char err[256] = "";
    PrDebug *dbg = NULL;
    int ok = 0, i;
    if (FAILED(pr_create_device(cfg, &dev, &ctx, &dbg))) goto done;
    memset(sample, 102, 16);
    memset(sample + 16, 128, 8);
    td.Width = td.Height = 4;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    data.pSysMem = sample;
    data.SysMemPitch = 4;
    if (FAILED(dev->lpVtbl->CreateTexture2D(dev, &td, &data, &nv))) goto done;
    for (i = 0; i < 2; i++) {
        sd.Format = i ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        sd.Texture2DArray.MipLevels = 1;
        sd.Texture2DArray.ArraySize = 1;
        if (FAILED(dev->lpVtbl->CreateShaderResourceView(dev, (ID3D11Resource *)nv,
                         &sd, i ? &uvview : &yview))) goto done;
    }
    shader = pr_shader_create(dev, err, sizeof(err));
    if (!shader) goto done;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(dev->lpVtbl->CreateTexture2D(dev, &td, NULL, &target)) ||
        FAILED(dev->lpVtbl->CreateRenderTargetView(dev, (ID3D11Resource *)target, NULL, &rtv))) goto done;
    in.ay = yview; in.auv = uvview;
    in.storage_width = in.storage_height = 4;
    geo.target_width = geo.target_height = 4;
    frame.width = frame.height = 4;
    frame.sample_aspect_ratio = (AVRational){ 1, 1 };
    frame.colorspace = AVCOL_SPC_BT709;
    if (pr_shader_render(shader, ctx, rtv, &in, &frame, &geo, err, sizeof(err)) < 0) goto done;
    td.BindFlags = 0;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(dev->lpVtbl->CreateTexture2D(dev, &td, NULL, &staging))) goto done;
    ctx->lpVtbl->CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)target);
    if (SUCCEEDED(ctx->lpVtbl->Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        const uint8_t *px = mapped.pData;
        ok = px[0] > 70 && px[0] < 150 && px[1] > 70 && px[1] < 150 &&
             px[2] > 70 && px[2] < 150 && px[3] == 255;
        ctx->lpVtbl->Unmap(ctx, (ID3D11Resource *)staging, 0);
    }
done:
    pr_dbg_drain(dbg, "probe", 0);
    pr_shader_free(shader);
    PR_REL(uvview); PR_REL(yview); PR_REL(rtv);
    PR_REL(staging); PR_REL(target); PR_REL(nv);
    PR_REL(ctx); PR_REL(dev);
    pr_dbg_free(dbg);
    return ok;
}
