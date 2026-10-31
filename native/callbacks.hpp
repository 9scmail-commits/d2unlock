#pragma once
#include <windows.h>
#include "symbol.hpp"
#include <cstdint>
#include <vector>
#include <stdexcept>
#include <cstring>

namespace callbacks {
struct Message { int user; int id; unsigned char* parameter; int size; };
static_assert(sizeof(Message)==24 && offsetof(Message,parameter)==8);
using Get = bool (*)(int,Message*);
using Free = void (*)(int);

class Hooks {
    struct Slot { void** address; void* before; void* after; bool applied=false; };
    std::vector<Slot> slots;
public:
    Get get=nullptr;
    Free release=nullptr;
    void prepare(HMODULE api,HMODULE provider,Get next_get,Free next_free) {
        get=symbol<Get>(provider,"Steam_BGetCallback");
        release=symbol<Free>(provider,"Steam_FreeLastCallback");
        if(!get||!release)throw std::runtime_error("Steam callback exports unavailable");
        auto base=reinterpret_cast<unsigned char*>(api);
        auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if(dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<0||dos->e_lfanew>1024*1024)throw std::runtime_error("Invalid Steam API image");
        auto nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
        if(nt->Signature!=IMAGE_NT_SIGNATURE||nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC||nt->FileHeader.NumberOfSections>96)throw std::runtime_error("Invalid Steam API PE header");
        auto section=IMAGE_FIRST_SECTION(nt);unsigned gets=0,frees=0;
        for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i) {
            const auto& s=section[i];if(!(s.Characteristics&IMAGE_SCN_MEM_WRITE)||(s.Characteristics&IMAGE_SCN_MEM_EXECUTE))continue;
            auto start=s.VirtualAddress,size=s.Misc.VirtualSize;
            if(start>nt->OptionalHeader.SizeOfImage||size>nt->OptionalHeader.SizeOfImage-start||size>16*1024*1024)throw std::runtime_error("Invalid Steam API data section");
            std::vector<unsigned char> bytes(size);SIZE_T count=0;
            if(!ReadProcessMemory(GetCurrentProcess(),base+start,bytes.data(),size,&count)||count!=size)continue;
            for(size_t at=(8-start%8)%8;at+sizeof(void*)<=bytes.size();at+=sizeof(void*)) {
                void* value=nullptr;std::memcpy(&value,bytes.data()+at,sizeof(value));void* after=nullptr;
                if(value==reinterpret_cast<void*>(get)){after=reinterpret_cast<void*>(next_get);++gets;}
                else if(value==reinterpret_cast<void*>(release)){after=reinterpret_cast<void*>(next_free);++frees;}
                if(after)slots.push_back({reinterpret_cast<void**>(base+start+at),value,after,false});
            }
        }
        if(!gets||!frees||gets>8||frees>8)throw std::runtime_error("Steam callback dispatcher layout unsupported");
    }
    void apply() {
        for(int pass=0;pass<2;++pass)for(auto& s:slots) {
            if((s.before==reinterpret_cast<void*>(release))!=(pass==0))continue;
            if(InterlockedCompareExchangePointer(s.address,s.after,s.before)!=s.before){restore();throw std::runtime_error("Steam callback pointer changed during setup");}
            s.applied=true;
        }
    }
    void restore() noexcept {
        for(auto it=slots.rbegin();it!=slots.rend();++it)if(it->applied){InterlockedCompareExchangePointer(it->address,it->before,it->after);it->applied=false;}
    }
};
}
