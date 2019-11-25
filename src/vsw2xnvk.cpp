#include <map>
#include <mutex>
#include <fstream>
#include <thread>
#include <atomic>

#include "gpu.h"
#include "waifu2x.h"

#include "VSHelper.h"

class Semaphore {
private:
    std::atomic_int val;
public:
    explicit Semaphore(int init_value) : val(init_value) {
    }
    void wait() {
        int e;
        do {
            while ((e = val.load()) <= 0) {
                std::this_thread::yield();
            }
        } while (!val.compare_exchange_strong(e, e - 1));
    }
    void signal() {
        val.fetch_add(1);
    }
};

typedef struct {
    VSNodeRef *node;
    VSVideoInfo vi;
    Waifu2x *waifu2x;
    Semaphore *gpuSemaphore;
} FilterData;

static std::mutex g_lock{};
static int g_filter_instance_count = 0;
static std::map<int, Semaphore *> g_gpu_semaphore;

static bool filter(const VSFrameRef *src, VSFrameRef *dst, FilterData * const VS_RESTRICT d, const VSAPI *vsapi) noexcept {
    const int width = vsapi->getFrameWidth(src, 0);
    const int height = vsapi->getFrameHeight(src, 0);
    const int srcStride = vsapi->getStride(src, 0) / static_cast<int>(sizeof(uint8_t));
    const int dstStride = vsapi->getStride(dst, 0) / static_cast<int>(sizeof(uint8_t));
    auto *srcR = reinterpret_cast<const uint8_t *>(vsapi->getReadPtr(src, 0));
    auto *srcG = reinterpret_cast<const uint8_t *>(vsapi->getReadPtr(src, 1));
    auto *srcB = reinterpret_cast<const uint8_t *>(vsapi->getReadPtr(src, 2));

    auto *srcInterleaved = new uint8_t[width * height * 3];
    auto *dstInterleaved = new uint8_t[d->vi.width * d->vi.height * 3];

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const int pos = (width * y + x) * 3;
            srcInterleaved[pos + 0] = srcR[x];
            srcInterleaved[pos + 1] = srcG[x];
            srcInterleaved[pos + 2] = srcB[x];
        }
        srcR += srcStride;
        srcG += srcStride;
        srcB += srcStride;
    }

    d->gpuSemaphore->wait();
    ncnn::Mat inImage = ncnn::Mat(width, height, static_cast<void *>(srcInterleaved), static_cast<size_t>(3), 3);
    ncnn::Mat outImage = ncnn::Mat(d->vi.width, d->vi.height, static_cast<void *>(dstInterleaved),static_cast<size_t>(3), 3);
    d->waifu2x->process(inImage, outImage);
    d->gpuSemaphore->signal();

    auto * VS_RESTRICT dstR = reinterpret_cast<uint8_t *>(vsapi->getWritePtr(dst, 0));
    auto * VS_RESTRICT dstG = reinterpret_cast<uint8_t *>(vsapi->getWritePtr(dst, 1));
    auto * VS_RESTRICT dstB = reinterpret_cast<uint8_t *>(vsapi->getWritePtr(dst, 2));

    for (int y = 0; y < d->vi.height; y++) {
        for (int x = 0; x < d->vi.width; x++) {
            const int pos = (d->vi.width * y + x) * 3;
            dstR[x] = dstInterleaved[pos + 0];
            dstG[x] = dstInterleaved[pos + 1];
            dstB[x] = dstInterleaved[pos + 2];
        }
        dstR += dstStride;
        dstG += dstStride;
        dstB += dstStride;
    }

    delete[] srcInterleaved;
    delete[] dstInterleaved;

    return true;
}

static void VS_CC filterInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    auto *d = static_cast<FilterData *>(*instanceData);
    vsapi->setVideoInfo(&d->vi, 1, node);
}

static const VSFrameRef *VS_CC filterGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    auto *d = static_cast<FilterData *>(*instanceData);

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrameRef *src = vsapi->getFrameFilter(n, d->node, frameCtx);

        VSFrameRef * dst = vsapi->newVideoFrame(d->vi.format, d->vi.width, d->vi.height, src, core);
        filter(src, dst, d, vsapi);

        vsapi->freeFrame(src);
        return dst;
    }

    return nullptr;
}

static void VS_CC filterFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    auto *d = static_cast<FilterData *>(instanceData);
    vsapi->freeNode(d->node);
    delete d->waifu2x;
    delete d;

    std::lock_guard<std::mutex> guard(g_lock);
    g_filter_instance_count--;
    if (g_filter_instance_count == 0) {
        ncnn::destroy_gpu_instance();
        for (auto pair : g_gpu_semaphore) {
            delete pair.second;
        }
        g_gpu_semaphore.clear();
    }
}

static void VS_CC filterCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    FilterData d{};

    d.node = vsapi->propGetNode(in, "clip", 0, nullptr);
    d.vi = *vsapi->getVideoInfo(d.node);

    {
        std::lock_guard<std::mutex> guard(g_lock);

        if (g_filter_instance_count == 0)
            ncnn::create_gpu_instance();

        g_filter_instance_count++;
    }

    int gpuId, noise, scale, tileSize, gpuThread;
    std::string paramPath, modelPath;
    try {
        int err;

        if (!isConstantFormat(&d.vi) || d.vi.format->colorFamily != cmRGB || d.vi.format->sampleType != stInteger || d.vi.format->bitsPerSample != 8)
            throw std::string{ "only constant RGB format and 8 bit integer input supported" };

        gpuId = int64ToIntS(vsapi->propGetInt(in, "gpu_id", 0, &err));
        if (gpuId < 0 || gpuId >= ncnn::get_gpu_count())
            throw std::string{"invaild gpu_id"};

        noise = int64ToIntS(vsapi->propGetInt(in, "noise", 0, &err));
        if (noise < -1 || noise > 3)
            throw std::string{ "noise must be -1, 0, 1, 2, or 3" };

        scale = int64ToIntS(vsapi->propGetInt(in, "scale", 0, &err));
        if (err)
            scale = 2;
        if (scale != 1 && scale != 2)
            throw std::string{ "scale must be 1 or 2" };

        tileSize = int64ToIntS(vsapi->propGetInt(in, "tile_size", 0, &err));
        if (err)
            tileSize = 180;
        if (tileSize < 32)
            throw std::string{ "tile size must be greater than or equal to 32" };

        gpuThread = int64ToIntS(vsapi->propGetInt(in, "gpu_thread", 0, &err));
        if (err || gpuThread <= 0)
            gpuThread = 2;
        gpuThread = std::min(gpuThread, int64ToIntS(ncnn::get_gpu_info(gpuId).compute_queue_count));

        d.vi.width *= scale;
        d.vi.height *= scale;

        // set model path
        const std::string pluginDir{ vsapi->getPluginPath(vsapi->getPluginById("net.nlzy.vsw2xnvk", core)) };
        const std::string modelsDir{ pluginDir.substr(0, pluginDir.find_last_of('/')) + "/models/" };
        std::string modelName;
        if (noise == -1)
            modelName = "scale2.0x_model";
        else if (scale == 1)
            modelName = "noise" + std::to_string(noise) + "_model";
        else
            modelName = "noise" + std::to_string(noise) + "_scale2.0x_model";

        paramPath = modelsDir + modelName + ".param";
        modelPath = modelsDir + modelName + ".bin";

        // check model file readable
        std::ifstream pf(paramPath);
        std::ifstream mf(modelPath);
        if (!pf.good() || !mf.good())
            throw std::string{ "can't open model file" };

    } catch (std::string &err) {
        {
            std::lock_guard<std::mutex> guard(g_lock);

            g_filter_instance_count--;
            if (g_filter_instance_count == 0)
                ncnn::destroy_gpu_instance();
        }
        vsapi->setError(out, ("Waifu2x-NCNN-Vulkan: " + err).c_str());
        vsapi->freeNode(d.node);
        return;
    }

    {
        std::lock_guard<std::mutex> guard(g_lock);

        if (!g_gpu_semaphore.count(gpuId))
            g_gpu_semaphore.insert(std::pair<int, Semaphore *>(0, new Semaphore(gpuThread)));

        d.gpuSemaphore = g_gpu_semaphore.at(gpuId);

        d.waifu2x = new Waifu2x(gpuId);
        d.waifu2x->scale = scale;
        d.waifu2x->noise = noise;
        d.waifu2x->tilesize = tileSize;
        d.waifu2x->prepadding = 7;
#if _WIN32
        std::wstring pp(paramPath.begin(), paramPath.end());
        std::wstring mp(modelPath.begin(), modelPath.end());
        d.waifu2x->load(pp, mp);
#else
        d.waifu2x->load(paramPath, modelPath);
#endif
    }

#if _WIN32
    // HACK: vsapi->freeNode always crash on Windows if we don't sleep after model load
    Sleep(1000);
#endif

    auto *data = new FilterData{ d };

    vsapi->createFilter(in, out, "Waifu2x", filterInit, filterGetFrame, filterFree, fmParallel, 0, data, core);
}

//////////////////////////////////////////
// Init

VS_EXTERNAL_API(void) VapourSynthPluginInit(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin) {
    configFunc("net.nlzy.vsw2xnvk", "w2xnvk", "VapourSynth Waifu2x NCNN Vulkan Plugin", VAPOURSYNTH_API_VERSION, 1, plugin);
    registerFunc("Waifu2x", "clip:clip;"
                            "noise:int:opt;"
                            "scale:int:opt;"
                            "tile_size:int:opt;"
                            "gpu_id:int:opt;"
                            "gpu_thread:int:opt;"
                            , filterCreate, nullptr, plugin);
}