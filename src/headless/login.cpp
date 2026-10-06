#include "login.h"
#include "options.h"
#include "game_state.h"
#include <framework/net/protocol.h>
#include <framework/luaengine/luainterface.h>
#include <framework/core/eventdispatcher.h>
#include <framework/core/clock.h>
#include <framework/util/crypt.h>
#include <boost/asio.hpp>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <iomanip>
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <csignal>
#include <filesystem>
#include <sstream>
#include <set>
#include <cmath>
#include <cctype>
#include <cerrno>
#include <pwd.h>

extern asio::io_service g_ioService;
namespace headless {
namespace {
// Public OTCv8 const.lua OTSERV_RSA. Candidate, never asserted as NTO's key.
const char* otRsa =
 "1091201329673994292788609605089955415282375029027981291234687579"
 "3726629149257644633073969600111060390723088861007265581882535850"
 "3429057592827629436413108566029093628212635953836686562675849720"
 "6207862794310902180176810615217550567108238764764442605581471797"
 "07119674283982419152118103759076030616683978566631413";
struct Credentials {
    std::string account, password;
    ~Credentials() {
        if (!account.empty()) OPENSSL_cleanse(&account[0], account.size());
        if (!password.empty()) OPENSSL_cleanse(&password[0], password.size());
    }
};
class Tty {
    int fd;
    termios saved{};
public:
    Tty() : fd(open("/dev/tty", O_RDWR | O_CLOEXEC)) {
        if (fd < 0) throw std::runtime_error("login requires an interactive terminal");
        if (tcgetattr(fd,&saved)) { close(fd); throw std::runtime_error("cannot protect terminal input"); }
        auto mode=saved; mode.c_lflag &= ~(ECHO | ECHONL);
        if (tcsetattr(fd,TCSANOW,&mode)) { close(fd); throw std::runtime_error("cannot disable terminal echo"); }
    }
    ~Tty() { tcsetattr(fd,TCSANOW,&saved); close(fd); }
    void read(const char* prompt, std::string& value) {
        if (write(fd,prompt,std::char_traits<char>::length(prompt))<0) throw std::runtime_error("terminal unavailable");
        char c;
        while (::read(fd,&c,1)==1) {
            if (c=='\n') {
                if (write(fd,"\n",1)<0) throw std::runtime_error("terminal unavailable");
                return;
            }
            if (c!='\r') value.push_back(c);
            if (value.size()>80) throw std::runtime_error("credential exceeds encrypted login block capacity");
        }
        throw std::runtime_error("terminal input interrupted");
    }
};
struct LoginConfig {
    std::string host, modulus=otRsa;
    std::string character;
    unsigned holdSeconds=120;
    bool daemon=false;
    bool reconnect=false;
    unsigned reconnectDelay=5, reconnectMaxDelay=60;
    bool inventoryInfo=false;
    bool localPlatformContext=false;
    std::string pidFile,logFile,profileDir;
    uint16 port=0;
    uint16 clientId=21; // Game::getOs() in the pinned upstream: Linux = 21.
    uint32 dat=0, spr=0, pic=0x56C5DDE7;
    std::vector<uint8> context;
    std::shared_ptr<GameMetadata> metadata;
    std::string trainingModules;
    ~LoginConfig() {
        if (!context.empty()) OPENSSL_cleanse(context.data(),context.size());
    }
};
LoginConfig configOf(int argc, char** argv) {
    LoginConfig config;
    bool candidate=false;
    std::string metadataPath;
    unsigned outfitExtraBytes=0;
    for (int i=2;i<argc;++i) {
        std::string option=argv[i];
        if (option=="--upstream-860") { candidate=true; continue; }
        if (option=="--stay-online") { config.holdSeconds=0; continue; }
        if (option=="--daemon") { config.daemon=true; continue; }
        if (option=="--reconnect") { config.reconnect=true; continue; }
        if (option=="--nto-item-info") {config.inventoryInfo=true;continue;}
        if (option=="--local-platform-context") {config.localPlatformContext=true;continue;}
        if (i+1>=argc) throw std::runtime_error("login option requires a value");
        const std::string value=argv[++i];
        if (option=="--host") config.host=value;
        else if(option=="--pid-file") config.pidFile=value;
        else if(option=="--log-file") config.logFile=value;
        else if(option=="--profile-dir") config.profileDir=value;
        else if (option=="--character") config.character=value;
        else if (option=="--metadata") metadataPath=value;
        else if (option=="--training-modules") config.trainingModules=value;
        else if (option=="--reconnect-delay" || option=="--reconnect-max-delay") {
            size_t used=0;auto seconds=std::stoul(value,&used);
            if(used!=value.size() || seconds<1 || seconds>300)throw std::runtime_error("invalid reconnect delay");
            if(option=="--reconnect-delay")config.reconnectDelay=seconds;else config.reconnectMaxDelay=seconds;
        }
        else if (option=="--outfit-extra-bytes") {size_t used=0;auto n=std::stoul(value,&used);if(used!=value.size() || n>16)throw std::runtime_error("invalid observed outfit extension length");outfitExtraBytes=n;}
        else if (option=="--hold-seconds") {
            size_t used=0; auto seconds=std::stoul(value,&used);
            if(used!=value.size() || seconds<1 || seconds>900) throw std::runtime_error("invalid session duration");
            config.holdSeconds=static_cast<unsigned>(seconds);
        }
        else if (option=="--port") {
            size_t used=0; auto port=std::stoul(value,&used);
            if (used!=value.size() || port<1 || port>65535) throw std::runtime_error("invalid login port");
            config.port=static_cast<uint16>(port);
        } else if (option=="--rsa-public") {
            std::ifstream file(value); if (!file || !std::getline(file,config.modulus)) throw std::runtime_error("cannot read public RSA modulus");
        } else if (option=="--official-context") {
            std::ifstream file(value,std::ios::binary);
            if (!file) throw std::runtime_error("cannot read authorized login context");
            std::vector<uint8> bytes(4097);
            file.read(reinterpret_cast<char*>(bytes.data()),bytes.size());
            bytes.resize(static_cast<size_t>(file.gcount()));
            if (bytes.size()<28 || bytes.size()>4096 || std::string(bytes.begin(),bytes.begin()+6)!="UHCTX1")
                throw std::runtime_error("invalid authorized login context");
            const auto u16=[&](size_t at)->uint16 { if(at+2>bytes.size()) throw std::runtime_error("truncated login context"); return bytes[at]|(bytes[at+1]<<8); };
            const auto u32=[&](size_t at)->uint32 { return u16(at)|(static_cast<uint32>(u16(at+2))<<16); };
            config.clientId=u16(6);
            if(u16(8)!=860) throw std::runtime_error("unsupported context version");
            size_t at=10;
            for(unsigned field=0;field<3;++field) {
                const auto length=u16(at); at+=2;
                if(at+length>bytes.size()) throw std::runtime_error("truncated context field");
                at+=length;
            }
            if(at+12!=bytes.size()) throw std::runtime_error("unexpected context layout");
            config.context.assign(bytes.begin()+10,bytes.begin()+at);
            config.dat=u32(at); config.spr=u32(at+4); config.pic=u32(at+8);
            OPENSSL_cleanse(bytes.data(),bytes.size());
        } else if (option=="--client-id" || option=="--dat-signature" || option=="--spr-signature" || option=="--pic-signature") {
            size_t used=0;
            auto number=std::stoull(value,&used,0);
            if (used!=value.size() || number>0xffffffffULL || (option=="--client-id" && number>65535))
                throw std::runtime_error("invalid public login field");
            if (option=="--client-id") config.clientId=static_cast<uint16>(number);
            else if (option=="--dat-signature") config.dat=static_cast<uint32>(number);
            else if (option=="--spr-signature") config.spr=static_cast<uint32>(number);
            else config.pic=static_cast<uint32>(number);
        } else throw std::runtime_error("unsupported login option; credentials must be entered interactively");
    }
    if (!candidate || !config.port || (config.host!="127.0.0.1" && config.host!="cliente.ntoultimate.com.br"))
        throw std::runtime_error("usage: --login --host <verified-host|127.0.0.1> --port N --upstream-860 [--rsa-public public-modulus-file] [--official-context private-context-file]");
    if (config.modulus.size()<300 || config.modulus.size()>310 || config.modulus.find_first_not_of("0123456789")!=std::string::npos)
        throw std::runtime_error("expected a 1024-bit decimal public RSA modulus");
    // --character is allowed for both loopback (testing) and real server (explicit selection).
    // Daemon without --character is valid: interactive selection will fill it before fork.
    if(!config.profileDir.empty() && config.profileDir[0]!='/') throw std::runtime_error("profile directory must be absolute");
    if(config.reconnect && (!config.daemon || config.holdSeconds || config.reconnectDelay>config.reconnectMaxDelay))
        throw std::runtime_error("reconnect requires --daemon --stay-online and valid backoff bounds");
    if(config.localPlatformContext) {
        // Keep the genuine protocol field and signatures; report this host rather
        // than redistributing the captured Windows username and CPU description.
        if(config.context.size()<6)throw std::runtime_error("local platform context requires a client context template");
        const size_t size=config.context[0]|(static_cast<size_t>(config.context[1])<<8);
        if(size+6!=config.context.size() || config.context[size+2] || config.context[size+3] || config.context[size+4] || config.context[size+5])
            throw std::runtime_error("client context template must have empty platform fields");
        std::array<char,16384> buffer{};passwd entry{};passwd* user=nullptr;
        if(getpwuid_r(geteuid(),&entry,buffer.data(),buffer.size(),&user)!=0 || !user || !user->pw_name)
            throw std::runtime_error("cannot read local platform username");
        std::string username=user->pw_name,cpu,line;
        std::ifstream cpuinfo("/proc/cpuinfo");
        while(std::getline(cpuinfo,line)) {
            const auto colon=line.find(':');if(colon==std::string::npos)continue;
            auto key=line.substr(0,colon);key.erase(key.find_last_not_of(" \t")+1);
            if(key!="model name")continue;
            const auto begin=line.find_first_not_of(" \t",colon+1);
            if(begin!=std::string::npos)cpu=line.substr(begin,20);
            break;
        }
        if(username.empty() || username.size()>80 || cpu.empty())throw std::runtime_error("local platform data unavailable on this architecture");
        config.context.resize(size+2);
        for(const auto& value:{username,cpu}) {
            config.context.push_back(static_cast<uint8>(value.size()));config.context.push_back(0);
            config.context.insert(config.context.end(),value.begin(),value.end());
        }
    }
    if(config.daemon && config.profileDir.empty() && (config.pidFile.empty() || config.logFile.empty() || config.pidFile[0]!='/' || config.logFile[0]!='/'))
        throw std::runtime_error("daemon requires absolute --pid-file and --log-file paths");
    if(!metadataPath.empty()) {
        config.metadata=std::make_shared<GameMetadata>();config.metadata->load(metadataPath);
        config.metadata->outfitExtraBytes=outfitExtraBytes;
        config.metadata->inventoryInfo=config.inventoryInfo;
        if(config.metadata->signature!=config.dat) throw std::runtime_error("metadata does not match authenticated DAT signature");
    }
    if(!config.trainingModules.empty() && (!config.metadata || !std::filesystem::is_directory(config.trainingModules)))
        throw std::runtime_error("live training requires metadata and a trusted modules directory");
    return config;
}
int privateFile(const std::string& path,int flags) {
    const int fd=open(path.c_str(),flags|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
    struct stat info{};
    if(fd<0 || fstat(fd,&info) || !S_ISREG(info.st_mode) || info.st_uid!=geteuid()) {
        if(fd>=0) close(fd);
        throw std::runtime_error("unsafe daemon state file");
    }
    if(fchmod(fd,0600)) {close(fd);throw std::runtime_error("cannot protect daemon state file");}
    return fd;
}
bool detach(const LoginConfig& config) {
    std::cout.flush();std::cerr.flush();
    g_ioService.notify_fork(asio::io_service::fork_prepare);
    const auto child=fork();
    if(child<0) {g_ioService.notify_fork(asio::io_service::fork_parent);throw std::runtime_error("cannot fork session");}
    if(child>0) {
        g_ioService.notify_fork(asio::io_service::fork_parent);
        std::cout << "DAEMON_STARTED pid=" << child << " character=" << config.character << "; credentials remain in memory only" << std::endl;
        return true;
    }
    g_ioService.notify_fork(asio::io_service::fork_child);
    if(setsid()<0) throw std::runtime_error("cannot detach session");
    umask(0077);
    const int log=privateFile(config.logFile,O_WRONLY|O_APPEND);
    const int input=open("/dev/null",O_RDONLY|O_CLOEXEC);
    if(input<0 || dup2(input,STDIN_FILENO)<0 || dup2(log,STDOUT_FILENO)<0 || dup2(log,STDERR_FILENO)<0) {
        close(log);if(input>=0)close(input);throw std::runtime_error("cannot redirect detached session");
    }
    close(log);close(input);
    const int pid=privateFile(config.pidFile,O_WRONLY);
    const auto text=std::to_string(getpid())+"\n";
    const bool failed=ftruncate(pid,0) || write(pid,text.data(),text.size())!=static_cast<ssize_t>(text.size());
    close(pid);
    if(failed) throw std::runtime_error("cannot record session PID");
    std::cout << "DAEMON_SESSION pid=" << getpid() << " character=" << config.character << std::endl;
    return false;
}
struct CharacterEndpoint {
    std::string name, host;
    uint16 port=0;
};
// Authenticated session and logical state. All network/Lua callbacks run on its owner thread.
class GameLoginSession final : public Protocol {
public:
    GameLoginSession(const LoginConfig& config, Credentials& credentials, CharacterEndpoint endpoint)
        : config(config),credentials(credentials),endpoint(std::move(endpoint)) {
        if(config.metadata){state.metadata=*config.metadata;loginPending=state.feature(35);newSpeedLaw=state.feature(36);clientPing=state.feature(24);}
    }
    GameState state;
    bool done=false, online=false;
    int result=1;
    std::chrono::steady_clock::time_point onlineSince{};
    unsigned frames=0, unparsedFrames=0, pingReplies=0;
    unsigned retryAfter=0;
    unsigned trainingRequests=0,powerdownRequests=0,attackRequests=0;
    bool trainingStarted=false,trainingAcknowledged=false,trainingStopped=false;
    std::chrono::steady_clock::time_point lastFrame{};
    void start() { connect(endpoint.host,endpoint.port); }
    void stopTraining() {
        trainingStopped=true;
        if(trainingTick)trainingTick->cancel();
        state.talk=nullptr;
        state.notice=nullptr;
        if(trainingStarted){g_lua.callGlobalField("uh_live","stop");trainingStarted=false;}
    }
    void logout() {
        if(online && isConnected()) { OutputMessagePtr message(new OutputMessage); message->addU8(20); send(message); }
    }
private:
    LoginConfig config;
    Credentials& credentials;
    CharacterEndpoint endpoint;
    bool challenged=false, first=true, loginPending=false, newSpeedLaw=false, clientPing=false;
    std::array<bool,256> reportedOpcodes{};
    unsigned currentOpcode=0;
    ScheduledEventPtr trainingTick;
    uint32 attackSequence=0;
    std::set<std::string> noticed;
    static void pushPosition(LuaInterface* lua,GamePosition p) {
        lua->newTable();lua->pushInteger(p.x);lua->setField("x");lua->pushInteger(p.y);lua->setField("y");lua->pushInteger(p.z);lua->setField("z");
    }
    void scheduleTraining() {
        g_clock.update();
        trainingTick=g_dispatcher.scheduleEventEx("liveTraining",[this]{
            if(done || !online || !state.ready())return;
            g_lua.callGlobalField("uh_live","tick");
            if(!done)scheduleTraining();
        },100);
    }
    void attachTraining() {
        g_lua.registerGlobalFunction("uh_now",[](LuaInterface* lua){g_clock.update();lua->pushNumber(g_clock.millis());return 1;});
        g_lua.registerGlobalFunction("uh_error",[this](LuaInterface* lua){lua->popString();std::cout << "TRAINING_CALLBACK_FAILURE" << std::endl;done=true;result=5;return 0;});
        g_lua.registerGlobalFunction("uh_snapshot",[this](LuaInterface* lua){
            lua->newTable();lua->pushBoolean(online && !done && isConnected() && state.ready());lua->setField("online");
            lua->pushString(endpoint.name);lua->setField("name");
            if(state.statsKnown && state.maxMana)lua->pushNumber(std::floor(100.0*state.mana/state.maxMana));else lua->pushNil();lua->setField("manaPercent");
            if(state.statesKnown)lua->pushBoolean((state.states&16384)!=0);else lua->pushNil();lua->setField("inPz");
            lua->pushBoolean(state.attackId!=0);lua->setField("attacking");
            lua->pushNumber(state.playerId);lua->setField("playerId");
            lua->pushNumber(state.attackId);lua->setField("attackId");
            lua->pushNumber(state.followId);lua->setField("followId");
            pushPosition(lua,state.position);lua->setField("position");
            lua->newTable();unsigned index=0;
            for(const auto& entry:state.creatures){const auto& c=entry.second;if(!c.visible)continue;
                lua->newTable();lua->pushNumber(c.id);lua->setField("id");lua->pushString(c.name);lua->setField("name");lua->pushBoolean(c.monster);lua->setField("monster");
                lua->pushBoolean(c.id<0x40000000);lua->setField("player");
                pushPosition(lua,c.pos);lua->setField("position");lua->rawSeti(++index);
            }
            lua->setField("spectators");return 1;
        });
        g_lua.registerGlobalFunction("uh_say",[this](LuaInterface* lua){
            auto text=lua->popString();if(done || !online || !state.ready() || !isConnected())return 0;
            if(text!="!treinar" && text!="powerdown" && text!="Kai"){done=true;result=5;return 0;}
            OutputMessagePtr m(new OutputMessage);m->addU8(150);m->addU8(1);m->addString(text);send(m);
            if(text=="!treinar"){if(!trainingRequests++)std::cout << "TRAINING_COMMAND !treinar" << std::endl;}
            if(text=="powerdown"){if(!powerdownRequests++)std::cout << "TRAINING_COMMAND powerdown" << std::endl;}
            return 0;
        });
        g_lua.registerGlobalFunction("uh_attack",[this](LuaInterface* lua){
            double raw=lua->popNumber();if(!std::isfinite(raw) || raw<1 || raw>0xffffffffULL || raw!=std::floor(raw) || done || !online || !state.ready() || !isConnected() || (state.states&16384))return 0;
            auto id=static_cast<uint32>(raw);auto it=state.creatures.find(id);if(it==state.creatures.end())return 0;const auto& c=it->second;
            auto name=c.name;std::transform(name.begin(),name.end(),name.begin(),[](unsigned char v){return std::tolower(v);});
            auto d=std::max(std::abs(state.position.x-c.pos.x),std::abs(state.position.y-c.pos.y));
            if(!c.visible || !c.monster || name!="trainer" || c.pos.z!=state.position.z || d>7)return 0;
            OutputMessagePtr m(new OutputMessage);m->addU8(161);m->addU32(id);state.attackSequence=++attackSequence;if(state.feature(32))m->addU32(attackSequence);send(m);state.attackId=id;++attackRequests;
            std::cout << "TRAINING_ATTACK id=" << id << " distance=" << d << std::endl;return 0;
        });
        g_lua.registerGlobalFunction("uh_follow",[this](LuaInterface* lua){
            const double raw=lua->popNumber();
            if(!std::isfinite(raw) || raw<1 || raw>0xffffffffULL || raw!=std::floor(raw) || done || !online || !state.ready() || !isConnected() || !(state.states&16384))return 0;
            const auto id=static_cast<uint32>(raw);const auto it=state.creatures.find(id);if(it==state.creatures.end())return 0;
            const auto& c=it->second;auto name=c.name;std::transform(name.begin(),name.end(),name.begin(),[](unsigned char v){return std::tolower(v);});
            if(!c.visible || !c.monster || name!="trainer" || c.pos.z!=state.position.z || std::max(std::abs(state.position.x-c.pos.x),std::abs(state.position.y-c.pos.y))>7 || state.followId==id)return 0;
            OutputMessagePtr m(new OutputMessage);m->addU8(162);m->addU32(id);state.attackSequence=++attackSequence;if(state.feature(32))m->addU32(attackSequence);send(m);state.followId=id;
            std::cout<<"TRAINING_FOLLOW id="<<id<<std::endl;return 0;
        });
        g_lua.registerGlobalFunction("uh_cancel",[this](LuaInterface*){
            if(done || !online || !isConnected() || (!state.attackId && !state.followId))return 0;
            OutputMessagePtr m(new OutputMessage);m->addU8(190);send(m);state.attackId=state.followId=0;return 0;
        });
        const auto load=[this](const char* name,const char* global){
            std::ifstream input(std::filesystem::path(config.trainingModules)/name,std::ios::binary);std::ostringstream content;content<<input.rdbuf();
            if(!input || content.str().empty() || content.str().size()>65536)throw std::runtime_error("invalid trusted training module");
            g_lua.loadFunction(content.str(),name);g_lua.safeCall(0,global?1:0);if(global)g_lua.setGlobal(global);
        };
        load("runtime.lua","UHRuntime");load("training.lua","UHTraining");load("live_training.lua",nullptr);
        trainingStarted=true;
        state.notice=[this](unsigned mode,const std::string& text){
            if(noticed.size()>=16 || !noticed.insert(text).second)return;
            auto safe=text;for(const auto* secret:{&credentials.account,&credentials.password}){if(secret->empty())continue;size_t at=0;while((at=safe.find(*secret,at))!=std::string::npos){safe.replace(at,secret->size(),"[redacted]");at+=10;}}
            for(char& c:safe)if(static_cast<unsigned char>(c)<32)c=' ';
            std::cout << "TRAINING_NOTICE mode=" << mode << " text=" << safe.substr(0,180) << std::endl;
        };
        state.talk=[this](const std::string& name,unsigned level,unsigned mode,const std::string& text){
            auto lower=text;std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return std::tolower(c);});
            if(name==endpoint.name && lower=="on!"){trainingAcknowledged=true;std::cout << "TRAINING_ON_ACK" << std::endl;}
            g_lua.callGlobalField("uh_live","talk",name,level,mode,text);
        };
        scheduleTraining();
        unsigned trainers=0;for(const auto& entry:state.creatures)if(entry.second.visible && entry.second.monster && entry.second.name=="Trainer")++trainers;
        std::cout << "TRAINING_CONTEXT position=" << state.position.x << "," << state.position.y << "," << state.position.z << " trainers=" << trainers << " pz=" << ((state.states&16384)!=0) << std::endl;
        std::cout << "TRAINING_ACTIVE macros=!treinar,powerdown,target periods=100,100,200" << std::endl;
    }
    void onConnect() override { enableChecksum(); recv(); }
    void authenticate(uint32 timestamp,uint8 random) {
        OutputMessagePtr message(new OutputMessage);
        message->addU8(10); message->addU16(config.clientId); message->addU16(860);
        for(const auto byte:config.context) message->addU8(byte);
        const auto start=message->getMessageSize();
        message->addU8(0);
        uint32 key[4];
        if(RAND_bytes(reinterpret_cast<unsigned char*>(key),sizeof(key))!=1) throw std::runtime_error("secure RNG failed");
        setXteaKey(key[0],key[1],key[2],key[3]);
        for(auto part:key) message->addU32(part);
        OPENSSL_cleanse(key,sizeof(key));
        message->addU8(0); // ordinary player, never GM
        message->addString(credentials.account); message->addString(endpoint.name); message->addString(credentials.password);
        message->addU32(timestamp); message->addU8(random);
        message->addString("OTCv8"); message->addU16(300);
        const int padding=g_crypt.rsaGetSize()-static_cast<int>(message->getMessageSize()-start);
        if(padding<0) throw std::runtime_error("game credentials exceed encrypted block capacity");
        message->addPaddingBytes(padding); message->encryptRsa(); send(message); enableXteaEncryption();
        std::cout << "GAME_AUTH_REQUEST character=" << endpoint.name << "; fresh RSA/XTEA and server challenge" << std::endl;
    }
    void onError(const boost::system::error_code& error) override {
        std::cout << "game transport failure: " << error.message() << std::endl;
        result=3; done=true;
    }
    void onRecv(const InputMessagePtr& message) override {
        ++frames;
        lastFrame=std::chrono::steady_clock::now();
        try {
            if(first) {
                first=false;
                const auto size=message->getU16();
                if(size!=message->getUnreadSize()) throw std::runtime_error("invalid challenge size");
            }
            while(!message->eof()) {
                const auto opcode=message->getU8();
                currentOpcode=opcode;
                if(opcode==31 && !challenged) {
                    const auto timestamp=message->getU32(); const auto random=message->getU8();
                    if(!message->eof()) throw std::runtime_error("unexpected challenge extension");
                    challenged=true; authenticate(timestamp,random);
                } else if(opcode==20 && config.reconnect) {
                    auto notice=message->getString();
                    std::transform(notice.begin(),notice.end(),notice.begin(),[](unsigned char c){return std::tolower(c);});
                    const bool maintenance=notice.find("server save")!=std::string::npos || notice.find("maintenance")!=std::string::npos ||
                        notice.find("server is closed")!=std::string::npos || notice.find("server is currently closed")!=std::string::npos ||
                        notice.find("gameworld is currently closed")!=std::string::npos || notice.find("game world is closed")!=std::string::npos ||
                        notice.find("shutting down")!=std::string::npos || notice.find("server is saving")!=std::string::npos ||
                        notice.find("already logged in")!=std::string::npos || notice.find("already online")!=std::string::npos;
                    if(!notice.empty())OPENSSL_cleanse(notice.data(),notice.size());
                    std::cout<<"GAME_LOGIN_REJECTED opcode=20 retryable_server_state="<<maintenance<<std::endl;
                    result=maintenance ? 3 : 4;done=true;return;
                } else if(opcode==22 && config.reconnect) {
                    auto notice=message->getString();
                    if(!notice.empty())OPENSSL_cleanse(notice.data(),notice.size());
                    retryAfter=message->getU8();
                    std::cout<<"GAME_LOGIN_WAIT seconds="<<retryAfter<<std::endl;
                    result=3;done=true;return;
                } else if(opcode==21) {
                    auto notice=message->getString();
                    if(!notice.empty())OPENSSL_cleanse(notice.data(),notice.size());
                } else if(opcode==20 || opcode==22 || opcode==24) {
                    // Do not log server strings that could echo credentials/tokens.
                    std::cout << "GAME_LOGIN_REJECTED opcode=" << static_cast<unsigned>(opcode) << std::endl;
                    result=4; done=true; return;
                } else if(opcode==50) {
                    message->getU8(); auto buffer=message->getString();
                    if(!buffer.empty()) OPENSSL_cleanse(&buffer[0],buffer.size());
                } else if(opcode==67) {
                    auto count=message->getU16();
                    if(count>message->getUnreadSize()/2) throw std::runtime_error("truncated feature list");
                    for(unsigned i=0;i<count;++i) {
                        const auto feature=message->getU8(); const bool enabled=message->getU8()!=0;
                        if(feature==35) loginPending=enabled;
                        if(feature==36) newSpeedLaw=enabled;
                        if(feature==24) clientPing=enabled;
                        if(config.metadata && feature<state.metadata.features.size())state.metadata.features[feature]=enabled;
                    }
                } else if(opcode==10 && loginPending) {
                    OutputMessagePtr enter(new OutputMessage); enter->addU8(15); send(enter);
                } else if(opcode==10 || opcode==23) {
                    if(!challenged || online) throw std::runtime_error("unexpected login transition");
                    const auto playerId=message->getU32(); const auto beat=message->getU16();
                    if(newSpeedLaw) {message->getDouble();message->getDouble();message->getDouble();}
                    message->getU8();
                    if(!playerId || !beat) throw std::runtime_error("invalid player login header");
                    online=true; result=0; onlineSince=std::chrono::steady_clock::now();
                    state.playerId=playerId;
                    std::cout << "GAME_LOGIN_ACCEPTED character=" << endpoint.name << " player_id=" << playerId
                              << " server_beat=" << beat << std::endl;
                } else if(opcode==15 && online) {
                    std::cout << "GAME_ENTER_CONFIRMED" << std::endl;
                } else if(opcode==29 || opcode==30) {
                    const bool needsReply=(opcode==30 && !clientPing) || (opcode==29 && clientPing);
                    if(needsReply) {OutputMessagePtr reply(new OutputMessage);reply->addU8(30);send(reply);++pingReplies;}
                } else if(config.metadata && state.parse(opcode,message)) {
                } else {
                    if(config.metadata)state.compatible=false;
                    ++unparsedFrames;
                    if(!reportedOpcodes[opcode]) {
                        reportedOpcodes[opcode]=true;
                        std::cout << "GAME_UNPARSED opcode=" << static_cast<unsigned>(opcode) << " remaining=" << message->getUnreadSize() << std::endl;
                    }
                    if(!online) {
                        std::cout << "unsupported startup opcode=" << static_cast<unsigned>(opcode) << std::endl;
                        result=5;done=true;return;
                    }
                    if(config.metadata){result=5;done=true;return;}
                    // Never scan opaque map bytes for opcodes or fabricate game state.
                    // Retain no payload; consume only the bounded framed transport.
                    break;
                }
            }
            if(!done && !trainingStopped && config.metadata && state.ready() && !trainingStarted && !config.trainingModules.empty())attachTraining();
            recv();
        } catch(const std::exception& error) {
            if(config.metadata)std::cout << "GAME_STATE_PARSE_FAILURE opcode=" << currentOpcode << " unread=" << message->getUnreadSize() << " reason=" << error.what() << std::endl;
            std::cout << "game response incompatible or truncated" << std::endl;
            result=5;done=true;
        }
    }
};
class AccountLogin final : public Protocol {
public:
    AccountLogin(const LoginConfig& config, Credentials& credentials) : config(config), credentials(credentials) {}
    bool done=false, characterList=false;
    int result=1;
    CharacterEndpoint selected;
    unsigned matches=0;
    std::vector<CharacterEndpoint> endpoints;
private:
    LoginConfig config;
    Credentials& credentials;
    void onConnect() override {
        std::cout << "login TCP connected; testing public OTCv8 860 candidate" << std::endl;
        OutputMessagePtr message(new OutputMessage);
        message->addU8(1); // ClientEnterAccount
        message->addU16(config.clientId); // Default Linux ID, or explicitly configured protocol client ID.
        message->addU16(860);
        for (const auto byte:config.context) message->addU8(byte);
        message->addU32(config.dat); message->addU32(config.spr);
        message->addU32(config.pic);
        const auto start=message->getMessageSize();
        uint32 key[4];
        if (RAND_bytes(reinterpret_cast<unsigned char*>(key),sizeof(key))!=1) throw std::runtime_error("secure RNG failed");
        setXteaKey(key[0],key[1],key[2],key[3]);
        message->addU8(0);
        for (auto part:key) message->addU32(part);
        OPENSSL_cleanse(key,sizeof(key));
        message->addString(credentials.account); message->addString(credentials.password);
        message->addString("OTCv8"); message->addU16(300);
        const int padding=g_crypt.rsaGetSize() - static_cast<int>(message->getMessageSize()-start);
        if (padding<0) throw std::runtime_error("credentials exceed encrypted login block capacity");
        message->addPaddingBytes(padding);
        message->encryptRsa();
        enableChecksum(); send(message); enableXteaEncryption(); recv();
        std::cout << "RSA-encrypted account request sent once; awaiting character list" << std::endl;
    }
    void onError(const boost::system::error_code& error) override {
        std::cout << "login transport failure: " << error.message() << std::endl;
        done=true; result=3;
    }
    std::string redact(std::string text) {
        for (const auto* secret:{&credentials.account,&credentials.password}) {
            if (secret->empty()) continue;
            size_t pos=0;
            while ((pos=text.find(*secret,pos))!=std::string::npos) { text.replace(pos,secret->size(),"[redacted]"); pos+=10; }
        }
        for (char& c:text) if (static_cast<unsigned char>(c)<32) c=' ';
        return text;
    }
    void onRecv(const InputMessagePtr& message) override {
        try {
            bool rejected=false;
            while (!message->eof()) {
                const auto opcode=message->getU8();
                if (opcode==10 || opcode==11) {
                    std::cout << "login rejected: " << redact(message->getString()) << std::endl;
                    rejected=true;
                } else if (opcode==20) { message->getString(); std::cout << "login MOTD received" << std::endl; }
                else if (opcode==30) { std::cout << "login requires client update" << std::endl; rejected=true; }
                else if (opcode==40) { auto token=message->getString(); if (!token.empty()) OPENSSL_cleanse(&token[0],token.size()); }
                else if (opcode==100) {
                    const auto count=message->getU8();
                    endpoints.clear();
                    for (unsigned i=0;i<count;++i) {
                        auto name=message->getString(); message->getString(); auto ip=message->getU32(); auto port=message->getU16();
                        asio::ip::address_v4::bytes_type address{{static_cast<uint8>(ip),static_cast<uint8>(ip>>8),static_cast<uint8>(ip>>16),static_cast<uint8>(ip>>24)}};
                        endpoints.push_back({name,asio::ip::address_v4(address).to_string(),port});
                        if(!config.character.empty() && name==config.character) {
                            ++matches;
                            selected=endpoints.back();
                        }
                    }
                    message->getU16(); // premium days, 860 layout
                    std::cout << "CHARACTER_LIST count=" << endpoints.size() << std::endl;
                    characterList=true;
                } else throw std::runtime_error("unexpected opcode in public 860 account response");
            }
            if (!characterList && !rejected) {
                recv(); // MOTD/session key may arrive in a separate frame.
                return;
            }
            result=characterList && !rejected ? 0 : 4;
            if(result==0 && !config.character.empty() && (matches!=1 || !selected.port || selected.host=="0.0.0.0")) {
                std::cout << "requested character missing, ambiguous or invalid endpoint" << std::endl;
                result=6;
            }
        } catch (const std::exception&) {
            std::cout << "login response incompatible or truncated; no character list verified" << std::endl;
            result=5;
        }
        done=true;
    }
};
}
int selectCharacter(const LoginConfig& config, CharacterEndpoint& selected, const std::vector<CharacterEndpoint>& endpoints) {
    if(config.character.empty() && endpoints.empty()) {
        std::cout << "no characters available on this account" << std::endl;
        return 6;
    }
    if(!config.character.empty()) {
        return 0;
    }
    if(endpoints.empty()) {
        std::cout << "no characters available on this account" << std::endl;
        return 6;
    }
    for(size_t i=0;i<endpoints.size();++i) {
        std::cout << (i+1) << ". " << endpoints[i].name << std::endl;
    }
    std::cout << "Escolha o personagem: " << std::flush;
    std::string input;
    std::getline(std::cin,input);
    if(std::cin.eof()) {
        std::cout << std::endl;
        return 130;
    }
    size_t used=0;
    int choice=0;
    try {
        choice=std::stoi(input,&used);
        if(used!=input.size() || choice<1 || choice>static_cast<int>(endpoints.size())) {
            std::cout << "opção inválida: escolha um número entre 1 e " << endpoints.size() << std::endl;
            return 6;
        }
    } catch(...) {
        std::cout << "opção inválida: digite um número" << std::endl;
        return 6;
    }
    selected=endpoints[choice-1];
    return 0;
}
int runLoginProbe(int argc, char** argv) {
    rlimit noCore{0,0}; setrlimit(RLIMIT_CORE,&noCore); // credentials must never enter a core dump
    LoginConfig config;
    Credentials credentials;
    ProfileLock sessionLock;
    try {
        config=configOf(argc,argv);
        std::cout << "candidate: checksum + RSA/XTEA, account strings; client-id=" << config.clientId
                  << "; public signatures configured explicitly (defaults DAT/SPR=0); no compatibility assumed" << std::endl;
        Tty terminal; terminal.read("Conta (sem echo): ",credentials.account);
        terminal.read("Senha (sem echo): ",credentials.password);
        if (credentials.account.empty() || credentials.password.empty()) throw std::runtime_error("empty credentials");
    } catch (const std::exception& error) { std::cerr << error.what() << std::endl; return 2; }
    g_lua.init();
    g_crypt.rsaSetPublicKey(config.modulus,"65537");
    if (g_crypt.rsaGetSize()!=128) {
        g_lua.terminate();
        std::cerr << "public login candidate requires a 1024-bit RSA key" << std::endl;
        return 2;
    }
    stdext::shared_object_ptr<AccountLogin> login(new AccountLogin(config,credentials));
    stdext::shared_object_ptr<GameLoginSession> game;
    bool stopping=false, supervisorWorker=false;
    int workerReport=-1;
    unsigned retryAfter=0;
    boost::asio::signal_set signals(g_ioService,SIGINT,SIGTERM);
    signals.async_wait([&](const boost::system::error_code& ec,int) { if (!ec) { stopping=true;login->done=true; login->result=130; if(game){game->done=true;game->result=130;} } });
    auto guard=asio::make_work_guard(g_ioService);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    try {
        login->connect(config.host,config.port);
        while (!login->done && std::chrono::steady_clock::now()<deadline) {
            g_ioService.reset(); g_ioService.run_one_for(std::chrono::milliseconds(100));
            g_clock.update(); Connection::poll(); g_dispatcher.poll();
        }
        if (!login->done) std::cout << "login timeout; no character list verified" << std::endl;
    } catch (const std::exception&) { std::cout << "login protocol failure; no character list verified" << std::endl; login->result=5; }
    int result=login->result;
    login->disconnect();
    // Character selection: interactive if --character was not given; explicit if it was.
    // This must happen before the daemon fork so the TTY is still available for the prompt,
    // and so that config.character is set before post-selection validation.
    if(result==0) {
        if(config.character.empty()) {
            result=selectCharacter(config,login->selected,login->endpoints);
            if(result==0) config.character=login->selected.name; // propagate into config for logging/training
        }
        // Post-selection validation: daemon and --stay-online (holdSeconds==0) need a character.
        if(result==0 && (config.daemon || config.holdSeconds==0) && config.character.empty()) {
            std::cerr << "persistent mode requires a character selection" << std::endl;
            result=2;
        }
        // Post-selection validation: training-modules needs a character name to be known.
        if(result==0 && !config.trainingModules.empty() && config.character.empty()) {
            std::cerr << "live training requires an exact character" << std::endl;
            result=2;
        }
    }
    if(result==0 && config.daemon) {
        try {
            if(!config.profileDir.empty()) {
                std::filesystem::create_directories(config.profileDir);
                struct stat info{};
                if(lstat(config.profileDir.c_str(),&info) || !S_ISDIR(info.st_mode) || info.st_uid!=geteuid())
                    throw std::runtime_error("unsafe profile directory");
                if(chmod(config.profileDir.c_str(),0700))throw std::runtime_error("cannot protect profiles");
                std::string identity;
                for(const auto& part:{config.host,std::to_string(config.port),credentials.account,config.character})
                    identity+=std::to_string(part.size())+":"+part;
                unsigned char digest[SHA256_DIGEST_LENGTH];
                SHA256(reinterpret_cast<const unsigned char*>(identity.data()),identity.size(),digest);
                OPENSSL_cleanse(identity.data(),identity.size());
                std::ostringstream key;key<<std::hex<<std::setfill('0');for(auto byte:digest)key<<std::setw(2)<<unsigned(byte);
                const auto base=config.profileDir+"/"+key.str();
                config.pidFile=base+".pid";config.logFile=base+".log";
                sessionLock.acquire(base+".lock");
                const int fd=privateFile(base+".profile",O_WRONLY);
                std::ostringstream entry;entry<<"character="<<std::quoted(config.character)<<"\n";
                const auto content=entry.str();
                const bool failed=ftruncate(fd,0) || write(fd,content.data(),content.size())!=static_cast<ssize_t>(content.size());close(fd);
                if(failed)throw std::runtime_error("cannot save character profile");
                std::cout<<"PROFILE_READY character="<<config.character<<" pid_file="<<config.pidFile<<" log_file="<<config.logFile<<std::endl;
            } else sessionLock.acquire(config.pidFile+".lock");
        } catch(const std::exception& error) {std::cerr<<error.what()<<std::endl;return 2;}
        try {if(detach(config)) return 0;}
        catch(const std::exception& error) {std::cerr << error.what() << std::endl;return 2;}
    }
    if(result==0 && config.reconnect) {
        // The supervisor retains credentials only in memory and owns the profile lock.
        // Each worker has a fresh process, Lua VM state, game objects and crypto keys.
        unsigned attempt=0, delay=config.reconnectDelay;
        const auto pump=[&] {g_ioService.reset();g_ioService.run_one_for(std::chrono::milliseconds(100));Connection::poll();};
        while(!stopping) {
            ++attempt;
            const auto started=std::chrono::steady_clock::now();
            int report[2];
            if(pipe2(report,O_CLOEXEC)<0){result=2;break;}
            g_ioService.notify_fork(asio::io_service::fork_prepare);
            const pid_t owner=getpid();
            const auto child=fork();
            if(child<0) {g_ioService.notify_fork(asio::io_service::fork_parent);close(report[0]);close(report[1]);result=2;break;}
            if(child==0) {
                close(report[0]);workerReport=report[1];
                g_ioService.notify_fork(asio::io_service::fork_child);
                supervisorWorker=true;
                // Do not leave an orphan session if the supervisor itself is killed.
                if(prctl(PR_SET_PDEATHSIG,SIGTERM)<0 || getppid()!=owner) {result=130;break;}
                std::cout<<"SESSION_WORKER_STARTED pid="<<getpid()<<" attempt="<<attempt<<std::endl;
                if(attempt>1) {
                    login.reset(new AccountLogin(config,credentials));
                    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(10);
                    try {
                        login->connect(config.host,config.port);
                        while(!login->done && !stopping && std::chrono::steady_clock::now()<until)pump();
                        result=stopping ? 130 : (login->done ? login->result : 3);
                    } catch(const std::exception&) {result=5;}
                    login->disconnect();
                    if(result==0)std::cout<<"RECONNECT_CHARACTER_VERIFIED character="<<config.character<<std::endl;
                }
                break;
            }
            close(report[1]);
            g_ioService.notify_fork(asio::io_service::fork_parent);
            int status=0;
            pid_t waited=0;
            while((waited=waitpid(child,&status,WNOHANG))==0 && !stopping)pump();
            if(stopping) {
                kill(child,SIGTERM);
                const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(5);
                while((waited=waitpid(child,&status,WNOHANG))==0 && std::chrono::steady_clock::now()<until)pump();
                if(waited==0){kill(child,SIGKILL);while(waitpid(child,&status,0)<0 && errno==EINTR){}}
                close(report[0]);
                result=130;break;
            }
            if(waited<0){close(report[0]);result=2;break;}
            unsigned serverWait=0;
            if(read(report[0],&serverWait,sizeof(serverWait))!=sizeof(serverWait))serverWait=0;
            close(report[0]);
            const auto ended=WIFSIGNALED(status) ? 3 : WEXITSTATUS(status);
            std::cout<<"SESSION_WORKER_END pid="<<child<<" result="<<ended<<" signaled="<<WIFSIGNALED(status)<<std::endl;
            // Authentication, missing character, protocol and Lua failures need intervention.
            if(ended!=3){result=ended;break;}
            if(std::chrono::steady_clock::now()-started>=std::chrono::seconds(60))delay=config.reconnectDelay;
            unsigned random=0;if(RAND_bytes(reinterpret_cast<unsigned char*>(&random),sizeof(random))!=1)random=0;
            const auto seconds=std::max(serverWait,std::min(config.reconnectMaxDelay,delay+random%(delay/5+1)));
            std::cout<<"RECONNECT_WAIT seconds="<<seconds<<std::endl;
            const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
            while(!stopping && std::chrono::steady_clock::now()<until)pump();
            delay=std::min(config.reconnectMaxDelay,delay*2);
        }
        if(!supervisorWorker && stopping)result=130;
    }
    if(result==0 && (!config.reconnect || supervisorWorker)) {
        game.reset(new GameLoginSession(config,credentials,login->selected));
        auto gameDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        auto nextReport=std::chrono::steady_clock::now()+std::chrono::seconds(30);
        try {
            game->start();
            while(!game->done) {
                const auto now=std::chrono::steady_clock::now();
                if(!game->online && now>=gameDeadline) {std::cout << "game login timeout" << std::endl;game->result=3;break;}
                if(game->online && now-game->lastFrame>=std::chrono::seconds(90)) {std::cout<<"game idle timeout"<<std::endl;game->result=3;break;}
                if(game->online && config.holdSeconds && now-game->onlineSince>=std::chrono::seconds(config.holdSeconds)) break;
                if(game->online && now>=nextReport) {
                    std::cout << "GAME_HEALTH character=" << config.character
                              << " online_seconds=" << std::chrono::duration_cast<std::chrono::seconds>(now-game->onlineSince).count()
                              << " frames=" << game->frames << " ping_replies=" << game->pingReplies
                              << " last_frame_age=" << std::chrono::duration_cast<std::chrono::seconds>(now-game->lastFrame).count() << std::endl;
                    nextReport=now+std::chrono::seconds(30);
                    if(config.metadata)std::cout << "GAME_STATE map=" << game->state.mapKnown << " stats=" << game->state.statsKnown
                        << " states=" << game->state.statesKnown << " compatible=" << game->state.compatible << " mana=" << game->state.mana << "/" << game->state.maxMana
                        << " creatures=" << game->state.creatures.size() << std::endl;
                    if(game->trainingStarted)std::cout << "TRAINING_HEALTH active=1 requests=" << game->trainingRequests << " acknowledged=" << game->trainingAcknowledged
                        << " powerdown=" << game->powerdownRequests << " attacks=" << game->attackRequests << " target_updates=" << game->state.targetHealthUpdates << " in_pz=" << ((game->state.states&16384)!=0) << std::endl;
                }
                auto wait=g_dispatcher.nextWaitMs();if(wait<0 || wait>1000)wait=1000;
                g_ioService.reset();g_ioService.run_one_for(std::chrono::milliseconds(wait));
                g_clock.update();Connection::poll();g_dispatcher.poll();
            }
            result=game->result;
            retryAfter=game->retryAfter;
            game->stopTraining();
            game->logout();
            auto flushUntil=std::chrono::steady_clock::now()+std::chrono::milliseconds(250);
            while(std::chrono::steady_clock::now()<flushUntil) {g_ioService.reset();g_ioService.run_one_for(std::chrono::milliseconds(20));Connection::poll();g_dispatcher.poll();}
            std::cout << "GAME_SESSION_END frames=" << game->frames << " unparsed_frames=" << game->unparsedFrames << " ping_replies=" << game->pingReplies << " missing_mapped_things=" << game->state.missingMappedThings << std::endl;
        } catch(const std::exception&) {std::cout << "game protocol failure" << std::endl;game->stopTraining();result=5;}
        game->disconnect();
    }
    signals.cancel(); Connection::poll(); g_dispatcher.shutdown(); game.reset(); login.reset();
    g_lua.terminate(); Connection::terminate();
    if(config.daemon && !supervisorWorker) unlink(config.pidFile.c_str());
    if(workerReport>=0){const auto ignored=write(workerReport,&retryAfter,sizeof(retryAfter));(void)ignored;close(workerReport);}
    return result;
}
}
