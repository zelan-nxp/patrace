// Warning: This tool is a huge hack, use with care!

#include <utility>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES3/gl31.h>
#include <GLES3/gl32.h>
#include <limits.h>
#include <unordered_map>
#include <algorithm>

#include "tool/parse_interface.h"

#include "common/in_file.hpp"
#include "common/file_format.hpp"
#include "common/api_info.hpp"
#include "common/parse_api.hpp"
#include "common/trace_model.hpp"
#include "common/gl_utility.hpp"
#include "common/os.hpp"
#include "common/memory.hpp"
#include "eglstate/context.hpp"
#include "tool/config.hpp"
#include "base/base.hpp"
#include "tool/utils.hpp"

#define DEDUP_BUFFERS 1
#define DEDUP_UNIFORMS 2
#define DEDUP_TEXTURES 4
#define DEDUP_SCISSORS 8
#define DEDUP_BLENDFUNC 16
#define DEDUP_ENABLE 32
#define DEDUP_DEPTHFUNC 64
#define DEDUP_VERTEXATTRIB 128
#define DEDUP_MAKECURRENT 256
#define DEDUP_PROGRAMS 512
#define DEDUP_CSB 1024

static bool patch = false;
static common::patchfile patchfile;
static bool replace = false;
static std::pair<int, int> dedups;
static bool onlycount = false;
static bool verbose = false;
static FILE* fp = stdout;
static int lastframe = -1;
static bool debug = false;
static std::map<int, int> csv_ranges;
static int remove_until = -1;
#define DEBUG_LOG(...) if (debug) DBG_LOG(__VA_ARGS__)

static void printHelp()
{
    std::cout <<
        "Usage : deduplicator [OPTIONS] trace_file.pat new_file.pat\n"
        "Only works for single-threaded traces for now.\n"
        "Options:\n"
        "  --buffers     Deduplicate glBindBuffer calls\n"
        "  --textures    Deduplicate glBindTexture and glBindSampler calls\n"
        "  --uniforms    Deduplicate glUniform2/3f calls\n"
        "  --scissors    Deduplicate glScissor calls\n"
        "  --blendfunc   Deduplicate glBlendFunc and glBlendColor calls\n"
        "  --depthfunc   Deduplicate glDepthFunc calls\n"
        "  --enable      Deduplicate glEnable/glDisable calls\n"
        "  --vertexattr  Deduplicate glVertexAttribPointer calls\n"
        "  --makecurrent Deduplicate eglMakeCurrent calls\n"
        "  --programs    Deduplicate glUseProgram calls\n"
        "  --csb         Deduplicate client side buffer calls\n"
        "  --all         Deduplicate all the above\n"
        "  --end FRAME   End frame (terminates trace here)\n"
        "  --last FRAME  Stop doing changes at this frame (copies the remaining trace without changes)\n"
        "  --replace     Replace calls with glEnable(GL_INVALID_INDEX) instead of removing\n"
        "  --csv FILE    Remove draw calls from given ranges from a CSV file (start of range is inclusive, end exclusive)\n"
        "  --nofbo F1 F2 Remove draw calls between FBOs F1 and F2\n" // TBD
        "  --verbose     Print more information while running\n"
        "  -c            Only count and report instances, do no changes\n"
        "  -d            Debug mode (print a lot of info)\n"
        "  -o FILE       Write log to file\n"
        "  -p FILE       Apply patch from file\n"
        "  -h            Print help\n"
        "  -v            Print version\n"
        ;
}

static void printVersion()
{
    std::cout << PATRACE_VERSION << std::endl;
}

static void writeout(common::OutFile &outputFile, common::CallTM *call, bool injected = false)
{
    dedups.second++;
    if (onlycount || patch) return;
    const unsigned int WRITE_BUF_LEN = 150*1024*1024;
    static char buffer[WRITE_BUF_LEN];
    char *dest = buffer;
    dest = call->Serialize(dest, -1, injected);
    outputFile.Write(buffer, dest-buffer);
}

static void dedup(common::OutFile& outputFile, common::CallTM *call, int &stat)
{
    if (patch && !replace)
    {
        common::patchfile_remove(patchfile, call->mCallNo);
    }
    else if (replace && !onlycount)
    {
        common::CallTM enable("glEnable");
        enable.mArgs.push_back(new common::ValueTM((GLenum)GL_INVALID_INDEX));
        if (!patch) writeout(outputFile, &enable, true);
        else common::patchfile_replace(patchfile, enable);
    }
    dedups.first++;
    stat++;
}

static bool in_array(const std::string &value, const std::vector<std::string> &array)
{
    return std::find(array.begin(), array.end(), value) != array.end();
}

void deduplicate(ParseInterface& input, common::OutFile& outputFile, int endframe, int tid, int flags)
{
    common::CallTM *call = nullptr;
    std::unordered_map<GLuint, std::unordered_map<GLenum, GLuint>> buffers;
    std::unordered_map<GLuint, std::unordered_map<GLuint, GLfloat>> uniform1f;
    std::unordered_map<GLuint, std::unordered_map<GLuint, std::tuple<GLfloat, GLfloat>>> uniform2f;
    std::unordered_map<GLuint, std::unordered_map<GLuint, std::tuple<GLfloat, GLfloat, GLfloat>>> uniform3f;
    std::unordered_map<GLuint, std::unordered_map<GLenum, GLuint>> textures;
    std::unordered_map<GLuint, std::unordered_map<GLenum, GLuint>> samplers;
    std::unordered_map<GLuint, std::tuple<GLint, GLint, GLsizei, GLsizei>> scissors;
    std::unordered_map<GLuint, std::tuple<GLenum, GLenum>> blendfunc;
    std::unordered_map<GLuint, GLenum> depthfunc;
    std::unordered_map<GLuint, std::unordered_map<GLenum, bool>> enabled;
    std::unordered_map<GLuint, std::tuple<GLfloat, GLfloat, GLfloat, GLfloat>> blendcolor;
    std::unordered_map<GLuint, std::unordered_map<GLuint, std::tuple<GLuint, GLint, GLenum, GLboolean, GLsizei, uint64_t>>> vertexattrib;
    std::unordered_map<GLuint, std::unordered_map<GLuint, bool>> enablevertex;
    std::unordered_map<GLuint, std::unordered_map<GLuint, GLuint>> vertexdivisor;
    std::unordered_map<GLuint, std::unordered_map<GLuint, GLuint>> mappedbuffers_flushbit; //context, buffer, bit
    std::unordered_map<GLuint, GLuint> old_program_id;
    std::unordered_map<GLuint, int> old_context_id; //thread, context
    std::unordered_map<GLuint, int> old_surface_id; //context, surface
    std::unordered_map<GLuint, std::pair<int, int>> csbdedup_possible;

    std::pair<int, int> vertexattrs;
    std::pair<int, int> bindbuffers;
    std::pair<int, int> enables; // includes disables
    std::pair<int, int> scissordupes;
    std::pair<int, int> blendcols;
    std::pair<int, int> bindtexs;
    std::pair<int, int> bindsamps;
    std::pair<int, int> uniforms;
    std::pair<int, int> depthfuncs;
    std::pair<int, int> blendfuncs;
    std::pair<int, int> useprograms;
    std::pair<int, int> makecurr;
    std::pair<int, int> csb;
    std::pair<int, int> customcsv;
    int makecurr_harmless = 0;
    int last_swap = 0;
    std::vector<std::string> rendering = { "glDiscardFramebufferEXT", "glClear", "glClearBufferfi", "glClearBufferfv" }; // + glDraw*

    // Go through entire trace file
    while ((call = input.next_call()))
    {
        int cur_ctx = input.context_index; //the context idx of current call
        if (lastframe != -1 && input.frames >= lastframe)
        {
            writeout(outputFile, call);
            continue;
        }

        if (csv_ranges.count(call->mCallNo)) { assert(remove_until == -1); remove_until = csv_ranges[call->mCallNo]; }
        if ((int)call->mCallNo == remove_until) remove_until = -1;
        if (remove_until != -1 && (in_array(call->mCallName, rendering) || call->mCallName.compare(0, 6, "glDraw") == 0)) { dedup(outputFile, call, customcsv.first); continue; }

        if (call->mCallName == "glUseProgram")
        {
            useprograms.second++;
            const GLenum id = call->mArgs[0]->GetAsUInt();
            if (old_program_id.find(cur_ctx) == old_program_id.end())
            {
                old_program_id[cur_ctx] = id;
                writeout(outputFile, call);
            }
            else if (id != old_program_id[cur_ctx] || id == 0 || !(flags & DEDUP_PROGRAMS))
            {
                uniform1f[cur_ctx].clear();
                uniform2f[cur_ctx].clear();
                uniform3f[cur_ctx].clear();
                vertexattrib[cur_ctx].clear();
                enablevertex[cur_ctx].clear();
                vertexdivisor[cur_ctx].clear();
                old_program_id[cur_ctx] = id;
                writeout(outputFile, call);
            }
            else
            {
                dedup(outputFile, call, useprograms.first);
                assert(old_program_id[cur_ctx] == id);
            }
            csbdedup_possible[cur_ctx] = std::make_pair(-1, -1);
        }
        else if (call->mCallName == "eglMakeCurrent")
        {
            int surface = call->mArgs[1]->GetAsInt();
            int readsurface = call->mArgs[2]->GetAsInt();
            assert(readsurface == surface);

            if(scissors.find(cur_ctx) == scissors.end()) scissors[cur_ctx] = std::make_tuple(-1, -1, -1, -1);
            if(blendfunc.find(cur_ctx) == blendfunc.end()) blendfunc[cur_ctx] = std::make_tuple(GL_NONE, GL_NONE);
            if(depthfunc.find(cur_ctx) == depthfunc.end()) depthfunc[cur_ctx] = GL_NONE;
            if(blendcolor.find(cur_ctx) == blendcolor.end()) blendcolor[cur_ctx] = std::make_tuple(0.0f, 0.0f, 0.0f, 0.0f);

            makecurr.second++;
            if(old_context_id.find(call->mTid) == old_context_id.end() || old_surface_id.find(cur_ctx) == old_surface_id.end())
            {
                writeout(outputFile, call);
            }
            else if (cur_ctx == old_context_id[call->mTid] && old_surface_id[cur_ctx] == surface && (flags & DEDUP_MAKECURRENT))
            {
                dedup(outputFile, call, makecurr.first);
                if (last_swap == (int)call->mCallNo - 1 || input.frames == 0) makecurr_harmless++;
            }
            else
            {
                writeout(outputFile, call);
            }
            old_context_id[call->mTid] = cur_ctx;
            old_surface_id[cur_ctx] = surface;
        }
        else if (call->mCallName == "glBindBuffer" && (flags & (DEDUP_BUFFERS | DEDUP_CSB)))
        {
            bindbuffers.second++;
            const GLenum target = call->mArgs[0]->GetAsUInt();
            const GLuint id = call->mArgs[1]->GetAsUInt();
            if (csbdedup_possible.find(cur_ctx) != csbdedup_possible.end() && (int)target == csbdedup_possible[cur_ctx].second) csbdedup_possible[cur_ctx] = std::make_pair(-1, -1);
            if (buffers.find(cur_ctx) == buffers.end() || buffers[cur_ctx].count(target) == 0 || buffers[cur_ctx].at(target) != id)
            {
                writeout(outputFile, call);
                buffers[cur_ctx][target] = id;
            }
            else if (flags & DEDUP_BUFFERS) dedup(outputFile, call, bindbuffers.first);
            else writeout(outputFile, call);
        }
        else if (call->mCallName == "glVertexAttribDivisor" && (flags & DEDUP_VERTEXATTRIB))
        {
            enables.second++;
            const GLuint target = call->mArgs[0]->GetAsUInt();
            const GLuint divisor = call->mArgs[1]->GetAsUInt();
            if (vertexdivisor[cur_ctx].count(target) > 0 && vertexdivisor[cur_ctx][target] == divisor) dedup(outputFile, call, enables.first);
            else writeout(outputFile, call);
            vertexdivisor[cur_ctx][target] = divisor;
        }
        else if (call->mCallName == "glEnableVertexAttribArray" && (flags & DEDUP_VERTEXATTRIB))
        {
            enables.second++;
            const GLenum target = call->mArgs[0]->GetAsUInt();
            if (enablevertex[cur_ctx].count(target) > 0 && enablevertex[cur_ctx][target]) dedup(outputFile, call, enables.first);
            else writeout(outputFile, call);
            enablevertex[cur_ctx][target] = true;
        }
        else if (call->mCallName == "glDisableVertexAttribArray" && (flags & DEDUP_VERTEXATTRIB))
        {
            enables.second++;
            const GLenum target = call->mArgs[0]->GetAsUInt();
            if (enablevertex[cur_ctx].count(target) > 0 && !enablevertex[cur_ctx][target]) dedup(outputFile, call, enables.first);
            else writeout(outputFile, call);
            enablevertex[cur_ctx][target] = false;
        }
        else if (call->mCallName == "glEnable" && (flags & DEDUP_ENABLE))
        {
            enables.second++;
            const GLenum key = call->mArgs[0]->GetAsUInt();
            if (enabled.find(cur_ctx) == enabled.end() || enabled[cur_ctx].count(key) == 0 || !enabled[cur_ctx].at(key))
            {
                writeout(outputFile, call);
                enabled[cur_ctx][key] = true;
            }
            else dedup(outputFile, call, enables.first);
        }
        else if (call->mCallName == "glDisable" && (flags & DEDUP_ENABLE))
        {
            enables.second++;
            const GLenum key = call->mArgs[0]->GetAsUInt();
            if (enabled.find(cur_ctx) == enabled.end() || enabled[cur_ctx].count(key) == 0 || enabled[cur_ctx].at(key))
            {
                writeout(outputFile, call);
                enabled[cur_ctx][key] = false;
            }
            else dedup(outputFile, call, enables.first);
        }
        else if (call->mCallName == "glScissor" && (flags & DEDUP_SCISSORS))
        {
            scissordupes.second++;
            const GLint x = call->mArgs[0]->GetAsInt();
            const GLint y = call->mArgs[1]->GetAsInt();
            const GLsizei width = call->mArgs[2]->GetAsInt();
            const GLsizei height = call->mArgs[3]->GetAsInt();
            auto val = std::make_tuple(x, y, width, height);
            if (scissors[cur_ctx] != val)
            {
                writeout(outputFile, call);
                scissors[cur_ctx] = val;
            }
            else dedup(outputFile, call, scissordupes.first);
        }
        else if (call->mCallName == "glDepthFunc" && (flags & DEDUP_DEPTHFUNC))
        {
            depthfuncs.second++;
            const GLenum func = call->mArgs[0]->GetAsUInt();
            if (depthfunc[cur_ctx] != func)
            {
                writeout(outputFile, call);
                depthfunc[cur_ctx] = func;
            }
            else dedup(outputFile, call, depthfuncs.first);
        }
        else if (call->mCallName == "glBlendFunc" && (flags & DEDUP_BLENDFUNC))
        {
            blendfuncs.second++;
            const GLenum sfactor = call->mArgs[0]->GetAsUInt();
            const GLenum dfactor = call->mArgs[1]->GetAsUInt();
            auto val = std::make_tuple(sfactor, dfactor);
            if (blendfunc[cur_ctx] != val)
            {
                writeout(outputFile, call);
                blendfunc[cur_ctx] = val;
            }
            else dedup(outputFile, call, blendfuncs.first);
        }
        else if (call->mCallName == "glVertexAttribPointer" && (flags & DEDUP_VERTEXATTRIB))
        {
            vertexattrs.second++;
            const GLuint index = call->mArgs[0]->GetAsUInt();
            const GLint size = call->mArgs[1]->GetAsInt();
            const GLenum type = call->mArgs[2]->GetAsUInt();
            const GLboolean normalized = (GLboolean)call->mArgs[3]->GetAsUInt();
            const GLsizei stride = call->mArgs[4]->GetAsInt();
            const uint64_t pointer = call->mArgs[5]->GetAsUInt64();
            auto val = std::make_tuple(index, size, type, normalized, stride, pointer);
            if (vertexattrib[cur_ctx].count(index) == 0 || vertexattrib[cur_ctx][index] != val)
            {
                writeout(outputFile, call);
                vertexattrib[cur_ctx][index] = val;
            }
            else dedup(outputFile, call, vertexattrs.first);
        }
        else if (call->mCallName == "glBlendColor" && (flags & DEDUP_BLENDFUNC))
        {
            blendcols.second++;
            const GLfloat r = call->mArgs[0]->GetAsFloat();
            const GLfloat g = call->mArgs[1]->GetAsFloat();
            const GLfloat b = call->mArgs[2]->GetAsFloat();
            const GLfloat a = call->mArgs[3]->GetAsFloat();
            auto val = std::make_tuple(r, g, b, a);
            if (blendcolor[cur_ctx] != val)
            {
                writeout(outputFile, call);
                blendcolor[cur_ctx] = val;
            }
            else dedup(outputFile, call, blendcols.first);
        }
        else if (call->mCallName == "glBindTexture" && (flags & DEDUP_TEXTURES))
        {
            bindtexs.second++;
            const GLenum target = call->mArgs[0]->GetAsUInt();
            const GLuint id = call->mArgs[1]->GetAsUInt();
            if (textures.find(cur_ctx) == textures.end() || textures[cur_ctx].count(target) == 0 || textures[cur_ctx].at(target) != id)
            {
                writeout(outputFile, call);
                textures[cur_ctx][target] = id;
                samplers[cur_ctx].clear();
            }
            else dedup(outputFile, call, bindtexs.first);
        }
        else if (call->mCallName == "glBindSampler" && (flags & DEDUP_TEXTURES))
        {
            bindsamps.second++;
            const GLenum target = call->mArgs[0]->GetAsUInt();
            const GLuint id = call->mArgs[1]->GetAsUInt();
            if (samplers.find(cur_ctx) == samplers.end() || samplers[cur_ctx].count(target) == 0 || samplers[cur_ctx].at(target) != id)
            {
                writeout(outputFile, call);
                samplers[cur_ctx][target] = id;
            }
            else dedup(outputFile, call, bindsamps.first);
        }
        else if ((call->mCallName == "glUniform1f" || call->mCallName == "glUniform1fv") && (flags & DEDUP_UNIFORMS))
        {
            uniforms.second++;
            const GLuint location = call->mArgs[0]->GetAsUInt();
            const GLsizei count = (call->mCallName == "glUniform1f") ? 1 : call->mArgs[1]->GetAsUInt();
            const GLfloat v1 = (call->mCallName == "glUniform1f") ? call->mArgs[1]->GetAsFloat() : call->mArgs[2]->mArray[0].GetAsFloat();
            if (uniform1f.find(cur_ctx) == uniform1f.end() || count != 1 || uniform1f[cur_ctx].count(location) == 0 || uniform1f[cur_ctx].at(location) != v1)
            {
                writeout(outputFile, call);
                uniform1f[cur_ctx][location] = v1;
            }
            else dedup(outputFile, call, uniforms.first);
        }
        else if (call->mCallName == "glUniform2f" && (flags & DEDUP_UNIFORMS))
        {
            uniforms.second++;
            const GLuint location = call->mArgs[0]->GetAsUInt();
            const GLfloat v1 = call->mArgs[1]->GetAsFloat();
            const GLfloat v2 = call->mArgs[2]->GetAsFloat();
            if (uniform2f.find(cur_ctx) == uniform2f.end() || uniform2f[cur_ctx].count(location) == 0 || std::get<0>(uniform2f[cur_ctx].at(location)) != v1 || std::get<1>(uniform2f[cur_ctx].at(location)) != v2)
            {
                writeout(outputFile, call);
                uniform2f[cur_ctx][location] = std::make_tuple(v1, v2);
            }
            else dedup(outputFile, call, uniforms.first);
        }
        else if (call->mCallName == "glUniform2fv" && (flags & DEDUP_UNIFORMS))
        {
            uniforms.second++;
            const GLuint location = call->mArgs[0]->GetAsUInt();
            const GLsizei count = call->mArgs[1]->GetAsUInt();
            const GLfloat v1 = call->mArgs[2]->mArray[0].GetAsFloat();
            const GLfloat v2 = call->mArgs[2]->mArray[1].GetAsFloat();
            if (uniform2f.find(cur_ctx) == uniform2f.end() || count != 1 || uniform2f[cur_ctx].count(location) == 0 || std::get<0>(uniform2f[cur_ctx].at(location)) != v1 || std::get<1>(uniform2f[cur_ctx].at(location)) != v2)
            {
                writeout(outputFile, call);
                uniform2f[cur_ctx][location] = std::make_tuple(v1, v2);
            }
            else dedup(outputFile, call, uniforms.first);
        }
        else if (call->mCallName == "glUniform3f" && (flags & DEDUP_UNIFORMS))
        {
            uniforms.second++;
            const GLuint location = call->mArgs[0]->GetAsUInt();
            const GLfloat v1 = call->mArgs[1]->GetAsFloat();
            const GLfloat v2 = call->mArgs[2]->GetAsFloat();
            const GLfloat v3 = call->mArgs[3]->GetAsFloat();
            if (uniform3f.find(cur_ctx) == uniform3f.end() || uniform3f[cur_ctx].count(location) == 0 || std::get<0>(uniform3f[cur_ctx].at(location)) != v1 || std::get<1>(uniform3f[cur_ctx].at(location)) != v2 || std::get<2>(uniform3f[cur_ctx].at(location)) != v3)
            {
                writeout(outputFile, call);
                uniform3f[cur_ctx][location] = std::make_tuple(v1, v2, v3);
            }
            else dedup(outputFile, call, uniforms.first);
        }
        else if (call->mCallName == "glUniform3fv" && (flags & DEDUP_UNIFORMS))
        {
            uniforms.second++;
            const GLuint location = call->mArgs[0]->GetAsUInt();
            const GLsizei count = call->mArgs[1]->GetAsUInt();
            const GLfloat v1 = call->mArgs[2]->mArray[0].GetAsFloat();
            const GLfloat v2 = call->mArgs[2]->mArray[1].GetAsFloat();
            const GLfloat v3 = call->mArgs[2]->mArray[2].GetAsFloat();
            if (uniform3f.find(cur_ctx) == uniform3f.end() || count != 1 || uniform3f[cur_ctx].count(location) == 0 || std::get<0>(uniform3f[cur_ctx].at(location)) != v1 || std::get<1>(uniform3f[cur_ctx].at(location)) != v2 || std::get<2>(uniform3f[cur_ctx].at(location)) != v3)
            {
                writeout(outputFile, call);
                uniform3f[cur_ctx][location] = std::make_tuple(v1, v2, v3);
            }
            else dedup(outputFile, call, uniforms.first);
        }
        else if (call->mCallName == "eglGetError")
        {
            writeout(outputFile, call);
            if (last_swap == (int)call->mCallNo - 1) last_swap++; // pretend this call doesn't exist for purposes of checking if we just swapped
        }
        else if (call->mCallName == "eglSwapBuffers" && input.frames != endframe) // log (slow) progress
        {
            if (verbose) DBG_LOG("Frame %d / %d\n", (int)input.frames, endframe);
            writeout(outputFile, call);
            last_swap = call->mCallNo;
        }
        else if (call->mCallName == "eglSwapBuffers" && input.frames == endframe) // terminate here?
        {
            writeout(outputFile, call);
            if (verbose) DBG_LOG("Ending!\n");
            break;
        }
        else if (call->mCallName == "glMapBufferRange" && (flags & DEDUP_CSB))
        {
            const GLenum target = call->mArgs[0]->GetAsUInt();
            const GLuint access = call->mArgs[3]->GetAsUInt();
            int cur_buf = buffers[cur_ctx][target];
            if (access & GL_MAP_FLUSH_EXPLICIT_BIT_EXT)
            {
                mappedbuffers_flushbit[cur_ctx][cur_buf] = 0;
            }
            else
            {
                mappedbuffers_flushbit[cur_ctx][cur_buf] = -1;
            }
            writeout(outputFile, call);
        }
        else if (call->mCallName == "glFlushMappedBufferRange" && (flags & DEDUP_CSB))
        {
            const GLenum target = call->mArgs[0]->GetAsUInt();
            int cur_buf = buffers[cur_ctx][target];
            if (mappedbuffers_flushbit[cur_ctx][cur_buf] == 0)
            {
                mappedbuffers_flushbit[cur_ctx][cur_buf] = 1;
            }
            writeout(outputFile, call);
        }
        else if (call->mCallName == "glCopyClientSideBuffer" && (flags & DEDUP_CSB))
        {
            csb.second++;
            const GLenum target = call->mArgs[0]->GetAsUInt();
            const GLuint name = call->mArgs[1]->GetAsUInt();
            int cur_buf = buffers[cur_ctx][target];
            if ((csbdedup_possible.find(cur_ctx) != csbdedup_possible.end() && csbdedup_possible[cur_ctx] == std::make_pair((int)name, (int)target)) || mappedbuffers_flushbit[cur_ctx][cur_buf] == 1)
            {
                dedup(outputFile, call, csb.first);
                continue;
            }
            csbdedup_possible[cur_ctx] = std::make_pair((int)name, (int)target);
            writeout(outputFile, call);
        }
        else if (call->mCallName == "glUnmapBuffer" && (flags & DEDUP_CSB))
        {
            const GLenum target = call->mArgs[0]->GetAsUInt();
            int cur_buf = buffers[cur_ctx][target];
            if (csbdedup_possible.find(cur_ctx) != csbdedup_possible.end() && (int)target == csbdedup_possible[cur_ctx].second) csbdedup_possible[cur_ctx] = std::make_pair(-1, -1);
            mappedbuffers_flushbit[cur_ctx][cur_buf] = -1;
            writeout(outputFile, call);
        }
        else if (call->mCallName == "glClientSideBufferData" && (flags & DEDUP_CSB))
        {
            csbdedup_possible[cur_ctx] = std::make_pair(-1, -1);
            writeout(outputFile, call);
        }
        else if (call->mCallName == "glPatchClientSideBuffer" && (flags & DEDUP_CSB))
        {
            csbdedup_possible[cur_ctx] = std::make_pair(-1, -1);
            writeout(outputFile, call);
        }
        else if (call->mCallName == "glClientSideBufferSubData" && (flags & DEDUP_CSB))
        {
            csbdedup_possible[cur_ctx] = std::make_pair(-1, -1);
            writeout(outputFile, call);
        }
        else
        {
            writeout(outputFile, call);
        }
    }
    fprintf(fp, "Removed %d / %d calls (%d%%)\n", dedups.first, dedups.second, dedups.first * 100 / dedups.second);
    if (useprograms.first) fprintf(fp, "Removed %d / %d glUseProgram calls (%d%%)\n", useprograms.first, useprograms.second, useprograms.first * 100 / useprograms.second);
    if (vertexattrs.first) fprintf(fp, "Removed %d / %d vertex attr calls (%d%%)\n", vertexattrs.first, vertexattrs.second, vertexattrs.first * 100 / vertexattrs.second);
    if (bindbuffers.first) fprintf(fp, "Removed %d / %d bindbuffer calls (%d%%)\n", bindbuffers.first, bindbuffers.second, bindbuffers.first * 100 / bindbuffers.second);
    if (enables.first) fprintf(fp, "Removed %d / %d enable/disable calls (%d%%)\n", enables.first, enables.second, enables.first * 100 / enables.second);
    if (scissordupes.first) fprintf(fp, "Removed %d / %d scissor calls (%d%%)\n", scissordupes.first, scissordupes.second, scissordupes.first * 100 / scissordupes.second);
    if (blendcols.first) fprintf(fp, "Removed %d / %d blendcol calls (%d%%)\n", blendcols.first, blendcols.second, blendcols.first * 100 / blendcols.second);
    if (bindtexs.first) fprintf(fp, "Removed %d / %d bindtexture calls (%d%%)\n", bindtexs.first, bindtexs.second, bindtexs.first * 100 / bindtexs.second);
    if (bindsamps.first) fprintf(fp, "Removed %d / %d bindsampler calls (%d%%)\n", bindsamps.first, bindsamps.second, bindsamps.first * 100 / bindsamps.second);
    if (uniforms.first) fprintf(fp, "Removed %d / %d relevant uniform calls (%d%%)\n", uniforms.first, uniforms.second, uniforms.first * 100 / uniforms.second);
    if (depthfuncs.first) fprintf(fp, "Removed %d / %d depth func calls (%d%%)\n", depthfuncs.first, depthfuncs.second, depthfuncs.first * 100 / depthfuncs.second);
    if (blendfuncs.first) fprintf(fp, "Removed %d / %d blend func calls (%d%%)\n", blendfuncs.first, blendfuncs.second, blendfuncs.first * 100 / blendfuncs.second);
    if (makecurr.first) fprintf(fp, "Removed %d / %d makecurrent calls (%d%%, at least %d were harmless on Mali)\n", makecurr.first, makecurr.second, makecurr.first * 100 / makecurr.second, makecurr_harmless);
    if (csb.first) fprintf(fp, "Removed %d / %d glCopyClientSideBuffer func calls (%d%%)\n", csb.first, csb.second, csb.first * 100 / csb.second);
    if (customcsv.first) fprintf(fp, "Removed %d / %d func calls from custom CSV (%d%%)\n", customcsv.first, dedups.second, customcsv.first * 100 / dedups.second);
}

int main(int argc, char **argv)
{
    int endframe = -1;
    int argIndex = 1;
    int flags = 0;
    for (; argIndex < argc; ++argIndex)
    {
        std::string arg = argv[argIndex];

        if (arg[0] != '-')
        {
            break;
        }
        else if (arg == "-h")
        {
            printHelp();
            return 1;
        }
        else if (arg == "--end")
        {
            argIndex++;
            endframe = atoi(argv[argIndex]);
        }
        else if (arg == "--last")
        {
            argIndex++;
            lastframe = atoi(argv[argIndex]);
        }
        else if (arg == "--buffers")
        {
            flags |= DEDUP_BUFFERS;
        }
        else if (arg == "--textures")
        {
            flags |= DEDUP_TEXTURES;
        }
        else if (arg == "--uniforms")
        {
            flags |= DEDUP_UNIFORMS;
        }
        else if (arg == "--scissors")
        {
            flags |= DEDUP_SCISSORS;
        }
        else if (arg == "--blendfunc")
        {
            flags |= DEDUP_BLENDFUNC;
        }
        else if (arg == "--depthfunc")
        {
            flags |= DEDUP_DEPTHFUNC;
        }
        else if (arg == "--vertexattr")
        {
            flags |= DEDUP_VERTEXATTRIB;
        }
        else if (arg == "--programs")
        {
            flags |= DEDUP_PROGRAMS;
        }
        else if (arg == "--csb")
        {
            flags |= DEDUP_CSB;
        }
        else if (arg == "--makecurrent")
        {
            flags |= DEDUP_MAKECURRENT;
        }
        else if (arg == "--all")
        {
            flags |= INT32_MAX;
        }
        else if (arg == "--enable")
        {
            flags |= DEDUP_ENABLE;
        }
        else if (arg == "--replace")
        {
            replace = true;
        }
        else if (arg == "--verbose")
        {
            verbose = true;
        }
        else if (arg == "-c")
        {
            onlycount = true;
        }
        else if (arg == "-d")
        {
            debug = true;
        }
        else if (arg == "--csv")
        {
            argIndex++;
            if (argIndex == argc) { printHelp(); return -3; }
            std::string filename = argv[argIndex];
            FILE* csvfp = fopen(filename.c_str(), "r");
            if (!csvfp) { std::cerr << "Error: Could not open file for reading: "  << filename << std::endl; return -5; };
            int r = -1;
            do {
                int start = -1;
                int end = -1;
                r = fscanf(csvfp, "%d,%d\n", &start, &end);
                if (r == 2) csv_ranges[start] = end;
            } while (r == 2);
            fclose(csvfp);
        }
        else if (arg == "-o")
        {
            argIndex++;
            if (argIndex == argc) { printHelp(); return -3; }
            std::string filename = argv[argIndex];
            fp = fopen(filename.c_str(), "w");
            if (!fp) { std::cerr << "Error: Could not open file for writing: "  << filename << std::endl; return -4; };
        }
        else if (arg == "-p")
        {
            patch = true;
        }
        else if (arg == "-v")
        {
            printVersion();
            return 0;
        }
        else
        {
            std::cerr << "Error: Unknown option " << arg << std::endl;
            printHelp();
            return 1;
        }
    }

    if ((argIndex + 2 > argc && !onlycount) || (onlycount && argIndex + 1 > argc))
    {
        printHelp();
        return 1;
    }
    std::string source_trace_filename = argv[argIndex++];
    ParseInterface inputFile(true);
    inputFile.setQuickMode(true);
    inputFile.setScreenshots(false);
    if (!inputFile.open(source_trace_filename))
    {
        std::cerr << "Failed to open for reading: " << source_trace_filename << std::endl;
        return 1;
    }

    Json::Value header = inputFile.header;
    if (header.isMember("multiThread") && header.get("multiThread", false).asBool())
    {
        fprintf(fp, "Is MultiThread trace.\n");
    }

    common::OutFile outputFile;
    if (patch)
    {
        const char* patchfilename = argv[argIndex++];
        patchfile = common::patchfile_open(inputFile.inputFile, patchfilename);
        DBG_LOG("Opened patchfile %s\n", patchfilename);
    }
    else if (!onlycount)
    {
        std::string target_trace_filename = argv[argIndex++];
        if (!outputFile.Open(target_trace_filename.c_str()))
        {
            std::cerr << "Failed to open for writing: " << target_trace_filename << std::endl;
            return 1;
        }
        Json::Value info;
        std::string cmdline;
        for (int i = 1; i < argc - 2; i++) { cmdline += argv[i]; if (i >= argc - 2) cmdline += std::string(" "); }
        info["command_line"] = cmdline;
        addConversionEntry(header, "deduplicator", source_trace_filename, info);
        Json::FastWriter writer;
        const std::string json_header = writer.write(header);
        outputFile.mHeader.jsonLength = json_header.size();
        outputFile.WriteHeader(json_header.c_str(), json_header.size());
    }
    deduplicate(inputFile, outputFile, endframe, header["defaultTid"].asInt(), flags);
    inputFile.close();
    if (!onlycount) outputFile.Close();
    if (patch) common::patchfile_close(patchfile);
    return 0;
}
