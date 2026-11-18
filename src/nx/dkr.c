/*
 * deko3d renderer for the Switch build: the vkr.h API (src/vk/vkr.c is the
 * Vulkan one, src/web/wgr.c the WebGPU one), so vkr_backend.c drives it
 * unchanged.
 *
 * The shaders are the Vulkan build's (xbox.vert, xbox_vsh.vert, xbox.frag),
 * turned into deko3d GLSL at build time (deko_shader.cmake: push constants
 * become a uniform block, no descriptor sets, no specialization constants)
 * and compiled offline with uam. The combiners are interpreted per pixel,
 * as with RECOMP_VKR_SPEC=0 on the desktop.
 *
 * deko3d has no pipeline objects: blend, depth/stencil, colour-write and
 * rasterizer state are bound per draw, which is how the NV2A's state
 * arrives anyway.
 *
 * Frames: two slots, each with its own command memory, upload ring (vertices,
 * indices, uniforms, texture pixels), fence and readback buffers. Recording
 * a slot waits until its previous frame was released (vkr_frame_release).
 * Textures freed while recording are destroyed when their slot is next begun,
 * after every frame that could use them has finished.
 *
 * Not yet: offscreen targets (shadow maps: the executor draws them in
 * software), BC7 / HD textures, mip chains, MSAA, the scene and CRT passes.
 */
#if defined(__SWITCH__)
#include "vkr.h"

#include <deko3d.h>
#include <switch.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xbox_vert_dksh.h"
#include "xbox_vsh_vert_dksh.h"
#include "xbox_frag_dksh.h"
#include "xbox_frag_wdepth_dksh.h"

#define DKR_FRAMES     2
#define DKR_CMD_BYTES  (4u << 20)       /* per slot, grown by cb_add_mem */
#define DKR_UP_BYTES   (48u << 20)      /* per slot: everything a frame uploads */
#define DKR_MAX_IMG    8192             /* image descriptors (textures) */
#define DKR_MAX_SAMP   64
#define DKR_IMG_WHITE  0

typedef struct {
    uint32_t magic, header_sz, control_sz, code_sz, programs_off, num_programs;
} DkshHeader;

struct VkrTex {
    uint64_t key;
    uint32_t w, h, flags;
    uint32_t id;                        /* image descriptor index */
    uint32_t last_use;
    DkMemBlock mem;
    DkImage img;
    VkrTex *next_free;                  /* deferred destruction list */
};

typedef struct {
    DkCmdBuf cmd;
    DkMemBlock cmd_mem;
    DkMemBlock extra[32];               /* command memory added mid-frame */
    int nextra;
    DkMemBlock up;
    uint8_t *up_cpu;
    DkGpuAddr up_gpu;
    uint32_t up_used;
    DkFence fence;
    int busy, submitted;
    DkMemBlock rb, rbh;                 /* guest-size and full-size readback */
    uint32_t rb_bytes, rbh_bytes;
    VkrTex *dead;                       /* textures freed while it recorded */
} DkrFrame;

/* The per-draw "push constants" (xbox.vert / xbox_vsh.vert / xbox.frag PC). */
typedef struct {
    float inv_size[2], inv_zmax, fog_mode;
    float fog_param[2], point_size, point_params;
    uint32_t in_base, nin, prog_base, const_base;       /* xbox_vsh.vert */
    float squeeze2d, squeeze_all, hud_shift, keep_edges;
    uint32_t alpha_func, alpha_ref;                      /* xbox.frag, offset 64 */
    float w_scale;
    uint32_t point_sprite, tag;
    uint32_t pad[3];
} DkrPC;
_Static_assert(sizeof(DkrPC) == 96, "DkrPC layout");
_Static_assert(sizeof(VkrUniforms) == 320, "VkrUniforms layout");

struct VkrContext {
    DkDevice dev;
    DkQueue queue;
    DkMemBlock code;
    DkShader vs_fixed, vs_vsh, fs, fs_wdepth;
    DkMemBlock desc;                    /* image then sampler descriptors */
    DkImageDescriptor *img_desc;
    DkSamplerDescriptor *samp_desc;
    DkGpuAddr img_desc_gpu, samp_desc_gpu;
    uint32_t samp_key[DKR_MAX_SAMP];
    int nsamp;
    int desc_dirty;                     /* a descriptor written since the last invalidate */
    uint32_t free_ids[DKR_MAX_IMG];
    int nfree_ids;

    DkrFrame fr[DKR_FRAMES];
    int cur;                            /* slot being recorded, -1 none */
    int next;
    pthread_mutex_t lock;
    pthread_cond_t freed;

    /* targets */
    DkMemBlock color_mem, depth_mem, down_mem;
    DkImage color, depth, down;
    uint32_t w, h, sw, sh;
    float zmax;
    float scale, scale_x, squeeze2d, squeeze_all;
    uint32_t gsc[4];
    DkScissor sc;

    /* textures */
    VkrTex **texs;
    uint32_t ntexs, captexs;
    int32_t *hidx;
    uint32_t hcap;
    uint64_t tex_bytes;
    uint32_t tex_gen, flip_serial;
    int purge_req;
    VkrTex *white;

    /* GPU vertex programs: the program and constants uploaded this frame */
    uint32_t vsh_const_off, vsh_prog_off, vsh_prog_hash, vsh_prog_n;
    float vsh_consts[192][4];

    int said_full;
};

static void set_viewport(DkCmdBuf cmd, const DkViewport *vp);

static void dk_debug(void *user, const char *context, DkResult result, const char *message)
{
    (void)user;
    fprintf(stderr, "[DKR] %s: error %u: %s\n", context, (unsigned)result, message ? message : "");
}

static uint32_t align_up(uint32_t n, uint32_t a) { return (n + a - 1) & ~(a - 1); }

static DkMemBlock mem_block(VkrContext *c, uint32_t bytes, uint32_t flags)
{
    DkMemBlockMaker m;
    dkMemBlockMakerDefaults(&m, c->dev, align_up(bytes, DK_MEMBLOCK_ALIGNMENT));
    m.flags = flags;
    return dkMemBlockCreate(&m);
}

/* ---- shaders ------------------------------------------------------------ */

static int load_shader(VkrContext *c, DkShader *sh, const unsigned char *dksh, unsigned long len,
                       uint32_t *code_off)
{
    const DkshHeader *hdr = (const DkshHeader *)dksh;
    DkShaderMaker m;
    if (len < sizeof *hdr || hdr->magic != 0x48534B44u || hdr->control_sz + hdr->code_sz > len)
        return 0;
    memcpy((uint8_t *)dkMemBlockGetCpuAddr(c->code) + *code_off, dksh + hdr->control_sz, hdr->code_sz);
    dkShaderMakerDefaults(&m, c->code, *code_off);
    m.control = dksh;                   /* the control section is the file's head */
    dkShaderInitialize(sh, &m);
    *code_off = align_up(*code_off + hdr->code_sz, DK_SHADER_CODE_ALIGNMENT);
    return 1;
}

static int load_shaders(VkrContext *c)
{
    uint32_t off = 0, total = xbox_vert_dksh_len + xbox_vsh_vert_dksh_len + xbox_frag_dksh_len
                            + xbox_frag_wdepth_dksh_len + 4 * DK_SHADER_CODE_ALIGNMENT
                            + DK_SHADER_CODE_UNUSABLE_SIZE;
    c->code = mem_block(c, total, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached
                                  | DkMemBlockFlags_Code);
    if (!c->code)
        return 0;
    return load_shader(c, &c->vs_fixed, xbox_vert_dksh, xbox_vert_dksh_len, &off)
        && load_shader(c, &c->vs_vsh, xbox_vsh_vert_dksh, xbox_vsh_vert_dksh_len, &off)
        && load_shader(c, &c->fs, xbox_frag_dksh, xbox_frag_dksh_len, &off)
        && load_shader(c, &c->fs_wdepth, xbox_frag_wdepth_dksh, xbox_frag_wdepth_dksh_len, &off);
}

/* ---- descriptors -------------------------------------------------------- */

static int image_id_alloc(VkrContext *c)
{
    return c->nfree_ids ? (int)c->free_ids[--c->nfree_ids] : -1;
}

static void image_desc_write(VkrContext *c, uint32_t id, const DkImage *img)
{
    DkImageView v;
    dkImageViewDefaults(&v, img);
    dkImageDescriptorInitialize(&c->img_desc[id], &v, false, false);
    c->desc_dirty = 1;
}

/* NV2A address modes: 1 wrap, 2 mirror, 3 clamp, 4 border, 5 clamp-to-edge */
static DkWrapMode wrap_mode(uint32_t a)
{
    switch (a) {
    case 2:  return DkWrapMode_MirroredRepeat;
    case 3: case 5: return DkWrapMode_ClampToEdge;
    case 4:  return DkWrapMode_ClampToBorder;
    default: return DkWrapMode_Repeat;
    }
}

static uint32_t sampler_id(VkrContext *c, uint32_t au, uint32_t av, int linear)
{
    uint32_t key = (au & 7) | (av & 7) << 3 | (uint32_t)(linear != 0) << 6;
    DkSampler s;
    int i;
    for (i = 0; i < c->nsamp; i++)
        if (c->samp_key[i] == key)
            return (uint32_t)i;
    if (c->nsamp == DKR_MAX_SAMP)
        return 0;
    dkSamplerDefaults(&s);
    s.minFilter = s.magFilter = linear ? DkFilter_Linear : DkFilter_Nearest;
    s.mipFilter = DkMipFilter_None;
    s.wrapMode[0] = wrap_mode(au);
    s.wrapMode[1] = wrap_mode(av);
    s.wrapMode[2] = DkWrapMode_Repeat;
    s.borderColor[0].value_f = s.borderColor[1].value_f = 0.0f;   /* transparent black */
    s.borderColor[2].value_f = s.borderColor[3].value_f = 0.0f;
    dkSamplerDescriptorInitialize(&c->samp_desc[c->nsamp], &s);
    c->samp_key[c->nsamp] = key;
    c->desc_dirty = 1;
    return (uint32_t)c->nsamp++;
}

/* ---- frames ------------------------------------------------------------- */

static void cb_add_mem(void *user, DkCmdBuf cmd, size_t min_req)
{
    VkrContext *c = (VkrContext *)user;
    DkrFrame *f = &c->fr[0];
    uint32_t bytes = min_req > (1u << 20) ? (uint32_t)min_req : (1u << 20);
    DkMemBlock m;
    for (int i = 0; i < DKR_FRAMES; i++)
        if (c->fr[i].cmd == cmd)
            f = &c->fr[i];
    if (f->nextra == (int)(sizeof f->extra / sizeof f->extra[0])) {
        fprintf(stderr, "[DKR] command memory exhausted\n");
        abort();
    }
    m = mem_block(c, bytes, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    f->extra[f->nextra++] = m;
    dkCmdBufAddMemory(cmd, m, 0, align_up(bytes, DK_MEMBLOCK_ALIGNMENT));
}

static void tex_destroy(VkrContext *c, VkrTex *t);

/* Start recording into the next slot once its last frame is done with. */
static int begin(VkrContext *c)
{
    DkrFrame *f;
    int i;
    if (c->cur >= 0)
        return 1;
    f = &c->fr[c->next];
    pthread_mutex_lock(&c->lock);
    while (f->busy)
        pthread_cond_wait(&c->freed, &c->lock);
    pthread_mutex_unlock(&c->lock);
    if (f->submitted) {
        dkFenceWait(&f->fence, -1);
        f->submitted = 0;
    }
    /* What was freed while this slot last recorded can go now. */
    while (f->dead) {
        VkrTex *t = f->dead;
        f->dead = t->next_free;
        if (t->id != DKR_IMG_WHITE)
            c->free_ids[c->nfree_ids++] = t->id;
        tex_destroy(c, t);
    }
    for (i = 0; i < f->nextra; i++)
        dkMemBlockDestroy(f->extra[i]);
    f->nextra = 0;
    dkCmdBufClear(f->cmd);
    dkCmdBufAddMemory(f->cmd, f->cmd_mem, 0, DKR_CMD_BYTES);
    f->up_used = 0;
    c->cur = c->next;
    c->vsh_const_off = c->vsh_prog_off = 0xFFFFFFFFu;

    /* State every frame starts from. */
    {
        DkCmdBuf cmd = f->cmd;
        DkRasterizerState rs;
        DkColorState cs;
        DkViewport vp = { 0, 0, (float)c->sw, (float)c->sh, 0.0f, 1.0f };
        dkCmdBufBindImageDescriptorSet(cmd, c->img_desc_gpu, DKR_MAX_IMG);
        dkCmdBufBindSamplerDescriptorSet(cmd, c->samp_desc_gpu, DKR_MAX_SAMP);
        dkRasterizerStateDefaults(&rs);
        rs.cullMode = DkFace_None;
        dkCmdBufBindRasterizerState(cmd, &rs);
        dkColorStateDefaults(&cs);
        dkCmdBufBindColorState(cmd, &cs);
        if (c->sw) {
            DkImageView cv, dv;
            dkImageViewDefaults(&cv, &c->color);
            dkImageViewDefaults(&dv, &c->depth);
            dkCmdBufBindRenderTarget(cmd, &cv, &dv);
            set_viewport(cmd, &vp);
            dkCmdBufSetScissors(cmd, 0, &c->sc, 1);
        }
    }
    return 1;
}

/* Room in the slot's upload ring: GPU address of `bytes` at `align`. */
static int alloc(VkrContext *c, uint32_t bytes, uint32_t align, uint8_t **cpu, DkGpuAddr *gpu,
                 uint32_t *off_out)
{
    DkrFrame *f;
    uint32_t off;
    if (!begin(c))
        return 0;
    f = &c->fr[c->cur];
    off = align_up(f->up_used, align);
    if (off + bytes > DKR_UP_BYTES) {
        if (!c->said_full++)
            fprintf(stderr, "[DKR] upload buffer full (%u MB this frame)\n", DKR_UP_BYTES >> 20);
        return 0;
    }
    f->up_used = off + bytes;
    *cpu = f->up_cpu + off;
    *gpu = f->up_gpu + off;
    if (off_out)
        *off_out = off;
    return 1;
}

static DkCmdBuf cmd_of(VkrContext *c) { return c->fr[c->cur].cmd; }

/* The shaders put y down in clip space (Vulkan); deko3d's clip space has it
 * up (its upper-left origin is the window's), so y is negated on the way. */
static void set_viewport(DkCmdBuf cmd, const DkViewport *vp)
{
    static const DkViewportSwizzle flip = {
        DkSwizzle_PositiveX, DkSwizzle_NegativeY, DkSwizzle_PositiveZ, DkSwizzle_PositiveW
    };
    dkCmdBufSetViewports(cmd, 0, vp, 1);
    dkCmdBufSetViewportSwizzles(cmd, 0, &flip, 1);
}

/* ---- context ------------------------------------------------------------ */

VkrContext *vkr_create(void)
{
    VkrContext *c = (VkrContext *)calloc(1, sizeof *c);
    DkDeviceMaker dm;
    DkQueueMaker qm;
    const char *r;
    double v;
    int i;

    if (!c)
        return NULL;
    dkDeviceMakerDefaults(&dm);         /* depth 0..1, origin upper left: as Vulkan */
    dm.cbDebug = dk_debug;
    if (!(c->dev = dkDeviceCreate(&dm)))
        goto fail;
    dkQueueMakerDefaults(&qm, c->dev);
    qm.flags = DkQueueFlags_Graphics | DkQueueFlags_MediumPrio | DkQueueFlags_EnableZcull;
    if (!(c->queue = dkQueueCreate(&qm)))
        goto fail;
    if (!load_shaders(c)) {
        fprintf(stderr, "[DKR] shader load failed\n");
        goto fail;
    }
    c->desc = mem_block(c, DKR_MAX_IMG * sizeof(DkImageDescriptor)
                           + DKR_MAX_SAMP * sizeof(DkSamplerDescriptor),
                        DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    if (!c->desc)
        goto fail;
    c->img_desc = (DkImageDescriptor *)dkMemBlockGetCpuAddr(c->desc);
    c->samp_desc = (DkSamplerDescriptor *)(c->img_desc + DKR_MAX_IMG);
    c->img_desc_gpu = dkMemBlockGetGpuAddr(c->desc);
    c->samp_desc_gpu = c->img_desc_gpu + DKR_MAX_IMG * sizeof(DkImageDescriptor);
    for (i = DKR_MAX_IMG - 1; i > DKR_IMG_WHITE; i--)
        c->free_ids[c->nfree_ids++] = (uint32_t)i;
    sampler_id(c, 1, 1, 0);             /* 0: wrap, nearest -- the fallback */

    for (i = 0; i < DKR_FRAMES; i++) {
        DkrFrame *f = &c->fr[i];
        DkCmdBufMaker cm;
        dkCmdBufMakerDefaults(&cm, c->dev);
        cm.userData = c;
        cm.cbAddMem = cb_add_mem;
        f->cmd_mem = mem_block(c, DKR_CMD_BYTES, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
        f->up = mem_block(c, DKR_UP_BYTES, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
        if (!f->cmd_mem || !f->up || !(f->cmd = dkCmdBufCreate(&cm)))
            goto fail;
        f->up_cpu = (uint8_t *)dkMemBlockGetCpuAddr(f->up);
        f->up_gpu = dkMemBlockGetGpuAddr(f->up);
    }
    pthread_mutex_init(&c->lock, NULL);
    pthread_cond_init(&c->freed, NULL);
    c->cur = -1;
    r = getenv("RECOMP_RES");
    v = r ? atof(r) : 0;
    c->scale = v >= 240 ? (float)(v / 480.0) : (v >= 1.0 && v <= 8.0) ? (float)v : 1.0f;
    r = getenv("RECOMP_ASPECT");
    vkr_set_aspect(c, r ? (float)atof(r) : 0.0f);
    fprintf(stderr, "[DKR] deko3d renderer, internal scale x%.2f\n", c->scale);
    return c;
fail:
    fprintf(stderr, "[DKR] setup failed\n");
    return NULL;   /* the backend falls back to the software rasteriser */
}

void vkr_destroy(VkrContext *c)
{
    if (c && c->queue)
        dkQueueWaitIdle(c->queue);
    /* lives as long as the process */
}

void vkr_info(VkrContext *c, uint32_t *w, uint32_t *h, uint32_t *msaa, uint64_t *tex_bytes)
{
    *w = c->sw; *h = c->sh; *msaa = 1; *tex_bytes = c->tex_bytes;
}

uint32_t vkr_tex_generation(VkrContext *c) { return c->tex_gen; }
void vkr_tex_purge(VkrContext *c) { c->purge_req = 1; }
int vkr_has_bc7(VkrContext *c) { (void)c; return 0; }
uint32_t vkr_tex_flags(const VkrTex *t) { return t ? t->flags : 0; }
void vkr_scene_set(VkrContext *c, const VkrScene *s) { (void)c; (void)s; }
void vkr_scene_run(VkrContext *c) { (void)c; }

/* ---- target ------------------------------------------------------------- */

static void wait_all_idle(VkrContext *c)
{
    int s;
    for (s = 0; s < DKR_FRAMES; s++) {
        if (s == c->cur)
            continue;
        pthread_mutex_lock(&c->lock);
        while (c->fr[s].busy)
            pthread_cond_wait(&c->freed, &c->lock);
        pthread_mutex_unlock(&c->lock);
    }
    dkQueueWaitIdle(c->queue);
}

static int make_image(VkrContext *c, DkImage *img, DkMemBlock *mem, uint32_t w, uint32_t h,
                      DkImageFormat fmt, uint32_t flags)
{
    DkImageLayoutMaker lm;
    DkImageLayout layout;
    dkImageLayoutMakerDefaults(&lm, c->dev);
    lm.flags = flags;
    lm.format = fmt;
    lm.dimensions[0] = w;
    lm.dimensions[1] = h;
    dkImageLayoutInitialize(&layout, &lm);
    *mem = mem_block(c, align_up((uint32_t)dkImageLayoutGetSize(&layout),
                                 dkImageLayoutGetAlignment(&layout)),
                     DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
    if (!*mem)
        return 0;
    dkImageInitialize(img, &layout, *mem, 0);
    return 1;
}

static void apply_scissor(VkrContext *c);

int vkr_set_target(VkrContext *c, uint32_t w, uint32_t h, float zmax)
{
    uint32_t sw = (uint32_t)((float)w * c->scale_x + 0.5f);
    uint32_t sh = (uint32_t)((float)h * c->scale + 0.5f);
    int s;

    if (sw != c->sw || sh != c->sh || w != c->w || h != c->h) {
        /* Frames in flight may still read the old images. */
        if (c->cur >= 0) {
            vkr_flip_submit(c);         /* end what was recorded on the old target */
            vkr_frame_release(c, (c->next + DKR_FRAMES - 1) % DKR_FRAMES);
        }
        wait_all_idle(c);
        if (c->color_mem) dkMemBlockDestroy(c->color_mem);
        if (c->depth_mem) dkMemBlockDestroy(c->depth_mem);
        if (c->down_mem) dkMemBlockDestroy(c->down_mem);
        c->color_mem = c->depth_mem = c->down_mem = NULL;
        if (!make_image(c, &c->color, &c->color_mem, sw, sh, DkImageFormat_BGRA8_Unorm,
                        DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine)
            || !make_image(c, &c->depth, &c->depth_mem, sw, sh, DkImageFormat_Z24S8,
                           DkImageFlags_UsageRender | DkImageFlags_HwCompression))
            return 0;
        if ((sw != w || sh != h)
            && !make_image(c, &c->down, &c->down_mem, w, h, DkImageFormat_BGRA8_Unorm,
                           DkImageFlags_Usage2DEngine))
            return 0;
        for (s = 0; s < DKR_FRAMES; s++) {
            DkrFrame *f = &c->fr[s];
            if (f->rb) dkMemBlockDestroy(f->rb);
            if (f->rbh) dkMemBlockDestroy(f->rbh);
            f->rb = mem_block(c, w * h * 4, DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached);
            f->rbh = (sw != w || sh != h)
                   ? mem_block(c, sw * sh * 4, DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached)
                   : NULL;
            if (!f->rb)
                return 0;
        }
        if (sw != w || sh != h)
            fprintf(stderr, "[DKR] internal resolution %ux%u (x%.2f/x%.2f of %ux%u)\n",
                    sw, sh, c->scale_x, c->scale, w, h);
    }
    c->w = w; c->h = h; c->sw = sw; c->sh = sh; c->zmax = zmax;
    c->gsc[0] = c->gsc[1] = 0; c->gsc[2] = w; c->gsc[3] = h;
    apply_scissor(c);
    if (c->cur >= 0) {
        DkCmdBuf cmd = cmd_of(c);
        DkImageView cv, dv;
        DkViewport vp = { 0, 0, (float)sw, (float)sh, 0.0f, 1.0f };
        dkImageViewDefaults(&cv, &c->color);
        dkImageViewDefaults(&dv, &c->depth);
        dkCmdBufBindRenderTarget(cmd, &cv, &dv);
        set_viewport(cmd, &vp);
        dkCmdBufSetScissors(cmd, 0, &c->sc, 1);
    }
    return 1;
}

static void apply_scissor(VkrContext *c)
{
    uint32_t x = c->gsc[0], y = c->gsc[1], w = c->gsc[2], h = c->gsc[3];
    if (x >= c->w || y >= c->h) { x = y = 0; w = h = 0; }
    if (x + w > c->w) w = c->w - x;
    if (y + h > c->h) h = c->h - y;
    if (c->scale != 1.0f || c->scale_x != 1.0f) {
        float sx = c->squeeze_all > 0.5f ? c->scale : c->scale_x;
        float ox = c->squeeze_all > 0.5f ? ((float)c->sw - (float)c->w * c->scale) * 0.5f : 0.0f;
        uint32_t x1 = (uint32_t)(ox + (float)(x + w) * sx + 0.5f);
        uint32_t y1 = (uint32_t)((float)(y + h) * c->scale + 0.5f);
        x = (uint32_t)(ox + (float)x * sx + 0.5f);
        y = (uint32_t)((float)y * c->scale + 0.5f);
        if (x1 > c->sw) x1 = c->sw;
        if (y1 > c->sh) y1 = c->sh;
        w = x1 > x ? x1 - x : 0;
        h = y1 > y ? y1 - y : 0;
    }
    c->sc.x = x; c->sc.y = y; c->sc.width = w; c->sc.height = h;
    if (c->cur >= 0)
        dkCmdBufSetScissors(cmd_of(c), 0, &c->sc, 1);
}

void vkr_set_scissor(VkrContext *c, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    c->gsc[0] = x; c->gsc[1] = y; c->gsc[2] = w; c->gsc[3] = h;
    apply_scissor(c);
}

void vkr_clear(VkrContext *c, const float rgba[4], int clear_depth, float depth)
{
    if (!c->sw || !begin(c))
        return;
    dkCmdBufClearColorFloat(cmd_of(c), 0, DkColorMask_RGBA, rgba[0], rgba[1], rgba[2], rgba[3]);
    if (clear_depth)
        dkCmdBufClearDepthStencil(cmd_of(c), true, depth, 0, 0);
}

void vkr_clear_ds(VkrContext *c, int clear_depth, float depth, int clear_stencil, uint32_t stencil)
{
    if (!c->sw || (!clear_depth && !clear_stencil) || !begin(c))
        return;
    dkCmdBufClearDepthStencil(cmd_of(c), clear_depth != 0, depth,
                              clear_stencil ? 0xFF : 0, (uint8_t)stencil);
}

/* ---- offscreen targets: not yet (the executor draws them in software) --- */

int vkr_offscreen_use(VkrContext *c, uint32_t key, uint32_t w, uint32_t h, float scale, float zmax)
{
    (void)c; (void)key; (void)w; (void)h; (void)scale; (void)zmax;
    return 0;
}
void vkr_offscreen_done(VkrContext *c) { (void)c; }
VkrTex *vkr_offscreen_tex(VkrContext *c, uint32_t key) { (void)c; (void)key; return NULL; }
int vkr_offscreen_size(VkrContext *c, uint32_t key, uint32_t *w, uint32_t *h)
{
    (void)c; (void)key; (void)w; (void)h;
    return 0;
}

/* ---- textures ----------------------------------------------------------- */

static uint32_t tex_slot(uint64_t key, uint32_t cap)
{
    key ^= key >> 33; key *= 0xff51afd7ed558ccdull; key ^= key >> 33;
    return (uint32_t)key & (cap - 1);
}

static int tex_rehash(VkrContext *c)
{
    uint32_t cap = 1024, i;
    while (cap < c->ntexs * 2 + 2)
        cap <<= 1;
    free(c->hidx);
    c->hidx = (int32_t *)malloc(cap * sizeof *c->hidx);
    if (!c->hidx) { c->hcap = 0; return 0; }
    c->hcap = cap;
    for (i = 0; i < cap; i++)
        c->hidx[i] = -1;
    for (i = 0; i < c->ntexs; i++) {
        uint32_t h = tex_slot(c->texs[i]->key, cap);
        while (c->hidx[h] >= 0)
            h = (h + 1) & (cap - 1);
        c->hidx[h] = (int32_t)i;
    }
    return 1;
}

static void tex_destroy(VkrContext *c, VkrTex *t)
{
    (void)c;
    if (t->mem)
        dkMemBlockDestroy(t->mem);
    free(t);
}

/* Unlisted now; destroyed, and its descriptor reused, when the slot that may
 * last have drawn with it -- the one recording, else the one last submitted --
 * is next begun. Its fence then covers that frame and, the queue being in
 * order, every frame before it. */
static void tex_retire(VkrContext *c, VkrTex *t)
{
    int s = c->cur >= 0 ? c->cur : (c->next + DKR_FRAMES - 1) % DKR_FRAMES;
    DkrFrame *f = &c->fr[s];
    c->tex_bytes -= (uint64_t)t->w * t->h * 4;
    t->next_free = f->dead;
    f->dead = t;
}

static void tex_evict(VkrContext *c, uint32_t age)
{
    uint32_t i, k = 0, freed = 0;
    for (i = 0; i < c->ntexs; i++) {
        VkrTex *t = c->texs[i];
        if (t != c->white && (age == 0 || c->flip_serial - t->last_use > age)) {
            tex_retire(c, t);
            freed++;
        } else
            c->texs[k++] = t;
    }
    if (!freed)
        return;
    c->ntexs = k;
    c->tex_gen++;
    tex_rehash(c);
}

VkrTex *vkr_tex_find(VkrContext *c, uint64_t key)
{
    uint32_t h;
    if (!c->hcap)
        return NULL;
    for (h = tex_slot(key, c->hcap); c->hidx[h] >= 0; h = (h + 1) & (c->hcap - 1)) {
        VkrTex *t = c->texs[c->hidx[h]];
        if (t->key == key) {
            t->last_use = c->flip_serial;
            return t;
        }
    }
    return NULL;
}

/* Pixels into the ring and a copy into the texture, ahead of the draws. */
static int upload_pixels(VkrContext *c, VkrTex *t, const uint32_t *px)
{
    uint8_t *cpu;
    DkGpuAddr gpu;
    DkImageView view;
    DkImageRect rect = { 0, 0, 0, t->w, t->h, 1 };
    DkCopyBuf src;
    if (!alloc(c, t->w * t->h * 4, 256, &cpu, &gpu, NULL))
        return 0;
    memcpy(cpu, px, (size_t)t->w * t->h * 4);
    dkImageViewDefaults(&view, &t->img);
    src.addr = gpu;
    src.rowLength = t->w * 4;
    src.imageHeight = t->h;
    dkCmdBufCopyBufferToImage(cmd_of(c), &src, &view, &rect, 0);
    dkCmdBufBarrier(cmd_of(c), DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_L2Cache);
    return 1;
}

static void tex_index_add(VkrContext *c, VkrTex *t)
{
    if (c->ntexs == c->captexs) {
        c->captexs = c->captexs ? c->captexs * 2 : 256;
        c->texs = (VkrTex **)realloc(c->texs, c->captexs * sizeof *c->texs);
    }
    c->texs[c->ntexs++] = t;
    c->tex_bytes += (uint64_t)t->w * t->h * 4;
    if (c->ntexs * 2 + 2 > c->hcap)
        tex_rehash(c);
    else {
        uint32_t h = tex_slot(t->key, c->hcap);
        while (c->hidx[h] >= 0)
            h = (h + 1) & (c->hcap - 1);
        c->hidx[h] = (int32_t)(c->ntexs - 1);
    }
}

VkrTex *vkr_tex_create_ex(VkrContext *c, uint64_t key, uint32_t w, uint32_t h,
                          const uint32_t *pixels, int mips)
{
    VkrTex *t;
    int id;
    (void)mips;                         /* one level for now */
    if (!w || !h || w > 4096 || h > 4096 || !begin(c))
        return NULL;
    id = c->white ? image_id_alloc(c) : DKR_IMG_WHITE;
    if (id < 0) {
        tex_evict(c, 0);                /* out of descriptors: drop the cache */
        return NULL;
    }
    t = (VkrTex *)calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->key = key; t->w = w; t->h = h; t->id = (uint32_t)id;
    t->last_use = c->flip_serial;
    if (!make_image(c, &t->img, &t->mem, w, h, DkImageFormat_BGRA8_Unorm, DkImageFlags_Usage2DEngine)
        || !upload_pixels(c, t, pixels)) {
        if (t->mem) dkMemBlockDestroy(t->mem);
        free(t);
        if (id != DKR_IMG_WHITE) c->free_ids[c->nfree_ids++] = (uint32_t)id;
        return NULL;
    }
    image_desc_write(c, t->id, &t->img);
    tex_index_add(c, t);
    return t;
}

VkrTex *vkr_tex_create(VkrContext *c, uint64_t key, uint32_t w, uint32_t h, const uint32_t *pixels)
{
    return vkr_tex_create_ex(c, key, w, h, pixels, 0);
}

int vkr_tex_update(VkrContext *c, VkrTex *t, uint64_t key, const uint32_t *pixels)
{
    if (!begin(c) || !upload_pixels(c, t, pixels))
        return 0;
    t->last_use = c->flip_serial;
    if (t->key != key) {
        t->key = key;
        tex_rehash(c);
    }
    return 1;
}

VkrTex *vkr_tex_create_bc7(VkrContext *c, uint64_t key, uint32_t w, uint32_t h,
                           uint32_t mips, const void *data, size_t size, uint32_t flags)
{
    (void)c; (void)key; (void)w; (void)h; (void)mips; (void)data; (void)size; (void)flags;
    return NULL;
}

/* ---- draws -------------------------------------------------------------- */

static DkBlendFactor blend_factor(uint32_t f)
{
    switch (f) {
    case 0x0000: return DkBlendFactor_Zero;
    case 0x0001: return DkBlendFactor_One;
    case 0x0300: return DkBlendFactor_SrcColor;
    case 0x0301: return DkBlendFactor_InvSrcColor;
    case 0x0302: return DkBlendFactor_SrcAlpha;
    case 0x0303: return DkBlendFactor_InvSrcAlpha;
    case 0x0304: return DkBlendFactor_DstAlpha;
    case 0x0305: return DkBlendFactor_InvDstAlpha;
    case 0x0306: return DkBlendFactor_DstColor;
    case 0x0307: return DkBlendFactor_InvDstColor;
    case 0x0308: return DkBlendFactor_SrcAlphaSaturate;
    case 0x8001: return DkBlendFactor_ConstColor;
    case 0x8002: return DkBlendFactor_InvConstColor;
    case 0x8003: return DkBlendFactor_ConstAlpha;
    case 0x8004: return DkBlendFactor_InvConstAlpha;
    default:     return DkBlendFactor_One;
    }
}

static DkBlendOp blend_op(uint32_t e)
{
    switch (e) {
    case 0x800A: return DkBlendOp_Sub;
    case 0x800B: return DkBlendOp_RevSub;
    case 0x8007: return DkBlendOp_Min;
    case 0x8008: return DkBlendOp_Max;
    default:     return DkBlendOp_Add;
    }
}

static DkStencilOp stencil_op(uint32_t gl)
{
    switch (gl) {
    case 0x0000: return DkStencilOp_Zero;
    case 0x1E01: return DkStencilOp_Replace;
    case 0x1E02: return DkStencilOp_Incr;
    case 0x1E03: return DkStencilOp_Decr;
    case 0x150A: return DkStencilOp_Invert;
    case 0x8507: return DkStencilOp_IncrWrap;
    case 0x8508: return DkStencilOp_DecrWrap;
    default:     return DkStencilOp_Keep;
    }
}

/* GL compare enum 0x200..0x207 -> DkCompareOp (Never = 1 .. Always = 8). */
static DkCompareOp compare_op(uint32_t gl, DkCompareOp dflt)
{
    return gl >= 0x200 && gl <= 0x207 ? (DkCompareOp)(gl - 0x200 + 1) : dflt;
}

/* Everything a draw binds but its vertices: shaders, state, uniforms,
 * textures. pc must already hold the vertex-program fields (if any). */
static int bind_draw(VkrContext *c, const VkrDrawState *st, DkrPC *pc, int gpu_vsh)
{
    DkCmdBuf cmd = cmd_of(c);
    const DkShader *sh[2];
    DkBlendState bs;
    DkColorState cs;
    DkColorWriteState cw;
    DkDepthStencilState ds;
    DkResHandle th[4];
    uint8_t *cpu;
    DkGpuAddr rc_gpu, pc_gpu;
    int k;

    pc->inv_size[0] = 1.0f / (float)c->w;
    pc->inv_size[1] = 1.0f / (float)c->h;
    pc->inv_zmax = 1.0f / c->zmax;
    pc->fog_mode = st->u.fog_enable ? st->u.fog_param[2] : 0.0f;
    pc->fog_param[0] = st->u.fog_param[0];
    pc->fog_param[1] = st->u.fog_param[1];
    pc->point_size = st->point_size * c->scale;
    pc->point_params = st->point_params ? c->scale : 0.0f;
    pc->squeeze2d = c->squeeze2d;
    pc->squeeze_all = c->squeeze_all;
    pc->hud_shift = c->squeeze_all > 0.5f ? 0.0f : st->hud_shift;
    pc->keep_edges = st->keep_edges ? 1.0f : 0.0f;
    pc->alpha_func = st->alpha_func;
    pc->alpha_ref = st->alpha_ref;
    pc->w_scale = st->w_buffer ? 1.0f / c->zmax : 0.0f;
    pc->point_sprite = (uint32_t)(st->topology == VKR_TOPO_POINTS && st->point_sprite)
                     | (uint32_t)(st->clip2d != 0) << 1 | 1u << 8;
    pc->tag = (uint32_t)st->rider;

    if (!alloc(c, sizeof st->u, DK_UNIFORM_BUF_ALIGNMENT, &cpu, &rc_gpu, NULL))
        return 0;
    memcpy(cpu, &st->u, sizeof st->u);
    if (!alloc(c, sizeof *pc, DK_UNIFORM_BUF_ALIGNMENT, &cpu, &pc_gpu, NULL))
        return 0;
    memcpy(cpu, pc, sizeof *pc);

    sh[0] = gpu_vsh ? &c->vs_vsh : &c->vs_fixed;
    sh[1] = st->w_buffer ? &c->fs_wdepth : &c->fs;
    dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, sh, 2);
    dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 1, pc_gpu, sizeof *pc);
    dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 1, pc_gpu, sizeof *pc);
    dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 0, rc_gpu, sizeof st->u);

    for (k = 0; k < 4; k++) {
        VkrTex *t = st->tex[k] ? st->tex[k] : c->white;
        th[k] = dkMakeTextureHandle(t->id, sampler_id(c, st->addr_u[k], st->addr_v[k], st->linear[k]));
    }
    if (c->desc_dirty) {
        /* the GPU caches descriptors: new ones must be read afresh */
        dkCmdBufBarrier(cmd, DkBarrier_None, DkInvalidateFlags_Descriptors);
        c->desc_dirty = 0;
    }
    dkCmdBufBindTextures(cmd, DkStage_Fragment, 0, th, 4);

    dkColorStateDefaults(&cs);
    cs.blendEnableMask = st->blend_enable ? 1u : 0u;
    dkCmdBufBindColorState(cmd, &cs);
    dkColorWriteStateDefaults(&cw);
    dkColorWriteStateSetMask(&cw, 0, st->write_mask & 0xF);
    dkCmdBufBindColorWriteState(cmd, &cw);
    if (st->blend_enable) {
        DkBlendFactor sf = blend_factor(st->blend_src), df = blend_factor(st->blend_dst);
        DkBlendOp op = blend_op(st->blend_eq);
        dkBlendStateDefaults(&bs);
        dkBlendStateSetOps(&bs, op, op);
        dkBlendStateSetFactors(&bs, sf, df, sf, df);
        dkCmdBufBindBlendState(cmd, 0, &bs);
        dkCmdBufSetBlendConst(cmd, st->blend_color[0], st->blend_color[1],
                              st->blend_color[2], st->blend_color[3]);
    }
    dkDepthStencilStateDefaults(&ds);
    ds.depthTestEnable = st->depth_test != 0;
    ds.depthWriteEnable = st->depth_write != 0;
    ds.depthCompareOp = compare_op(st->depth_func, DkCompareOp_Lequal);
    ds.stencilTestEnable = st->stencil_test != 0;
    if (st->stencil_test) {
        ds.stencilFrontFailOp = ds.stencilBackFailOp = stencil_op(st->stencil_fail);
        ds.stencilFrontDepthFailOp = ds.stencilBackDepthFailOp = stencil_op(st->stencil_zfail);
        ds.stencilFrontPassOp = ds.stencilBackPassOp = stencil_op(st->stencil_zpass);
        ds.stencilFrontCompareOp = ds.stencilBackCompareOp = compare_op(st->stencil_func, DkCompareOp_Always);
        dkCmdBufSetStencil(cmd, DkFace_FrontAndBack, (uint8_t)st->stencil_write_mask,
                           (uint8_t)st->stencil_ref, (uint8_t)st->stencil_read_mask);
    }
    dkCmdBufBindDepthStencilState(cmd, &ds);
    if (st->topology == VKR_TOPO_POINTS)
        dkCmdBufSetPointSize(cmd, pc->point_size > 1.0f ? pc->point_size : 1.0f);
    else if (st->topology == VKR_TOPO_LINES)
        dkCmdBufSetLineWidth(cmd, c->scale > 1.0f ? c->scale : 1.0f);
    return 1;
}

static DkPrimitive prim_of(uint32_t topo)
{
    return topo == VKR_TOPO_POINTS ? DkPrimitive_Points
         : topo == VKR_TOPO_LINES ? DkPrimitive_Lines : DkPrimitive_Triangles;
}

static int ensure_white(VkrContext *c)
{
    static const uint32_t white = 0xFFFFFFFFu;
    if (!c->white)
        c->white = vkr_tex_create(c, 0x77686974ull, 1, 1, &white);
    return c->white != NULL;
}

int vkr_draw(VkrContext *c, const VkrVertex *v, uint32_t nverts,
             const uint32_t *idx, uint32_t nidx, const VkrDrawState *st)
{
    DkrPC pc;
    uint8_t *vcpu, *icpu;
    DkGpuAddr vgpu, igpu;
    DkVtxAttribState va[8];
    DkVtxBufferState vb = { sizeof(VkrVertex), 0 };
    DkCmdBuf cmd;
    int k;

    if (!nidx || !c->sw || !begin(c) || !ensure_white(c))
        return 0;
    if (!alloc(c, nverts * (uint32_t)sizeof *v, 16, &vcpu, &vgpu, NULL)
        || !alloc(c, nidx * 4, 16, &icpu, &igpu, NULL))
        return 0;
    memcpy(vcpu, v, nverts * sizeof *v);
    memcpy(icpu, idx, nidx * 4);
    memset(&pc, 0, sizeof pc);
    if (!bind_draw(c, st, &pc, 0))
        return 0;
    cmd = cmd_of(c);
    memset(va, 0, sizeof va);
    for (k = 0; k < 8; k++) {
        va[k].bufferId = 0;
        va[k].offset = (uint32_t)(k * 16);
        va[k].size = DkVtxAttribSize_4x32;
        va[k].type = DkVtxAttribType_Float;
    }
    dkCmdBufBindVtxAttribState(cmd, va, 8);
    dkCmdBufBindVtxBufferState(cmd, &vb, 1);
    dkCmdBufBindVtxBuffer(cmd, 0, vgpu, nverts * (uint32_t)sizeof *v);
    dkCmdBufBindIdxBuffer(cmd, DkIdxFormat_Uint32, igpu);
    dkCmdBufDrawIndexed(cmd, prim_of(st->topology), nidx, 1, 0, 0, 0);
    return 1;
}

int vkr_draw_vsh(VkrContext *c, const float *in, uint32_t nverts, uint32_t nin,
                 const uint32_t *prog, uint32_t nins, uint32_t prog_hash,
                 const float (*consts)[4], const uint32_t *idx, uint32_t nidx,
                 const VkrDrawState *st)
{
    DkrPC pc;
    uint8_t *cpu;
    DkGpuAddr gpu;
    uint32_t in_bytes = nverts * (nin ? nin : 1) * 16, off;
    DkrFrame *f;
    DkCmdBuf cmd;

    if (!nidx || !c->sw || !begin(c) || !ensure_white(c))
        return 0;
    if (c->vsh_const_off == 0xFFFFFFFFu || memcmp(c->vsh_consts, consts, sizeof c->vsh_consts)) {
        if (!alloc(c, (uint32_t)sizeof c->vsh_consts, 16, &cpu, &gpu, &off))
            return 0;
        memcpy(cpu, consts, sizeof c->vsh_consts);
        memcpy(c->vsh_consts, consts, sizeof c->vsh_consts);
        c->vsh_const_off = off;
    }
    if (c->vsh_prog_off == 0xFFFFFFFFu || c->vsh_prog_hash != prog_hash || c->vsh_prog_n != nins) {
        if (!alloc(c, nins * 32, 16, &cpu, &gpu, &off))
            return 0;
        memcpy(cpu, prog, nins * 32);
        c->vsh_prog_off = off;
        c->vsh_prog_hash = prog_hash;
        c->vsh_prog_n = nins;
    }
    memset(&pc, 0, sizeof pc);
    if (!alloc(c, in_bytes, 16, &cpu, &gpu, &off))
        return 0;
    memcpy(cpu, in, in_bytes);
    pc.in_base = off / 16;
    pc.nin = nin ? nin : 1;
    pc.prog_base = c->vsh_prog_off / 16;
    pc.const_base = c->vsh_const_off / 16;
    if (!alloc(c, nidx * 4, 16, &cpu, &gpu, NULL))
        return 0;
    memcpy(cpu, idx, nidx * 4);
    if (!bind_draw(c, st, &pc, 1))
        return 0;
    cmd = cmd_of(c);
    f = &c->fr[c->cur];
    /* The program pulls its inputs, program and constants from the ring. */
    dkCmdBufBindVtxAttribState(cmd, NULL, 0);
    dkCmdBufBindStorageBuffer(cmd, DkStage_Vertex, 0, f->up_gpu, DKR_UP_BYTES);
    dkCmdBufBindIdxBuffer(cmd, DkIdxFormat_Uint32, gpu);
    dkCmdBufDrawIndexed(cmd, prim_of(st->topology), nidx, 1, 0, 0, 0);
    return 1;
}

void vkr_set_pillarbox(VkrContext *c, int on)
{
    float v = on && c->squeeze2d != 1.0f ? 1.0f : 0.0f;
    if (v != c->squeeze_all) {
        c->squeeze_all = v;
        apply_scissor(c);
    }
}

void vkr_set_aspect(VkrContext *c, float aspect)
{
    c->scale_x = c->scale;
    c->squeeze2d = 1.0f;
    if (aspect > 4.0f / 3.0f + 0.01f) {
        float f = aspect * 0.75f;
        c->scale_x = c->scale * f;
        c->squeeze2d = 1.0f / f;
    }
}

uint32_t vkr_width(const VkrContext *c) { return c->w; }
uint32_t vkr_height(const VkrContext *c) { return c->h; }

/* ---- frames ------------------------------------------------------------- */

int vkr_flip_submit(VkrContext *c)
{
    DkCmdBuf cmd;
    DkrFrame *f;
    DkImageView cv, dv;
    DkImageRect full, guest;
    DkCopyBuf cb;
    int slot;

    if (!c->sw || !begin(c))
        return -1;
    slot = c->cur;
    f = &c->fr[slot];
    cmd = f->cmd;
    if (c->squeeze_all > 0.5f) {
        /* Pillarboxed frame: the bars are black (nothing draws there). */
        uint32_t bw = (uint32_t)(((float)c->sw - (float)c->w * c->scale) * 0.5f);
        if (bw) {
            DkScissor bars[2] = { { 0, 0, bw, c->sh }, { c->sw - bw, 0, bw, c->sh } };
            for (int i = 0; i < 2; i++) {
                dkCmdBufSetScissors(cmd, 0, &bars[i], 1);
                dkCmdBufClearColorFloat(cmd, 0, DkColorMask_RGBA, 0, 0, 0, 1);
            }
            dkCmdBufSetScissors(cmd, 0, &c->sc, 1);
        }
    }
    dkCmdBufBarrier(cmd, DkBarrier_Fragments, DkInvalidateFlags_L2Cache);
    dkImageViewDefaults(&cv, &c->color);
    full = (DkImageRect){ 0, 0, 0, c->sw, c->sh, 1 };
    guest = (DkImageRect){ 0, 0, 0, c->w, c->h, 1 };
    if (!c->down_mem) {
        cb = (DkCopyBuf){ dkMemBlockGetGpuAddr(f->rb), c->w * 4, c->h };
        dkCmdBufCopyImageToBuffer(cmd, &cv, &guest, &cb, 0);
    } else {
        /* The full image for the presenter, and a filtered guest-size copy
         * for guest memory (what the title and the screenshots read). */
        cb = (DkCopyBuf){ dkMemBlockGetGpuAddr(f->rbh), c->sw * 4, c->sh };
        dkCmdBufCopyImageToBuffer(cmd, &cv, &full, &cb, 0);
        dkImageViewDefaults(&dv, &c->down);
        dkCmdBufBlitImage(cmd, &cv, &full, &dv, &guest, DkBlitFlag_FilterLinear, 0);
        dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_L2Cache);
        cb = (DkCopyBuf){ dkMemBlockGetGpuAddr(f->rb), c->w * 4, c->h };
        dkCmdBufCopyImageToBuffer(cmd, &dv, &guest, &cb, 0);
    }
    dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_L2Cache);
    dkCmdBufSignalFence(cmd, &f->fence, true);
    dkQueueSubmitCommands(c->queue, dkCmdBufFinishList(cmd));
    dkQueueFlush(c->queue);
    if (dkQueueIsInErrorState(c->queue)) {
        static int said;
        if (!said++)
            fprintf(stderr, "[DKR] GPU queue in error state (frame %u)\n", c->flip_serial);
    }
    pthread_mutex_lock(&c->lock);
    f->busy = 1;
    f->submitted = 1;
    f->rb_bytes = c->w * c->h * 4;
    f->rbh_bytes = c->down_mem ? c->sw * c->sh * 4 : 0;
    pthread_mutex_unlock(&c->lock);
    c->cur = -1;
    c->next = (slot + 1) % DKR_FRAMES;
    c->said_full = 0;
    if (++c->flip_serial % 120 == 0)
        tex_evict(c, 300);              /* every ~2 s: textures unused for 5 s */
    if (c->tex_bytes > (512ull << 20) || c->purge_req) {
        c->purge_req = 0;
        tex_evict(c, 0);
    }
    if (c->flip_serial == 1)
        fprintf(stderr, "[DKR] first frame submitted (%ux%u)\n", c->sw, c->sh);
    return slot;
}

const uint8_t *vkr_frame_pixels(VkrContext *c, int slot)
{
    DkrFrame *f = &c->fr[slot];
    dkFenceWait(&f->fence, -1);
    dkMemBlockFlushCpuCache(f->rb, 0, f->rb_bytes);   /* drop stale lines before reading */
    return (const uint8_t *)dkMemBlockGetCpuAddr(f->rb);
}

const uint8_t *vkr_frame_pixels_full(VkrContext *c, int slot, uint32_t *w, uint32_t *h)
{
    DkrFrame *f = &c->fr[slot];
    if (!f->rbh || !f->rbh_bytes)
        return NULL;
    dkMemBlockFlushCpuCache(f->rbh, 0, f->rbh_bytes);
    *w = c->sw; *h = c->sh;
    return (const uint8_t *)dkMemBlockGetCpuAddr(f->rbh);
}

void vkr_frame_release(VkrContext *c, int slot)
{
    pthread_mutex_lock(&c->lock);
    c->fr[slot].busy = 0;
    pthread_cond_broadcast(&c->freed);
    pthread_mutex_unlock(&c->lock);
}

int vkr_read_back(VkrContext *c, uint8_t *out)
{
    int slot = vkr_flip_submit(c);
    const uint8_t *px = slot < 0 ? NULL : vkr_frame_pixels(c, slot);
    if (px)
        memcpy(out, px, (size_t)c->w * c->h * 4);
    if (slot >= 0)
        vkr_frame_release(c, slot);
    return px != NULL;
}
#endif /* __SWITCH__ */
