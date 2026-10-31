#pragma once
#include <windows.h>
#include <sddl.h>
#include <aclapi.h>
#include <filesystem>
#include <vector>
#include <span>
#include <stdexcept>
namespace security {
inline void plain_path(const std::filesystem::path& p){
    auto full=std::filesystem::absolute(p).lexically_normal(),part=full.root_path();
    for(const auto& component:full.relative_path()){part/=component;auto a=GetFileAttributesW(part.c_str());if(a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("Links are not permitted in application data paths");}
}
inline void private_directory(const std::filesystem::path& p){
    plain_path(p);std::filesystem::create_directories(p);HANDLE token=nullptr;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))throw std::runtime_error("Cannot identify file owner");
    DWORD n=0;GetTokenInformation(token,TokenUser,nullptr,0,&n);std::vector<unsigned char> data(n);
    bool ok=GetTokenInformation(token,TokenUser,data.data(),n,&n)!=0;CloseHandle(token);if(!ok)throw std::runtime_error("Cannot read file owner");
    LPWSTR sid=nullptr;if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid,&sid))throw std::runtime_error("Invalid file owner");
    std::wstring acl=L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;"+std::wstring(sid)+L")";LocalFree(sid);
    PSECURITY_DESCRIPTOR desc=nullptr;if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(),SDDL_REVISION_1,&desc,nullptr))throw std::runtime_error("Invalid private ACL");
    PACL dacl=nullptr;BOOL present=false,def=false;GetSecurityDescriptorDacl(desc,&present,&dacl,&def);
    auto result=SetNamedSecurityInfoW(const_cast<wchar_t*>(p.c_str()),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,nullptr,nullptr,dacl,nullptr);LocalFree(desc);
    if(result!=ERROR_SUCCESS)throw std::runtime_error("Cannot protect application directory");
}
inline void write(const std::filesystem::path& p,std::span<const unsigned char> bytes){
    plain_path(p);auto temp=p;temp+=L"."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetCurrentThreadId())+L"."+std::to_wstring(GetTickCount64())+L".tmp";
    HANDLE h=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(h==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot create private temporary file");
    DWORD actual=0;bool ok=bytes.size()<=MAXDWORD&&WriteFile(h,bytes.data(),DWORD(bytes.size()),&actual,nullptr)&&actual==bytes.size()&&FlushFileBuffers(h);CloseHandle(h);
    if(!ok){DeleteFileW(temp.c_str());throw std::runtime_error("Atomic write failed");}
    if(!MoveFileExW(temp.c_str(),p.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){DeleteFileW(temp.c_str());throw std::runtime_error("Atomic commit failed");}
}
}
