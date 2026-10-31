#pragma once
#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include "security.hpp"
namespace assets {
namespace fs = std::filesystem;
using Bytes = std::vector<unsigned char>;
inline std::string lower(std::string s) { for(auto& c:s) c=static_cast<char>(tolower(static_cast<unsigned char>(c))); return s; }
inline std::wstring wide(const std::string& s) {
    int n=MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),nullptr,0); std::wstring out(n,0);
    MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),out.data(),n); return out;
}
inline std::string utf8(const std::wstring& s) {
    int n=WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),nullptr,0,nullptr,nullptr); std::string out(n,0);
    WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),out.data(),n,nullptr,nullptr); return out;
}
inline Bytes read(const fs::path& p) {
    std::ifstream in(p,std::ios::binary|std::ios::ate);
    if(!in) throw std::runtime_error("Cannot read: "+utf8(p.wstring()));
    auto n=in.tellg(); if(n<0||n>512*1024*1024) throw std::runtime_error("File size limit");
    Bytes b(static_cast<size_t>(n)); in.seekg(0);
    if(!in.read(reinterpret_cast<char*>(b.data()),b.size())) throw std::runtime_error("Truncated file"); return b;
}
inline void write(const fs::path& p,const Bytes& b) {
    security::write(p,b);
}
inline std::string decode(const Bytes& b) {
    if(b.size()>=2&&b[0]==255&&b[1]==254) {
        if(b.size()%2) throw std::runtime_error("Invalid UTF16");
        std::wstring w; for(size_t i=2;i<b.size();i+=2) w.push_back(wchar_t(b[i]|(b[i+1]<<8))); return utf8(w);
    }
    size_t at=b.size()>=3&&b[0]==239&&b[1]==187&&b[2]==191?3:0;
    return std::string(b.begin()+at,b.end());
}
struct KV {
    std::string value;
    std::vector<std::pair<std::string,KV>> children;
    const KV& get(const std::string& key) const {
        static const KV empty; auto k=lower(key);
        for(auto i=children.rbegin();i!=children.rend();++i) if(lower(i->first)==k)return i->second;return empty;
    }
    std::string text(const std::string& key,const std::string& fallback="") const {auto& n=get(key);return n.value.empty()?fallback:n.value;}
};
class Parser {
    const std::string& s;size_t p=0;int kind=0;
    std::string token() {
        for(;;) {
            while(p<s.size()&&isspace(static_cast<unsigned char>(s[p])))++p;
            if(s.compare(p,2,"//")==0){while(p<s.size()&&s[p]!='\n')++p;continue;}
            if(s.compare(p,2,"/*")==0){auto end=s.find("*/",p+2);if(end==s.npos)throw std::runtime_error("Unclosed comment");p=end+2;continue;}break;
        }
        if(p==s.size()){kind=0;return{};}char c=s[p++];
        if(c=='{'||c=='}'){kind=c;return{};}kind=1;std::string out;
        if(c=='"') {
            while(p<s.size()) { c=s[p++];if(c=='"')return out;
                if(c=='\\'&&p<s.size()){char next=s[p];if(next=='"'||next=='\\'||next=='n'||next=='r'||next=='t'){++p;c=next=='n'?'\n':next=='r'?'\r':next=='t'?'\t':next;}}
                out+=c;
            }throw std::runtime_error("Unclosed string");
        }
        out+=c;while(p<s.size()&&!isspace(static_cast<unsigned char>(s[p]))&&s[p]!='{'&&s[p]!='}')out+=s[p++];return out;
    }
    KV block(unsigned depth) {
        if(depth>128)throw std::runtime_error("KV depth limit");KV out;
        for(;;){auto key=token();if(!kind){if(depth)throw std::runtime_error("Unclosed block");return out;}
            if(kind=='}'){if(!depth)throw std::runtime_error("Unexpected brace");return out;}
            if(kind!=1)throw std::runtime_error("Expected key");auto value=token();KV node;
            if(kind=='{')node=block(depth+1);else if(kind==1)node.value=std::move(value);else throw std::runtime_error("Expected value");
            out.children.emplace_back(std::move(key),std::move(node));}
    }
public: explicit Parser(const std::string& text):s(text){} KV parse(){return block(0);}
};
inline KV merge(KV base,const KV& over) {
    for(auto& [key,n]:over.children){auto old=base.get(key);auto val=!old.children.empty()&&!n.children.empty()?merge(old,n):n;
        std::erase_if(base.children,[&](auto& e){return lower(e.first)==lower(key);});base.children.emplace_back(key,std::move(val));}return base;
}
inline fs::path steam() {
    wchar_t path[32768];DWORD len=sizeof(path);
    if(RegGetValueW(HKEY_CURRENT_USER,L"Software\\Valve\\Steam",L"SteamPath",RRF_RT_REG_SZ,nullptr,path,&len)!=ERROR_SUCCESS)
        throw std::runtime_error("Steam is not installed");
    return fs::path(path);
}
inline fs::path dota() {
    auto root=steam();std::vector<fs::path> roots{root};auto libraries=root/L"steamapps/libraryfolders.vdf";
    if(fs::exists(libraries)){auto text=decode(read(libraries));auto doc=Parser(text).parse();for(auto& [_,v]:doc.get("libraryfolders").children)if(!v.text("path").empty())roots.push_back(wide(v.text("path")));}
    for(auto& r:roots){auto game=r/L"steamapps/common/dota 2 beta";if(fs::exists(game/L"game/dota/pak01_dir.vpk"))return game;}
    throw std::runtime_error("Dota 2 is not installed");
}
inline std::uint64_t little(const unsigned char* p,unsigned n) {std::uint64_t v=0;for(unsigned i=0;i<n;++i)v|=std::uint64_t(p[i])<<(i*8);return v;}
inline void append(Bytes& b,std::uint64_t v,unsigned n) {for(unsigned i=0;i<n;++i)b.push_back(static_cast<unsigned char>(v>>(8*i)));}
inline Bytes fingerprint(const fs::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA i{};if(!GetFileAttributesExW(p.c_str(),GetFileExInfoStandard,&i))throw std::runtime_error("VPK missing");
    Bytes out{'O','V','I','S','R','C','0','1'};
    append(out,(std::uint64_t(i.nFileSizeHigh)<<32)|i.nFileSizeLow,8);append(out,(std::uint64_t(i.ftLastWriteTime.dwHighDateTime)<<32)|i.ftLastWriteTime.dwLowDateTime,8);return out;
}
class VPK {
    struct Entry{unsigned crc,archive,offset,length;Bytes preload;};
    fs::path path;std::uint64_t data_start;std::map<std::string,Entry> entries;
public:
    explicit VPK(fs::path p,bool models=false):path(std::move(p)) {
        std::ifstream in(path,std::ios::binary);unsigned char h[28];if(!in.read(reinterpret_cast<char*>(h),12))throw std::runtime_error("VPK header");
        auto version=little(h+4,4),tree_size=little(h+8,4);
        if(little(h,4)!=0x55AA1234||(version!=1&&version!=2)||tree_size>256*1024*1024)throw std::runtime_error("VPK format");
        if(version==2&&!in.read(reinterpret_cast<char*>(h+12),16))throw std::runtime_error("VPK v2 header");
        data_start=(version==2?28:12)+tree_size;Bytes tree(tree_size);
        if(!in.read(reinterpret_cast<char*>(tree.data()),tree.size()))throw std::runtime_error("VPK tree");size_t at=0;
        auto str=[&](){size_t begin=at;while(at<tree.size()&&tree[at])++at;if(at==tree.size())throw std::runtime_error("VPK string");return std::string(tree.begin()+begin,tree.begin()+at++);};
        for(auto ext=str();!ext.empty();ext=str())for(auto dir=str();!dir.empty();dir=str())for(auto name=str();!name.empty();name=str()) {
            if(at+18>tree.size())throw std::runtime_error("VPK entry");auto e=&tree[at];auto preload=little(e+4,2);
            Entry item{unsigned(little(e,4)),unsigned(little(e+6,2)),unsigned(little(e+8,4)),unsigned(little(e+12,4)),{}};
            if(little(e+16,2)!=65535||at+18+preload>tree.size())throw std::runtime_error("VPK terminator");at+=18;
            if(dir=="scripts"||dir.rfind("scripts/",0)==0||dir.rfind("resource/localization",0)==0||(models&&dir.rfind("models/",0)==0)) {
                item.preload.assign(tree.begin()+at,tree.begin()+at+preload);
                entries.emplace((dir==" "?"":dir+"/")+name+(ext==" "?"":"."+ext),std::move(item));}
            at+=preload;
        }
    }
    bool has(const std::string& key)const{return entries.contains(key);}
    std::vector<std::string> list(const std::string& prefix)const{std::vector<std::string> out;
        for(auto it=entries.lower_bound(prefix);it!=entries.end()&&it->first.rfind(prefix,0)==0;++it)out.push_back(it->first);return out;}
    Bytes get(const std::string& key) const {
        auto it=entries.find(key);if(it==entries.end())throw std::runtime_error("Resource missing: "+key);auto& e=it->second;
        if(e.length>256*1024*1024)throw std::runtime_error("Resource size limit");auto source=path;std::uint64_t offset=e.offset+data_start;
        if(e.archive!=0x7fff){char suffix[32];snprintf(suffix,sizeof(suffix),"_%03u.vpk",e.archive);auto n=path.filename().wstring();source=path.parent_path()/(n.substr(0,n.size()-8)+wide(suffix));offset=e.offset;}
        Bytes b=e.preload;b.resize(b.size()+e.length);std::ifstream in(source,std::ios::binary);in.seekg(offset);
        if(!in.read(reinterpret_cast<char*>(b.data()+e.preload.size()),e.length))throw std::runtime_error("Truncated VPK resource");
        static const auto table=[](){std::vector<unsigned> t(256);for(unsigned i=0;i<256;++i){unsigned c=i;for(int j=0;j<8;++j)c=(c>>1)^((c&1)?0xedb88320:0);t[i]=c;}return t;}();
        unsigned crc=~0u;for(auto v:b)crc=table[(crc^v)&255]^(crc>>8);if((~crc)!=e.crc)throw std::runtime_error("VPK CRC mismatch");return b;
    }
};
}
