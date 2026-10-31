#include "resources.hpp"
#include <wincrypt.h>
#include <set>
#include <functional>
#include <iostream>
using namespace assets;
bool numeric(const std::string& s){return !s.empty()&&std::all_of(s.begin(),s.end(),[](unsigned char c){return isdigit(c);});}
int wmain(int argc,wchar_t** argv) {
    try {
        if(argc!=2&&argc!=3)throw std::runtime_error("Usage: CatalogBuilder [Dota directory] <output directory>");
        fs::path game=argc==3?fs::path(argv[1]):dota(),out=argv[argc-1],pak=game/L"game/dota/pak01_dir.vpk";fs::create_directories(out);
        auto stamp=fingerprint(pak);VPK archive(pak);auto raw=archive.get("scripts/items/items_game.txt");
        auto text=decode(raw);auto document=Parser(text).parse();auto& schema=document.get("items_game");
        std::map<std::string,KV> prefabs,cache;for(auto& [k,v]:schema.get("prefabs").children)prefabs[k]=v;
        std::set<std::string> resolving;std::function<KV(const KV&)> resolve;
        resolve=[&](const KV& node){KV result;std::istringstream list(node.text("prefab"));std::string name;
            while(list>>name){if(!cache.contains(name)){if(!prefabs.contains(name)||!resolving.insert(name).second)throw std::runtime_error("Invalid prefab: "+name);cache[name]=resolve(prefabs.at(name));resolving.erase(name);}result=merge(std::move(result),cache.at(name));}
            return merge(std::move(result),node);};
        std::map<std::string,unsigned> qualities;for(auto& [k,v]:schema.get("qualities").children)qualities[k]=std::stoul(v.text("value","4"));
        struct Record{unsigned quality;std::set<unsigned> styles;};std::map<unsigned,Record> records;
        for(auto& [k,node]:schema.get("items").children) {
            if(!numeric(k))continue;auto item=resolve(node);
            const auto slot=item.text("item_slot","none"),kind=item.text("item_class");
            const bool cosmetic=kind=="dota_item_wearable"||(kind=="tool"&&slot=="weather");
            if(!cosmetic||item.text("baseitem")=="1"||slot=="none"||slot.empty()||item.text("hidden")=="1"||item.text("enabled")=="0")continue;
            auto q=qualities.find(item.text("item_quality"));Record rec{q==qualities.end()?4:q->second,{0}};
            for(auto& [style,_]:item.get("visuals").get("styles").children)if(numeric(style)&&std::stoul(style)<255)rec.styles.insert(std::stoul(style));records[std::stoul(k)]=std::move(rec);
        }
        if(records.empty()||records.size()>100000)throw std::runtime_error("Invalid catalogue size");unsigned attr=0;
        for(auto& [k,v]:schema.get("attributes").children)if(v.text("name")=="unlocked styles")attr=std::stoul(k);
        Bytes bin{'O','V','I','C','A','T','0','2'};append(bin,2,4);append(bin,records.size(),4);append(bin,attr,4);
        HCRYPTPROV provider=0;HCRYPTHASH hash=0;unsigned char digest[32];DWORD size=32;
        if(!CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT)||!CryptCreateHash(provider,CALG_SHA_256,0,0,&hash)||!CryptHashData(hash,raw.data(),DWORD(raw.size()),0)||!CryptGetHashParam(hash,HP_HASHVAL,digest,&size,0))throw std::runtime_error("SHA256 failed");
        CryptDestroyHash(hash);CryptReleaseContext(provider,0);bin.insert(bin.end(),digest,digest+32);
        for(auto& [id,r]:records){append(bin,id,4);append(bin,r.quality,4);append(bin,r.styles.size(),4);for(auto style:r.styles)append(bin,style,4);}
        Bytes emoticons;
        auto registry=Parser(decode(archive.get("scripts/emoticons_definitions.txt"))).parse();
        for(const auto& [_,entry]:registry.get("emoticons_definitions").children){
            const auto& path=entry.value;if(path.rfind("scripts/",0)!=0||path.find("..")!=std::string::npos||!archive.has(path))continue;
            auto definitions=Parser(decode(archive.get(path))).parse();
            for(const auto& [id,_]:definitions.get("emoticons").children)if(numeric(id)){
                auto n=std::stoul(id);if(!n||n>65535)continue;if(emoticons.size()<=n/8)emoticons.resize(n/8+1);emoticons[n/8]|=1u<<(n%8);
            }
        }
        if(emoticons.empty())throw std::runtime_error("Emoticon definitions missing");write(out/L"emoticons.bin",emoticons);
        auto heroes_text=decode(archive.get("scripts/npc/npc_heroes.txt"));auto heroes=Parser(heroes_text).parse();std::ostringstream hero_data;
        auto definitions=heroes.get("DOTAHeroes");
        for(auto& [key,value]:heroes.children)if(key=="#base"&&value.value.rfind("heroes/",0)==0&&value.value.find("..")==std::string::npos){
            auto include_text=decode(archive.get("scripts/npc/"+value.value));auto included=Parser(include_text).parse();
            for(auto& entry:included.get("DOTAHeroes").children)definitions.children.push_back(entry);
        }
        for(auto& [name,v]:definitions.children) {
            auto id=v.text("HeroID");if(!numeric(id)||std::stoul(id)==0)continue;
            auto shortname=name;if(shortname.rfind("npc_dota_hero_",0)==0)shortname.erase(0,14);
            auto resource="scripts/events/plus_heroes/"+(shortname=="ringmaster"?"ring_master":shortname)+".eventactions";
            if(!archive.has(resource))continue;auto actiontext=decode(archive.get(resource));auto actions=Parser(actiontext).parse();
            for(auto& [action_name,a]:actions.get("event_actions").children)if(action_name.rfind("hero_xp_",0)==0){
                std::set<unsigned> thresholds;for(auto& [level,_]:a.get("rewards").children)if(numeric(level))thresholds.insert(std::stoul(level));
                std::string display=v.text("workshop_guide_name",shortname);std::replace(display.begin(),display.end(),'\t',' ');
                hero_data<<id<<'\t'<<a.text("action_id")<<'\t'<<display;for(auto xp:thresholds)hero_data<<'\t'<<xp;hero_data<<'\n';break;}
        }
        if(stamp!=fingerprint(pak))throw std::runtime_error("Dota updated during catalogue generation");
        auto h=hero_data.str();if(h.empty())throw std::runtime_error("Hero catalogue is empty");write(out/L"heroes.tsv",Bytes(h.begin(),h.end()));write(out/L"catalog.bin",bin);write(out/L"catalog.source",stamp);
        std::cout<<"Definitions: "<<records.size()<<"; bytes: "<<bin.size()<<"\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
