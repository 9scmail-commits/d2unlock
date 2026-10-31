#pragma once
#include "wire.hpp"
#include <map>
#include <sstream>
#include <optional>
#include <set>
namespace tiers {
struct Hero { unsigned id=0,action=0; std::string name; std::vector<unsigned> xp; };
inline std::map<unsigned,Hero> parse_heroes(const std::string& text) {
    std::map<unsigned,Hero> result;std::istringstream input(text);std::string line;
    while(std::getline(input,line)) {std::istringstream row(line);std::string id,action,name;
        if(!std::getline(row,id,'\t')||!std::getline(row,action,'\t')||!std::getline(row,name,'\t'))continue;
        Hero hero{unsigned(std::stoul(id)),unsigned(std::stoul(action)),name,{}};unsigned xp;
        while(row>>xp)hero.xp.push_back(xp);if(!hero.xp.empty()&&hero.xp.size()<=100)result[hero.id]=std::move(hero);
    }return result;
}
class Ranks {
    unsigned account;
    std::map<unsigned,Hero> heroes;
    std::map<unsigned,unsigned> levels;
    std::optional<wire::Message> original;
    std::uint64_t next_job=0x4F56544900000000ULL;
    std::set<std::uint64_t> requests;
    bool patch(wire::Message& points,unsigned account_field,unsigned event_field,unsigned actions_field)const {
        if(points.number(account_field)!=account||points.number(event_field)!=19)return false;
        for(auto [hero,level]:levels) {
            auto& definition=heroes.at(hero);
            std::erase_if(points.fields,[&](const wire::Field& f){return f.number==actions_field&&f.type==2&&wire::Message::parse(f.data).number(1)==definition.action;});
            wire::Message a;a.add(1,definition.action);a.add(2,definition.xp.at(level-1));points.add_bytes(actions_field,a.encode());
        }return !levels.empty();
    }
public:
    Ranks(unsigned owner,std::map<unsigned,Hero> data):account(owner),heroes(std::move(data)){}
    void configure(const std::string& text) {
        std::istringstream in(text);std::string magic;unsigned id,level;std::map<unsigned,unsigned> next;
        if(!(in>>magic)||magic!="OVITIER1")throw std::runtime_error("Invalid tier settings");
        while(in>>id){if(!(in>>level))throw std::runtime_error("Truncated tier settings");
            if(!heroes.contains(id)||level>heroes.at(id).xp.size())throw std::runtime_error("Unsupported hero level");if(level)next[id]=level;}
        levels=std::move(next);
    }
    bool incoming(wire::Packet& p) {
        auto type=p.type&0x7fffffff;
        if(type==7388){
            const bool own=private_response(p);
            if(own)requests.erase(p.header.number(11));
            if(p.header.number(13,1)!=1)return false;
            if(own&&p.body.number(7)==0&&p.body.number(3)==19)p.body.set(7,account);
            if(p.body.number(7)!=account||p.body.number(3)!=19)return false;
            original=p.body;
            if(own){p=*update();return true;}
            return patch(p.body,7,3,6);
        }
        if(type!=15001&&type!=15002)return false;
        const bool own=private_response(p);
        if(own)requests.erase(p.header.number(11));
        if(type==15001&&p.body.number(1)!=1)return false;
        auto bytes=p.body.bytes(2);if(bytes.empty())return false;auto points=wire::Message::parse(bytes);
        if(points.number(1)!=account||points.number(2)!=19)return false;
        wire::Message dota;
        for(auto [old_field,new_field]:std::initializer_list<std::pair<unsigned,unsigned>>{{1,7},{2,3},{3,1},{4,2},{5,4},{6,5},{8,8},{9,10}})
            for(const auto& f:points.fields)if(f.number==old_field&&f.type!=2)dota.add(new_field,f.value,f.type);
        for(const auto& f:points.fields)if(f.number==7&&f.type==2)dota.add_bytes(6,f.data);
        original=std::move(dota);
        if(own){p=*update();return true;}
        if(!patch(points,1,2,7))return false;
        p.body.erase(2);p.body.add_bytes(2,points.encode());return true;
    }
    std::optional<wire::Packet> update()const {
        if(!original)return{};auto body=*original;patch(body,7,3,6);
        auto p=wire::packet(7388,std::move(body));p.header.add(200,3);return p;
    }
    bool private_response(const wire::Packet& p)const{return ((p.type&0x7fffffff)==7388||(p.type&0x7fffffff)==15001)&&requests.contains(p.header.number(11,UINT64_MAX));}
    wire::Packet request(){
        if(requests.size()>=128)throw std::runtime_error("Too many pending hero XP requests");
        wire::Message b;b.add(1,19);b.add(2,account);auto p=wire::packet(7387,std::move(b));
        auto job=++next_job;requests.insert(job);p.header.add(10,job,1);return p;
    }
};
}
