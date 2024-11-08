#ifndef _TRACER_OUT_FILE_HPP_
#define _TRACER_OUT_FILE_HPP_

#include <stdio.h>
#include <errno.h>
#include <string>

#include <common/file_format.hpp>
#include <common/os_string.hpp>

namespace common {

/// We abuse the fact that Linux doesn't actually use allocated memory unless it is touched, and allocate an absurd amount of it.
/// That way we don't have to check for buffer overruns and Linux takes care of stitching memory together for us using its virtual
/// memory system.
#define SNAPPY_MAX_SIZE UINT32_MAX

/// Try to keep compression buffers below this size.
#define SNAPPY_CHUNK_SIZE (16*1024*1024)

class OutFile {
public:
    OutFile() { Init(); }
    OutFile(const char *name) { Init(); Open(name); }
    ~OutFile();

    bool Open(const char* name = NULL, bool writeSigBook = true, const std::vector<std::string> *sigbook = NULL, bool write_timestamp = false);
    void Close();
    void Flush();
    void WriteHeader(const char* buf, unsigned int len, bool verbose = true);

    /// Give us some scratch memory. You can call this before you have opened the output file.
    char* Scratch() const { return mCacheP; }

    /// Let us know how much memory we just used from our scratch memory.
    void Progress(ssize_t used) { mCacheP += used; if (UsedSize() > SNAPPY_CHUNK_SIZE) Flush(); }

    /// Deprecated legacy function that does a totally unnecessary memcpy.
    inline void Write(const void* buf, unsigned int len) { memcpy(mCacheP, buf, len); Progress(len); }

    std::string getFileName() const;

    common::BHeaderV3 mHeader;

private:
    void Init();
    inline ssize_t UsedSize() const { return mCacheP - mCache; }

    inline void filewrite(const char* ptr, size_t size)
    {
        size_t written = 0;
        int err = 0;
        do {
            written = fwrite(ptr, 1, size, mStream);
            ptr += written;
            size -= written;
            err = ferror(mStream);
        } while (size > 0 && (err == EAGAIN || err == EWOULDBLOCK || err == EINTR));
        if (err)
        {
            DBG_LOG("Failed to write: %s\n", strerror(err));
            os::abort();
        }
    }

    void WriteCompressedLength(unsigned int len)
    {
        unsigned char buf[4];
        buf[0] = len & 0xff; len >>= 8;
        buf[1] = len & 0xff; len >>= 8;
        buf[2] = len & 0xff; len >>= 8;
        buf[3] = len & 0xff; len >>= 8;
        filewrite((char*)buf, sizeof(buf));
    }

    void FlushHeader();
    void WriteSigBook(const std::vector<std::string> *sigbook, bool write_timestamp = false);
    os::String AutogenTraceFileName();

    bool                mIsOpen = false;
    FILE*               mStream = nullptr;
    char*               mCache = nullptr;
    char*               mCacheP = nullptr;
    char*               mCompressedCache = nullptr;
    std::string         mFileName;
};

}

#endif
