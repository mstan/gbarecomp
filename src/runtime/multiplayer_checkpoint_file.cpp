#include "multiplayer_checkpoint.h"
#include <atomic>
#include <fstream>
#include <system_error>
#include <algorithm>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace gbarecomp {
namespace {
[[noreturn]] void io_error(const char* action) {
#ifdef _WIN32
    const auto code=static_cast<int>(GetLastError());
#else
    const auto code=errno;
#endif
    throw std::system_error(code,std::system_category(),action);
}
struct PendingFile {
    std::filesystem::path path;
#ifdef _WIN32
    HANDLE handle=INVALID_HANDLE_VALUE;
#else
    int handle=-1;
#endif
    bool owned=false;
    ~PendingFile() {
#ifdef _WIN32
        if (handle!=INVALID_HANDLE_VALUE) CloseHandle(handle);
#else
        if (handle>=0) ::close(handle);
#endif
        if (owned) { std::error_code ignored; std::filesystem::remove(path,ignored); }
    }
};
}
bool gba_store_agreed_checkpoint(const GbaNetplayCheckpointAgreement& agreement,
                                 const std::filesystem::path& path,std::string* error) {
    try {
        const auto bytes=agreement.archive(); // fail before any IO unless agreed
        if (path.empty() || path.filename().empty()) throw std::invalid_argument("checkpoint path has no filename");
        const auto target=std::filesystem::absolute(path);
        static std::atomic<std::uint64_t> sequence{0};
#ifdef _WIN32
        const auto pid=GetCurrentProcessId();
#else
        const auto pid=::getpid();
#endif
        PendingFile temp;
        for (unsigned attempt=0;attempt<16;++attempt) {
            temp.path=target;
            temp.path+=".tmp-"+std::to_string(pid)+"-"+std::to_string(sequence.fetch_add(1));
#ifdef _WIN32
            temp.handle=CreateFileW(temp.path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
            if (temp.handle!=INVALID_HANDLE_VALUE) { temp.owned=true; break; }
            if (GetLastError()!=ERROR_FILE_EXISTS && GetLastError()!=ERROR_ALREADY_EXISTS) io_error("create checkpoint staging file");
#else
            temp.handle=::open(temp.path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
            if (temp.handle>=0) { temp.owned=true; break; }
            if (errno!=EEXIST) io_error("create checkpoint staging file");
#endif
        }
        if (!temp.owned) throw std::runtime_error("checkpoint staging names are occupied");
        std::size_t offset=0;
        while (offset<bytes.size()) {
#ifdef _WIN32
            DWORD written=0;
            if (!WriteFile(temp.handle,bytes.data()+offset,
                static_cast<DWORD>(std::min<std::size_t>(bytes.size()-offset,1024*1024)),&written,nullptr))
                io_error("write checkpoint");
            if (!written) throw std::runtime_error("short checkpoint write");
#else
            const auto written=::write(temp.handle,bytes.data()+offset,bytes.size()-offset);
            if (written<0) { if (errno==EINTR) continue; io_error("write checkpoint"); }
            if (!written) throw std::runtime_error("short checkpoint write");
#endif
            offset+=static_cast<std::size_t>(written);
        }
#ifdef _WIN32
        if (!FlushFileBuffers(temp.handle)) io_error("flush checkpoint");
        if (!CloseHandle(temp.handle)) io_error("close checkpoint");
        temp.handle=INVALID_HANDLE_VALUE;
        if (!MoveFileExW(temp.path.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
            io_error("replace paired checkpoint");
        temp.owned=false;
#else
        if (::fsync(temp.handle)) io_error("flush checkpoint");
        const auto fd=temp.handle; temp.handle=-1;
        if (::close(fd)) io_error("close checkpoint");
        if (::rename(temp.path.c_str(),target.c_str())) io_error("replace paired checkpoint");
        temp.owned=false;
        const int directory=::open(target.parent_path().c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
        if (directory<0) io_error("open checkpoint directory for flush");
        const auto flushed=::fsync(directory); const auto saved_errno=errno;
        ::close(directory); errno=saved_errno;
        if (flushed) io_error("flush checkpoint directory");
#endif
        if (error) error->clear();
        return true;
    } catch (const std::exception& e) { if (error) *error=e.what(); return false; }
}
bool gba_load_checkpoint_archive(const std::filesystem::path& path,const std::string& identity,
                                 GbaConfirmedCheckpoint& out,std::string* error) {
    try {
        std::ifstream file(path,std::ios::binary|std::ios::ate);
        if (!file) throw std::runtime_error("cannot open paired checkpoint");
        const auto length=file.tellg();
        if (length<=0 || length>8*1024*1024+1100) throw std::runtime_error("invalid paired checkpoint file size");
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
        file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(bytes.data()),bytes.size())) throw std::runtime_error("short checkpoint read");
        return gba_decode_checkpoint_archive(bytes,identity,out,error);
    } catch (const std::exception& e) { if (error) *error=e.what(); return false; }
}
}
