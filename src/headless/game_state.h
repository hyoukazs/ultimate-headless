#pragma once
#include <framework/net/inputmessage.h>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <array>
#include <iostream>
#include <map>
#include <vector>
#include <functional>
#include <fstream>
#include <tuple>
#include <stdexcept>
#include <algorithm>

namespace headless {
// Logical wire state only. Layout follows the pinned ProtocolGame parser;
// item-dependent lengths come from the official client's exported attributes.
struct GamePosition {
    int x=0,y=0,z=0;
    bool operator<(const GamePosition& o) const {return std::tie(x,y,z)<std::tie(o.x,o.y,o.z);}
};
struct GameCreature {uint32_t id=0; std::string name; GamePosition pos; bool monster=false,visible=false;unsigned healthPercent=100;};
struct ItemMetadata {bool stackable=false,chargeable=false,fluid=false,splash=false,container=false; int phases=1,priority=5;};
struct GameMetadata {
    std::array<bool,129> features{};
    std::map<unsigned,ItemMetadata> items;
    uint32_t signature=0;
    unsigned outfitExtraBytes=0; // Explicit observed fork extension; never enabled by upstream defaults.
    bool inventoryInfo=false;
    void load(const std::string& path) {
        std::ifstream file(path,std::ios::binary|std::ios::ate);
        if(!file || file.tellg()>8*1024*1024 || file.tellg()<0) throw std::runtime_error("invalid metadata file size");
        file.seekg(0); boost::property_tree::ptree root; boost::property_tree::read_json(file,root);
        if(root.get<std::string>("schema")!="UH_METADATA_1" || root.get<unsigned>("clientVersion")!=860 || root.get<unsigned>("protocolVersion")!=860)
            throw std::runtime_error("unsupported metadata schema/version");
        signature=root.get<uint32_t>("datSignature");
        const auto boolean=[](const boost::property_tree::ptree& p,const std::string& name) {
            const auto value=p.get<std::string>(name);
            if(value!="true" && value!="false") throw std::runtime_error("invalid metadata boolean");
            return value=="true";
        };
        for(unsigned i=1;i<=128;++i) features[i]=boolean(root,"features."+std::to_string(i));
        for(const auto& entry:root.get_child("items")) {
            size_t used=0; auto id=std::stoul(entry.first,&used);
            if(used!=entry.first.size() || id<100 || id>65535 || items.count(id)) throw std::runtime_error("invalid metadata item ID");
            const auto& v=entry.second; ItemMetadata m;
            m.stackable=boolean(v,"stackable");m.chargeable=boolean(v,"chargeable");m.fluid=boolean(v,"fluid");m.splash=boolean(v,"splash");m.container=boolean(v,"container");
            m.phases=v.get<int>("phases");if(m.phases<1 || m.phases>255) throw std::runtime_error("invalid item animation phases");
            m.priority=boolean(v,"ground")?0:boolean(v,"border")?1:boolean(v,"bottom")?2:boolean(v,"top")?3:5;
            items.emplace(id,m);
        }
        if(items.empty() || items.size()!=root.get<unsigned>("itemCount") || items.rbegin()->first!=items.size()+99)
            throw std::runtime_error("incomplete item metadata");
        if(features[89] || features[90]) throw std::runtime_error("unsupported metadata protocol extension");
    }
};
class GameState {
public:
    GameMetadata metadata;
    uint32_t playerId=0,attackId=0,followId=0,states=0;
    uint32_t attackSequence=0;
    unsigned targetHealthUpdates=0;
    unsigned missingMappedThings=0;
    unsigned stackClamps=0;
    GamePosition position;
    unsigned health=0,maxHealth=0,mana=0,maxMana=0;
    bool mapKnown=false,statsKnown=false,statesKnown=false,compatible=true;
    std::map<uint32_t,GameCreature> creatures;
    std::function<void(const std::string&,unsigned,unsigned,const std::string&)> talk;
    std::function<void(unsigned,const std::string&)> notice;
    bool ready() const {return compatible && mapKnown && statsKnown && statesKnown && maxMana>0;}
    bool feature(unsigned id) const {return id<metadata.features.size() && metadata.features[id];}
    bool parse(unsigned opcode,const InputMessagePtr& msg) {
        switch(opcode) {
        case 66: {int x=msg->getU8(),y=msg->getU8(); if(x>64 || y>64) throw std::runtime_error("invalid aware range");left=x/2;top=y/2;width=2*left+2;height=2*top+2;break;}
        case 100: {position=pos(msg); tiles.clear();for(auto& c:creatures)c.second.visible=false; map(msg,position.x-left,position.y-top,position.z,width,height); mapKnown=true;break;}
        case 75: {auto p=pos(msg);int floor=msg->getU8(); if(floor==p.z){position=p;mapKnown=true;} floorMap(msg,p.x-left,p.y-top,floor,width,height,p.z-floor,0);break;}
        case 101: case 102: case 103: case 104: {
            auto p=feature(31)?pos(msg):position;
            if(opcode==101)--p.y; if(opcode==102)++p.x;if(opcode==103)++p.y;if(opcode==104)--p.x;
            position=p;
            map(msg,p.x-left+(opcode==102?width-1:0),p.y-top+(opcode==103?height-1:0),p.z,
                (opcode==102 || opcode==104)?1:width,(opcode==101 || opcode==103)?1:height);break;
        }
        case 105: {auto p=pos(msg);tile(msg,p);break;}
        case 106: {auto p=pos(msg);int stack=feature(19)?msg->getU8():-1; auto t=thing(msg);
            // The server may reference tile contents the client has not fully tracked
            // (e.g. changes skipped as unmapped by opcodes 107-109, or tiles at the edge
            // of the known area). An explicit stack beyond the known list is healed by
            // priority-ordered insertion and counted, instead of ending the session.
            const auto knownIt=tiles.find(p);
            const size_t known=knownIt==tiles.end()?0:knownIt->second.size();
            if(stack>=0 && stack!=255 && stack>static_cast<int>(known)) {
                ++stackClamps;
                if(stackClamps<=8)std::cout<<"GAME_STATE_STACK_CLAMPED x="<<p.x<<" y="<<p.y<<" z="<<p.z<<" stack="<<stack<<" known="<<known<<" creature="<<t.creature<<" item="<<t.item<<std::endl;
                stack=-1;
            }
            add(p,t,stack);break;}
        case 107: {auto where=mapped(msg);auto t=thing(msg);if(!hasMapped(where)){++missingMappedThings;break;}auto old=tiles.at(where.first).at(where.second);remove(where.first,where.second,old.creature && old.creature==t.creature);add(where.first,t,where.second);break;}
        case 108: {auto where=mapped(msg);if(!hasMapped(where)){++missingMappedThings;break;}remove(where.first,where.second);break;}
        case 109: {auto where=mapped(msg);auto p=pos(msg);if(!hasMapped(where)){++missingMappedThings;break;}auto& list=tiles.at(where.first);auto t=list.at(where.second);if(!t.creature){++missingMappedThings;break;}remove(where.first,where.second,true);add(p,t,-1);break;}
        case 120: {msg->getU8();thing(msg);if(metadata.inventoryInfo){for(unsigned count=0;;++count){if(count>=64)throw std::runtime_error("unterminated NTO item attributes");auto tag=msg->getU8();if(!tag)break;if(tag==30)msg->getString();else if(tag==33 || tag==73 || tag==35 || tag==37 || tag==79 || tag==80 || tag==71 || tag==72 || (tag>=74 && tag<=78))msg->getU32();else throw std::runtime_error("unsupported NTO item attribute "+std::to_string(tag));}}break;}
        case 121: msg->getU8();break;
        case 130: skip(msg,2);break;
        case 131: pos(msg);if(feature(16))msg->getU16();else msg->getU8();break;
        case 132: pos(msg);msg->getU8();msg->getString();if(feature(127))msg->getString();break;
        case 133: pos(msg);pos(msg);if(feature(77))msg->getU16();else msg->getU8();break;
        case 140: {auto id=msg->getU32();auto hp=msg->getU8();if(feature(122) && msg->getU8()==1)msg->getU8();auto it=creatures.find(id);if(it!=creatures.end()){it->second.healthPercent=hp;if(id==attackId)++targetHealthUpdates;}break;}
        case 141: msg->getU32();skip(msg,2);break;
        case 142: msg->getU32();outfit(msg);break;
        case 143: msg->getU32();if(feature(36))msg->getU16();msg->getU16();break;
        case 144: case 145: case 146: msg->getU32();msg->getU8();break;
        case 160: stats(msg);break;
        case 161: for(int i=0;i<(feature(76)?13:7);++i){if(feature(29))msg->getU16();else msg->getU8();if(feature(9)){if(feature(53))msg->getU16();else msg->getU8();}if(i<7)msg->getU8();}break;
        case 162: states=feature(105)?msg->getU32():feature(48)?msg->getU16():msg->getU8();statesKnown=true;break;
        case 163: {auto sequence=feature(32)?msg->getU32():0;if(!sequence || sequence==attackSequence){attackId=0;followId=0;}break;}
        case 164: case 165: msg->getU8();msg->getU32();break;
        case 166: msg->getU32();break;
        case 167: skip(msg,3);if(feature(50))msg->getU8();break;
        case 170: parseTalk(msg);break;
        case 180: {unsigned mode=msg->getU8(); if(mode<21 || mode>27)throw std::runtime_error("unsupported text message mode "+std::to_string(mode));auto text=msg->getString();if(notice)notice(mode,text);break;}
        case 181: msg->getU8();break;
        case 182: msg->getU16();break;
        case 210: msg->getU32();msg->getString();msg->getU8();break;
        case 211: msg->getU32();msg->getU8();break;
        default:return false;
        }
        return true;
    }
private:
    struct Entry {uint32_t creature=0;unsigned item=0;int priority=4;};
    std::map<GamePosition,std::vector<Entry>> tiles;
    int left=8,top=6,width=18,height=14;
    static void skip(const InputMessagePtr& m,unsigned n){while(n--)m->getU8();}
    static GamePosition pos(const InputMessagePtr& m){int x=m->getU16(),y=m->getU16(),z=m->getU8();if(z>15)throw std::runtime_error("invalid map floor");return {x,y,z};}
    void outfit(const InputMessagePtr& m) {
        unsigned look=feature(42)?m->getU16():m->getU8();
        if(look){skip(m,4);if(feature(44))m->getU8();}else m->getU16();
        if(feature(12))m->getU16();if(feature(104)){m->getU16();m->getU16();}
        if(feature(106))m->getString();if(feature(113)){m->getU16();m->getU16();}
        skip(m,metadata.outfitExtraBytes);
    }
    Entry thing(const InputMessagePtr& m) {
        unsigned id=m->getU16();
        if(id==96){m->getU8();m->getString();m->getString();return {};}
        if(id==97 || id==98 || id==99) {
            uint32_t cid;
            if(id==97){auto old=m->getU32();cid=m->getU32();if(old!=cid)creatures.erase(old);auto name=m->getString();auto& c=creatures[cid];c.id=cid;c.name=name;c.monster=cid>=0x40000000 && cid<0x80000000;}
            else cid=m->getU32();
            if(!creatures.count(cid))throw std::runtime_error("unknown cached creature");
            if(id==99){m->getU8();if(feature(100))m->getU8();}
            else {
                creatures.at(cid).healthPercent=m->getU8();if(feature(122) && m->getU8()==1)m->getU8();m->getU8();outfit(m);skip(m,2);m->getU16();skip(m,2);
                if(feature(14) && id==97)m->getU8();if(feature(41))m->getU8();if(feature(54))m->getU8();
                if(feature(41)){m->getU8();m->getU16();}m->getU8();
            }
            return {cid,0,4};
        }
        auto it=metadata.items.find(id);if(it==metadata.items.end())throw std::runtime_error("unknown logical item type "+std::to_string(id));
        const auto& t=it->second;if(feature(41))m->getU8();
        if(t.stackable || t.chargeable){if(feature(108))m->getU16();else m->getU8();}else if(t.fluid || t.splash)m->getU8();
        else if(t.container && feature(123)){if(m->getU8())m->getU32();}
        if(feature(15) && t.phases>1)m->getU8();
        if(feature(126)){auto n=m->getU16();if(n>m->getUnreadSize()/10)throw std::runtime_error("invalid custom attributes");while(n--){m->getU16();m->getU64();}}
        return {0,id,t.priority};
    }
    void add(GamePosition p,Entry t,int stack) {
        auto& list=tiles[p];if(list.size()>=256)throw std::runtime_error("tile stack overflow");
        if(stack<0 || stack==255){stack=0;const bool append=t.priority<=4;while(stack<static_cast<int>(list.size()) && (append?list[stack].priority<=t.priority:list[stack].priority<t.priority))++stack;}
        if(stack>static_cast<int>(list.size()))throw std::runtime_error("invalid insertion stack");
        list.insert(list.begin()+stack,t);if(t.creature){auto& c=creatures.at(t.creature);c.pos=p;c.visible=true;if(t.creature==playerId)position=p;}
    }
    void remove(GamePosition p,unsigned stack,bool keepAttack=false) {
        auto& list=tiles.at(p);if(stack>=list.size())throw std::runtime_error("invalid removal stack");
        if(list[stack].creature){auto it=creatures.find(list[stack].creature);if(it!=creatures.end())it->second.visible=false;if(!keepAttack && list[stack].creature==attackId)attackId=0;}
        list.erase(list.begin()+stack);
    }
    std::pair<GamePosition,unsigned> mapped(const InputMessagePtr& m) {
        auto x=m->getU16();if(x!=65535){int y=m->getU16(),z=m->getU8();auto stack=m->getU8();return {{x,y,z},stack};}
        auto id=m->getU32();auto it=creatures.find(id);if(it==creatures.end() || !it->second.visible)return {{-1,-1,-1},256};
        auto p=it->second.pos;const auto tileIt=tiles.find(p);if(tileIt!=tiles.end()){const auto& list=tileIt->second;for(unsigned i=0;i<list.size();++i)if(list[i].creature==id)return {p,i};}
        return {{-1,-1,-1},256};
    }
    bool hasMapped(const std::pair<GamePosition,unsigned>& where) const {
        const auto it=tiles.find(where.first);return it!=tiles.end() && where.second<it->second.size();
    }
    int tile(const InputMessagePtr& m,GamePosition p) {
        auto& list=tiles[p];for(auto t:list)if(t.creature && creatures.count(t.creature))creatures[t.creature].visible=false;list.clear();
        if(m->peekU16()>=0xff00)return m->getU16()&255;
        if(feature(13))m->getU16();
        for(int i=0;i<256;++i){if(m->peekU16()>=0xff00)return m->getU16()&255;add(p,thing(m),i);}
        throw std::runtime_error("unterminated tile stack");
    }
    int floorMap(const InputMessagePtr& m,int x,int y,int z,int w,int h,int offset,int skipped) {
        if(z<0 || z>15)throw std::runtime_error("invalid floor description");
        for(int nx=0;nx<w;++nx)for(int ny=0;ny<h;++ny){GamePosition p{x+nx+offset,y+ny+offset,z};if(!skipped)skipped=tile(m,p);else{auto& list=tiles[p];for(auto t:list)if(t.creature && creatures.count(t.creature))creatures[t.creature].visible=false;list.clear();--skipped;}}
        return skipped;
    }
    void map(const InputMessagePtr& m,int x,int y,int z,int w,int h) {
        int skipped=0,start=z>7?z-2:7,end=z>7?std::min(15,z+2):0,step=z>7?1:-1;
        for(int floor=start;floor!=end+step;floor+=step)skipped=floorMap(m,x,y,floor,w,h,z-floor,skipped);
    }
    void stats(const InputMessagePtr& m) {
        unsigned hp=feature(28)?m->getU32():m->getU16(),maxHp=feature(28)?m->getU32():m->getU16();
        if(feature(6))m->getU32();else m->getU16();if(feature(8))m->getU32();
        if(feature(7))m->getU64();else m->getU32();if(feature(83))m->getU32();else m->getU16();m->getU8();if(feature(66))m->getDouble();
        unsigned mp=feature(28)?m->getU32():m->getU16(),maxMp=feature(28)?m->getU32():m->getU16();
        if(feature(79))m->getU16();else m->getU8();if(feature(9))m->getU8();m->getU8();
        if(feature(84))m->getU16();else m->getU8();if(feature(43))m->getU16();if(feature(9))m->getU16();if(feature(10))m->getU16();if(feature(20))m->getU16();
        if(!maxHp || !maxMp)throw std::runtime_error("invalid player stats maximum");health=hp;maxHealth=maxHp;mana=mp;maxMana=maxMp;statsKnown=true;
    }
    void parseTalk(const InputMessagePtr& m) {
        if(feature(45))m->getU32();auto name=m->getString();unsigned level=feature(46)?(feature(83)?m->getU32():m->getU16()):0;auto mode=m->getU8();
        if(mode<=5 && mode>=1 || mode==19 || mode==20)pos(m);
        else if(mode==7 || mode==8 || mode==13 || mode==15)m->getU16();
        else if(mode==9)m->getU32();
        else if(mode!=6 && mode!=10 && mode!=11 && mode!=12 && mode!=14)throw std::runtime_error("unknown talk mode");
        auto text=m->getString();if(talk)talk(name,level,mode,text);
    }
};
}
