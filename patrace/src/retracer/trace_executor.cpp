#include "trace_executor.hpp"

#include "retracer.hpp"
#include <retracer/glws.hpp>

#include <retracer/retrace_api.hpp>
#include "retracer/value_map.hpp"

#include "json/writer.h"
#include "json/reader.h"

#include <limits.h>
#include <zlib.h>
#include <errno.h>

#include "common/base64.hpp"
#include "common/os_string.hpp"
#include "common/trace_callset.hpp"
#include "retracer/afrc_enum.hpp"

#include "libcollector/interface.hpp"

#include <sstream>
#include <vector>
#include <sys/stat.h>
#include <algorithm>

#ifdef ANDROID
#include <android/log.h>
#endif

std::string TraceExecutor::mResultFile;
std::vector<std::string> TraceExecutor::mErrorList;

using namespace retracer;

void TraceExecutor::overrideDefaultsWithJson(Json::Value &value)
{
    retracer::RetraceOptions& options = gRetracer.mOptions;

    options.mDoOverrideResolution = value.get("overrideResolution", false).asBool();
    options.mOverrideResW = value.get("overrideWidth", -1).asInt();
    options.mOverrideResH = value.get("overrideHeight", -1).asInt();
    options.mFailOnShaderError = value.get("overrideFailOnShaderError", options.mFailOnShaderError).asBool();
    options.mOverrideMSAA = value.get("overrideMSAA", options.mOverrideMSAA).asInt();
    if (options.mOverrideMSAA != -1)
        DBG_LOG("Override the existing MSAA for fbo attachment with new MSAA: %d\n", options.mOverrideMSAA);
    options.mLoopTimes = value.get("loopTimes", options.mLoopTimes).asInt();
    if (value.isMember("loopSeconds"))
    {
        options.mLoopSeconds = value["loopSeconds"].asInt();
    }

    if (value.isMember("fpslimit"))
    {
        options.mFixedFps = value["fpslimit"].asInt();
    }

    if (value.get("finishBeforeSwap", false).asBool())
    {
        options.mFinishBeforeSwap = true;
    }

    if (value.get("intervalswap", false).asBool())
    {
        options.mIntervalSwap = true;
    }

    if (value.get("flushWork", false).asBool())
    {
        options.mFlushWork = true;
    }

    if (value.get("translucent_surface", false).asBool())
    {
        options.mTranslucentSurface = true;
    }

    options.mForceVRS = value.get("forceVRS", options.mForceVRS).asInt();
    if (options.mForceVRS != -1 && (options.mForceVRS < 0x96A6 || options.mForceVRS > 0x96AE))
    {
        gRetracer.reportAndAbort("Bad value for option forceVRS");
    }

    if (options.mDoOverrideResolution && (options.mOverrideResW < 0 || options.mOverrideResH < 0))
    {
        gRetracer.reportAndAbort("Missing actual resolution when resolution override set");
    }
    if (options.mDoOverrideResolution)
    {
        options.mOverrideResRatioW = options.mOverrideResW / (float) options.mWindowWidth;
        options.mOverrideResRatioH = options.mOverrideResH / (float) options.mWindowHeight;
    }

    options.mForceAnisotropicLevel = value.get("forceAnisotropicLevel", options.mForceAnisotropicLevel).asInt();

    EglConfigInfo eglConfig(
        value.get("colorBitsRed", -1).asInt(),
        value.get("colorBitsGreen", -1).asInt(),
        value.get("colorBitsBlue", -1).asInt(),
        value.get("colorBitsAlpha", -1).asInt(),
        value.get("depthBits", -1).asInt(),
        value.get("stencilBits", -1).asInt(),
        value.get("msaa", -1).asInt(),
        0
    );

    if (value.isMember("cpumask"))
    {
        options.mCpuMask = value.get("cpumask", "").asString();
    }

    options.mForceSingleWindow = value.get("forceSingleWindow", options.mForceSingleWindow).asBool();
    options.mForceOffscreen = value.get("offscreen", options.mForceOffscreen).asBool();
    options.mPbufferRendering = value.get("noscreen", options.mPbufferRendering).asBool();
    options.mSingleSurface = value.get("singlesurface", options.mSingleSurface).asInt();

    options.mOverrideConfig = eglConfig;
    options.mMeasurePerFrame = value.get("measurePerFrame", false).asBool();

    if (options.mOverrideConfig.msaa_samples > 0)
        DBG_LOG("Enable multi sample: %d, for EGL window surface.\n", options.mOverrideConfig.msaa_samples);

    if (value.isMember("frames")) {
        std::string frames = value.get("frames", "").asString();
        DBG_LOG("Frame string: %s\n", frames.c_str());
        unsigned int start, end;
        if (sscanf(frames.c_str(), "%u-%u", &start, &end) == 2)
        {
            if (start >= end)
            {
                gRetracer.reportAndAbort("Start frame must be lower than end frame. (End frame is never played.)");
            }
            options.mBeginMeasureFrame = start;
            options.mEndMeasureFrame = end;
        } else {
            gRetracer.reportAndAbort("Invalid frames parameter [ %s ]", frames.c_str());
        };
    }

    options.mCallStats = value.get("callStats", options.mCallStats).asBool();
    if (options.mCallStats && (options.mEndMeasureFrame == INT32_MAX))
    {
        gRetracer.reportAndAbort("callStats requires frames to also be present in the JSON input!");
    }

    if(value.isMember("perfrange")){
        std::string perfrange = value.get("perfrange","").asString();
        unsigned int start, end;
        if (sscanf(perfrange.c_str(), "%u-%u", &start, &end) == 2)
        {
            if (start >= end)
            {
                gRetracer.reportAndAbort("Start perf frame must be lower than end perf frame.");
            }
            options.mPerfStart = start;
            options.mPerfStop = end;
        }
        else {
            gRetracer.reportAndAbort("Invalid perf range parameter [ %s ]", perfrange.c_str());
        }
    }

    if (value.isMember("scriptpath")) {
        options.mScriptPath = value.get("scriptpath", "").asString();
        delete options.mScriptCallSet;
        options.mScriptCallSet = new common::CallSet(value.get("scriptcallset", "").asCString());
    }

    if (value.isMember("patch")) {
        options.mPatchPath = value.get("patch", "").asString();
    }

#ifdef ANDROID
    options.mPerfPath = value.get("perfpath","/system/bin/simpleperf").asString();
    options.mPerfOut = value.get("perfout","/sdcard/perf.data").asString();
#else
    options.mPerfPath = value.get("perfpath","/usr/bin/perf").asString();
    options.mPerfOut = value.get("perfout","perf.data").asString();
#endif
    options.mPerfFreq = value.get("perffreq",1000).asInt();
    options.mPerfEvent = value.get("perfevent", "").asString();
    options.mPerfCmd = value.get("perfcmd", "").asString();
    options.mPreload = value.get("preload", false).asBool();
    options.mRunAll = value.get("runAllCalls", false).asBool();

    // Values needed by CLI and GUI
    options.mSnapshotPrefix = value.get("snapshotPrefix", "").asString();

    if (options.mSnapshotPrefix.compare("*") == 0)
    {
        // Create temporary directory to store snapshots in
#if defined(ANDROID)
        std::string snapsDir = "/sdcard/apitrace/retracer-snaps/";
        system("rm -rf /sdcard/apitrace/retracer-snaps/");
        system("mkdir -p /sdcard/apitrace/retracer-snaps");
#elif defined(__APPLE__)
        std::string snapsDir = "/tmp/retracer-snaps/";
        // Can't use system() on iOS
        mkdir(snapsDir.c_str(), 0777);
#else // fbdev / desktop
        std::string snapsDir = "/tmp/retracer-snaps/";
        int sysRet = system("rm -rf /tmp/retracer-snaps/");
        sysRet += system("mkdir -p /tmp/retracer-snaps/");
        if (sysRet != 0)
        {
            DBG_LOG("Failed to prepare directory: %s\n", snapsDir.c_str());
        }
#endif
        options.mSnapshotPrefix = snapsDir;
    }

    options.mSnapshotFrameNames = value.get("snapshotFrameNames", false).asBool();

    // Whether or not to upload taken snapshots.
    options.mUploadSnapshots = value.get("snapshotUpload", false).asBool();

    if (value.isMember("snapshotCallset")) {
        DBG_LOG("snapshotCallset = %s\n", value.get("snapshotCallset", "").asCString());
        delete options.mSnapshotCallSet;
        options.mSnapshotCallSet = new common::CallSet( value.get("snapshotCallset", "").asCString() );
    }

    options.mStateLogging = value.get("statelog", false).asBool();
    stateLoggingEnabled = value.get("drawlog", false).asBool();
    options.mDebug = (int)value.get("debug", false).asBool();
    if (options.mDebug)
    {
        DBG_LOG("Debug mode enabled.\n");
    }

    if (value.get("offscreenBigTiles", false).asBool())
    {
        // Draw offscreen using 4 big tiles, so that their contents are easily visible
        options.mOnscrSampleH *= 12;
        options.mOnscrSampleW *= 12;
        options.mOnscrSampleNumX = 2;
        options.mOnscrSampleNumY = 2;
    }
    else if (value.get("offscreenSingleTile", false).asBool())
    {
        // Draw offscreen using 1 big tile
        options.mOnscrSampleH *= 10;
        options.mOnscrSampleW *= 10;
        options.mOnscrSampleNumX = 1;
        options.mOnscrSampleNumY = 1;
    }

    if (value.get("multithread", false).asBool())
    {
        options.mMultiThread = true;
    }

    if (value.isMember("instrumentation"))
    {
        DBG_LOG("Legacy instrumentation support requested -- fix your JSON! Translating...\n");
        Json::Value legacy;
        for (const Json::Value& v : value["instrumentation"])
        {
            Json::Value emptyDict;
            legacy[v.asString()] = emptyDict;
        }
        gRetracer.mCollectors = new Collection(legacy);
        gRetracer.mCollectors->initialize();
    }

    if (value.isMember("collectors"))
    {
        options.mCollectorEnabled = true;
        options.mCollectorValue = value["collectors"];
    }

    if (value.get("dmaSharedMem", false).asBool())
    {
        options.dmaSharedMemory = true;
    }

    if (value.get("perfmon", false).asBool())
    {
        options.mPerfmon = true;
    }

#ifdef ANDROID
    options.mPerfmonOut = value.get("perfmonout", "/sdcard/").asString();
#else
    options.mPerfmonOut = value.get("perfmonout", "./").asString();
#endif

    if (value.get("step", false).asBool())
    {
        options.mStepMode = true;
    }

#ifdef ENABLE_PERFPERAPI
    if (value.get("perfperapi", false).asBool())
    {
        if (!options.mCollectorEnabled)
        {
            gRetracer.reportAndAbort("perfperapi requires collectors to be enabled.");
        }
        options.mPerfPerApi = true;
    }

    options.mPerfPerApiOutDir = value.get("perfperapiOutDir", "./perfperapi").asString();
#endif

    options.eglAfrcRate = value.get("eglSurfaceCompressionFixedRate", -1).asInt();
    if (options.eglAfrcRate != -1 && (options.eglAfrcRate < compression_fixed_rate_disabled || options.eglAfrcRate >= compression_fixed_rate_flag_end))
    {
        DBG_LOG("!!!WARNING: Invalid compression control flag (%d) on eglSurface. Should be between %d and %d.\n", options.eglAfrcRate, compression_fixed_rate_disabled, compression_fixed_rate_flag_end-1);
        options.eglAfrcRate = -1;
    }

    options.eglImageAfrcRate = value.get("eglImageCompressionFixedRate", -1).asInt();
    if (options.eglImageAfrcRate != -1 && (options.eglImageAfrcRate != compression_fixed_rate_default && options.eglImageAfrcRate != compression_fixed_rate_disabled))
    {
        DBG_LOG("!!!WARNING: Invalid compression control flag (%d) on eglImage. Should be %d or %d.\n", options.eglImageAfrcRate, compression_fixed_rate_disabled, compression_fixed_rate_default);
        options.eglImageAfrcRate = -1;
    }

    options.texAfrcRate = value.get("glesTextureCompressionFixedRate", -1).asInt();
    if (options.texAfrcRate != -1 && (options.texAfrcRate < compression_fixed_rate_disabled || options.texAfrcRate >= compression_fixed_rate_flag_end))
    {
        DBG_LOG("!!!WARNING: Invalid compression control flag (%d) on texture. Should be between %d and %d.\n", options.texAfrcRate, compression_fixed_rate_disabled, compression_fixed_rate_flag_end-1);
        options.texAfrcRate = -1;
    }

    if (value.isMember("loadShaderCache"))
    {
        options.mShaderCacheFile = value.get("loadShaderCache", "").asString();
        options.mShaderCacheLoad = true;
    }
    else if (value.isMember("saveShaderCache"))
    {
        options.mShaderCacheFile = value.get("saveShaderCache", "").asString();
        options.mShaderCacheLoad = false;
    }
    options.mCacheOnly = value.get("cacheOnly", options.mCacheOnly).asBool();
    if (value.isMember("loadShaderCache") && value.isMember("saveShaderCache")) gRetracer.reportAndAbort("loadShaderCache and saveShaderCache cannot be used at the same time in the JSON input!");
    if (!value.isMember("saveShaderCache") && value.isMember("cacheOnly")) gRetracer.reportAndAbort("cacheOnly requires saveShaderCache to also be present in the JSON input!");

    if (value.isMember("saveBlobCache"))
    {
        options.mBlobShaderCacheFile = value.get("saveBlobCache", "").asString();
        options.mSaveBlobCache = true;
        options.mLoadBlobCache = false;
    }
    else if (value.isMember("loadBlobCache"))
    {
        options.mBlobShaderCacheFile = value.get("loadBlobCache", "").asString();
        options.mSaveBlobCache = false;
        options.mLoadBlobCache = true;
    }
    if (value.isMember("loadBlobCache") && value.isMember("saveBlobCache")) gRetracer.reportAndAbort("loadBlobCache and saveBlobCache cannot be used at the same time in the JSON input!");

    options.mInstrumentationDelay = value.get("instrumentationDelay", 0).asUInt();

    if (value.isMember("skipfence"))
    {
        std::vector<std::pair<unsigned int, unsigned int>> ranges;
        for (auto ranges_itr : value["skipfence"])
        {
            ranges.push_back(std::make_pair(ranges_itr[0].asInt(), ranges_itr[1].asInt()));
        }

        if (ranges.size() == 0)
        {
            gRetracer.reportAndAbort("Bad value for option -skipfence, must give at least one frame range.");
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

        merged_ranges.push_back(std::make_pair(start, end));

        if (merged_ranges.size() > 0)
        {
            options.mSkipFence = true;
            options.mSkipFenceRanges = merged_ranges;
        }
        else
        {
            options.mSkipFence = false;
        }
    }
    else
    {
        options.mSkipFence = false;
    }

    DBG_LOG("Thread: %d - override: %s (%d, %d)\n",
            options.mRetraceTid, options.mDoOverrideResolution ? "Yes" : "No", options.mOverrideResW, options.mOverrideResH);
    Json::FastWriter json_write;
    std::string str = json_write.write(value);
    DBG_LOG("Input parameters %s",str.c_str());
}

/**
 Inits the global retracer object from data provided in JSON format.

 @param json_data Parameters in json format.
 @param trace_dir The directory containing the trace file.
 @param result_file The path where the result should be written.
 @return Returns true if successful. If false is returned, an error might be written to the result file.
 */
void TraceExecutor::initFromJson(const std::string& json_data, const std::string& trace_dir, const std::string& result_file)
{
    mResultFile = result_file;

    /*
     * The order is important here:
     *
     * 1. Read trace filename from JSON
     * 2. Set up function pointer entries.
     * 3. Open tracefile and read header defaults
     * 4. Override header defaults with options from the JSON structure + other config like instrumentation.
     */

    Json::Value value;
    Json::Reader reader;
    if (!reader.parse(json_data, value))
    {
        gRetracer.reportAndAbort("JSON parse error: %s", reader.getFormattedErrorMessages().c_str());
    }

    // A path is absolute if
    // -on Unix, it begins with a slash
    // -on Windows, it begins with (back)slash after chopping of potential drive letter

    if (value.isMember("file"))
    {
        bool pathIsAbsolute = value.get("file", "").asString()[0] == '/';
        std::string traceFilePath;
        if (pathIsAbsolute)
        {
            traceFilePath = value.get("file", "").asString();
        } else {
            traceFilePath = std::string(trace_dir) + "/" + value.get("file", "").asString();
        }
        gRetracer.mOptions.mFileName = traceFilePath;
    }

    // 2. now that defaults are loaded
    overrideDefaultsWithJson(value);

#ifdef ANDROID
    bool claim_memory = value.get("claimMemory", false).asBool();
    if (claim_memory){
        float reserveFactor = 0.95f;
        MemoryInfo::reserveAndReleaseMemory(MemoryInfo::getFreeMemoryRaw() * reserveFactor);
    }
#endif

#ifndef _WIN32
    unsigned int membudget = 0;
    membudget = value.get("membudget", membudget).asInt();

    if (MemoryInfo::getFreeMemory() < ((unsigned long)membudget)*1024*1024) {
        unsigned long diff = ((unsigned long)membudget)*1024*1024 - MemoryInfo::getFreeMemory();
        gRetracer.reportAndAbort("Cannot satisfy required memory budget, lacking %lu MiB. Aborting...", diff);
    }
#endif
}

void TraceExecutor::writeError(const std::string &error_description)
{
#ifdef ANDROID
#ifdef PLATFORM_64BIT
    __android_log_write(ANDROID_LOG_FATAL, "paretrace64", error_description.c_str());
#else
    __android_log_write(ANDROID_LOG_FATAL, "paretrace32", error_description.c_str());
#endif
#else
    DBG_LOG("%s\n", error_description.c_str());
#endif
    mErrorList.push_back(error_description);
    if (!writeData(Json::Value(), 0, 0.0f))
    {
        DBG_LOG("Failed to output error log!\n");
    }
    clearError();
}

void TraceExecutor::clearResult()
{
    clearError();
}

void TraceExecutor::clearError()
{
    mErrorList.clear();
}

#ifdef ENABLE_PERFPERAPI
bool TraceExecutor::savePerfPerApiData(Json::Value &collector_res)
{
    try
    {
        DBG_LOG("Processing perapi data...\n");

        // perapi_data[thread_name][api entrypoint][perf event] = total num of counters
        std::map<std::string, std::map<std::string, std::map<std::string, int64_t>>> perapi_data;

        // num_calls_data[thread_name][api entrypoint] =
        //   num of calls (caller), num of calls with perf data (caller + other threads)
        std::map<std::string, std::map<std::string, std::pair<int64_t, int64_t>>> num_calls_data;

        // Background counter data. (total_counters, num_frames)
        std::map<std::string, std::pair<int64_t, int64_t>> bg_counter_data;

        std::set<std::string> all_events;
        for (Json::Value &thread_data : collector_res["perf"]["thread_data"]) {
            std::string thread_name = thread_data["CCthread"].asString();
            auto event_names = thread_data.getMemberNames();
            for (std::string &event_name : event_names) {
                if (!event_name.compare("CCthread:ScopeNumCalls")) {
                    for (unsigned int func_id = 0; func_id < thread_data[event_name].size();
                         func_id++) {
                        int64_t num_calls = thread_data[event_name][func_id].asInt64();
                        if (num_calls == 0)
                            continue;
                        const char *func_name = func_id == 0 ? "noop" : common::gApiInfo.IdToNameArr[func_id];
                        if (!func_name) {
                            DBG_LOG("Unknown API id %d\n", func_id);
                            continue;
                        }
                        num_calls_data[thread_name][func_name].first = num_calls;
                    }
                    thread_data.removeMember(event_name);
                }
                if (!event_name.compare("CCthread:ScopeNumWithPerf")) {
                    for (uint32_t func_id = 0; func_id < thread_data[event_name].size();
                         func_id++) {
                        int64_t num_calls = thread_data[event_name][func_id].asInt64();
                        if (num_calls == 0)
                            continue;
                        const char *func_name = func_id == 0 ? "noop" : common::gApiInfo.IdToNameArr[func_id];
                        if (!func_name) {
                            DBG_LOG("Unknown API id %d\n", func_id);
                            continue;
                        }
                        num_calls_data[thread_name][func_name].second = num_calls;
                    }
                    thread_data.removeMember(event_name);
                }

                // Parse counter data for each API
                if (event_name.find(":ScopeSum") != std::string::npos) {
                    std::string event = event_name.substr(0, event_name.find(":"));
                    all_events.insert(event);
                    for (uint32_t func_id = 0; func_id < thread_data[event_name].size();
                         func_id++) {
                        int64_t event_value = thread_data[event_name][func_id].asInt64();
                        if (event_value == 0)
                            continue;
                        const char *func_name = func_id == 0 ? "noop" : common::gApiInfo.IdToNameArr[func_id];
                        if (!func_name) {
                            DBG_LOG("Unknown API id %d\n", func_id);
                            continue;
                        }
                        perapi_data[thread_name][func_name][event] = event_value;
                    }
                    thread_data.removeMember(event_name);
                }
            }
        }

        const char *basedir = gRetracer.mOptions.mPerfPerApiOutDir.c_str();
        mkdir(basedir, 0777);
        // Output main thread perapi data
        auto mainThreadData = perapi_data.find("replayMainThreads");
        if (mainThreadData == perapi_data.end()) {
            DBG_LOG("No main thread data found in perapi data.\n");
            return false;
        }
        std::string file_name = std::string(basedir) + "/perfperapi.csv";
        FILE *fp = fopen(file_name.c_str(), "w");
        if (!fp) {
            DBG_LOG("Failed to open output CSV %s: %s\n", file_name.c_str(), strerror(errno));
            return false;
        }
        fprintf(fp, "Entrypoint,Num Calls,");
        for (const auto &event_item : all_events) {
            fprintf(fp, "%s_PerCall,", event_item.c_str());
        }
        fprintf(fp, "\n");
        for (const auto &api : mainThreadData->second) {
            std::string func_name = api.first;
            int64_t num_calls = num_calls_data["replayMainThreads"][func_name].first;
            fprintf(fp, "%s,%lld,", func_name.c_str(), (long long int)num_calls);
            for (const auto &event : all_events) {
                auto it = api.second.find(event);
                int64_t event_value = it != api.second.end() ? it->second : 0;
                if (it != api.second.end()) {
                    double per_call =
                        num_calls ? (static_cast<double>(event_value) / num_calls) : 0;
                    fprintf(fp, "%.8lf,", per_call);
                } else {
                    fprintf(fp, "0,");
                }
            }
            fprintf(fp, "\n");
        }
        fclose(fp);

        DBG_LOG("Per API perf data written to directory %s.\n", basedir);
    }
    catch (...) {
        DBG_LOG("ERROR: Exception raised during saving per API data. Potentially broken JSON value returned by collector module.\n");
        return false;
    }
    return true;
}
#endif

bool TraceExecutor::writeData(Json::Value result_data_value, int frames, float duration)
{
    Json::Value result_value;
    if (!mErrorList.empty())
    {
        Json::Value error_list_value;
        Json::Value error_list_description;
        for (const std::string& v : mErrorList)
        {
            error_list_description.append(v);
            error_list_value.append("TRACE_ERROR_GENERIC");
        }
        result_value["error"] = error_list_value;
        result_value["error_description"] = error_list_description;
    }
    else if (frames > 0 || duration > 0.0f)
    {
        Json::Value result_list_value;
        if (gRetracer.mCollectors)
        {
            auto collector_res = gRetracer.mCollectors->results();

#ifdef ENABLE_PERFPERAPI
            if (gRetracer.mOptions.mPerfPerApi) {
                TraceExecutor::savePerfPerApiData(collector_res);
            }
#endif

            try {
                result_data_value["frame_data"] = collector_res;
                if (result_data_value["frame_data"].isMember("ferret")) {
                    if ( result_data_value["frame_data"]["ferret"].empty() ) {
                        DBG_LOG("Ferret data detected, but the results are empty.\n");
                    } else {
                        DBG_LOG("Ferret data detected, checking for postprocessed results...\n");
                        // Calculate CPU FPS
                        if (result_data_value["frame_data"]["ferret"].isMember("postprocessed_results")) {
                            DBG_LOG("Postprocessed ferret data detected, calculating CPU FPS!\n");

                            int main_thread = result_data_value["frame_data"]["ferret"]["postprocessed_results"]["main_thread_index"].asInt();
                            double main_thread_megacycles = result_data_value["frame_data"]["ferret"]["postprocessed_results"]["main_thread_megacycles"].asInt();
                            DBG_LOG("Main (heaviest) thread index set to: %d, main thread mega cycles consumed = %f\n", main_thread, main_thread_megacycles);

                            result_data_value["main_thread_cpu_runtime@3GHz"] = main_thread_megacycles / 3000.0;

                            double main_thread_cpu_runtime_3ghz = result_data_value["main_thread_cpu_runtime@3GHz"].asDouble();
                            if (main_thread_cpu_runtime_3ghz == 0.0) {
                                DBG_LOG("WARNING: Main thread CPU megacycles reported as 0. Possibly invalid ferret results.\n");
                                result_data_value["cpu_fps_main_thread@3GHz"] = 0.0;
                            } else {
                                result_data_value["cpu_fps_main_thread@3GHz"] = static_cast<double>(frames) / main_thread_cpu_runtime_3ghz;
                            }

                            double total_megacycles = result_data_value["frame_data"]["ferret"]["postprocessed_results"]["megacycles_sum"].asDouble();
                            DBG_LOG("Total CPU mega cycles consumed = %f\n", total_megacycles);

                            result_data_value["full_system_cpu_runtime@3GHz"] = total_megacycles / 3000.0;

                            double full_system_cpu_runtime_3ghz = result_data_value["full_system_cpu_runtime@3GHz"].asDouble();
                            if (full_system_cpu_runtime_3ghz == 0.0) {
                                DBG_LOG("WARNING: Total CPU megacycles reported as 0. Possibly invalid ferret results.\n");
                                result_data_value["cpu_fps_full_system@3GHz"] = 0.0;
                            } else {
                                result_data_value["cpu_fps_full_system@3GHz"] = static_cast<double>(frames) / full_system_cpu_runtime_3ghz;
                            }

                            if (duration < 5.0) {
                                DBG_LOG("WARNING: Runtime was less than 5 seconds, this can lead to inaccurate CPU load measurements (increasing runtime is highly recommended)\n");
                            }

                            DBG_LOG("CPU full system FPS@3GHz = %f, CPU main thread FPS@3GHz = %f\n", result_data_value["cpu_fps_full_system@3GHz"].asDouble(), result_data_value["cpu_fps_main_thread@3GHz"].asDouble());
                        } else {
                            DBG_LOG("Ferret data has no postprocessed results! Possibly wrong input parameters (check frequency list and cpu list in input json)\n");
                        }
                    }
                }
            } catch (...) {
                DBG_LOG("ERROR: Exception raised during CPU FPS calculations. Potentially broken JSON value returned by collector module.\n");
            }
        }

        // Get chosen EGL configuration information
        Json::Value fb_config;
        if (gRetracer.mOptions.mForceOffscreen)
        {
            fb_config["msaaSamples"] = gRetracer.mOptions.mOffscreenConfig.msaa_samples;
            fb_config["colorBitsRed"] = gRetracer.mOptions.mOffscreenConfig.red;
            fb_config["colorBitsGreen"] = gRetracer.mOptions.mOffscreenConfig.green;
            fb_config["colorBitsBlue"] = gRetracer.mOptions.mOffscreenConfig.blue;
            fb_config["colorBitsAlpha"] = gRetracer.mOptions.mOffscreenConfig.alpha;
            fb_config["depthBits"] = gRetracer.mOptions.mOffscreenConfig.depth;
            fb_config["stencilBits"] = gRetracer.mOptions.mOffscreenConfig.stencil;
        }
        else
        {
            EglConfigInfo info = GLWS::instance().getSelectedEglConfig();
            fb_config["msaaSamples"] = info.msaa_sample_buffers == 1 && info.msaa_samples > 0 ? info.msaa_samples : 0;
            fb_config["colorBitsRed"] = info.red;
            fb_config["colorBitsGreen"] = info.green;
            fb_config["colorBitsBlue"] = info.blue;
            fb_config["colorBitsAlpha"] = info.alpha;
            fb_config["depthBits"] = info.depth;
            fb_config["stencilBits"] = info.stencil;
        }
        result_data_value["fb_config"] = fb_config;

        result_data_value["egl_info"] = GLWS::instance().getEglInfoJson();

        // Add to result list
        result_list_value.append(result_data_value);
        result_value["result"] = result_list_value;
    }

    Json::StyledWriter writer;
    std::string data = writer.write(result_value);

#ifdef ANDROID
    std::string outputfile = "/sdcard/results.json";
#else
    std::string outputfile = "results.json";
#endif
    if (!mResultFile.empty())
    {
        outputfile = mResultFile;
    }
    FILE* fp = fopen(outputfile.c_str(), "w");
    if (!fp)
    {
        DBG_LOG("Failed to open output JSON %s: %s\n", outputfile.c_str(), strerror(errno));
        return false;
    }
    size_t written;
    int err = 0;
    do
    {
        clearerr(fp);
        written = fwrite(data.c_str(), data.size(), 1, fp);
        err = ferror(fp);
    } while (!written && (err == EAGAIN || err == EWOULDBLOCK || err == EINTR));
    if (err)
    {
        DBG_LOG("Failed to write output JSON: %s\n", strerror(err));
    }
    fsync(fileno(fp));
    fclose(fp);
    DBG_LOG("Results written to %s\n", outputfile.c_str());
    return true;
}
