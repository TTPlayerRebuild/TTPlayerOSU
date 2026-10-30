#include "import_internal.h"
#include "export_internal.h"
#include <algorithm>
#include <cstring>
#include <set>

namespace ttp_osu::importer {
namespace {
namespace fs=std::filesystem;
[[noreturn]] void Bad(){throw Failure{HRESULT_FROM_WIN32(ERROR_INVALID_DATA)};}
struct Handle {
    HANDLE h{INVALID_HANDLE_VALUE};
    ~Handle(){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);}
};
struct Reader {
    std::vector<unsigned char> bytes;
    size_t position{};
    explicit Reader(const fs::path& path,const library::Context& context) {
        Handle file;file.h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,
            OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
        if(file.h==INVALID_HANDLE_VALUE)throw Failure{HRESULT_FROM_WIN32(GetLastError())};
        LARGE_INTEGER size{};
        if(!GetFileSizeEx(file.h,&size)||size.QuadPart<0||size.QuadPart>256ll*1024*1024)Bad();
        bytes.resize(static_cast<size_t>(size.QuadPart));
        size_t offset{};
        while(offset<bytes.size()) {
            context.Check();DWORD read{};
            if(!ReadFile(file.h,bytes.data()+offset,static_cast<DWORD>(std::min<size_t>(65536,bytes.size()-offset)),&read,nullptr)||!read)Bad();
            offset+=read;
        }
    }
    void Skip(size_t n){if(n>bytes.size()-position)Bad();position+=n;}
    uint8_t U8(){if(position==bytes.size())Bad();return bytes[position++];}
    uint32_t U32(){uint32_t n{};if(bytes.size()-position<4)Bad();std::memcpy(&n,bytes.data()+position,4);position+=4;return n;}
    uint32_t Count(){const auto n=U32();if(n>1000000)Bad();return n;}
    std::wstring String() {
        const auto marker=U8();if(!marker)return {};if(marker!=11)Bad();
        uint32_t size{};
        for(unsigned shift=0;;shift+=7) {
            if(shift>28)Bad();const auto b=U8();if(shift==28&&(b&0xf0))Bad();
            size|=uint32_t(b&127)<<shift;if(!(b&128))break;
        }
        if(size>131072||size>bytes.size()-position)Bad();
        if(!size)return {};
        const auto* value=reinterpret_cast<const char*>(bytes.data()+position);
        const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value,static_cast<int>(size),nullptr,0);
        if(count<=0||count>32767)Bad();std::wstring result(count,L'\0');
        if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value,static_cast<int>(size),result.data(),count)!=count||result.find(L'\0')!=result.npos)Bad();
        position+=size;return result;
    }
};
struct Record {std::wstring relative,hash;};
std::vector<Record> ReadIndex(const fs::path& path,const library::Context& context) {
    Reader r(path,context);const auto version=r.U32();
    if(version<20140609||version>20991231)throw Failure{HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)};
    r.Skip(4+1+8);r.String();const auto count=r.Count();
    std::vector<Record> records;records.reserve(count);
    // Layout matches the inspected stable 0x0600236C/0x0600236D, including
    // 20160408 entry lengths, their 20191106 removal and 20250107 float stars.
    for(uint32_t i=0;i<count;++i) {
        context.Check();uint32_t length{};
        if(version>=20160408&&version<20191106)length=r.U32();
        const size_t begin=r.position;
        for(int k=0;k<7;++k)r.String();
        auto hash=r.String(),name=r.String();r.Skip(1+6+8+16+8);
        for(int mode=0;mode<4;++mode) {
            const auto pairs=r.Count();
            for(uint32_t k=0;k<pairs;++k) {
                if(r.U8()!=8)Bad();r.Skip(4);
                if(r.U8()!=(version>=20250107?12:13))Bad();r.Skip(version>=20250107?4:8);
            }
        }
        r.Skip(12);const auto timings=r.Count();r.Skip(size_t(timings)*17);
        r.Skip(12+4+2+4+1);r.String();r.String();r.Skip(2);r.String();
        r.Skip(1+8);const bool osz2=r.U8()!=0;auto folder=r.String();r.Skip(8+5+4+1);
        if(length&&r.position-begin!=length)Bad();
        if(osz2||hash.size()!=32||name.empty()||folder.empty())continue;
        try {records.push_back({Fold(library::ExportMemberName(folder+L"/"+name)),Fold(hash)});}
        catch(const Failure&) {} // A bad entry may not become an arbitrary file path.
    }
    r.Skip(4);if(r.position!=r.bytes.size())Bad();
    return records;
}
}

std::vector<InstalledMember> MatchStable(const fs::path& executable,const fs::path& songs,
    const std::vector<Member>& members,const library::Context& context) {
    const auto index=ReadIndex(executable.parent_path()/L"osu!.db",context);
    const auto first=std::find_if(members.begin(),members.end(),[](const auto& m){return m.beatmap;});
    if(first==members.end())return {};
    const auto suffix=L"/"+Fold(first->name),hash=Fold(first->md5);
    std::map<std::wstring,std::wstring> hashes;
    for(const auto& entry:index)hashes.emplace(entry.relative,entry.hash);
    std::set<std::wstring> tried;
    for(const auto& entry:index) {
        context.Check();
        if(entry.hash!=hash||!entry.relative.ends_with(suffix))continue;
        const auto folder=entry.relative.substr(0,entry.relative.size()-suffix.size());
        if(!tried.insert(folder).second)continue;
        bool matched=true;
        for(const auto& member:members)if(member.beatmap) {
            const auto it=hashes.find(folder+L"/"+Fold(member.name));
            if(it==hashes.end()||it->second!=Fold(member.md5)){matched=false;break;}
        }
        if(!matched)continue;
        std::vector<InstalledMember> files;files.reserve(members.size());
        for(size_t i=0;i<members.size();++i)files.push_back({songs/fs::path(folder)/fs::path(members[i].name),i});
        if(VerifyFiles(files,members,context))return files;
    }
    return {};
}
}
