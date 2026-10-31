#pragma once
#include "resources.hpp"
#include "inventory.hpp"
namespace session {
inline assets::Bytes clean_cache(const assets::Bytes& input,size_t& removed){
    auto file=wire::Message::parse(input);removed=0;
    for(auto& f:file.fields)if(f.number==2&&f.type==2){auto cache=wire::Message::parse(f.data);bool dirty=false;
        for(auto& t:cache.fields)if(t.number==4&&t.type==2){auto type=wire::Message::parse(t.data);if(type.number(1)!=1)continue;
            std::erase_if(type.fields,[&](const wire::Field& o){if(o.number!=2||o.type!=2)return false;auto item=wire::Message::parse(o.data);if(!inventory::reserved(item.number(1)))return false;++removed;dirty=true;return true;});t.data=type.encode();}
        if(dirty){cache.erase(3);f.data=cache.encode();}
    }
    return removed?file.encode():input;
}
struct CacheGuard {
    HANDLE file=INVALID_HANDLE_VALUE;
    CacheGuard(const assets::fs::path& path,const assets::fs::path& backups){
        security::plain_path(path);security::private_directory(backups);
        if(assets::fs::exists(path)){
            if(assets::fs::file_size(path)>16*1024*1024)throw std::runtime_error("Inventory cache exceeds limit");
            auto before=assets::read(path);size_t removed=0;auto after=clean_cache(before,removed);
            if(removed){auto backup=backups/(path.filename().wstring()+L"."+std::to_wstring(GetTickCount64())+L".bak");assets::write(backup,before);assets::write(path,after);}
        }
        file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot protect Dota inventory cache; refusing local inventory changes");
    }
    ~CacheGuard(){if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);}
    CacheGuard(const CacheGuard&)=delete;
};
inline unsigned active_account(){DWORD account=0,n=sizeof(account);if(RegGetValueW(HKEY_CURRENT_USER,L"Software\\Valve\\Steam\\ActiveProcess",L"ActiveUser",RRF_RT_REG_DWORD,nullptr,&account,&n)!=ERROR_SUCCESS||!account)throw std::runtime_error("Active Steam account unavailable");return account;}
}
