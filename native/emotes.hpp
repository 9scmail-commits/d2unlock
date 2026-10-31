#pragma once
#include "wire.hpp"
namespace emotes {
inline wire::Packet response(unsigned account,const wire::Bytes& mask,const wire::Packet* request=nullptr){
    wire::Message access;access.add(1,account);access.add_bytes(2,mask);
    wire::Message body;body.add_bytes(1,access.encode());
    if(request)return wire::reply(7504,std::move(body),*request);
    auto p=wire::packet(7504,std::move(body));p.header.add(200,3);return p;
}
inline bool patch(wire::Packet& p,unsigned account,const wire::Bytes& mask){
    if((p.type&0x7fffffff)!=7504)return false;
    auto a=wire::Message::parse(p.body.bytes(1));if(a.number(1)!=account)return false;
    auto bits=a.bytes(2);bits.resize(std::max(bits.size(),mask.size()));
    for(size_t i=0;i<mask.size();++i)bits[i]|=mask[i];
    a.erase(2);a.add_bytes(2,std::move(bits));p.body.erase(1);p.body.add_bytes(1,a.encode());return true;
}
}
