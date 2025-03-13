#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#include "retracer/retracer.hpp"

#include "retracer/glstate.hpp"
#include "retracer/retrace_api.hpp"
#include "retracer/trace_executor.hpp"
#include "retracer/forceoffscreen/offscrmgr.h"
#include "retracer/glws.hpp"
#include "retracer/config.hpp"

#include "libcollector/interface.hpp"
#include "helper/states.h"
#include "helper/shadermod.hpp"

#include "libcollector/collectors/perf.hpp"

#include "dispatch/eglproc_auto.hpp"

#include "common/image.hpp"
#include "common/os_string.hpp"
#include "common/pa_exception.h"
#include "common/gl_extension_supported.hpp"

#include "hwcpipe/hwcpipe.hpp"
#include "hwcpipe/counter_database.hpp"
#include "device/product_id.hpp"

#include "json/writer.h"
#include "json/reader.h"

#include <chrono>
#include <errno.h>
#include <algorithm> // for std::min/max
#include <string>
#include <sstream>
#include <vector>
#include <sys/stat.h>
#include <fcntl.h>
#include <libgen.h> // basename
#include <sys/types.h>
#include <signal.h>
#include <unistd.h>
#include <sched.h>
#include <sys/time.h>
#include <sys/resource.h>

using namespace common;
using namespace os;
using namespace glstate;
using namespace image;

#include <fstream>

using namespace std;

void retrace_eglMakeCurrent(char* src);
void retrace_eglCreateContext(char* src);
void retrace_eglCreateWindowSurface(char* src);

namespace retracer {

Retracer gRetracer;

static inline uint64_t gettime()
{
    struct timespec t;
    // CLOCK_MONOTONIC_COARSE is much more light-weight, but resolution is quite poor.
    // CLOCK_PROCESS_CPUTIME_ID is another possibility, it ignores rest of system, but costs more,
    // and also on some CPUs process migration between cores can screw up such measurements.
    // CLOCK_MONOTONIC is therefore a reasonable and portable compromise.
    clock_gettime(CLOCK_MONOTONIC_RAW, &t);
    return ((uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec);
}

/// -- libGPUCounter support
struct HWCPipeHandler
{
    std::vector<hwcpipe::gpu> gpus;
    std::vector<hwcpipe::sampler_config> sampler_configs;
    std::vector<std::vector<hwcpipe_counter>> gpu_counters;
    std::vector<hwcpipe::sampler<>> samplers;
    std::vector<std::string> output_filenames;
    std::vector<std::fstream> output_file_handles;

    ~HWCPipeHandler()
    {
        for (auto& fhandle : output_file_handles)
        {
            if (fhandle)
            {
                fhandle.close();
            }
        }
        for (long unsigned int idx=0; idx<gpus.size(); idx++)
            gpu_counters[idx].clear();
        gpus.clear();
        sampler_configs.clear();
        gpu_counters.clear();
        samplers.clear();
        output_filenames.clear();
        output_file_handles.clear();
    }
    void hwcpipe_init(const std::string &result_path);
    void write_result_header(const std::string &result_path);
    void sample_counters();
    void sample_stop();
};
static HWCPipeHandler *mHWCPipeHandler = NULL;

static std::string get_result_path(uint32_t gpu_id, const std::string &result_path)
{
    int index = 0;

    std::string output_name = std::string("gpu_id_") + std::to_string(gpu_id) + std::string("_per_frame_counters.csv");
    std::string output_path = result_path + std::to_string(index) + std::string("_") + output_name;

    std::ifstream tfile(output_path.c_str());
    while(tfile.good())
    {
        tfile.close();

        index += 1;
        output_path = result_path + std::to_string(index) + std::string("_") + output_name;

        tfile.open(output_path.c_str());
    }

    DBG_LOG("Writing gpu-counter results to file: %s \n", output_path.c_str());

    return output_path;
}

static const char *get_product_family_name(hwcpipe::device::gpu_family f)
{
    using gpu_family = hwcpipe::device::gpu_family;

    switch (f) {
    case gpu_family::bifrost:
        return "Bifrost";
    case gpu_family::midgard:
        return "Midgard";
    case gpu_family::valhall:
        return "Valhall";
    case gpu_family::fifthgen:
        return "Arm 5th Gen";
    default:
        return "Unknown";
    }
}

static void print_gpu_meta(const hwcpipe::gpu& gpu)
{
    DBG_LOG("------------------------------------------------------------\n");
    std::stringstream ss;
    ss << " GPU Device " << gpu.get_device_number() << "\n";
    DBG_LOG("%s", ss.str().c_str());

    ss.str("");
    ss.clear();
    ss << "    Product Family:  " << get_product_family_name(gpu.get_gpu_family()) << "\n";
    DBG_LOG("%s", ss.str().c_str());

    ss.str("");
    ss.clear();
    ss << "    Number of Cores: " << gpu.num_shader_cores() << "\n";
    DBG_LOG("%s", ss.str().c_str());

    ss.str("");
    ss.clear();
    ss << "    Bus Width:       " << gpu.bus_width() << "\n";
    DBG_LOG("%s", ss.str().c_str());
}

void HWCPipeHandler::hwcpipe_init(const std::string &result_path)
{
    // Setup and start HWCP sampling
    // Detect all GPUs
    for (const auto &gpu : hwcpipe::find_gpus()) {
        gpus.push_back(gpu);
    }

	sampler_configs.reserve(sampler_configs.size());
	gpu_counters.resize(gpus.size());
	samplers.reserve(gpus.size());

	uint32_t gpu_index = 0;
	for (auto& gpu : gpus)
	{
		print_gpu_meta(gpu);
		auto counter_db = hwcpipe::counter_database{};

		sampler_configs.push_back(hwcpipe::sampler_config(gpu));
		auto& config = sampler_configs.back();

		for (hwcpipe_counter counter : counter_db.counters_for_gpu(gpu)) {
			std::error_code ec = config.add_counter(counter);
			if (ec) {
				// Should not happen because counters_for_gpu(gpu) only returns
				// known counters, by definition
				assert(false);
			}
			gpu_counters[gpu_index].push_back(counter);
		}

		samplers.push_back(hwcpipe::sampler<>(config));
		auto& sampler = samplers.back();
		std::error_code ec = sampler.start_sampling();
		if (ec) {
			std::cout << ec.message() << std::endl;
		}
		gpu_index += 1;
	}

	// Write the CSV header here
	write_result_header(result_path);
}

void HWCPipeHandler::write_result_header(const std::string &result_path)
{
    // This should only be called once
    assert(output_filenames.size() == 0);

    uint32_t gpu_index = 0;
    for (auto& gpu_counter_list : gpu_counters)
    {
        std::string result_file_path = get_result_path(gpu_index, result_path);
        gpu_index += 1;

        output_filenames.push_back(result_file_path);

        hwcpipe::counter_metadata metadata;
        auto counter_db = hwcpipe::counter_database{};

        output_file_handles.emplace_back();
        std::fstream& file = output_file_handles.back();
        file.open(result_file_path.c_str(), std::fstream::out);
        if (!file) {
            DBG_LOG("Failed to open file for header output: %s, %s\n", result_file_path.c_str(), strerror(errno));
            continue;
        }

        bool is_first_counter = true;
        for (auto& counter : gpu_counter_list) {
            std::error_code ec = counter_db.describe_counter(counter, metadata);
            if (ec) {
                DBG_LOG("Error when fetching counter metadata: %s\n", ec.message().c_str());
                assert(false);
            }

            if (is_first_counter)
            {
                file << metadata.name;
                is_first_counter = false;
            }
            else
            {
                file << "," << metadata.name;
            }
        }

        file << "\n";
    }
}

void HWCPipeHandler::sample_counters()
{
    for (auto& sampler : samplers)
    {
        std::error_code ec = sampler.sample_now();
        if (ec) {
            DBG_LOG("Error when sampling counter: %s\n", ec.message().c_str());
            continue;
        }
    }

    // Read all counters and store
    uint32_t sampler_index = 0;
    for (auto& gpu_counter_list : gpu_counters)
    {
        std::fstream& file = output_file_handles[sampler_index];

        hwcpipe::counter_sample sample;
        bool first_value = true;
        for (hwcpipe_counter counter : gpu_counter_list) {
            std::error_code ec = samplers[sampler_index].get_counter_value(counter, sample);
            if (ec) {
                DBG_LOG("ERROR: Failed to sample counter!");
                assert(false);
            }

            std::string delimiter = ",";
            if (first_value)
            {
                delimiter = "";
                first_value = false;
            }

            switch (sample.type) {
                case hwcpipe::counter_sample::type::uint64:{
                    file << delimiter << sample.value.uint64;
                    break;
                }
                case hwcpipe::counter_sample::type::float64:{
                    file << delimiter << sample.value.float64;
                    break;
                }
                default:{
                    break;
                }
            }
        }

        file << "\n";

        sampler_index += 1;
    }
}

void HWCPipeHandler::sample_stop()
{
    for (auto& sampler : samplers)
    {
        std::error_code ec = sampler.stop_sampling();
        if (ec) DBG_LOG("Error when stopping sample: %s.\n", ec.message().c_str());
    }
}

Retracer::~Retracer()
{
    delete mCollectors;
    delete mHWCPipeHandler;
    mCollectors = nullptr;
    mHWCPipeHandler = nullptr;

#ifndef NDEBUG
    if (mVBODataSize) DBG_LOG("VBO data size : %u\n", mVBODataSize);
    if (mTextureDataSize) DBG_LOG("Uncompressed texture data size : %u\n", mTextureDataSize);
    if (mCompressedTextureDataSize) DBG_LOG("Compressed texture data size : %u\n", mCompressedTextureDataSize);
    if (mCSBuffers.total_size()) DBG_LOG("Client-side memory data size : %lu\n", (unsigned long)mCSBuffers.total_size());
#endif
}

bool Retracer::OpenTraceFile(const char* filename)
{
    if (!mFile.Open(filename))
        return false;

    if (mOptions.mPatchPath.size() != 0)
    {
        if (!mFile.OpenPatchFile(mOptions.mPatchPath.c_str()))
            return false;
    }

    mFileFormatVersion = mFile.getHeaderVersion();
    mStateLogger.open(std::string(filename) + ".retracelog");
    loadRetraceOptionsFromHeader();
    mFinish.store(false);

    return true;
}

__attribute__ ((noinline)) static int noop(int a)
{
    return a + 1;
}

void Retracer::CloseTraceFile()
{
    mFile.Close();
    mFileFormatVersion = INVALID_VERSION;
    mStateLogger.close();
    mState.Reset();
    mCSBuffers.clear();
    mSnapshotPaths.clear();
    results.clear();
    mCallStats.clear();
    mVBODataSize = 0;
    mTextureDataSize = 0;
    mCompressedTextureDataSize = 0;
    mSurfaceCount = 0;
    mpOffscrMgr = nullptr;
    mpQuad = nullptr;
    frameBudget = INT64_MAX;
    drawBudget = INT64_MAX;
    mMosaicNeedToBeFlushed = false;
    delayedPerfmonInit = false;
    shaderCacheFile = NULL;
    shaderCacheIndex.clear();
    shaderCache.clear();
    conditions.clear();
    threads.clear();
    thread_remapping.clear();
    swapvals.clear();
    cachevals.clear();
    syncvals.clear();
    mInitTime = 0;
    mInitTimeMono = 0;
    mInitTimeMonoRaw = 0;
    mInitTimeBoot = 0;
    mEndFrameTime = 0;
    mTimerBeginTime = 0;
    mTimerBeginTimeMono = 0;
    mTimerBeginTimeMonoRaw = 0;
    mTimerBeginTimeBoot = 0;
    mFinishSwapTime = 0;
    child = 0;
    mLoopTimes = 0;
    mLoopBeginTime = 0;
    mCurFrameNo = 0;
    mCurDrawNo = 0;
    mRollbackCallNo = 0;

    if (shaderCacheFile)
    {
        fclose(shaderCacheFile);
        shaderCacheFile = NULL;
    }
}

bool Retracer::loadRetraceOptionsByThreadId(int tid)
{
    const Json::Value jsThread = mFile.getJSONThreadById(tid);
    if (jsThread.isNull())
    {
        DBG_LOG("No stored EGL config for this tid: %d\n", tid);
        return false;
    }
    if (mOptions.mWindowWidth == 0) mOptions.mWindowWidth = jsThread["winW"].asInt();
    if (mOptions.mWindowHeight == 0) mOptions.mWindowHeight = jsThread["winH"].asInt();
    mOptions.mOnscreenConfig = jsThread["EGLConfig"];
    mOptions.mOffscreenConfig = jsThread["EGLConfig"];
    return true;
}

void Retracer::loadRetraceOptionsFromHeader()
{
    // Load values from headers first, then any valid commandline parameters override the header defaults.
    const Json::Value jsHeader = mFile.getJSONHeader();
    if (mOptions.mRetraceTid == -1) mOptions.mRetraceTid = jsHeader.get("defaultTid", -1).asInt();
    if (mOptions.mRetraceTid == -1) reportAndAbort("No thread ID set!");
    if (jsHeader.isMember("forceSingleWindow")) mOptions.mForceSingleWindow = jsHeader.get("forceSingleWindow", false).asBool();
    if (jsHeader.isMember("singleSurface")) mOptions.mSingleSurface = jsHeader.get("singleSurface", false).asInt();
    if (mOptions.mForceSingleWindow && mOptions.mSingleSurface != -1) reportAndAbort("forceSingleWindow and singleSurface cannot be used together");
    if (mOptions.mForceSingleWindow) DBG_LOG("Enabling force single window option\n");
    if (jsHeader.isMember("multiThread")) mOptions.mMultiThread = jsHeader.get("multiThread", false).asBool();
    if (mOptions.mMultiThread) DBG_LOG("Enabling multiple thread option\n");
    if (jsHeader.isMember("translucentSurface")) mOptions.mTranslucentSurface = jsHeader.get("translucentSurface", false).asBool();
    if (jsHeader.get("translucentSurface", false).asBool()) DBG_LOG("Enabling translucentSurface option from Json header\n");
    if (jsHeader.isMember("skipfence")) {
        std::vector<std::pair<unsigned int, unsigned int>> ranges;
        for (const auto& ranges_itr : jsHeader["skipfence"])
        {
            ranges.push_back(std::make_pair(ranges_itr[0].asInt(), ranges_itr[1].asInt()));
        }

        if (ranges.size() == 0)
        {
            reportAndAbort("Bad value for option -skipfence, must give at least one frame range.");
        }

        std::sort(ranges.begin(), ranges.end());

        unsigned int start = ranges[0].first;
        unsigned int end = ranges[0].second;

        std::vector<std::pair<unsigned int, unsigned int>> merged_ranges;
        for (unsigned int i = 0; i < ranges.size() - 1; ++i)
        {
            if (ranges[i].second >= ranges[i + 1].first)
            {
                if (ranges[i].second < ranges[i + 1].second)
                {
                    end = ranges[i + 1].second;
                }
                else
                {
                    i += 1;
                }
            }
            else
            {
                merged_ranges.push_back(std::make_pair(start, end));
                start = ranges[i + 1].first;
                end = ranges[i + 1].second;
            }
        }

        if (merged_ranges.size() > 0)
        {
            mOptions.mSkipFence = true;
            mOptions.mSkipFenceRanges = merged_ranges;
        }
        else
        {
            mOptions.mSkipFence = false;
        }
    }
    if (mOptions.mSkipFence) DBG_LOG("Enabling fence skip option\n");
    switch (jsHeader["glesVersion"].asInt())
    {
    case 1: mOptions.mApiVersion = PROFILE_ES1; break;
    case 2: mOptions.mApiVersion = PROFILE_ES2; break;
    case 3: mOptions.mApiVersion = PROFILE_ES3; break;
    default: DBG_LOG("Error: Invalid glesVersion parameter\n"); break;
    }
    loadRetraceOptionsByThreadId(mOptions.mRetraceTid);
    const Json::Value& linkErrorWhiteListCallNum = jsHeader["linkErrorWhiteListCallNum"];
    for(unsigned int i=0; i<linkErrorWhiteListCallNum.size(); i++)
    {
        mOptions.mLinkErrorWhiteListCallNum.push_back(linkErrorWhiteListCallNum[i].asUInt());
    }
    if (mOptions.mForceOffscreen)
    {
        // When running offscreen, force onscreen EGL to most compatible mode known: 5650 00
        mOptions.mOnscreenConfig = EglConfigInfo(5, 6, 5, 0, 0, 0, 0, 0);
        mOptions.mOffscreenConfig.override(mOptions.mOverrideConfig);
    }
    else
    {
        mOptions.mOnscreenConfig.override(mOptions.mOverrideConfig);
    }

    const int required_major = jsHeader.get("required_replayer_version_major", 0).asInt();
    const int required_minor = jsHeader.get("required_replayer_version_minor", 0).asInt();
    if (required_major > PATRACE_VERSION_MAJOR || (required_major == PATRACE_VERSION_MAJOR && required_minor > PATRACE_VERSION_MINOR))
    {
        reportAndAbort("Required replayer version is r%dp%d, your version is r%dp%d", required_major, required_minor, PATRACE_VERSION_MAJOR, PATRACE_VERSION_MINOR);
    }
}

std::vector<Texture> Retracer::getTexturesToDump()
{
    static const char* todump = getenv("RETRACE_DUMP_TEXTURES");

    if(!todump)
    {
        return std::vector<Texture>();
    }

    // Format:
    // "texid texid ..."
    std::string str(todump);
    std::istringstream textureNamesSS(str);

    std::vector<Texture> ret;
    GLint textureName = 0;
    Context& context = gRetracer.getCurrentContext();
    while (textureNamesSS >> textureName)
    {
        Texture t = {};
        t.handle = context.getTextureMap().RValue(textureName);
        ret.push_back(t);
    }
    return ret;
}

void Retracer::dumpUniformBuffers(unsigned int callNo)
{
    static const char* todump = getenv("RETRACE_DUMP_BUFFERS");

    if(!todump)
    {
        return;
    }

    State s({}, {GL_UNIFORM_BUFFER_BINDING});

    // Format = "bufId bufId ..."
    std::string str(todump);
    std::istringstream ubNamesSS(str);
    GLint ubName = 0;
    Context& context = gRetracer.getCurrentContext();
    while (ubNamesSS >> ubName)
    {
        GLint ubNameNew = context.getBufferMap().RValue(ubName);
        if (glIsBuffer(ubNameNew) == GL_FALSE)
        {
            DBG_LOG("%d is not a buffer.\n", ubNameNew);
            continue;
        }

        glBindBuffer(GL_UNIFORM_BUFFER, ubNameNew);
        GLint size = 0;
        glGetBufferParameteriv(GL_UNIFORM_BUFFER, GL_BUFFER_SIZE, &size);
        GLint is_mapped = 0;
        glGetBufferParameteriv(GL_UNIFORM_BUFFER, GL_BUFFER_MAPPED, &is_mapped);
        if(is_mapped == GL_TRUE)
        {
            glUnmapBuffer(GL_UNIFORM_BUFFER);
        }
        char* bufferdata = (char*)glMapBufferRange(GL_UNIFORM_BUFFER,
                                                   0,
                                                   size,
                                                   GL_MAP_READ_BIT);
        if (!bufferdata)
        {
            DBG_LOG("Couldn't map buffer %d\n", ubName);
            continue;
        }

        DBG_LOG("Bound buffer %d, size %d\n", ubName, size);
        //
        // Grab data
        std::stringstream ss;
        ss << mOptions.mSnapshotPrefix << std::setw(10) << std::setfill('0') << callNo << "_b" << ubName << ".bin";

        FILE* fd = fopen(ss.str().c_str(), "w");
        if (!fd)
        {
            DBG_LOG("Unable to open %s\n", ss.str().c_str());
            continue;
        }
        fwrite(bufferdata, 1, size, fd);
        fclose(fd);
        DBG_LOG("Wrote %s\n", ss.str().c_str());
    }
}

void Retracer::StepShot(unsigned int callNo, unsigned int frameNo, const char *filename)
{
    DBG_LOG("[%d] [Frame/Draw/Call %d/%d/%d] %s.\n", getCurTid(), frameNo, GetCurDrawId(), callNo, GetCurCallName());

    if (eglGetCurrentContext() != EGL_NO_CONTEXT)
    {
        GLint maxAttachments = getMaxColorAttachments();
        GLint maxDrawBuffers = getMaxDrawBuffers();
        for(int i=0; i<maxDrawBuffers; i++)
        {
            GLint colorAttachment = getColorAttachment(i);
            if(colorAttachment != GL_NONE)
            {
                image::Image *src = getDrawBufferImage(colorAttachment);
                if (src == NULL)
                {
                    DBG_LOG("Failed to dump bound framebuffer\n");
                    break;
                }

                std::string filenameToBeUsed;
                int attachmentIndex = colorAttachment - GL_COLOR_ATTACHMENT0;
                bool validAttachmentIndex = attachmentIndex >= 0 && attachmentIndex < maxAttachments;
                if (!validAttachmentIndex)
                {
                    attachmentIndex = 0;
                }

                std::stringstream ss;
#ifdef ANDROID
                    ss << "/sdcard/framebuffer" << "_c" << attachmentIndex << ".png";
#else
                    ss << "framebuffer" << "_c" << attachmentIndex << ".png";
#endif
                filenameToBeUsed = ss.str();

                if (src->writePNG(filenameToBeUsed.c_str()))
                {
                    DBG_LOG("Dump bound framebuffer to %s\n", filenameToBeUsed.c_str());
                }
                else
                {
                    DBG_LOG("Failed to dump bound framebuffer to %s\n", filenameToBeUsed.c_str());
                }
                delete src;
            }
        }
    }
    else
    {
        DBG_LOG("EGL context has not been created\n");
    }
}

void Retracer::TakeSnapshot(unsigned int callNo, unsigned int frameNo, const char *filename)
{
    // Only take snapshots inside the measurement range
    const bool inRange = mOptions.mBeginMeasureFrame <= frameNo && frameNo <= mOptions.mEndMeasureFrame;
    if (mOptions.mUploadSnapshots && !inRange)
    {
        return;
    }
    // Add state log dumps, if these are enabled, at the same time
    if (mOptions.mStateLogging)
    {
        mStateLogger.logState(getCurTid());
    }

    GLint maxAttachments = getMaxColorAttachments();
    GLint maxDrawBuffers = getMaxDrawBuffers();
    bool colorAttach = false;
    for (int i=0; i<maxDrawBuffers; i++)
    {
        GLint colorAttachment = getColorAttachment(i);
        if(colorAttachment != GL_NONE)
        {
            colorAttach = true;
            int readFboId = 0, drawFboId = 0;
            _glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFboId);
            _glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFboId);
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
            const unsigned int ON_SCREEN_FBO = 1;
#else
            const unsigned int ON_SCREEN_FBO = 0;
#endif
            if (gRetracer.mOptions.mForceOffscreen) {
                _glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ON_SCREEN_FBO);
                gRetracer.mpOffscrMgr->BindOffscreenReadFBO();
            }
            else {
                _glBindFramebuffer(GL_READ_FRAMEBUFFER, drawFboId);
            }
            image::Image *src = getDrawBufferImage(colorAttachment);
            _glBindFramebuffer(GL_READ_FRAMEBUFFER, readFboId);
            _glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFboId);
            if (src == NULL)
            {
                DBG_LOG("Failed to take snapshot for call no: %d\n", callNo);
                return;
            }

            std::string filenameToBeUsed;
            if (filename)
            {
                filenameToBeUsed = filename;
            }
            else // no incoming filename, we must generate one
            {
                int attachmentIndex = colorAttachment - GL_COLOR_ATTACHMENT0;
                bool validAttachmentIndex = attachmentIndex >= 0 && attachmentIndex < maxAttachments;
                if (!validAttachmentIndex)
                {
                    attachmentIndex = 0;
                }

                std::stringstream ss;
                if (mOptions.mSnapshotFrameNames || mOptions.mLoopTimes > 0 || mOptions.mLoopSeconds > 0)
                {
                    ss << mOptions.mSnapshotPrefix << std::setw(4) << std::setfill('0') << frameNo << "_l" << mLoopTimes << ".png";
                }
                else // use classic weird name
                {
                    ss << mOptions.mSnapshotPrefix << std::setw(10) << std::setfill('0') << callNo << "_c" << attachmentIndex << ".png";
                }
                filenameToBeUsed = ss.str();
            }

            if (src->writePNG(filenameToBeUsed.c_str()))
            {
                DBG_LOG("Snapshot (frame %d, call %d) : %s\n", frameNo, callNo, filenameToBeUsed.c_str());

                // Register the snapshot to be uploaded
                if (mOptions.mUploadSnapshots)
                {
                    mSnapshotPaths.push_back(filenameToBeUsed);
                }
            }
            else
            {
                DBG_LOG("Failed to write snapshot : %s\n", filenameToBeUsed.c_str());
            }

            delete src;
        }
    }
    if (!colorAttach)   // no color attachment, there might be a depth attachment
    {
        DBG_LOG("no color attachment, there might be a depth attachment\n");
        int readFboId = 0, drawFboId = 0;
        _glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFboId);
        _glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFboId);
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
        const unsigned int ON_SCREEN_FBO = 1;
#else
        const unsigned int ON_SCREEN_FBO = 0;
#endif
        if (gRetracer.mOptions.mForceOffscreen) {
            _glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ON_SCREEN_FBO);
            gRetracer.mpOffscrMgr->BindOffscreenReadFBO();
        }
        else {
            _glBindFramebuffer(GL_READ_FRAMEBUFFER, drawFboId);
        }
        image::Image *src = getDrawBufferImage(GL_DEPTH_ATTACHMENT);
        _glBindFramebuffer(GL_READ_FRAMEBUFFER, readFboId);
        _glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFboId);
        if (src == NULL)
        {
            DBG_LOG("Failed to take snapshot for call no: %d\n", callNo);
            return;
        }

        std::string filenameToBeUsed;
        if (filename)
        {
            filenameToBeUsed = filename;
        }
        else // no incoming filename, we must generate one
        {
            std::stringstream ss;
            if (mOptions.mSnapshotFrameNames)
            {
                ss << mOptions.mSnapshotPrefix << std::setw(4) << std::setfill('0') << frameNo << ".png";
            }
            else
            {
                ss << mOptions.mSnapshotPrefix << std::setw(10) << std::setfill('0') << callNo << "_depth.png";
            }
            filenameToBeUsed = ss.str();
        }

        if (src->writePNG(filenameToBeUsed.c_str()))
        {
            DBG_LOG("Snapshot (frame %d, call %d) : %s\n", frameNo, callNo, filenameToBeUsed.c_str());

            // Register the snapshot to be uploaded
            if (mOptions.mUploadSnapshots)
            {
                mSnapshotPaths.push_back(filenameToBeUsed);
            }
        }
        else
        {
            DBG_LOG("Failed to write snapshot : %s\n", filenameToBeUsed.c_str());
        }

        delete src;
    }

    std::vector<Texture> textures = getTexturesToDump();
    GLfloat vertices[8] = { 0.0f, 1.0f,
                            0.0f, 0.0f,
                            1.0f, 1.0f,
                            1.0f, 0.0f };
    for (std::vector<Texture>::iterator it = textures.begin(); it != textures.end(); ++it)
    {
        dumpTexture(*it, callNo, &vertices[0]);
    }

    dumpUniformBuffers(callNo);
}

#if 0
std::string GetShader(const char* filepath)
{
    ifstream ifs;
    ifs.open(filepath);
    if (!ifs.is_open())
        DBG_LOG("Failed to open shader source: %s\n", filepath);
    else
        DBG_LOG("Open shader source: %s\n", filepath);

    std::string ret;
    std::string line;

    while (getline(ifs, line)) {
        DBG_LOG("%s\n", line.c_str());
        ret += line;
        ret += "\n";
    }

    return ret;
}

void DumpUniforms()
{
    GLint program;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);

    if (!program)
        return;

    GLint uniCount;
    glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &uniCount);


    char uniName[256];
    const int UNI_VALUE_BYTE_LEN = 10*1024;
    char uniValue[UNI_VALUE_BYTE_LEN];

    for (int uniLoc = 0; uniLoc < uniCount; ++uniLoc)
    {
        GLint   uniSize;
        GLenum  uniType;
        glGetActiveUniform(program, uniLoc, 256, NULL, &uniSize, &uniType, uniName);

        GLint location;
        location = glGetUniformLocation(program, uniName);

        DBG_LOG("Active idx: #%d, Location: %d\n", uniLoc, location);
        if (uniType == GL_FLOAT) {
            if (uniSize * sizeof(float) > UNI_VALUE_BYTE_LEN)
                DBG_LOG("Error: UNI_VALUE_BYTE_LEN should be increased!!\n");

            glGetUniformfv(program, location, ((GLfloat*)uniValue));
            DBG_LOG("%s float=%f\n", uniName, ((GLfloat*)uniValue)[0]);
        } else if (uniType == GL_FLOAT_VEC2) {
            if (uniSize * 2*sizeof(float) > UNI_VALUE_BYTE_LEN)
                DBG_LOG("Error: UNI_VALUE_BYTE_LEN should be increased!!\n");

            glGetUniformfv(program, location, ((GLfloat*)uniValue));
            DBG_LOG("%s float2=%f\n", uniName, ((GLfloat*)uniValue)[0]);
        } else if (uniType == GL_FLOAT_VEC3) {
            if (uniSize * 3*sizeof(float) > UNI_VALUE_BYTE_LEN)
                DBG_LOG("Error: UNI_VALUE_BYTE_LEN should be increased!!\n");

            glGetUniformfv(program, location, ((GLfloat*)uniValue));
            DBG_LOG("%s float3=%f\n", uniName, ((GLfloat*)uniValue)[0]);
        } else if (uniType == GL_FLOAT_VEC4) {
            if (uniSize * 4*sizeof(float) > UNI_VALUE_BYTE_LEN)
                DBG_LOG("Error: UNI_VALUE_BYTE_LEN should be increased!!\n");

            glGetUniformfv(program, location, ((GLfloat*)uniValue));
            DBG_LOG("%s float4=%f\n", uniName, ((GLfloat*)uniValue)[0]);
        } else if (uniType == GL_INT || uniType == GL_BOOL || uniType == GL_SAMPLER_2D || uniType == GL_SAMPLER_CUBE) {
            if (uniSize * sizeof(int) > UNI_VALUE_BYTE_LEN)
                DBG_LOG("Error: UNI_VALUE_BYTE_LEN should be increased!!\n");

            glGetUniformiv(program, location, ((GLint*)uniValue));
            DBG_LOG("%s int=%d\n", uniName, ((GLint*)uniValue)[0]);
        } else if (uniType == GL_INT_VEC2 || uniType == GL_BOOL_VEC2) {
            if (uniSize * 2*sizeof(int) > UNI_VALUE_BYTE_LEN)
                DBG_LOG("Error: UNI_VALUE_BYTE_LEN should be increased!!\n");

            glGetUniformiv(program, location, ((GLint*)uniValue));
            DBG_LOG("%s int2=%d\n", uniName, ((GLint*)uniValue)[0]);
        } else if (uniType == GL_INT_VEC3 || uniType == GL_BOOL_VEC3) {
            if (uniSize * 3*sizeof(int) > UNI_VALUE_BYTE_LEN)
                DBG_LOG("Error: UNI_VALUE_BYTE_LEN should be increased!!\n");

            glGetUniformiv(program, location, ((GLint*)uniValue));
            DBG_LOG("%s int3=%d\n", uniName, ((GLint*)uniValue)[0]);
        } else if (uniType == GL_INT_VEC4 || uniType == GL_BOOL_VEC4) {
            if (uniSize * 4*sizeof(int) > UNI_VALUE_BYTE_LEN)
                DBG_LOG("Error: UNI_VALUE_BYTE_LEN should be increased!!\n");

            glGetUniformiv(program, location, ((GLint*)uniValue));
            DBG_LOG("%s int4=%d\n", uniName, ((GLint*)uniValue)[0]);
        } else if (uniType == GL_FLOAT_MAT2) {
            if (uniSize * 4*sizeof(float) > UNI_VALUE_BYTE_LEN)
                DBG_LOG("Error: UNI_VALUE_BYTE_LEN should be increased!!\n");

            glGetUniformfv(program, location, ((GLfloat*)uniValue));
            DBG_LOG("%s float2x2=%f\n", uniName, ((GLfloat*)uniValue)[0]);
        } else if (uniType == GL_FLOAT_MAT3) {
            if (uniSize * 9*sizeof(float) > UNI_VALUE_BYTE_LEN)
                DBG_LOG("Error: UNI_VALUE_BYTE_LEN should be increased!!\n");

            glGetUniformfv(program, location, ((GLfloat*)uniValue));
            DBG_LOG("%s float3x3=%f\n", uniName, ((GLfloat*)uniValue)[0]);
        } else if (uniType == GL_FLOAT_MAT4) {
            if (uniSize * 16*sizeof(float) > UNI_VALUE_BYTE_LEN)
                DBG_LOG("Error: UNI_VALUE_BYTE_LEN should be increased!!\n");

            glGetUniformfv(program, location, ((GLfloat*)uniValue));
            DBG_LOG("%s float4x4=%f\n", uniName, ((GLfloat*)uniValue)[0]);
        } else {
            DBG_LOG("Unknown uniform type: %d\n", uniType);
        }

        if (uniType == GL_SAMPLER_2D)
        {
            GLint oldActTex;
            _glGetIntegerv(GL_ACTIVE_TEXTURE, (GLint*)&oldActTex);

            GLint actTex = ((GLint*)uniValue)[0];
            _glActiveTexture(GL_TEXTURE0+actTex);
            GLint bindTex;
            _glGetIntegerv(GL_TEXTURE_BINDING_2D, &bindTex);
            DBG_LOG("    texture name: %d\n", bindTex);

            _glActiveTexture(oldActTex);
        }
    }

    GLint shdLoc;
    shdLoc = glGetUniformLocation(program, "unity_World2Shadow[0]");
    DBG_LOG("unity_World2Shadow[0] = %d\n", shdLoc);
    shdLoc = glGetUniformLocation(program, "unity_World2Shadow[1]");
    DBG_LOG("unity_World2Shadow[1] = %d\n", shdLoc);
    shdLoc = glGetUniformLocation(program, "unity_World2Shadow[2]");
    DBG_LOG("unity_World2Shadow[2] = %d\n", shdLoc);
    shdLoc = glGetUniformLocation(program, "unity_World2Shadow[3]");
    DBG_LOG("unity_World2Shadow[3] = %d\n", shdLoc);
}
#endif


#ifdef ANDROID
string getProcessNameByPid(const int pid)
{
    string pname;
    ifstream f("/proc/" + _to_string(pid) + "/cmdline");
    if (f.is_open())
    {
        f >> pname;
        f.close();
    }
    return pname;
}

#ifdef PLATFORM_64BIT
    #define SYSTEM_VENDOR_LIB_PREFIX "/system/vendor/lib64/egl/"
    #define SYSTEM_LIB_PREFIX "/system/lib64/egl/"
#else
    #define SYSTEM_VENDOR_LIB_PREFIX "/system/vendor/lib/egl/"
    #define SYSTEM_LIB_PREFIX "/system/lib/egl/"
#endif

// Configuration files
const char* applist_cfg_search_paths[] = {
    SYSTEM_VENDOR_LIB_PREFIX "appList.cfg",
    SYSTEM_LIB_PREFIX "appList.cfg",
    NULL,
};

const char* findFirst(const char** paths)
{
    const char* i;
    const char* last = "NONE";
    while ((i = *paths++))
    {
        if (access(i, R_OK) == 0)
        {
             return i;
        }
        last = i;
    }
    return last; // for informative error message and avoid crashing
}
#endif

void forceRenderMosaicToScreen()
{
    if (gRetracer.mMosaicNeedToBeFlushed)
    {
        if (gRetracer.mpOffscrMgr->last_tid != -1) {
            gRetracer.mCurCall.tid = gRetracer.mpOffscrMgr->last_tid;

            int last_non_zero_draw = gRetracer.mpOffscrMgr->last_non_zero_draw;
            int last_non_zero_ctx = gRetracer.mpOffscrMgr->last_non_zero_ctx;

            if (!gRetracer.mState.GetDrawable(last_non_zero_draw)) {
                int win = gRetracer.mState.GetWin(last_non_zero_draw);
                int parameter[8];
                parameter[0] = 0;                   // dpy, useless in retrace_eglCreateWindowSurface
                parameter[1] = 0;                   // config, useless in retrace_eglCreateWindowSurface
                parameter[2] = win;                 // native_window
                parameter[3] = sizeof(int) * 3;     // byte size of attrib_list
                parameter[4] = EGL_RENDER_BUFFER;
                parameter[5] = EGL_BACK_BUFFER;
                parameter[6] = EGL_NONE;
                parameter[7] = last_non_zero_draw;  // ret
                retrace_eglCreateWindowSurface(reinterpret_cast<char *>(parameter));
            }

            int parameter[5];
            parameter[0] = 0;                   // dpy, useless in retrace_eglMakeCurrent
            parameter[1] = last_non_zero_draw;  // draw
            parameter[2] = last_non_zero_draw;  // read, useless in retrace_eglMakeCurrent
            parameter[3] = last_non_zero_ctx;   // ctx
            parameter[4] = 1;                   // ret, useless in retrace_eglMakeCurrent
            retrace_eglMakeCurrent(reinterpret_cast<char *>(parameter));

            gRetracer.mpOffscrMgr->MosaicToScreenIfNeeded(true);
            retracer::Drawable* pDrawable = gRetracer.mState.mThreadArr[gRetracer.getCurTid()].getDrawable();
            if (pDrawable != NULL) {
                pDrawable->swapBuffers();
            }
            else {
                DBG_LOG("pDrawable == NULL. The mosaic picture of last several frames can't be rendered to screen. This might be a bug.\n");
            }
        }
    }
}

static void report_cpu_mask()
{
    cpu_set_t mask;
    std::string descr;
    int retval = sched_getaffinity(0, sizeof(mask), &mask);
    if (retval != 0)
    {
        DBG_LOG("Failed to get CPU mask: %s\n", strerror(errno));
    }
    for (unsigned i = 0; i < sizeof(mask) / CPU_ALLOC_SIZE(1); i++)
    {
        descr += CPU_ISSET(i, &mask) ? "1" : "0";
    }
    while (descr.back() == '0') descr.pop_back(); // on Android, string will be very long otherwise
    DBG_LOG("Current CPU mask: %s\n", descr.c_str());
}

static void set_cpu_mask(const std::string& descr)
{
    cpu_set_t mask;
    CPU_ZERO(&mask);
    for (unsigned i = 0; i < descr.size(); i++)
    {
        if (descr.at(i) == '1')
        {
            CPU_SET(i, &mask);
        }
        else if (descr.at(i) != '0')
        {
            DBG_LOG("Invalid CPU mask: %s!\n", descr.c_str());
            return;
        }
    }
    int retval = sched_setaffinity(0, sizeof(mask), &mask);
    if (retval != 0)
    {
        DBG_LOG("Failed to set CPU mask: %s\n", strerror(errno));
    }
}

// Only one thread runs at a time, so no need for mutexing etc. except for when we go to sleep.
void Retracer::RetraceThread(const int threadidx, const int our_tid)
{
    std::unique_lock<std::mutex> lk(mConditionMutex);
    thread_result r;
    r.our_tid = our_tid;
    unsigned int skip_fence_range_index = 0;

    std::string thread_name = "patrace-" + _to_string(our_tid);
    set_thread_name(thread_name.c_str());

    while (!mFinish.load(std::memory_order_consume))
    {
        bool isSwapBuffers = swapvals.at(mCurCall.funcId);
        if (isSwapBuffers)
        {
            if (mState.pDrawableSet.count(mState.mThreadArr[our_tid].getDrawable())>0)
            {
                DBG_LOG("Skip PbufferSurface SwapBuffers\n");
                isSwapBuffers = false;
            }
        }

        if (mOptions.mSnapshotCallSet && (mOptions.mSnapshotCallSet->contains(mCurFrameNo, mFile.ExIdToName(mCurCall.funcId))) && isSwapBuffers)
        {
            TakeSnapshot(mFile.curCallNo - 1, mCurFrameNo);
        }

        if (fptr)
        {
            r.total++;
            if (isSwapBuffers)
            {
                // call glFinish() before eglSwapbuffers in every frame when mFinishBeforeSwap is true or in frame#0 of a FF trace
                if (mOptions.mFinishBeforeSwap || (mCurFrameNo == 0 && mFile.isFFTrace()))
                {
                    _glFinish();
                }

                if (mOptions.mSkipFence && ((mCurFrameNo + 1) > mOptions.mSkipFenceRanges[skip_fence_range_index].second))
                {
                    skip_fence_range_index += 1;

                    if (skip_fence_range_index >= mOptions.mSkipFenceRanges.size())
                    {
                        mOptions.mSkipFence = false;
                    }
                }
            }

            if (mOptions.mDebug > 1) DBG_LOG("    %s: t%d, c%d, f%d \n", mFile.ExIdToName(mCurCall.funcId), our_tid, mFile.curCallNo, mCurFrameNo);

            if (mOptions.mSkipFence && syncvals[mCurCall.funcId] && (mCurFrameNo >= mOptions.mSkipFenceRanges[skip_fence_range_index].first) && (mCurFrameNo <= mOptions.mSkipFenceRanges[skip_fence_range_index].second))
            {
                if (mOptions.mDebug) DBG_LOG("    FENCE SKIP : function name: %s (id: %d), call no: %d\n", mFile.ExIdToName(mCurCall.funcId), mCurCall.funcId, mFile.curCallNo);
            }
            else if (mOptions.mCallStats && mCurFrameNo >= mOptions.mBeginMeasureFrame && mCurFrameNo < mOptions.mEndMeasureFrame)
            {
                const char *funcName = mFile.ExIdToName(mCurCall.funcId);
                uint64_t pre = gettime();
                (*(RetraceFunc)fptr)(src);
                uint64_t post = gettime();
                mCallStats[funcName].count++;
                mCallStats[funcName].time += post - pre;
            }
            else if (!mOptions.mCacheOnly || cachevals[mCurCall.funcId])
            {
                (*(RetraceFunc)fptr)(src);
            }

            // Error Check
            if (mOptions.mDebug && hasCurrentContext())
            {
                CheckGlError();
            }
            r.swaps += (int)isSwapBuffers;
        }
        else if (mOptions.mDebug && mOptions.mRunAll)
        {
            DBG_LOG("    Unsupported function : %s, call no: %d\n", mFile.ExIdToName(mCurCall.funcId), mFile.curCallNo);
        }

        if (isSwapBuffers)
        {
            if (mOptions.mPerfStart == (int)mCurFrameNo) // before perf frame
            {
                PerfStart();
            }
            else if (mOptions.mPerfStop == (int)mCurFrameNo) // last frame
            {
                PerfEnd();
            }

            if (mOptions.mScriptCallSet && (mOptions.mScriptCallSet->contains(mCurFrameNo, mFile.ExIdToName(mCurCall.funcId))) && mOptions.mScriptPath.size() > 0)  // trigger script at the begining of specific frame
            {
                TriggerScript(mOptions.mScriptPath.c_str());
            }

            if (mOptions.mDebug)
            {
                const long pages = sysconf(_SC_AVPHYS_PAGES);
                const long page_size = sysconf(_SC_PAGE_SIZE);
                const long available = pages * page_size;
                struct rusage usage;
                getrusage(RUSAGE_SELF, &usage);
                long curr_rss = -1;
                FILE* fp = NULL;
                if ((fp = fopen( "/proc/self/statm", "r" )))
                {
                    if (fscanf(fp, "%*s%ld", &curr_rss) == 1)
                    {
                        curr_rss *= page_size;
                    }
                    fclose(fp);
                }
                const double f = 1024.0 * 1024.0;
                DBG_LOG("Frame %d memory (mb): %.02f max RSS, %.02f current RSS, %.02f available, %lu client side memory, %.02f loaded file data\n",
                        mCurFrameNo, (double)usage.ru_maxrss / 1024.0, (double)curr_rss / f, (double)available / f, (unsigned long)mCSBuffers.total_size(), (double)mFile.memoryUsed() / f);
            }

            const int secs = (os::getTime() - mTimerBeginTime) / os::timeFrequency;
            if (mCurFrameNo >= mOptions.mEndMeasureFrame && (mOptions.mLoopTimes > mLoopTimes || (mOptions.mLoopSeconds > 0 && secs < mOptions.mLoopSeconds)))
            {
                DBG_LOG("Executing rollback %d / %d times - %d / %d secs\n", mLoopTimes, mOptions.mLoopTimes, secs, mOptions.mLoopSeconds);
                if (mCollectors) mCollectors->summarize();
                mFile.rollback();
                unsigned numOfFrames = mCurFrameNo - mOptions.mBeginMeasureFrame;
                mCurFrameNo = mOptions.mBeginMeasureFrame;
                mFile.curCallNo = mRollbackCallNo;
                int64_t endTime;
                const float duration = getDuration(mLoopBeginTime, &endTime);
                const float fps = ((double)numOfFrames) / duration;
                mLoopResults.push_back(fps);
                mLoopBeginTime = os::getTime();
                LoadBuffersMaps();
                mLoopTimes++;
            }
        }
        else if (mOptions.mSnapshotCallSet && (mOptions.mSnapshotCallSet->contains(mFile.curCallNo, mFile.ExIdToName(mCurCall.funcId))))
        {
            TakeSnapshot(mFile.curCallNo, mCurFrameNo);
        }

        while (frameBudget <= 0 && drawBudget <= 0) // Step mode
        {
            frameBudget = 0;
            drawBudget = 0;
            StepShot(mFile.curCallNo, mCurFrameNo);
            GLWS::instance().processStepEvent(); // will wait here for user input to increase budgets
        }

        // ---------------------------------------------------------------------------
        // Get next call
skip_call:

        if (!mFile.GetNextCall(fptr, mCurCall, src))
        {
            mFinish.store(true);
            for (auto &cv : conditions) cv.notify_one(); // Wake up all other threads
            break;
        }
        // Skip call because it is on an ignored thread?
        if (!mOptions.mMultiThread && mCurCall.tid != mOptions.mRetraceTid)
        {
            r.skipped++;
            goto skip_call;
        }
        // Need to switch active thread?
        if (our_tid != mCurCall.tid)
        {
            latest_call_tid = mCurCall.tid; // need to use an atomic member copy of this here
            // Do we need to make this thread?
            if (thread_remapping.count(mCurCall.tid) == 0)
            {
                thread_remapping[mCurCall.tid] = threads.size();
                int newthreadidx = threads.size();
                conditions.emplace_back();
                results.emplace_back();
                threads.emplace_back(&Retracer::RetraceThread, this, (int)newthreadidx, (int)mCurCall.tid);
            }
            else // Wake up existing thread
            {
                const int otheridx = thread_remapping.at(mCurCall.tid);
                conditions.at(otheridx).notify_one();
            }
            r.handovers++;
            bool success = false;
            do {
                success = conditions.at(threadidx).wait_for(lk, std::chrono::milliseconds(50), [&]{ return our_tid == latest_call_tid || mFinish.load(std::memory_order_consume); });
                if (!success) r.timeouts++; else r.wakeups++;
            } while (!success);
        }
    }

    /* unbind the context and surface from current thread, thus eglTerminiate could clear up properly */
    if (gRetracer.mState.mThreadArr[our_tid].getDrawable() &&
        gRetracer.mState.mThreadArr[our_tid].getContext()) {
        _glFlush();
    }
    GLWS::instance().MakeCurrent(0, 0);
    gRetracer.mState.mThreadArr[our_tid].setDrawable(0);
    gRetracer.mState.mThreadArr[our_tid].setContext(0);

    results[threadidx] = r;
}

void Retracer::Retrace()
{
    if (!mOptions.mCpuMask.empty()) set_cpu_mask(mOptions.mCpuMask);
    report_cpu_mask();

    //pre-process of shader cache file if needed
    if (gRetracer.mOptions.mShaderCacheFile.size() > 0)
    {
        if (gRetracer.mOptions.mShaderCacheLoad)
            OpenShaderCacheFile();
        else
            DeleteShaderCacheFile();
    }

    mFile.setFrameRange(mOptions.mBeginMeasureFrame, mOptions.mEndMeasureFrame, mOptions.mMultiThread ? -1 : mOptions.mRetraceTid, mOptions.mPreload, mOptions.mLoopTimes != 0);

    mInitTime = os::getTime();
    mInitTimeMono = os::getTimeType(CLOCK_MONOTONIC);
    mInitTimeMonoRaw = os::getTimeType(CLOCK_MONOTONIC_RAW);
    mInitTimeBoot = os::getTimeType(CLOCK_BOOTTIME);

    swapvals.resize(mFile.getMaxSigId() + 1);
    swapvals[mFile.NameToExId("eglSwapBuffers")] = true;
    swapvals[mFile.NameToExId("eglSwapBuffersWithDamageKHR")] = true;
    swapvals[mFile.NameToExId("eglSwapBuffersWithDamageEXT")] = true;
    cachevals.resize(mFile.getMaxSigId() + 1);
    for (const auto& s : mFile.getFuncNames())
    {
        if (s.find("Uniform") != string::npos || s.find("Attrib") != string::npos || s.find("Shader") != string::npos || s.find("Program") != string::npos || (s[0] == 'e' && s[1] == 'g' && s[2] == 'l')
            || s.find("Feedback") != string::npos || s.find("Buffer") != string::npos)
        {
            cachevals[mFile.NameToExId(s.c_str())] = true;
        }
    }
    syncvals.resize(mFile.getMaxSigId() + 1);
    syncvals[mFile.NameToExId("eglClientWaitSync")] = true;
    syncvals[mFile.NameToExId("eglClientWaitSyncKHR")] = true;
    syncvals[mFile.NameToExId("eglWaitSync")] = true;
    syncvals[mFile.NameToExId("eglWaitSyncKHR")] = true;
    syncvals[mFile.NameToExId("glWaitSync")] = true;
    syncvals[mFile.NameToExId("glClientWaitSync")] = true;

    if (mOptions.mScriptCallSet && mOptions.mScriptCallSet->contains(0, "eglSwapBuffers") && mOptions.mScriptPath.size() > 0)
    {
        // Trigger Script before frame 0
        TriggerScript(mOptions.mScriptPath.c_str());
    }

    if (mOptions.mBeginMeasureFrame == 0 && mCurFrameNo == 0)
    {
        StartMeasuring(); // special case, otherwise triggered by eglSwapBuffers()
        delayedPerfmonInit = true;
    }

    // Get first packet on a relevant thread
    do
    {
        if (!mFile.GetNextCall(fptr, mCurCall, src) || mFinish.load(std::memory_order_consume))
        {
            reportAndAbort("Empty trace file!");
        }
    } while (!mOptions.mMultiThread && mCurCall.tid != mOptions.mRetraceTid);
    threads.resize(1);
    conditions.resize(1);
    results.resize(1);
    thread_remapping[mCurCall.tid] = 0;
    results[0].our_tid = mCurCall.tid;
    if (mOptions.mFixedFps != 0)
    {
        mMaxDuration = 1.0/mOptions.mFixedFps;
        mFixedFpsOldTime = os::getTime();
    }
    RetraceThread(0, mCurCall.tid); // run the first thread on this thread

    for (std::thread &t : threads)
    {
        if (t.joinable()) t.join();
    }

    // When we get here, we're all done
    if (mOptions.mForceOffscreen)
    {
        forceRenderMosaicToScreen();
    }

    if (GetGLESVersion() > 1)
    {
        GLWS::instance().MakeCurrent(gRetracer.mState.mThreadArr[gRetracer.getCurTid()].getDrawable(),
                                     gRetracer.mState.mThreadArr[gRetracer.getCurTid()].getContext());
        _glFinish();
    }
}

void Retracer::CheckGlError()
{
    GLenum error = glGetError();
    if (error == GL_NO_ERROR)
    {
        return;
    }
    DBG_LOG("[%d] %s  ERR: %d \n", GetCurCallId(), GetCurCallName(), error);
}

void Retracer::OnFrameComplete()
{
    if (getCurTid() == mOptions.mRetraceTid || mOptions.mMultiThread)
    {
        // Per frame measurement
        if (mOptions.mMeasureSwapTime && mEndFrameTime)
        {
            getDuration(mEndFrameTime, &mFinishSwapTime);
        }
    }
}

void Retracer::StartMeasuring()
{
    if (mOptions.mCollectorEnabled)
    {
#ifdef ENABLE_PERFPERAPI
        if (mOptions.mPerfPerApi)
        {
            DBG_LOG("Per GLES API perf counter instrumentation enabled. Retracing may took longer time.\n");
            mCollectors = new Collection(mOptions.mCollectorValue, true);
        }
        else
        {
            DBG_LOG("libcollector enabled.\n");
            mCollectors = new Collection(mOptions.mCollectorValue, false);
        }
#else
        DBG_LOG("libcollector enabled.\n");
        mCollectors = new Collection(mOptions.mCollectorValue, false);
#endif
        mOptions.mCollectorEnabled = false;
        mCollectors->initialize();
        mCollectors->start();
    }
    if (mOptions.mPerfmon)
    {
        mHWCPipeHandler = new HWCPipeHandler;
        mHWCPipeHandler->hwcpipe_init(mOptions.mPerfmonOut);
    }
    mRollbackCallNo = mFile.curCallNo;
    DBG_LOG("================== Start timer (Frame: %u) ==================\n", mCurFrameNo);
    mTimerBeginTime = mLoopBeginTime = os::getTime();
    mTimerBeginTimeMono = os::getTimeType(CLOCK_MONOTONIC);
    mTimerBeginTimeMonoRaw = os::getTimeType(CLOCK_MONOTONIC_RAW);
    mTimerBeginTimeBoot = os::getTimeType(CLOCK_BOOTTIME);
    mEndFrameTime = mTimerBeginTime;
}

void Retracer::SaveBuffersMaps()
{
    Context& context = gRetracer.getCurrentContext();
    for (const auto &pair : context.getBufferMap().GetCopy())
    {
        mBufferMapCheckpoint.insert(pair.first);
    }

    mCSBCheckpoint = mCSBuffers;
}

void Retracer::LoadBuffersMaps()
{
    if (mLoopTimes == 0)
    {
        Context& context = gRetracer.getCurrentContext();
        auto tmpBufferMap = context.getBufferMap().GetCopy();
        for (const auto &iter : tmpBufferMap)
        {
            if (mBufferMapCheckpoint.find(iter.first)==mBufferMapCheckpoint.end())
            {
                buffer_to_del.push_back(iter.first);
            }
        }
    }
    hardcode_glDeleteBuffers(buffer_to_del.size(), buffer_to_del.data());

    mCSBuffers.clear();
    mCSBuffers = mCSBCheckpoint;
}

void Retracer::OnNewFrame()
{
    if (getCurTid() == mOptions.mRetraceTid || mOptions.mMultiThread)
    {
        IncCurFrameId();

        if (mCurFrameNo == mOptions.mBeginMeasureFrame)
        {
            if (mOptions.mLoopTimes>0 && mLoopTimes==0)
            {
                SaveBuffersMaps();
            }
            if (mOptions.mFlushWork)
            {
                // First try to flush all the work we can
                _glFlush(); // force all GPU work to complete before this point
                const auto programs = gRetracer.getCurrentContext().getProgramMap().GetCopy();
                for (const auto program : programs) // force all compiler work to complete before this point
                {
                    GLint size = 0;
                    _glGetProgramiv(program.second, GL_PROGRAM_BINARY_LENGTH, &size);
                }
                sync(); // force all pending output to disk before this point
            }
            StartMeasuring();
        }
        // Per frame measurement
        if (mCurFrameNo > mOptions.mBeginMeasureFrame && mCurFrameNo <= mOptions.mEndMeasureFrame)
        {
            if (mOptions.mInstrumentationDelay > 0) {
                usleep(mOptions.mInstrumentationDelay);
            }
#ifdef ENABLE_PERFPERAPI
            if (mCollectors && !mOptions.mPerfPerApi) mCollectors->collect();
#else
            if (mCollectors) mCollectors->collect();
            if (mOptions.mPerfmon) mHWCPipeHandler->sample_counters();
#endif
        }
        if (mOptions.mFixedFps != 0) //Limited fps replay mode
        {
            int64_t curr_time;
            double duration = getDuration(mFixedFpsOldTime, &curr_time);
            double time_need_to_wait = (mMaxDuration-duration)*1000000.0 + mLegacyTime;
            if (time_need_to_wait >= 0)
            {
                usleep(time_need_to_wait);
                mLegacyTime = 0;
            }
            else mLegacyTime = time_need_to_wait;
            mFixedFpsOldTime = os::getTime();
        }
    }
}

void Retracer::TriggerScript(const char* scriptPath)
{
    char cmd[256];
    memset(cmd, 0, sizeof(cmd));
#ifdef ANDROID
    sprintf(cmd, "/system/bin/sh %s", scriptPath);
#else
    sprintf(cmd, "/bin/sh %s", scriptPath);
#endif

    int ret = system(cmd);

    DBG_LOG("Trigger script %s run result: %d\n", cmd, ret);
}

void Retracer::PerfStart()
{
    pid_t parent = getpid();
    child = fork();
    if (child == -1)
    {
        DBG_LOG("Failed to fork: %s\n", strerror(errno));
    }
    else if (child == 0)
    {
        if (mOptions.mPerfCmd != "")
        {
            bool isFreqdefined = false;
            const char* args[20];
            args[0] = mOptions.mPerfPath.c_str();
            DBG_LOG("Perf tracing %ld from process %ld with input cmd %s\n", (long)parent, (long)getpid(), mOptions.mPerfCmd.c_str());
            DBG_LOG("Perf path: %s\n", mOptions.mPerfPath.c_str());
            int i=1;
            args[i] = strtok(const_cast<char *>(mOptions.mPerfCmd.c_str())," ");
            while (args[i] != NULL)
            {
#ifdef ANDROID
                if (strcmp(args[i],"-f") == 0) isFreqdefined = true;
#else
                if (strncmp(args[i],"--freq",6) == 0)  isFreqdefined = true;
#endif
                i++;
                args[i] = strtok(NULL, " ");
            }
            std::string defaultargs;
#ifdef ANDROID
            std::string mypid = _to_string(parent);
            args[i++] = "-p";
            args[i++] = mypid.c_str();
            defaultargs = "-p " + mypid;
            if (!isFreqdefined)
            {
                std::string freqopt = _to_string(mOptions.mPerfFreq);
                args[i++] = "-f";
                args[i++] = freqopt.c_str();
                defaultargs += " -f " + freqopt;
            }
#else
            std::string mypid = "--pid=" + _to_string(parent);
            args[i++] = mypid.c_str();
            defaultargs = mypid;
            if (!isFreqdefined)
            {
                std::string freqopt = "--freq=" + _to_string(mOptions.mPerfFreq);
                args[i++] = freqopt.c_str();
                defaultargs += " "+ freqopt;
            }
#endif
            args[i] = nullptr;
            DBG_LOG("Perf default options: %s\n", defaultargs.c_str());
            if (execv(args[0], (char* const*)args) == -1)
            {
                DBG_LOG("Failed execv() for perf: %s\n", strerror(errno));
            }
        }
        else
        {
#ifdef ANDROID
            std::string freqopt = _to_string(mOptions.mPerfFreq);
            std::string mypid = _to_string(parent);
            DBG_LOG("Perf tracing %ld from process %ld with freq %ld and output in %s\n", (long)parent, (long)getpid(), (long)mOptions.mPerfFreq, mOptions.mPerfOut.c_str());
            const char* args[12] = { mOptions.mPerfPath.c_str(), "record", "-g", "-f", freqopt.c_str(), "-p", mypid.c_str(), "-o", mOptions.mPerfOut.c_str(), "-e", mOptions.mPerfEvent.c_str(), nullptr };
            if (mOptions.mPerfEvent == "")
            {
                args[9] = nullptr;
                args[10] = nullptr;
                DBG_LOG("Perf args: %s %s %s %s %s %s %s %s %s\n", args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8]);
            }
            else
            {
                DBG_LOG("Perf args: %s %s %s %s %s %s %s %s %s %s %s\n", args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10]);
            }
#else
            std::string freqopt = "--freq=" + _to_string(mOptions.mPerfFreq);
            std::string mypid = "--pid=" + _to_string(parent);
            std::string myfilename = mOptions.mPerfOut;
            std::string myfilearg = "--output=" + myfilename;
            DBG_LOG("Perf tracing %ld from process %ld with freq %ld and output in %s\n", (long)parent, (long)getpid(), (long)mOptions.mPerfFreq, myfilename.c_str());
            const char* args[9] = { mOptions.mPerfPath.c_str(), "record", "-g", freqopt.c_str(), mypid.c_str(), myfilearg.c_str(), "-e", mOptions.mPerfEvent.c_str(), nullptr };
            if (mOptions.mPerfEvent == "")
            {
                args[6] = nullptr;
                args[7] = nullptr;
                DBG_LOG("Perf args: %s %s %s %s %s %s\n", args[0], args[1], args[2], args[3], args[4], args[5]);
            }
            else
            {
                DBG_LOG("Perf args: %s %s %s %s %s %s %s %s\n", args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]);
            }
#endif
            if (execv(args[0], (char* const*)args) == -1)
            {
                DBG_LOG("Failed execv() for perf: %s\n", strerror(errno));
            }
        }
    }
    else
    {
        sleep(1); // nasty hack needed because otherwise we could sometimes finish before the child process could get started
    }
}

void Retracer::PerfEnd()
{
    if(child == -1)
        return;
    DBG_LOG("Killing instrumented process %ld\n", (long)child);
    if (kill(child, SIGINT) == -1)
    {
        DBG_LOG("Failed to send SIGINT to perf process: %s\n", strerror(errno));
    }
    usleep(200); // give it some time to exit, before proceeding
}

void Retracer::DiscardFramebuffers()
{
    GLint v = 0;
    _glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &v);
    if (v != 0)
    {
        static std::vector<GLenum> attachments = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3,
                                                   GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT };
        _glDiscardFramebufferEXT(GL_FRAMEBUFFER, attachments.size(), attachments.data());
    }
    else
    {
        static std::vector<GLenum> attachments = { GL_COLOR_EXT, GL_DEPTH_EXT, GL_STENCIL_EXT };
        _glDiscardFramebufferEXT(GL_FRAMEBUFFER, attachments.size(), attachments.data());
    }
}

#ifdef ENABLE_PERFPERAPI
void Retracer::CollectPerapiStart(uint16_t func_id, int tid) {
    mCollectors->collect_scope_start(func_id, COLLECT_REPLAY_THREADS, tid);
}

void Retracer::CollectPerapiStop(uint16_t func_id, int tid) {
    mCollectors->collect_scope_stop(func_id, COLLECT_REPLAY_THREADS, tid);
}
#endif

void Retracer::reportAndAbort(const char *format, ...)
{
    char buf[256];
    va_list ap;
    memset(buf, 0, sizeof(buf));
    sprintf(buf, "[c%u,f%u] ", GetCurCallId(), GetCurFrameId());
    va_start(ap, format);
    const unsigned len = strlen(buf);
    vsnprintf(buf + len, sizeof(buf) - len - 1, format, ap);
    va_end(ap);
    TraceExecutor::writeError(buf);
#ifdef __APPLE__
     throw PA_EXCEPTION(buf);
#elif ANDROID
    sleep(1); // give log call a chance before world dies below...
    ::exit(0); // too many failure calls will cause Android to hate us and blacklist our app until we uninstall it
#else
    ::abort();
#endif
}

void Retracer::saveResult(Json::Value& result)
{
    int64_t endTime;
    int64_t endTimeMono = os::getTimeType(CLOCK_MONOTONIC);
    int64_t endTimeMonoRaw = os::getTimeType(CLOCK_MONOTONIC_RAW);
    int64_t endTimeBoot = os::getTimeType(CLOCK_BOOTTIME);
    float duration = getDuration(mTimerBeginTime, &endTime);
    unsigned int numOfFrames = mCurFrameNo - mOptions.mBeginMeasureFrame;

    if(mTimerBeginTime != 0) {
        if (mLegacyTime < 0) DBG_LOG("FPS requird is too high! \n");
        const float fps = ((double)numOfFrames * std::max(1, mLoopTimes)) / duration;
        const float loopDuration = getDuration(mLoopBeginTime, &endTime);
        const float loopFps = ((double)numOfFrames) / loopDuration;
        DBG_LOG("================== End timer (Frame: %u) ==================\n", mCurFrameNo);
        DBG_LOG("Duration = %f\n", duration);
        DBG_LOG("Frame cnt = %d, FPS = %f\n", numOfFrames, fps);
        result["fps"] = fps;
        mLoopResults.push_back(loopFps);
    } else {
        DBG_LOG("Never rendered anything.\n");
        numOfFrames = 0;
        duration = 0;
        result["fps"] = 0;
    }

    result["loopFPS"] = Json::arrayValue;
    for (const auto fps : mLoopResults) result["loopFPS"].append(fps);
    result["time"] = duration;
    result["frames"] = numOfFrames;
    result["init_time"] = ((double)mInitTime) / os::timeFrequency;
    result["start_time"] = ((double)mTimerBeginTime) / os::timeFrequency;
    result["end_time"] = ((double)endTime) / os::timeFrequency;
    result["start_frame"] = mOptions.mBeginMeasureFrame;
    result["end_frame"] = mOptions.mEndMeasureFrame;
    result["init_time_monotonic"] = ((double)mInitTimeMono) / os::timeFrequency;
    result["start_time_monotonic"] = ((double)mTimerBeginTimeMono) / os::timeFrequency;
    result["end_time_monotonic"] = ((double)endTimeMono) / os::timeFrequency;
    result["init_time_monotonic_raw"] = ((double)mInitTimeMonoRaw) / os::timeFrequency;
    result["start_time_monotonic_raw"] = ((double)mTimerBeginTimeMonoRaw) / os::timeFrequency;
    result["end_time_monotonic_raw"] = ((double)endTimeMonoRaw) / os::timeFrequency;
    result["init_time_boot"] = ((double)mInitTimeBoot) / os::timeFrequency;
    result["start_time_boot"] = ((double)mTimerBeginTimeBoot) / os::timeFrequency;
    result["end_time_boot"] = ((double)endTimeBoot) / os::timeFrequency;
    result["patrace_version"] = PATRACE_VERSION;

    if (mOptions.mPerfmon)
    {
        mHWCPipeHandler->sample_stop();
    }
    if (mCollectors)
    {
        mCollectors->stop();
    }

    if (mOptions.mCallStats)
    {
        // First generate some info on no-op calls as a baseline
        int c = 0;
        for (int i = 0; i < 1000; i++)
        {
            auto pre = gettime();
            c = noop(c);
            auto post = gettime();
            mCallStats["NO-OP"].count++;
            mCallStats["NO-OP"].time += post - pre;
            usleep(c); // just to use c for something, to make 100% sure it is not optimized away
        }
#if ANDROID
        const char *filename = "/sdcard/callstats.csv";
#else
        const char *filename = "callstats.csv";
#endif
        FILE *fp = fopen(filename, "w");
        if (fp)
        {
            uint64_t total = 0;
            float noop_avg = (float)mCallStats["NO-OP"].time / mCallStats["NO-OP"].count;
            fprintf(fp, "Function,Calls,Time,Calibrated_Time\n");
            for (const auto& pair : mCallStats)
            {
                uint64_t noop = (uint64_t)(pair.second.count*noop_avg);
                uint64_t calibrated_time = (pair.second.time > noop) ? (pair.second.time - noop) : 0;
                fprintf(fp, "%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n", pair.first.c_str(), pair.second.count, pair.second.time, calibrated_time);
                // exclude APIs introduced from patrace
                if (strcmp(pair.first.c_str(), "glClientSideBufferData")   && strcmp(pair.first.c_str(), "glClientSideBufferSubData") &&
                    strcmp(pair.first.c_str(), "glCreateClientSideBuffer") && strcmp(pair.first.c_str(), "glDeleteClientSideBuffer") &&
                    strcmp(pair.first.c_str(), "glCopyClientSideBuffer")   && strcmp(pair.first.c_str(), "glPatchClientSideBuffer") &&
                    strcmp(pair.first.c_str(), "glGenGraphicBuffer_ARM")   && strcmp(pair.first.c_str(), "glGraphicBufferData_ARM") &&
                    strcmp(pair.first.c_str(), "glDeleteGraphicBuffer_ARM")&& strcmp(pair.first.c_str(), "NO-OP"))
                    total += calibrated_time;
            }
            fsync(fileno(fp));
            fclose(fp);

            const float ddk_fps = ((float)numOfFrames * std::max(1, mLoopTimes)) / ticksToSeconds(total);
            const float ddk_mspf = (1000 * ticksToSeconds(total)) / (float)numOfFrames;
            result["fps_ddk"] = ddk_fps;
            result["ms/frame_ddk"] = ddk_mspf;
            DBG_LOG("DDK FPS = %f, ms/frame = %f\n", ddk_fps, ddk_mspf);
            DBG_LOG("Writing callstats to %s\n", filename);
        }
        else
        {
            DBG_LOG("Failed to open output callstats in %s: %s\n", filename, strerror(errno));
        }
     }

    DBG_LOG("Saving results...\n");
    if (!TraceExecutor::writeData(result, numOfFrames, duration))
    {
        reportAndAbort("Error writing result file!");
    }
    mLoopResults.clear();
    TraceExecutor::clearResult();

    if (mOptions.mDebug)
    {
        for (unsigned threadidx = 0; threadidx < results.size(); threadidx++)
        {
            thread_result& r = results.at(threadidx);
            DBG_LOG("Thread %d (%d):\n", threadidx, r.our_tid);
            DBG_LOG("\tTotal calls: %d\n", r.total);
            DBG_LOG("\tSkipped calls: %d\n", r.skipped);
            DBG_LOG("\tSwapbuffer calls: %d\n", r.swaps);
            DBG_LOG("\tHandovers: %d\n", r.handovers);
            DBG_LOG("\tWakeups: %d\n", r.wakeups);
            DBG_LOG("\tTimeouts: %d\n", r.timeouts);
        }
    }

    GLWS::instance().Cleanup();
    CloseTraceFile();
#if ANDROID
    if (!mOptions.mForceSingleWindow)
    {
        // Figure out if we want to intercept paretracer itself.
        const char* appListPath = findFirst(applist_cfg_search_paths);
        ifstream appListIfs(appListPath);
        if (!appListIfs.is_open())  // no appList.cfg, user doesn't want to trace paretrace
        {
            return;
        }
        pid_t pid = getpid();
        std::string itAppName;
        std::string pname = getProcessNameByPid(getpid());
        while (appListIfs >> itAppName)
        {
            if (!strcmp(itAppName.c_str(), pname.c_str()))    // user wants to trace paretrace
            {
                kill(pid, SIGTERM);
                break;
            }
        }
    }
#endif
}

void pre_glDraw()
{
    if (!gRetracer.mOptions.mDoOverrideResolution)
    {
        return;
    }

    Context& context = gRetracer.getCurrentContext();

    GLuint curFB = context._current_framebuffer;
    Rectangle& curAppVP = gRetracer.mState.mThreadArr[gRetracer.getCurTid()].mCurAppVP;
    Rectangle& curAppSR = gRetracer.mState.mThreadArr[gRetracer.getCurTid()].mCurAppSR;
    Rectangle& curDrvVP = gRetracer.mState.mThreadArr[gRetracer.getCurTid()].mCurDrvVP;
    Rectangle& curDrvSR = gRetracer.mState.mThreadArr[gRetracer.getCurTid()].mCurDrvSR;

    retracer::Drawable * curDrawable = gRetracer.mState.mThreadArr[gRetracer.getCurTid()].getDrawable();

#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
    if (curFB > 1)
#else
    if (curFB != 0)
#endif
    {
        // a framebuffer object is bound
        // only call glViewport and glScissor if appVP is different from drvVp

        if (curAppVP != curDrvVP) {
            curDrvVP = curAppVP;
            glViewport(curDrvVP.x, curDrvVP.y, curDrvVP.w, curDrvVP.h);
        }
        if (curAppSR != curDrvSR) {
            curDrvSR = curAppSR;
            glScissor(curDrvSR.x, curDrvSR.y, curDrvSR.w, curDrvSR.h);
        }
    }
    else
    {
        // on-screen framebuffer is "bound"

        if (curDrvVP != curAppVP.Stretch(curDrawable->mOverrideResRatioW, curDrawable->mOverrideResRatioH)) {
            curDrvVP = curAppVP.Stretch(curDrawable->mOverrideResRatioW, curDrawable->mOverrideResRatioH);
            glViewport(curDrvVP.x, curDrvVP.y, curDrvVP.w, curDrvVP.h);
        }
        if (curDrvSR != curAppSR.Stretch(curDrawable->mOverrideResRatioW, curDrawable->mOverrideResRatioH)) {
            curDrvSR = curAppSR.Stretch(curDrawable->mOverrideResRatioW, curDrawable->mOverrideResRatioH);
            glScissor(curDrvSR.x, curDrvSR.y, curDrvSR.w, curDrvSR.h);
        }
    }
}

void post_glCompileShader(GLuint shader, GLuint originalShaderName)
{
    GLint rvalue;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &rvalue);
    if (rvalue == GL_FALSE)
    {
        GLint maxLength = 0, len = -1;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &maxLength);
        char *infoLog = NULL;
        if (maxLength > 0)
        {
            infoLog = (char *)malloc(maxLength);
            glGetShaderInfoLog(shader, maxLength, &len, infoLog);
        }
        DBG_LOG("Error in compiling shader %u: %s\n", originalShaderName, infoLog ? infoLog : "(n/a)");
        free(infoLog);
    }
}

void post_glShaderSource(GLuint shader, GLuint originalShaderName, GLsizei count, const GLchar **string, const GLint *length)
{
    if (gRetracer.mOptions.mShaderCacheFile.size() > 0 && string && count)
    {
        std::string cat;
        for (int i = 0; i < count; i++)
        {
            if (length) cat += std::string(string[i], length[i]);
            else cat += string[i];
        }
        gRetracer.getCurrentContext().setShaderSource(shader, cat);
    }
}

void DeleteShaderCacheFile()
{
    const std::string bpath = gRetracer.mOptions.mShaderCacheFile + ".bin";
    remove(bpath.c_str());
}

void SaveCacheToFile(std::map<std::vector<uint8_t>, std::vector<uint8_t>>& gApplicationCache)
{
    const std::string bpath = gRetracer.mOptions.mBlobShaderCacheFile + ".bin";
    std::ofstream file(bpath.c_str(), std::ios::binary);
    if (file.is_open())
    {
        for (const auto& entry : gApplicationCache)
        {
            EGLsizeiANDROID keySize = entry.first.size();
            EGLsizeiANDROID valueSize = entry.second.size();
            file.write(reinterpret_cast<const char*>(&keySize), sizeof(keySize));
            file.write(reinterpret_cast<const char*>(entry.first.data()), keySize);
            file.write(reinterpret_cast<const char*>(&valueSize), sizeof(valueSize));
            file.write(reinterpret_cast<const char*>(entry.second.data()), valueSize);
        }
        file.close();
        DBG_LOG("Successfully save shader cache file: %s\n", bpath.c_str());
    }
    else
    {
        DBG_LOG("Failed to save shader cache file: %s\n", bpath.c_str());
    }
}

void LoadCacheFromFile(std::map<std::vector<uint8_t>, std::vector<uint8_t>>& gApplicationCache)
{
    const std::string bpath = gRetracer.mOptions.mBlobShaderCacheFile + ".bin";
    std::ifstream file(bpath.c_str(), std::ios::binary);
    if (file.is_open())
    {
        while (file)
        {
            EGLsizeiANDROID keySize;
            file.read(reinterpret_cast<char*>(&keySize), sizeof(keySize));
            if (file.eof()) break;

            std::vector<uint8_t> keyVec(keySize);
            file.read(reinterpret_cast<char*>(keyVec.data()), keySize);

            EGLsizeiANDROID valueSize;
            file.read(reinterpret_cast<char*>(&valueSize), sizeof(valueSize));

            std::vector<uint8_t> valueVec(valueSize);
            file.read(reinterpret_cast<char*>(valueVec.data()), valueSize);

            gApplicationCache[keyVec] = valueVec;
        }
        file.close();
        DBG_LOG("Successfully load shader cache file: %s\n", bpath.c_str());
    }
    else
    {
        DBG_LOG("Failed to load shader cache file: %s\n", bpath.c_str());
    }
}

void OpenShaderCacheFile()
{
        const std::string bpath = gRetracer.mOptions.mShaderCacheFile + ".bin";
        gRetracer.shaderCacheFile = fopen(bpath.c_str(), "rb");
        if (!gRetracer.shaderCacheFile)
        {
            gRetracer.reportAndAbort("Failed to open shader cache file %s: %s", bpath.c_str(), strerror(errno));
        }

        const std::string ipath = gRetracer.mOptions.mShaderCacheFile + ".idx";
        FILE *idx = fopen(ipath.c_str(), "rb");
        if (idx)
        {
            std::vector<char> ver_md5;
            ver_md5.resize(MD5Digest::DIGEST_LEN * 2);
            int r = fread(ver_md5.data(), ver_md5.size(), 1, idx);
            if (r != 1)
            {
                gRetracer.reportAndAbort("Failed to read shader cache version: %s", ipath.c_str(), strerror(ferror(idx)));
            }
            gRetracer.shaderCacheVersionMD5 = std::string(ver_md5.data(), ver_md5.size());

            uint32_t size = 0;
            r = fread(&size, sizeof(size), 1, idx);
            if (r != 1)
            {
                gRetracer.reportAndAbort("Failed to read shader cache index size: %s", ipath.c_str(), strerror(ferror(idx)));
            }
            for (unsigned i = 0; i < size; i++)
            {
                std::vector<char> md5;
                uint64_t offset = 0;
                std::string md5_str;
                md5.resize(MD5Digest::DIGEST_LEN * 2);
                r = fread(md5.data(), md5.size(), 1, idx);
                r += fread(&offset, sizeof(offset), 1, idx);
                if (r != 2)
                {
                    gRetracer.reportAndAbort("Failed to read shader cache index: %s", ipath.c_str(), strerror(ferror(idx)));
                }
                md5_str = std::string(md5.data(), md5.size());
                gRetracer.shaderCacheIndex[md5_str] = offset;

                if (offset == UINT64_MAX) continue;  // skipped shadercacheIndex

                GLenum binaryFormat = GL_NONE;
                uint32_t size = 0;

                if (fseek(gRetracer.shaderCacheFile, offset, SEEK_SET) != 0)
                {
                    gRetracer.reportAndAbort("Could not seek to desired cache item at %ld", offset);
                }
                if (fread(&binaryFormat, sizeof(binaryFormat), 1, gRetracer.shaderCacheFile) != 1 || fread(&size, sizeof(size), 1, gRetracer.shaderCacheFile) != 1)
                {
                    gRetracer.reportAndAbort("Failed to read data from cache at %ld as %s: %s", offset, md5_str.c_str(), strerror(ferror(gRetracer.shaderCacheFile)));
                }
                if (binaryFormat == GL_NONE || size == 0)
                {
                    gRetracer.reportAndAbort("Invalid cache metadata at %ld for %s", offset, md5_str.c_str());
                }

                gRetracer.shaderCache[md5_str].format = binaryFormat;
                gRetracer.shaderCache[md5_str].buffer.resize(size);
                if (fread(gRetracer.shaderCache[md5_str].buffer.data(), gRetracer.shaderCache[md5_str].buffer.size(), 1, gRetracer.shaderCacheFile) != 1)
                {
                    gRetracer.reportAndAbort("Failed to read %d bytes of data from cache as %s: %s", size, md5_str.c_str(), strerror(ferror(gRetracer.shaderCacheFile)));
                }
            }
            fclose(idx);
            DBG_LOG("Found shader cache index file, loaded %d cache entries\n", (int)size);
        }
}

bool load_from_shadercache(GLuint program, GLuint originalProgramName, int status)
{
    assert(gRetracer.mOptions.mShaderCacheLoad);

    // check this particular shader
    std::vector<std::string> shaders;
    for (const GLuint shader_id : gRetracer.getCurrentContext().getShaderIDs(program))
    {
        shaders.push_back(gRetracer.getCurrentContext().getShaderSource(shader_id));
    }

    MD5Digest cached_md5(shaders);
    const std::string md5 = cached_md5.text();
    if (gRetracer.shaderCacheIndex.count(md5) == 0)
    {
        gRetracer.reportAndAbort("Could not find shader %s in cache!", md5.c_str());
    }

    if (gRetracer.shaderCacheIndex[md5] == UINT64_MAX)
    {
        if (gRetracer.mOptions.mDebug)
        {
            DBG_LOG("warning: skip load_from_shadercache for program %u because of error linking: status %d\n", originalProgramName, status);
        }
        return false;
    }
    _glGetError(); // clear
    _glProgramBinary(program, gRetracer.shaderCache[md5].format, gRetracer.shaderCache[md5].buffer.data(), gRetracer.shaderCache[md5].buffer.size());
    GLenum err = _glGetError();
    if (err != GL_NO_ERROR)
    {
        gRetracer.reportAndAbort("Failed to upload shader %s from cache for program %u(retraceProgram %u)!", md5.c_str(), originalProgramName, program);
    }
    if (gRetracer.mOptions.mDebug)
    {
        DBG_LOG("Loaded program %u from cache as %s.\n", originalProgramName, md5.c_str());
    }
    return true;
}

static void save_shadercache(GLuint program, GLuint originalProgramName, bool bSkipShadercache)
{
    std::vector<std::string> shaders;
    for (const GLuint shader_id : gRetracer.getCurrentContext().getShaderIDs(program))
    {
        shaders.push_back(gRetracer.getCurrentContext().getShaderSource(shader_id));
    }
    MD5Digest cached_md5(shaders);
    if (gRetracer.shaderCacheIndex.count(cached_md5.text()) == 0)
    {
        if (!bSkipShadercache)
        {
            // save and write binary to disk
            GLint len = 0;
            _glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &len);

            std::vector<char> buffer(len);
            GLenum binaryFormat = GL_NONE;
            _glGetProgramBinary(program, len, NULL, &binaryFormat, (void*)buffer.data());

            const std::string bpath = gRetracer.mOptions.mShaderCacheFile + ".bin";
            FILE* fp = fopen(bpath.c_str(), "ab+");
            if (!fp) gRetracer.reportAndAbort("Failed to open shader cache file %s for writing: %s", bpath.c_str(), strerror(errno));
            uint32_t size = len;
            (void)fseek(fp, 0, SEEK_END);
            long offset = ftell(fp);
            if (fwrite(&binaryFormat, sizeof(GLenum), 1, fp) != 1 || fwrite(&size, sizeof(size), 1, fp) != 1 || fwrite(buffer.data(), buffer.size(), 1, fp) != 1)
            {
                gRetracer.reportAndAbort("Failed to write data to shader cache file: %s", strerror(ferror(gRetracer.shaderCacheFile)));
            }
            fclose(fp);

            gRetracer.shaderCacheIndex[cached_md5.text()] = offset;
            if (gRetracer.mOptions.mDebug)
            {
                DBG_LOG("Saving program %u(retraceProgram %u) to shader cache as %s{.idx|.bin} with offset=%ld size=%ld md5=%s\n", originalProgramName, program, gRetracer.mOptions.mShaderCacheFile.c_str(), offset, (long)size, cached_md5.text().c_str());
            }
        }
        else
        {
            gRetracer.shaderCacheIndex[cached_md5.text()] = UINT64_MAX;
        }
        // Overwrite index on disk
        const std::string ipath = gRetracer.mOptions.mShaderCacheFile + ".idx";
        FILE *fp = fopen(ipath.c_str(), "wb");
        if (!fp)
        {
            gRetracer.reportAndAbort("Failed to open index file %s for writing: %s", ipath.c_str(), strerror(errno));
        }
        if (fwrite(gRetracer.shaderCacheVersionMD5.c_str(), gRetracer.shaderCacheVersionMD5.size(), 1, fp ) != 1)
        {
            gRetracer.reportAndAbort("Failed to write ddk version MD5 (%s) to shader cache index: %s", gRetracer.shaderCacheVersionMD5.c_str(), strerror(ferror(fp)));
        }

        uint32_t entries = gRetracer.shaderCacheIndex.size();
        if (fwrite(&entries, sizeof(entries), 1, fp) != 1)
        {
            gRetracer.reportAndAbort("Failed to write size of data to shader cache index: %s", strerror(ferror(fp)));
        }
        for (const auto &pair : gRetracer.shaderCacheIndex)
        {
            if (fwrite(pair.first.c_str(), pair.first.size(), 1, fp) != 1 || fwrite(&pair.second, sizeof(pair.second), 1, fp) != 1)
            {
                gRetracer.reportAndAbort("Failed to write data to shader cache index: %s", strerror(ferror(fp)));
            }
        }
        fclose(fp);
    }
}

void post_glLinkProgram(GLuint program, GLuint originalProgramName, int status)
{
    bool bSkipShadercache = false;
    GLint linkStatus;
    _glGetProgramiv(program, GL_LINK_STATUS, &linkStatus);
    if (linkStatus == GL_TRUE && status == 0)
    {
        // A bit of an odd case: Shader failed during tracing, but succeeds during replay. This can absolutely happen without being an
        // error (eg because feature checks can resolve differently on different platforms), but interesting information when debugging.
        if (gRetracer.mOptions.mDebug) DBG_LOG("Linking program id %u (retrace id %u) failed in trace but works on replay. This is not a problem.\n", originalProgramName, program);
    }
    else if (linkStatus == GL_FALSE && (status == -1 || status == 1))
    {
        GLint infoLogLength;
        GLint len = -1;
        char *infoLog = (char *)NULL;
        _glGetProgramiv(program, GL_INFO_LOG_LENGTH, &infoLogLength);
        if (infoLogLength > 0)
        {
            infoLog = (char *)malloc(infoLogLength);
            _glGetProgramInfoLog(program, infoLogLength, &len, infoLog);
        }
        vector<unsigned int>::iterator result = find(gRetracer.mOptions.mLinkErrorWhiteListCallNum.begin(), gRetracer.mOptions.mLinkErrorWhiteListCallNum.end(), gRetracer.GetCurCallId());
        if(result != gRetracer.mOptions.mLinkErrorWhiteListCallNum.end())
        {
            if (gRetracer.mOptions.mDebug) DBG_LOG("Error in linking program %u: %s. But this call has already been added to whitelist. So ignore this error and continue retracing (and skip shadercache).\n", originalProgramName, infoLog ? infoLog : "(n/a)");
            bSkipShadercache = true;
        }
        else
        {
            gRetracer.reportAndAbort("Error in linking program %u: %s", originalProgramName, infoLog ? infoLog : "(n/a)");
        }
        free(infoLog);
    }
    else if (linkStatus == GL_FALSE && status == 0)
    {
        if (gRetracer.mOptions.mDebug) DBG_LOG("Error in linking program %u(retraceProg %u) both in trace and retrace (skip it in shadercache).\n", originalProgramName, program);
        bSkipShadercache = true;
    }

    if (gRetracer.mOptions.mShaderCacheFile.size() > 0 && !gRetracer.mOptions.mShaderCacheLoad)
    {
        save_shadercache(program, originalProgramName, bSkipShadercache);
    }
}


void hardcode_glBindFramebuffer(int target, unsigned int framebuffer)
{
    gRetracer.getCurrentContext()._current_framebuffer = framebuffer;

#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
    const unsigned int ON_SCREEN_FBO = 1;
#else
    const unsigned int ON_SCREEN_FBO = 0;
#endif

    if (gRetracer.mOptions.mForceOffscreen && framebuffer == ON_SCREEN_FBO)
    {
        gRetracer.mpOffscrMgr->BindOffscreenFBO(target);
    }
    else
    {
        glBindFramebuffer(target, framebuffer);
    }

    if (gRetracer.mOptions.mForceVRS != -1)
    {
        _glShadingRateEXT(gRetracer.mOptions.mForceVRS);
    }
}

void hardcode_glDeleteBuffers(int n, unsigned int* oldIds)
{
    if (!gRetracer.hasCurrentContext()) return;

    Context& context = gRetracer.getCurrentContext();

    hmap<unsigned int>& idMap = context.getBufferMap();
    hmap<unsigned int>& idRevMap = context.getBufferRevMap();
    for (int i = 0; i < n; ++i)
    {
        unsigned int oldId = oldIds[i];
        unsigned int newId = idMap.RValue(oldId);
        glDeleteBuffers(1, &newId);
        idMap.LValue(oldId) = 0;
        idRevMap.LValue(newId) = 0;
    }
}

void hardcode_glDeleteFramebuffers(int n, unsigned int* oldIds)
{
    if (!gRetracer.hasCurrentContext()) return;

    Context& context = gRetracer.getCurrentContext();

    hmap<unsigned int>& idMap = context._framebuffer_map;
    for (int i = 0; i < n; ++i)
    {
        unsigned int oldId = oldIds[i];
        unsigned int newId = idMap.RValue(oldId);

        GLint preReadFboId, preDrawFboId;
        _glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &preReadFboId);
        _glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &preDrawFboId);

        glDeleteFramebuffers(1, &newId);
        idMap.LValue(oldId) = 0;

        if (gRetracer.mOptions.mForceOffscreen &&
            (newId == (unsigned int)preReadFboId || newId == (unsigned int)preDrawFboId))
        {
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
            const unsigned int ON_SCREEN_FBO = 1;
#else
            const unsigned int ON_SCREEN_FBO = 0;
#endif
            gRetracer.getCurrentContext()._current_framebuffer = ON_SCREEN_FBO;
            gRetracer.mpOffscrMgr->BindOffscreenFBO(GL_FRAMEBUFFER);
        }
    }
}

void hardcode_glDeleteRenderbuffers(int n, unsigned int* oldIds)
{
    if (!gRetracer.hasCurrentContext()) return;

    Context& context = gRetracer.getCurrentContext();

    hmap<unsigned int>& idMap = context.getRenderbufferMap();
    hmap<unsigned int>& idRevMap = context.getRenderbufferRevMap();
    for (int i = 0; i < n; ++i)
    {
        unsigned int oldId = oldIds[i];
        unsigned int newId = idMap.RValue(oldId);
        glDeleteRenderbuffers(1, &newId);
        idMap.LValue(oldId) = 0;
        idRevMap.LValue(newId) = 0;
    }
}

void hardcode_glDeleteTextures(int n, unsigned int* oldIds)
{
    if (!gRetracer.hasCurrentContext()) return;

    Context& context = gRetracer.getCurrentContext();

    hmap<unsigned int>& idMap = context.getTextureMap();
    hmap<unsigned int>& idRevMap = context.getTextureRevMap();
    for (int i = 0; i < n; ++i)
    {
        unsigned int oldId = oldIds[i];
        unsigned int newId = idMap.RValue(oldId);
        glDeleteTextures(1, &newId);
        idMap.LValue(oldId) = 0;
        idRevMap.LValue(newId) = 0;
    }
}

void hardcode_glDeleteTransformFeedbacks(int n, unsigned int* oldIds)
{
    if (!gRetracer.hasCurrentContext()) return;

    Context& context = gRetracer.getCurrentContext();

    hmap<unsigned int>& idMap = context._feedback_map;
    for (int i = 0; i < n; ++i)
    {
        unsigned int oldId = oldIds[i];
        unsigned int newId = idMap.RValue(oldId);
        glDeleteTransformFeedbacks(1, &newId);
        idMap.LValue(oldId) = 0;
    }
}

void hardcode_glDeleteQueries(int n, unsigned int* oldIds)
{
    if (!gRetracer.hasCurrentContext()) return;

    Context& context = gRetracer.getCurrentContext();

    hmap<unsigned int>& idMap = context._query_map;
    for (int i = 0; i < n; ++i)
    {
        unsigned int oldId = oldIds[i];
        unsigned int newId = idMap.RValue(oldId);
        glDeleteQueries(1, &newId);
        idMap.LValue(oldId) = 0;
    }
}

void hardcode_glDeleteSamplers(int n, unsigned int* oldIds)
{
    if (!gRetracer.hasCurrentContext()) return;

    Context& context = gRetracer.getCurrentContext();

    hmap<unsigned int>& idMap = context.getSamplerMap();
    hmap<unsigned int>& idRevMap = context.getSamplerRevMap();
    for (int i = 0; i < n; ++i)
    {
        unsigned int oldId = oldIds[i];
        unsigned int newId = idMap.RValue(oldId);
        glDeleteSamplers(1, &newId);
        idMap.LValue(oldId) = 0;
        idRevMap.LValue(newId) = 0;
    }
}

void hardcode_glDeleteVertexArrays(int n, unsigned int* oldIds)
{
    if (!gRetracer.hasCurrentContext()) return;

    Context& context = gRetracer.getCurrentContext();

    hmap<unsigned int>& idMap = context._array_map;
    for (int i = 0; i < n; ++i)
    {
        unsigned int oldId = oldIds[i];
        unsigned int newId = idMap.RValue(oldId);
        glDeleteVertexArrays(1, &newId);
        idMap.LValue(oldId) = 0;
    }
}

void hardcode_glAssertBuffer_ARM(GLenum target, GLsizei offset, GLsizei size, const char* md5)
{
    const char *ptr = (const char *)glMapBufferRange(target, offset, size, GL_MAP_READ_BIT);
    MD5Digest md5_bound_calc(ptr, size);
    std::string md5_bound = md5_bound_calc.text();
    glUnmapBuffer(target);
    if (md5_bound != md5) gRetracer.reportAndAbort("glAssertBuffer_ARM: MD5 sums differ at call %d", (int)gRetracer.mFile.curCallNo);
}

void hardcode_glAssertFramebuffer_ARM(GLenum target, GLint colorAttachment, const char* md5)
{
    int readFboId = 0, drawFboId = 0;
    _glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFboId);
    _glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFboId);
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
    const unsigned int ON_SCREEN_FBO = 1;
#else
    const unsigned int ON_SCREEN_FBO = 0;
#endif
    if (gRetracer.mOptions.mForceOffscreen)
    {
        _glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ON_SCREEN_FBO);
        gRetracer.mpOffscrMgr->BindOffscreenReadFBO();
    }
    else {
        _glBindFramebuffer(GL_READ_FRAMEBUFFER, drawFboId);
    }
    image::Image *image = getDrawBufferImage(colorAttachment);
    if (image == NULL) gRetracer.reportAndAbort("glAssertFramebuffer_ARM: Framebuffer unavailable!");
    const MD5Digest md5_bound_calc(image->pixels, image->size());
    if (md5_bound_calc.text() != md5) gRetracer.reportAndAbort("glAssertFramebuffer_ARM: MD5 sums differ at call %d", (int)gRetracer.mFile.curCallNo);
#if 0
    std::stringstream ss;
    ss << "assertfb_" << std::setw(10) << std::setfill('0') << gRetracer.mFile.curCallNo << ".png";
    std::string filenameToBeUsed = ss.str();
    image->writePNG(filenameToBeUsed.c_str());
#endif
    delete image;
    _glBindFramebuffer(GL_READ_FRAMEBUFFER, readFboId);
    _glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFboId);
}

GLuint lookUpPolymorphic(GLuint name, GLenum target)
{
    Context& context = gRetracer.getCurrentContext();
    if (target == GL_RENDERBUFFER)
    {
        return context.getRenderbufferMap().RValue(name);
    }
    // otherwise, assume texture type
    return context.getTextureMap().RValue(name);
}
GLuint lookUpPolymorphic2(GLuint name, GLenum target)
{
    Context& context = gRetracer.getCurrentContext();
    switch (target)
    {
    case  GL_BUFFER_OBJECT_EXT: return context.getBufferMap().RValue(name);
    case  GL_SHADER_OBJECT_EXT: return context.getShaderMap().RValue(name);
    case  GL_PROGRAM_OBJECT_EXT: return context.getProgramMap().RValue(name);
    case  GL_VERTEX_ARRAY_OBJECT_EXT: return context._array_map.RValue(name);
    case  GL_QUERY_OBJECT_EXT: return context._query_map.RValue(name);
    case  GL_PROGRAM_PIPELINE_OBJECT_EXT: return context._pipeline_map.RValue(name);
    case  GL_TEXTURE: return context.getTextureMap().RValue(name);
    case  GL_FRAMEBUFFER: return context.getFramebufferMap().RValue(name);
    case  GL_RENDERBUFFER: return context.getRenderbufferMap().RValue(name);
    case  GL_SAMPLER: return context.getSamplerMap().RValue(name);
    case  GL_TRANSFORM_FEEDBACK: return context._feedback_map.RValue(name);
    default:
        {DBG_LOG("Warning: failed type match for Label Object");
        return name;}
    }
}
GLuint lookUpPolymorphic3(GLuint name, GLenum target)
{
    Context& context = gRetracer.getCurrentContext();
    switch (target)
    {
    case  GL_BUFFER: return context.getBufferMap().RValue(name);
    case  GL_SHADER: return context.getShaderMap().RValue(name);
    case  GL_PROGRAM: return context.getProgramMap().RValue(name);
    case  GL_VERTEX_ARRAY: return context._array_map.RValue(name);
    case  GL_QUERY: return context._query_map.RValue(name);
    case  GL_PROGRAM_PIPELINE: return context._pipeline_map.RValue(name);
    case  GL_TEXTURE: return context.getTextureMap().RValue(name);
    case  GL_FRAMEBUFFER: return context.getFramebufferMap().RValue(name);
    case  GL_RENDERBUFFER: return context.getRenderbufferMap().RValue(name);
    case  GL_SAMPLER: return context.getSamplerMap().RValue(name);
    case  GL_TRANSFORM_FEEDBACK: return context._feedback_map.RValue(name);
    default:
        {DBG_LOG("Warning: failed type match for Label Object");
        return name;}
    }
}
EGLObjectKHR lookUpPolymorphic4(int64_t object, EGLenum objectType)
{
    bool found_Image = false;
    switch (objectType)
    {
    case  EGL_OBJECT_THREAD_KHR: return NULL;
    case  EGL_OBJECT_DISPLAY_KHR: return gRetracer.mState.mEglDisplay;
    case  EGL_OBJECT_CONTEXT_KHR: return gRetracer.mState.GetContext(object);
    case  EGL_OBJECT_SURFACE_KHR: return gRetracer.mState.GetDrawable(object);
    case  EGL_OBJECT_IMAGE_KHR: return gRetracer.mState.GetEGLImage(object, found_Image);
    case  EGL_OBJECT_SYNC_KHR: return gRetracer.mState.mEGLSyncMap.RValue((unsigned long long)object);
    case  EGL_OBJECT_STREAM_KHR: return (EGLObjectKHR)object;
    default:
        {DBG_LOG("Warning: failed type match for Label Object");
        return (EGLObjectKHR)object;}
    }
}

} /* namespace retracer */
