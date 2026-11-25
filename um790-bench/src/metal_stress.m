#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

static inline uint64_t nowNs(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static void getRusageUs(double *userMs, double *sysMs) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    *userMs = ru.ru_utime.tv_sec * 1000.0 + ru.ru_utime.tv_usec / 1000.0;
    *sysMs  = ru.ru_stime.tv_sec * 1000.0 + ru.ru_stime.tv_usec / 1000.0;
}

static int cmpDouble(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

static double pctVal(const double *sorted, int n, double p) {
    if (n <= 0) return 0.0;
    double idx = p * (n - 1);
    int lo = (int)idx;
    int hi = (lo + 1 < n) ? (lo + 1) : lo;
    double frac = idx - lo;
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

// ============================================================================
// 1. CPU Benchmark (1T / 8T / 16T compute + throttle check + memory bandwidth)
// ============================================================================

typedef struct {
    int tid;
    uint64_t stopNs;
    uint64_t opsFirstHalf;
    uint64_t opsSecondHalf;
    uint64_t halfNs;
    volatile uint64_t sink;
} CpuWorkerArg;

static void *cpuWorker(void *arg) {
    CpuWorkerArg *w = (CpuWorkerArg *)arg;
    uint64_t x = 0x9e3779b97f4a7c15ULL ^ (uint64_t)(w->tid + 1);
    double f = 1.000001 + 0.0001 * w->tid;
    uint64_t c1 = 0, c2 = 0;

    for (;;) {
        uint64_t t = nowNs();
        if (t >= w->stopNs) break;
        for (int k = 0; k < 2048; k++) {
            x ^= x >> 12;
            x ^= x << 25;
            x ^= x >> 27;
            x *= 0x2545F4914F6CDD1DULL;
            f = f * 1.00000011920929 + 0.0000001;
            if (f > 1e6) f -= 9e5;
        }
        if (t < w->halfNs) c1++;
        else c2++;
    }
    w->opsFirstHalf = c1;
    w->opsSecondHalf = c2;
    w->sink = x ^ (uint64_t)f;
    return NULL;
}

static double runCpuThreads(int nthreads, double seconds, double *throttleRatioOut) {
    uint64_t durNs = (uint64_t)(seconds * 1e9);
    uint64_t start = nowNs();
    uint64_t half  = start + durNs / 2;
    uint64_t stop  = start + durNs;

    CpuWorkerArg *args = calloc((size_t)nthreads, sizeof(CpuWorkerArg));
    pthread_t *tids = calloc((size_t)nthreads, sizeof(pthread_t));
    for (int i = 0; i < nthreads; i++) {
        args[i].tid = i;
        args[i].halfNs = half;
        args[i].stopNs = stop;
        pthread_create(&tids[i], NULL, cpuWorker, &args[i]);
    }
    uint64_t sum1 = 0, sum2 = 0;
    for (int i = 0; i < nthreads; i++) {
        pthread_join(tids[i], NULL);
        sum1 += args[i].opsFirstHalf;
        sum2 += args[i].opsSecondHalf;
    }
    double actualSec = (double)(nowNs() - start) / 1e9;
    free(args);
    free(tids);
    if (throttleRatioOut) {
        *throttleRatioOut = sum1 ? ((double)sum2 / (double)sum1) : 1.0;
    }
    // Units: Mops/sec (each chunk = 2048 mixed int+FP iterations)
    return ((double)(sum1 + sum2) * 2048.0 / 1e6) / actualSec;
}

static double runMemBandwidthGBs(void) {
    const size_t sz = 64u << 20; // 64 MiB
    void *src = malloc(sz);
    void *dst = malloc(sz);
    if (!src || !dst) {
        free(src);
        free(dst);
        return 0.0;
    }
    memset(src, 0x5A, sz);
    memset(dst, 0x00, sz);
    memcpy(dst, src, sz); // warmup

    const int iters = 16;
    uint64_t t0 = nowNs();
    for (int i = 0; i < iters; i++) {
        memcpy(dst, src, sz);
        ((volatile uint8_t *)dst)[i] ^= (uint8_t)i;
    }
    double sec = (double)(nowNs() - t0) / 1e9;
    volatile uint8_t guard = ((volatile uint8_t *)dst)[0];
    (void)guard;
    free(src);
    free(dst);
    // Count read+write bytes? Report copy payload GB/s clearly.
    return ((double)sz * iters / 1e9) / sec;
}

typedef struct {
    double score1T;
    double score8T;
    double score16T;
    double throttle16T;
    double smtScaling;
    double memCopyGBs;
} CpuBenchResult;

static CpuBenchResult runCpuBench(double secondsEach) {
    CpuBenchResult r = {0};
    double tr1 = 1.0, tr8 = 1.0, tr16 = 1.0;
    r.score1T  = runCpuThreads(1, secondsEach, &tr1);
    r.score8T  = runCpuThreads(8, secondsEach, &tr8);
    r.score16T = runCpuThreads(16, secondsEach, &tr16);
    r.throttle16T = tr16;
    r.smtScaling = r.score1T > 0 ? (r.score16T / r.score1T) : 0.0;
    r.memCopyGBs = runMemBandwidthGBs();
    (void)tr1;
    (void)tr8;
    return r;
}

// ============================================================================
// 2. Compositor Simulation Benchmark (WindowServer Metal->Vulkan hotspot test)
// ============================================================================

typedef struct {
    int frames;
    double wallMs;
    double encodeP50, encodeP95, encodeP99, encodeMax;
    double totalP50, totalP95, totalP99, totalMax;
    double avgFps;
    double pctUnder833;
    double cpuPct;
    int ok;
    char error[256];
} CompositeResult;

static CompositeResult runCompositeBench(id<MTLDevice> dev, int frames) {
    CompositeResult res;
    memset(&res, 0, sizeof(res));
    res.frames = frames;

    id<MTLCommandQueue> q = [dev newCommandQueue];
    if (!q) { snprintf(res.error, sizeof(res.error), "newCommandQueue failed"); return res; }

    // 4K render target matching WindowServer output
    MTLTextureDescriptor *rtDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:3840
                                                                                     height:2160
                                                                                  mipmapped:NO];
    rtDesc.storageMode = MTLStorageModePrivate;
    rtDesc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> rt = [dev newTextureWithDescriptor:rtDesc];
    if (!rt) { snprintf(res.error, sizeof(res.error), "4K RT alloc failed"); return res; }

    // 8 pooled 1920x1344 IOSurfaces
    const int nSurfs = 8;
    IOSurfaceRef surfs[8] = {NULL};
    for (int i = 0; i < nSurfs; i++) {
        NSDictionary *props = @{
            (id)kIOSurfaceWidth: @1920,
            (id)kIOSurfaceHeight: @1344,
            (id)kIOSurfaceBytesPerElement: @4,
            (id)kIOSurfaceBytesPerRow: @(1920 * 4),
            (id)kIOSurfacePixelFormat: @((uint32_t)'BGRA'),
        };
        surfs[i] = IOSurfaceCreate((__bridge CFDictionaryRef)props);
        if (!surfs[i]) {
            snprintf(res.error, sizeof(res.error), "IOSurfaceCreate failed at %d", i);
            for (int j = 0; j < i; j++) CFRelease(surfs[j]);
            return res;
        }
        IOSurfaceLock(surfs[i], 0, NULL);
        uint32_t *px = (uint32_t *)IOSurfaceGetBaseAddress(surfs[i]);
        if (px) px[0] = 0xFF102030u + (uint32_t)i;
        IOSurfaceUnlock(surfs[i], 0, NULL);
    }

    MTLTextureDescriptor *surfDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                        width:1920
                                                                                       height:1344
                                                                                    mipmapped:NO];
    surfDesc.storageMode = MTLStorageModeShared;
    surfDesc.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;

    // Non-256B small glyph/UI buffer texture
    id<MTLBuffer> glyphBuf = [dev newBufferWithLength:128 * 40 options:MTLResourceStorageModeShared];
    MTLTextureDescriptor *glyphDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                         width:28
                                                                                        height:40
                                                                                     mipmapped:NO];
    glyphDesc.usage = MTLTextureUsageShaderRead;

    NSString *src = @"#include <metal_stdlib>\n"
                    @"using namespace metal;\n"
                    @"struct VOut { float4 pos [[position]]; float2 uv; };\n"
                    @"vertex VOut vs_comp(uint vid [[vertex_id]], constant float4 *rect [[buffer(0)]]) {\n"
                    @"    const float2 c[4] = { float2(-1,-1), float2(1,-1), float2(-1,1), float2(1,1) };\n"
                    @"    VOut o;\n"
                    @"    float2 p = c[vid & 3] * rect->zw + rect->xy;\n"
                    @"    o.pos = float4(p, 0.0, 1.0);\n"
                    @"    o.uv = (c[vid & 3] * 0.5 + 0.5) * float2(1920.0, 1344.0);\n"
                    @"    return o;\n"
                    @"}\n"
                    @"fragment float4 fs_comp(VOut in [[stage_in]],\n"
                    @"                        texture2d<float> tex0 [[texture(0)]],\n"
                    @"                        texture2d<float> tex1 [[texture(1)]],\n"
                    @"                        sampler samp0 [[sampler(0)]],\n"
                    @"                        sampler samp1 [[sampler(1)]]) {\n"
                    @"    float4 a = tex0.sample(samp0, in.uv);\n"
                    @"    float4 b = tex1.sample(samp1, float2(0.5, 0.5));\n"
                    @"    return mix(a, b, 0.25);\n"
                    @"}\n";

    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&err];
    if (!lib) {
        snprintf(res.error, sizeof(res.error), "compile failed: %s", err.localizedDescription.UTF8String);
        for (int i = 0; i < nSurfs; i++) CFRelease(surfs[i]);
        return res;
    }

    MTLRenderPipelineDescriptor *rp = [MTLRenderPipelineDescriptor new];
    rp.vertexFunction = [lib newFunctionWithName:@"vs_comp"];
    rp.fragmentFunction = [lib newFunctionWithName:@"fs_comp"];
    rp.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:rp error:&err];
    if (!pso) {
        snprintf(res.error, sizeof(res.error), "PSO failed: %s", err.localizedDescription.UTF8String);
        for (int i = 0; i < nSurfs; i++) CFRelease(surfs[i]);
        return res;
    }

    MTLSamplerDescriptor *pixSd = [MTLSamplerDescriptor new];
    pixSd.normalizedCoordinates = NO;
    pixSd.minFilter = MTLSamplerMinMagFilterLinear;
    pixSd.magFilter = MTLSamplerMinMagFilterLinear;
    pixSd.sAddressMode = MTLSamplerAddressModeClampToZero;
    pixSd.tAddressMode = MTLSamplerAddressModeClampToZero;

    MTLSamplerDescriptor *normSd = [MTLSamplerDescriptor new];
    normSd.normalizedCoordinates = YES;
    normSd.minFilter = MTLSamplerMinMagFilterLinear;
    normSd.magFilter = MTLSamplerMinMagFilterLinear;

    double *encMs = calloc((size_t)frames, sizeof(double));
    double *totMs = calloc((size_t)frames, sizeof(double));
    if (!encMs || !totMs) {
        free(encMs); free(totMs);
        for (int i = 0; i < nSurfs; i++) CFRelease(surfs[i]);
        snprintf(res.error, sizeof(res.error), "OOM");
        return res;
    }

    // Warmup 4 frames
    for (int f = 0; f < 4; f++) {
        @autoreleasepool {
            id<MTLTexture> t0 = [dev newTextureWithDescriptor:surfDesc iosurface:surfs[0] plane:0];
            id<MTLTexture> t1 = [glyphBuf newTextureWithDescriptor:glyphDesc offset:0 bytesPerRow:128];
            id<MTLSamplerState> s0 = [dev newSamplerStateWithDescriptor:pixSd];
            id<MTLSamplerState> s1 = [dev newSamplerStateWithDescriptor:normSd];
            MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
            rpd.colorAttachments[0].texture = rt;
            rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
            rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rpd];
            float rect[4] = { 0.f, 0.f, 1.f, 1.f };
            [enc setRenderPipelineState:pso];
            [enc setVertexBytes:rect length:sizeof(rect) atIndex:0];
            [enc setFragmentTexture:t0 atIndex:0];
            [enc setFragmentTexture:t1 atIndex:1];
            [enc setFragmentSamplerState:s0 atIndex:0];
            [enc setFragmentSamplerState:s1 atIndex:1];
            [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            [enc endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
        }
    }

    double u0 = 0, s0u = 0, u1 = 0, s1u = 0;
    getRusageUs(&u0, &s0u);
    uint64_t wallStart = nowNs();

    int under833 = 0;
    for (int f = 0; f < frames; f++) {
        @autoreleasepool {
            uint64_t f0 = nowNs();
            id<MTLTexture> layers[8];
            for (int i = 0; i < nSurfs; i++) {
                layers[i] = [dev newTextureWithDescriptor:surfDesc iosurface:surfs[i] plane:0];
                if (!layers[i]) {
                    snprintf(res.error, sizeof(res.error), "surf wrap nil frame %d idx %d", f, i);
                    goto done_loop;
                }
            }
            id<MTLTexture> gtex = [glyphBuf newTextureWithDescriptor:glyphDesc offset:0 bytesPerRow:128];
            id<MTLSamplerState> ps = [dev newSamplerStateWithDescriptor:pixSd];
            id<MTLSamplerState> ns = [dev newSamplerStateWithDescriptor:normSd];
            if (!gtex || !ps || !ns) {
                snprintf(res.error, sizeof(res.error), "gtex/sampler nil frame %d", f);
                goto done_loop;
            }

            MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
            rpd.colorAttachments[0].texture = rt;
            rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
            rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rpd];
            [enc setRenderPipelineState:pso];
            [enc setFragmentSamplerState:ps atIndex:0];
            [enc setFragmentSamplerState:ns atIndex:1];
            [enc setFragmentTexture:gtex atIndex:1];
            for (int d = 0; d < 10; d++) {
                float rect[4] = { (float)(d - 5) * 0.05f, 0.0f, 0.5f, 0.5f };
                [enc setVertexBytes:rect length:sizeof(rect) atIndex:0];
                [enc setFragmentTexture:layers[d % nSurfs] atIndex:0];
                [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            }
            [enc endEncoding];
            [cb commit];
            uint64_t fEnc = nowNs();
            [cb waitUntilCompleted];
            uint64_t fEnd = nowNs();

            if (cb.status != MTLCommandBufferStatusCompleted) {
                snprintf(res.error, sizeof(res.error), "cb failed status=%lu", (unsigned long)cb.status);
                goto done_loop;
            }
            encMs[f] = (double)(fEnc - f0) / 1e6;
            totMs[f] = (double)(fEnd - f0) / 1e6;
            if (totMs[f] <= 8.333) under833++;
        }
    }
    res.ok = 1;

done_loop:
    res.wallMs = (double)(nowNs() - wallStart) / 1e6;
    getRusageUs(&u1, &s1u);
    res.cpuPct = res.wallMs > 0 ? (((u1 - u0) + (s1u - s0u)) / res.wallMs * 100.0) : 0.0;

    if (res.ok) {
        double sumTot = 0.0;
        for (int f = 0; f < frames; f++) sumTot += totMs[f];
        res.avgFps = sumTot > 0 ? ((double)frames * 1000.0 / sumTot) : 0.0;
        res.pctUnder833 = (double)under833 * 100.0 / (double)frames;
        qsort(encMs, (size_t)frames, sizeof(double), cmpDouble);
        qsort(totMs, (size_t)frames, sizeof(double), cmpDouble);
        res.encodeP50 = pctVal(encMs, frames, 0.50);
        res.encodeP95 = pctVal(encMs, frames, 0.95);
        res.encodeP99 = pctVal(encMs, frames, 0.99);
        res.encodeMax = encMs[frames - 1];
        res.totalP50  = pctVal(totMs, frames, 0.50);
        res.totalP95  = pctVal(totMs, frames, 0.95);
        res.totalP99  = pctVal(totMs, frames, 0.99);
        res.totalMax  = totMs[frames - 1];
    }

    free(encMs);
    free(totMs);
    for (int i = 0; i < nSurfs; i++) CFRelease(surfs[i]);
    return res;
}

// ============================================================================
// 3. VRAM + Shader Compile + Compute/Video Stress Test (Chrome crash regression)
// ============================================================================

typedef struct {
    int targetMb;
    int iters;
    int texturesHeld;
    int pipelinesCompiled;
    double wallMs;
    int ok;
    char error[256];
} VramStressResult;

static VramStressResult runVramStress(id<MTLDevice> dev, int targetMb, int iters) {
    VramStressResult res;
    memset(&res, 0, sizeof(res));
    res.targetMb = targetMb;
    res.iters = iters;

    uint64_t t0 = nowNs();
    id<MTLCommandQueue> q = [dev newCommandQueue];
    if (!q) { snprintf(res.error, sizeof(res.error), "newCommandQueue failed"); return res; }

    // Hold targetMb in 16 MiB 2048x2048 private textures
    int count = (targetMb + 15) / 16;
    NSMutableArray<id<MTLTexture>> *held = [NSMutableArray arrayWithCapacity:(NSUInteger)count];
    MTLTextureDescriptor *ptd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                   width:2048
                                                                                  height:2048
                                                                               mipmapped:NO];
    ptd.storageMode = MTLStorageModePrivate;
    ptd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    for (int i = 0; i < count; i++) {
        id<MTLTexture> t = [dev newTextureWithDescriptor:ptd];
        if (!t) {
            snprintf(res.error, sizeof(res.error), "private texture alloc failed at %d/%d", i, count);
            return res;
        }
        [held addObject:t];
    }
    res.texturesHeld = (int)held.count;

    // Video frame IOSurface (1920x1080)
    NSDictionary *vprops = @{
        (id)kIOSurfaceWidth: @1920,
        (id)kIOSurfaceHeight: @1080,
        (id)kIOSurfaceBytesPerElement: @4,
        (id)kIOSurfaceBytesPerRow: @(1920 * 4),
        (id)kIOSurfacePixelFormat: @((uint32_t)'BGRA'),
    };
    IOSurfaceRef vidSurf = IOSurfaceCreate((__bridge CFDictionaryRef)vprops);
    MTLTextureDescriptor *vd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:1920
                                                                                 height:1080
                                                                              mipmapped:NO];
    vd.storageMode = MTLStorageModeShared;
    vd.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;

    id<MTLBuffer> outBuf = [dev newBufferWithLength:256 * sizeof(float) options:MTLResourceStorageModeShared];
    id<MTLTexture> rtSmall = [dev newTextureWithDescriptor:
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:256
                                                          height:256
                                                       mipmapped:NO]];
    // Make rtSmall render target
    MTLTextureDescriptor *rtsd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                    width:256
                                                                                   height:256
                                                                                mipmapped:NO];
    rtsd.storageMode = MTLStorageModePrivate;
    rtsd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    rtSmall = [dev newTextureWithDescriptor:rtsd];

    const int nVariants = 12;
    NSMutableArray<id<MTLRenderPipelineState>> *rpsos = [NSMutableArray array];
    NSMutableArray<id<MTLComputePipelineState>> *cpsos = [NSMutableArray array];

    for (int i = 0; i < iters; i++) {
        @autoreleasepool {
            IOSurfaceLock(vidSurf, 0, NULL);
            uint32_t *p = (uint32_t *)IOSurfaceGetBaseAddress(vidSurf);
            if (p) {
                p[0] = (uint32_t)(0xFF000000u | ((i & 0xff) << 8));
                p[1920 * 1080 - 1] = p[0];
            }
            IOSurfaceUnlock(vidSurf, 0, NULL);

            if (i < nVariants) {
                NSString *src = [NSString stringWithFormat:
                    @"#include <metal_stdlib>\n"
                    @"using namespace metal;\n"
                    @"vertex float4 vs_v(uint vid [[vertex_id]]) {\n"
                    @"    const float2 c[4] = { float2(-1,-1), float2(1,-1), float2(-1,1), float2(1,1) };\n"
                    @"    return float4(c[vid & 3], 0.0, 1.0);\n"
                    @"}\n"
                    @"fragment float4 fs_v(texture2d<float> t [[texture(0)]]) {\n"
                    @"    constexpr sampler s(coord::normalized, filter::linear);\n"
                    @"    return t.sample(s, float2(0.5, 0.5)) * 0.5 + float4(0.0, %f, 0.0, 1.0);\n"
                    @"}\n"
                    @"kernel void cs_v(device float *o [[buffer(0)]], uint g [[thread_position_in_grid]]) {\n"
                    @"    o[g] = float(g + %d);\n"
                    @"}\n", (float)(i + 1) / 32.0f, i + 1];
                NSError *err = nil;
                id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&err];
                if (!lib) {
                    snprintf(res.error, sizeof(res.error), "compile %d failed: %s", i, err.localizedDescription.UTF8String);
                    CFRelease(vidSurf);
                    return res;
                }
                MTLRenderPipelineDescriptor *rp = [MTLRenderPipelineDescriptor new];
                rp.vertexFunction = [lib newFunctionWithName:@"vs_v"];
                rp.fragmentFunction = [lib newFunctionWithName:@"fs_v"];
                rp.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
                id<MTLRenderPipelineState> rps = [dev newRenderPipelineStateWithDescriptor:rp error:&err];
                id<MTLComputePipelineState> cps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"cs_v"] error:&err];
                if (!rps || !cps) {
                    snprintf(res.error, sizeof(res.error), "pipeline %d nil: %s", i, err.localizedDescription.UTF8String);
                    CFRelease(vidSurf);
                    return res;
                }
                [rpsos addObject:rps];
                [cpsos addObject:cps];
                res.pipelinesCompiled += 2;
            }

            id<MTLTexture> vtex = [dev newTextureWithDescriptor:vd iosurface:vidSurf plane:0];
            id<MTLCommandBuffer> cb = [q commandBuffer];

            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:cpsos[i % nVariants]];
            [ce setBuffer:outBuf offset:0 atIndex:0];
            [ce dispatchThreads:MTLSizeMake(64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            [ce endEncoding];

            MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
            rpd.colorAttachments[0].texture = rtSmall;
            rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
            rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rpd];
            [re setRenderPipelineState:rpsos[i % nVariants]];
            [re setFragmentTexture:vtex atIndex:0];
            [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            [re endEncoding];

            [cb commit];
            [cb waitUntilCompleted];
            if (cb.status != MTLCommandBufferStatusCompleted) {
                snprintf(res.error, sizeof(res.error), "vram stress cb failed iter %d", i);
                CFRelease(vidSurf);
                return res;
            }
            float *out = (float *)outBuf.contents;
            int expectBase = (i % nVariants) + 1;
            if (fabsf(out[7] - (float)(7 + expectBase)) > 0.01f) {
                snprintf(res.error, sizeof(res.error), "compute readback mismatch iter %d: got %f want %d",
                         i, out[7], 7 + expectBase);
                CFRelease(vidSurf);
                return res;
            }
        }
    }

    CFRelease(vidSurf);
    res.wallMs = (double)(nowNs() - t0) / 1e6;
    res.ok = 1;
    return res;
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        const char *mode = "all";
        int frames = 120;
        int targetMb = 320;
        int iters = 24;
        double cpuSec = 2.0;
        int json = 0;

        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--mode") && i + 1 < argc) mode = argv[++i];
            else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--mb") && i + 1 < argc) targetMb = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--iters") && i + 1 < argc) iters = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) cpuSec = atof(argv[++i]);
            else if (!strcmp(argv[i], "--json")) json = 1;
        }

        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) {
            fprintf(stderr, "Error: MTLCreateSystemDefaultDevice returned nil\n");
            return 1;
        }

        int doCpu  = (!strcmp(mode, "all") || !strcmp(mode, "cpu-bench"));
        int doComp = (!strcmp(mode, "all") || !strcmp(mode, "composite"));
        int doVram = (!strcmp(mode, "all") || !strcmp(mode, "vram-stress"));

        CpuBenchResult cpu = {0};
        CompositeResult comp = {0};
        VramStressResult vram = {0};

        if (doCpu)  cpu  = runCpuBench(cpuSec);
        if (doComp) comp = runCompositeBench(dev, frames);
        if (doVram) vram = runVramStress(dev, targetMb, iters);

        int overallOk = 1;
        if (doComp && !comp.ok) overallOk = 0;
        if (doVram && !vram.ok) overallOk = 0;

        if (json) {
            printf("{");
            printf("\"device\":\"%s\",\"ok\":%s", dev.name.UTF8String, overallOk ? "true" : "false");
            if (doCpu) {
                printf(",\"cpu\":{\"score_1t\":%.2f,\"score_8t\":%.2f,\"score_16t\":%.2f,"
                       "\"smt_scaling\":%.2f,\"throttle_ratio_16t\":%.3f,\"memcpy_gbs\":%.2f}",
                       cpu.score1T, cpu.score8T, cpu.score16T, cpu.smtScaling, cpu.throttle16T, cpu.memCopyGBs);
            }
            if (doComp) {
                printf(",\"composite\":{\"ok\":%s,\"frames\":%d,\"wall_ms\":%.2f,"
                       "\"encode_p50_ms\":%.3f,\"encode_p95_ms\":%.3f,\"encode_p99_ms\":%.3f,\"encode_max_ms\":%.3f,"
                       "\"total_p50_ms\":%.3f,\"total_p95_ms\":%.3f,\"total_p99_ms\":%.3f,\"total_max_ms\":%.3f,"
                       "\"avg_fps\":%.1f,\"pct_under_833ms\":%.1f,\"cpu_pct\":%.1f,\"error\":\"%s\"}",
                       comp.ok ? "true" : "false", comp.frames, comp.wallMs,
                       comp.encodeP50, comp.encodeP95, comp.encodeP99, comp.encodeMax,
                       comp.totalP50, comp.totalP95, comp.totalP99, comp.totalMax,
                       comp.avgFps, comp.pctUnder833, comp.cpuPct, comp.error);
            }
            if (doVram) {
                printf(",\"vram_stress\":{\"ok\":%s,\"target_mb\":%d,\"textures_held\":%d,"
                       "\"iters\":%d,\"pipelines_compiled\":%d,\"wall_ms\":%.2f,\"error\":\"%s\"}",
                       vram.ok ? "true" : "false", vram.targetMb, vram.texturesHeld,
                       vram.iters, vram.pipelinesCompiled, vram.wallMs, vram.error);
            }
            printf("}\n");
        } else {
            printf("Device: %s\n", dev.name.UTF8String);
            if (doCpu) {
                printf("[CPU] 1T=%.1f Mops/s | 8T=%.1f Mops/s | 16T=%.1f Mops/s | SMT=%.2fx | throttle16T=%.3f | memcpy=%.2f GB/s\n",
                       cpu.score1T, cpu.score8T, cpu.score16T, cpu.smtScaling, cpu.throttle16T, cpu.memCopyGBs);
            }
            if (doComp) {
                printf("[Composite] ok=%d frames=%d avg=%.1f FPS (<8.33ms: %.1f%%) | enc p50/p95/p99=%.3f/%.3f/%.3f ms | tot p50/p95/p99=%.3f/%.3f/%.3f ms | cpu=%.1f%% %s\n",
                       comp.ok, comp.frames, comp.avgFps, comp.pctUnder833,
                       comp.encodeP50, comp.encodeP95, comp.encodeP99,
                       comp.totalP50, comp.totalP95, comp.totalP99,
                       comp.cpuPct, comp.error);
            }
            if (doVram) {
                printf("[VRAM Stress] ok=%d held=%d (%d MiB) pipelines=%d iters=%d wall=%.1f ms %s\n",
                       vram.ok, vram.texturesHeld, vram.targetMb, vram.pipelinesCompiled, vram.iters, vram.wallMs, vram.error);
            }
        }
        return overallOk ? 0 : 2;
    }
}
