// Swiss army knife tool for patrace - for all kinds of misc stuff

#include <utility>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES3/gl31.h>
#include <GLES3/gl32.h>
#include <limits.h>
#include <set>
#include <unordered_map>

#include "tool/parse_interface.h"

#include "common/in_file.hpp"
#include "common/file_format.hpp"
#include "common/api_info.hpp"
#include "common/parse_api.hpp"
#include "common/trace_model.hpp"
#include "common/gl_utility.hpp"
#include "common/os.hpp"
#include "eglstate/context.hpp"
#include "tool/config.hpp"
#include "base/base.hpp"
#include "tool/utils.hpp"

#pragma GCC diagnostic ignored "-Wunused-variable"

#define DEBUG_LOG(...) if (debug) DBG_LOG(__VA_ARGS__)

static common::patchfile pf;
static bool patch = false;
static bool debug = false;
static bool onlycount = false;
static bool verbose = false;
static int endframe = -1;
static bool waitsync = false;
static bool removesync = false;
static bool removeTS = false;
static std::set<int> unused_mipmaps; // call numbers
static std::set<std::pair<int, int>> unused_textures; // context index + texture index
static std::set<std::pair<int, int>> unused_buffers; // context index + buffer index
static std::set<std::pair<int, int>> uninit_textures; // context index + texture index
static int lastframe = -1;
static bool cull_error = false;
static std::set<std::pair<int, int>> used_shaders; // context index + shader index
static bool remove_unused_shaders = false;
struct sumtype { std::string md5; GLint attachment; };
static std::unordered_map<int, sumtype> checksums; // call number -> checksum

static void printHelp()
{
    std::cout <<
        "Usage : converter [OPTIONS] trace_file.pat new_file.pat\n"
        "Options:\n"
        "  --waitsync    Add a non-zero timeout to glClientWaitSync and glWaitSync calls\n"
        "  --removesync  Remove all sync calls\n"
        "  --mipmap FILE Remove unused glGenerateMipmap calls. Need a usage CSV file from analyze_trace as input\n"
        "  --tex FILE    Remove unused texture calls. Need a usage CSV file from analyze_trace as input\n"
        "  --buf FILE    Remove unused buffer calls. Need a usage CSV file from analyze_trace as input\n"
        "  --utex FILE   Fix uninitialized texture storage calls. Need an uninitialized CSV file as input\n"
        "  --removeTS    Remove all timestamp calls\n"
        "  --shaders     Remove unused shaders\n"
        "  --end FRAME   End frame (terminates trace here)\n"
        "  --last FRAME  Stop doing changes at this frame (copies the remaining trace without changes)\n"
        "  --verbose     Print more information while running\n"
        "  --addsum CSV  Add checksums from CSV to the new file\n"
        "  -p            Instead of generate a new trace file, put changes into a patch file\n"
        "  -c            Only count and report instances, do no changes\n"
        "  -G            Remove all glGetError and eglGetError calls\n"
        "  -d            Print debug messages\n"
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
    if (patch || onlycount) return;
    const unsigned int WRITE_BUF_LEN = 150*1024*1024;
    static char buffer[WRITE_BUF_LEN];
    char *dest = buffer;
    dest = call->Serialize(dest, -1, injected);
    outputFile.Write(buffer, dest - buffer);
}

static void addout(common::OutFile &outputFile, common::CallTM *call, common::CallTM* provoking)
{
    assert(call->mCallId != 0);
    call->mTid = provoking->mTid;
    call->mCallNo = provoking->mCallNo;
    if (patch)
    {
        common::patchfile_insert_before(pf, *call);
        return;
    }
    if (onlycount) return;
    const unsigned int WRITE_BUF_LEN = 150*1024*1024;
    static char buffer[WRITE_BUF_LEN];
    char *dest = buffer;
    dest = call->Serialize(dest, -1, true);
    outputFile.Write(buffer, dest - buffer);
}

static void removeout(common::OutFile &outputFile, common::CallTM *call)
{
    if (patch)
    {
        common::patchfile_remove(pf, call->mCallNo);
        return;
    }
}

static void prepass(const std::string& source_trace_filename)
{
    ParseInterface input(true);
    input.setQuickMode(true);
    input.setScreenshots(false);

    if (!input.open(source_trace_filename))
    {
        std::cerr << "Failed to open for reading: " << source_trace_filename << std::endl;
        exit(-1);
    }

    common::CallTM *call = nullptr;
    while ((call = input.next_call()))
    {

        if (call->mCallName == "glAttachShader")
        {
            const GLenum program_id = call->mArgs[0]->GetAsUInt();
            const GLenum shader_id = call->mArgs[1]->GetAsUInt();
            const int program_index = input.contexts[input.context_index].programs.remap(program_id);
            const int shader_index = input.contexts[input.context_index].shaders.remap(shader_id);
            used_shaders.insert(std::make_pair(input.context_index, shader_index));
        }
    }
}

static void converter(ParseInterface& input, const std::string& source_trace_filename, common::OutFile& outputFile)
{
    common::CallTM *call = nullptr;
    int count = 0;
    Json::Value header = input.header;

    // Go through entire trace file
    while ((call = input.next_call()))
    {
        if (lastframe != -1 && input.frames >= lastframe)
        {
            writeout(outputFile, call);
            continue;
        }

        if (checksums.size() > 0 && checksums.count(call->mCallNo) > 0)
        {
            common::CallTM newcall("glAssertFramebuffer_ARM");
            newcall.mArgs.push_back(new common::ValueTM((GLenum)GL_DRAW_FRAMEBUFFER));
            newcall.mArgs.push_back(new common::ValueTM((GLint)checksums.at(call->mCallNo).attachment));
            newcall.mArgs.push_back(new common::ValueTM(checksums.at(call->mCallNo).md5));
            addout(outputFile, &newcall, call);
            count++;
        }

        if (remove_unused_shaders && (call->mCallName == "glShaderSource" || call->mCallName == "glCompileShader" || call->mCallName == "glGetShaderiv"
            || call->mCallName == "glDeleteShader" || call->mCallName == "glIsShader" || call->mCallName == "glGetShaderSource" || call->mCallName == "glGetShaderInfoLog"
            || call->mCallName == "glGetShaderInfoLog"))
        {
            const GLuint shader_id = call->mArgs[0]->GetAsUInt();
            int shader_index = UNBOUND;
            if (call->mCallName == "glDeleteShader") // a bit more complicated since we've already removed it from the remapping table by now
            {
                for (const auto& s : input.contexts[input.context_index].shaders) if (s.id == shader_id) shader_index = s.index;
            }
            else
            {
                shader_index = input.contexts[input.context_index].shaders.remap(shader_id);
            }
            if (used_shaders.count(std::make_pair(input.context_index, shader_index)) == 0) { count++; continue; }
        }
        else if (remove_unused_shaders && call->mCallName == "glCreateShader")
        {
            const GLuint shader_id = call->mRet.GetAsUInt();
            const int shader_index = input.contexts[input.context_index].shaders.remap(shader_id);
            if (used_shaders.count(std::make_pair(input.context_index, shader_index)) == 0) { count++; continue; }
        }

        if (call->mCallName == "glClientWaitSync")
        {
            const uint64_t timeout = call->mArgs[2]->GetAsUInt64();
            if (timeout == 0 && waitsync)
            {
                call->mArgs[2]->mUint64 = UINT64_MAX;
                count++;
            }
            if (!removesync) writeout(outputFile, call);
            else { removeout(outputFile, call); count++; }
        }
        else if (call->mCallName == "glDeleteSync" && removesync) { removeout(outputFile, call); count++; }
        else if (call->mCallName == "glFenceSync" && removesync) { removeout(outputFile, call); count++; }
        else if (call->mCallName == "glGetSynciv" && removesync) { removeout(outputFile, call); count++; }
        else if (call->mCallName == "glIsSync" && removesync) { removeout(outputFile, call); count++; }
        else if (call->mCallName == "glWaitSync" && removesync) { removeout(outputFile, call); count++; }
        else if (call->mCallName == "eglGetSyncAttribKHR" && removesync) { removeout(outputFile, call); count++; }
        else if (call->mCallName == "eglDestroySyncKHR" && removesync) { removeout(outputFile, call); count++; }
        else if (call->mCallName == "eglCreateSyncKHR" && removesync) { removeout(outputFile, call); count++; }
        else if (call->mCallName == "eglClientWaitSyncKHR" && removesync) { removeout(outputFile, call); count++; }
        else if (call->mCallName == "glGenerateMipmap" && unused_mipmaps.count(call->mCallNo) > 0) { removeout(outputFile, call); count++; }
        else if (call->mCallName == "glBufferData")
        {
            const GLenum target = call->mArgs[0]->GetAsUInt();
            const StateTracker::VertexArrayObject& vao = input.contexts[input.context_index].vaos.at(input.contexts[input.context_index].vao_index);
            if (vao.boundBufferIds.count(target) == 0 || vao.boundBufferIds.at(target).count(0) == 0)
            {
                printf("%d : no bound buffer!\n", (int)call->mCallNo);
                abort();
            }
            const GLuint id = vao.boundBufferIds.at(target).at(0).buffer;
            if (id == 0)
            {
                writeout(outputFile, call);
                continue;
            }
            if (!input.contexts[input.context_index].buffers.contains(id))
            {
                printf("%d : buffer %d not tracked!\n", (int)call->mCallNo, (int)id);
                abort();
            }
            const int buffer_index = input.contexts[input.context_index].buffers.remap(id);
            if (unused_buffers.count(std::make_pair(input.context_index, buffer_index)) == 0)
            {
                writeout(outputFile, call);
            } // else skip it
            else
            {
                count++;
                removeout(outputFile, call);
            }
        }
        else if ((call->mCallName == "eglGetError" || call->mCallName == "glGetError") && cull_error)
        {
            // don't output it
            count++;
            removeout(outputFile, call);
        }
        else if (removeTS && call->mCallName == "paTimestamp")
        {
            count++; // don't output it
            removeout(outputFile, call);
        }
        else if (call->mCallName == "glTexStorage3D" || call->mCallName == "glTexStorage2D" || call->mCallName == "glTexStorage1D"
             || call->mCallName == "glTexStorage3DEXT" || call->mCallName == "glTexStorage2DEXT" || call->mCallName == "glTexStorage1DEXT"
             || call->mCallName == "glTexImage3D" || call->mCallName == "glTexImage2D" || call->mCallName == "glTexImage1D" || call->mCallName == "glTexImage3DOES"
             || call->mCallName == "glCompressedTexImage3D" || call->mCallName == "glCompressedTexImage2D"
             || call->mCallName == "glCompressedTexImage1D" || call->mCallName == "glTexSubImage1D" || call->mCallName == "glTexSubImage2D"
             || call->mCallName == "glTexSubImage3D" || call->mCallName == "glCompressedTexSubImage2D" || call->mCallName == "glCompressedTexSubImage3D")
        {
            const GLenum target = interpret_texture_target(call->mArgs[0]->GetAsUInt());
            const GLuint unit = input.contexts[input.context_index].activeTextureUnit;
            const GLuint tex_id = input.contexts[input.context_index].textureUnits[unit][target];
            assert(tex_id != 0);
            const int target_texture_index = input.contexts[input.context_index].textures.remap(tex_id);
            if (input.contexts[input.context_index].textures.contains(tex_id) && tex_id != 0)
            {
                if (unused_textures.count(std::make_pair(input.context_index, target_texture_index)) > 0)
                {
                    count++;
                    removeout(outputFile, call);
                    continue;
                }
            }
            writeout(outputFile, call);
            if (call->mCallName == "glTexImage2D" && uninit_textures.count(std::make_pair(input.context_index, target_texture_index)) > 0)
            {
                const GLint level = call->mArgs[1]->GetAsUInt();
                //const GLint internalFormat = call->mArgs[2]->GetAsUInt();
                const GLsizei width = call->mArgs[3]->GetAsUInt();
                const GLsizei height = call->mArgs[4]->GetAsUInt();
                //GLint border = call->mArgs[5]->GetAsUInt();
                const GLenum format = call->mArgs[6]->GetAsUInt();
                const GLenum type = call->mArgs[7]->GetAsUInt();
                common::CallTM c("glTexSubImage2D");
                c.mArgs.push_back(new common::ValueTM(target));
                c.mArgs.push_back(new common::ValueTM(level)); // level
                c.mArgs.push_back(new common::ValueTM(0)); // xoffset
                c.mArgs.push_back(new common::ValueTM(0)); // yoffset
                c.mArgs.push_back(new common::ValueTM(width));
                c.mArgs.push_back(new common::ValueTM(height));
                c.mArgs.push_back(new common::ValueTM(format));
                c.mArgs.push_back(new common::ValueTM(type));
                const unsigned tsize = width * height * 4 * 4; // max size
                std::vector<char> zeroes(tsize);
                c.mArgs.push_back(common::CreateBlobOpaqueValue(tsize, zeroes.data()));
                addout(outputFile, &c, call);
                count++;
            }
            else if ((call->mCallName == "glTexStorage3D" || call->mCallName == "glTexImage3D") && uninit_textures.count(std::make_pair(input.context_index, target_texture_index)) > 0)
            {
                printf("Support for removing unused 3D textures not implemented yet!\n");
                assert(false); // TBD
            }
            else if ((call->mCallName == "glTexStorage2DMultisample" || call->mCallName == "glTexStorage2D") && uninit_textures.count(std::make_pair(input.context_index, target_texture_index)) > 0)
            {
                const GLsizei levels = call->mArgs[1]->GetAsUInt();
                const GLenum format = call->mArgs[2]->GetAsUInt();
                const GLsizei width = call->mArgs[3]->GetAsUInt();
                const GLsizei height = call->mArgs[4]->GetAsUInt();
                if (isCompressedFormat(format))
                {
                    for (int i = 0; i < levels; i++)
                    {
                        const GLsizei w = width / (i + 1);
                        const GLsizei h = height / (i + 1);
                        common::CallTM c("glCompressedTexSubImage2D");
                        c.mArgs.push_back(new common::ValueTM(target));
                        c.mArgs.push_back(new common::ValueTM(i)); // level
                        c.mArgs.push_back(new common::ValueTM(0)); // xoffset
                        c.mArgs.push_back(new common::ValueTM(0)); // yoffset
                        c.mArgs.push_back(new common::ValueTM(w));
                        c.mArgs.push_back(new common::ValueTM(h));
                        c.mArgs.push_back(new common::ValueTM(format));
                        const unsigned tsize = w * h * 4 * 4; // max size
                        c.mArgs.push_back(new common::ValueTM(tsize)); // image size
                        std::vector<char> zeroes(tsize);
                        c.mArgs.push_back(common::CreateBlobOpaqueValue(tsize, zeroes.data()));
                        addout(outputFile, &c, call);
                    }
                }
                else // uncompressed
                {
                    for (int i = 0; i < levels; i++)
                    {
                        const GLsizei w = width / (i + 1);
                        const GLsizei h = height / (i + 1);
                        common::CallTM c("glTexSubImage2D");
                        c.mArgs.push_back(new common::ValueTM(target));
                        c.mArgs.push_back(new common::ValueTM(i)); // level
                        c.mArgs.push_back(new common::ValueTM(0)); // xoffset
                        c.mArgs.push_back(new common::ValueTM(0)); // yoffset
                        c.mArgs.push_back(new common::ValueTM(w));
                        c.mArgs.push_back(new common::ValueTM(h));
                        c.mArgs.push_back(new common::ValueTM(sized_to_unsized_format(format)));
                        c.mArgs.push_back(new common::ValueTM(sized_to_unsized_type(format)));
                        const unsigned tsize = w * h * 4 * 4; // max size
                        std::vector<char> zeroes(tsize);
                        c.mArgs.push_back(common::CreateBlobOpaqueValue(tsize, zeroes.data()));
                        addout(outputFile, &c, call);
                    }
                }
                count++;
            }
        }
        else
        {
            writeout(outputFile, call);
        }
    }
    input.close();
    printf("Calls changed: %d\n", count);
    if (!onlycount && !patch)
    {
        Json::Value info;
        info["count"] = count;
        if (waitsync) info["waitsync"] = true;
        if (removesync) info["removesync"] = true;
        if (unused_mipmaps.size() > 0) info["remove_unused_mipmaps"] = true;
        if (unused_textures.size() > 0) info["remove_unused_textures"] = true;
        if (uninit_textures.size() > 0) info["remove_uninitialized_textures"] = true;
        if (endframe != -1) info["endframe"] = endframe;
        if (lastframe != -1) info["lastframe"] = lastframe;
        if (removeTS)
        {
            header.removeMember("timestamping");
            header.removeMember("first_timestamp");
            info["remove_timestamps"] = true;
        }
        addConversionEntry(header, "converter", source_trace_filename, info);
        Json::FastWriter writer;
        const std::string json_header = writer.write(header);
        outputFile.mHeader.jsonLength = json_header.size();
        outputFile.WriteHeader(json_header.c_str(), json_header.size());
    }
}

int main(int argc, char **argv)
{
    int argIndex = 1;
    bool do_prepass = false;
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
        else if (arg == "--verbose")
        {
            verbose = true;
        }
        else if (arg == "--waitsync")
        {
            waitsync = true;
        }
        else if (arg == "--removesync")
        {
            removesync = true;
        }
        else if (arg == "--shaders")
        {
            remove_unused_shaders = true;
            do_prepass = true;
        }
        else if (arg == "-p")
        {
            patch = true;
        }
        else if (arg == "-c")
        {
            onlycount = true;
        }
        else if (arg == "-G")
        {
            cull_error = true;
        }
        else if (arg == "-d")
        {
            debug = true;
        }
        else if (arg == "--removeTS")
        {
            removeTS = true;
        }
        else if (arg == "--addsum")
        {
            argIndex++;
            FILE* fp = fopen(argv[argIndex], "r");
            if (!fp) { printf("Error: Unable to open %s: %s\n", argv[argIndex], strerror(errno)); return -11; }
            int call = -1;
            GLint attachment = -1;
            char checksum[200];
            int ignore = fscanf(fp, "%*[^\n]\n");
            (void)ignore;
            while (fscanf(fp, "%d,%d,%*d,%*d,%*d,%s\n", &call, &attachment, checksum) == 3) checksums[call] = sumtype{ checksum, attachment };
            DBG_LOG("Loaded %d checksums from %s\n", (int)checksums.size(), argv[argIndex]);
            fclose(fp);
        }
        else if (arg == "--mipmap")
        {
            argIndex++;
            FILE* fp = fopen(argv[argIndex], "r");
            if (!fp) { printf("Error: Unable to open %s: %s\n", argv[argIndex], strerror(errno)); return -11; }
            int call = -1;
            int ignore = fscanf(fp, "%*[^\n]\n");
            (void)ignore;
            while (fscanf(fp, "%d,%*d,%*d,%*d\n", &call) == 1) unused_mipmaps.insert(call);
            fclose(fp);
        }
        else if (arg == "--utex")
        {
            argIndex++;
            FILE* fp = fopen(argv[argIndex], "r");
            if (!fp) { printf("Error: Unable to open %s: %s\n", argv[argIndex], strerror(errno)); return -11; }
            int ctxidx = 0;
            int txidx = 0;
            int ignore = fscanf(fp, "%*[^\n]\n"); // skip header line
            (void)ignore;
            // Call,Frame,TxIndex,TxId,ContextIndex,ContextId
            while (fscanf(fp, "%*d,%*d,%d,%*d,%d,%*d\n", &txidx, &ctxidx) == 2)
            {
                uninit_textures.insert(std::make_pair(ctxidx, txidx));
            }
            fclose(fp);
        }
        else if (arg == "--tex")
        {
            argIndex++;
            FILE* fp = fopen(argv[argIndex], "r");
            if (!fp) { printf("Error: Unable to open %s: %s\n", argv[argIndex], strerror(errno)); return -11; }
            int ctxidx = 0;
            int txidx = 0;
            int ignore = fscanf(fp, "%*[^\n]\n");
            (void)ignore;
            while (fscanf(fp, "%*d,%*d,%d,%*d,%d,%*d\n", &txidx, &ctxidx) == 2) unused_textures.insert(std::make_pair(ctxidx, txidx));
            fclose(fp);
        }
        else if (arg == "--buf")
        {
            argIndex++;
            FILE* fp = fopen(argv[argIndex], "r");
            if (!fp) { printf("Error: Unable to open %s: %s\n", argv[argIndex], strerror(errno)); return -11; }
            int ctxidx = 0;
            int bufidx = 0;
            int ignore = fscanf(fp, "%*[^\n]\n");
            (void)ignore;
            while (fscanf(fp, "%*d,%*d,%d,%*d,%d,%*d\n", &bufidx, &ctxidx) == 2) unused_buffers.insert(std::make_pair(ctxidx, bufidx));
            fclose(fp);
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
    if (do_prepass) prepass(source_trace_filename);
    ParseInterface input(true);
    input.setQuickMode(true);
    input.setScreenshots(false);
    if (!input.open(source_trace_filename))
    {
        std::cerr << "Failed to open for reading: " << source_trace_filename << std::endl;
        exit(-2);
    }
    common::OutFile outputFile;
    if (patch)
    {
        std::string target_trace_filename = argv[argIndex++];
        pf = common::patchfile_open(input.inputFile, target_trace_filename.c_str());
        DBG_LOG("Opened patchfile %s\n", target_trace_filename.c_str());
    }
    else if (!onlycount && !patch)
    {
        std::string target_trace_filename = argv[argIndex++];
        if (!outputFile.Open(target_trace_filename.c_str()))
        {
            std::cerr << "Failed to open for writing: " << target_trace_filename << std::endl;
            return 1;
        }
    }
    converter(input, source_trace_filename, outputFile);
    if (!onlycount && !patch) outputFile.Close();
    if (patch) common::patchfile_close(pf);
    return 0;
}
