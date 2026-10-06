// Protótipo headless — main sobre framework OTCv8 real (Lua/dispatcher/rede).
//
// Usa g_lua (LuaJIT), g_dispatcher (EventDispatcher), g_clock, Connection e
// Protocol/InputMessage/OutputMessage originais, com bindings próprios
// (log/later/now/quit, net*, core*, proto*, msg*). O loop bloqueia até I/O
// ou até a próxima deadline real do dispatcher (run_one_for + poll, nunca
// sleep fixo curto com trabalho pendente); g_clock é atualizado antes de
// cada poll.
// Rede SOMENTE loopback (127.0.0.1, imposto no binding); payloads de teste
// sintéticos; sem assets/envelopes/login do NTO.
// Códigos de saída: 0 ok; 2 erro de bootstrap/uso/validação de binding;
// 3 erro inesperado.

#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include "options.h"
#include "login.h"
#include <csignal>
#include <framework/core/resourcemanager.h>
#include <physfs.h>
#include <filesystem>
#include <framework/luaengine/luainterface.h>
#include <framework/core/eventdispatcher.h>
#include <framework/core/clock.h>
#include <framework/net/connection.h>
#include <framework/stdext/time.h>

#include "protocol_adapter.h"

// Padrão do próprio framework (protocol.cpp/server.cpp fazem o mesmo).
extern asio::io_service g_ioService;

namespace {

std::atomic<bool> s_done{false};
std::chrono::steady_clock::time_point s_start;
long s_wakeups = 0;
int s_exitCode = 0;

struct Slot {
    ScheduledEventPtr ev;
    int fnref = 0;
    bool done = false;
    // Sem deadline espelhada: a espera consulta o dispatcher
    // (EventDispatcher::nextWaitMs), fonte única e autoritativa.
};

std::vector<std::shared_ptr<Slot>> s_pending;

void eraseSlot(const std::shared_ptr<Slot>& slot)
{
    for (auto it = s_pending.begin(); it != s_pending.end(); ++it) {
        if (*it == slot) {
            s_pending.erase(it);
            return;
        }
    }
}

// Dispara um timer: marca, executa o callback Lua (erros contidos pelo
// signalCall; qualquer exceção restante não vaza) e libera a referência.
// Nunca roda contra VM destruída: slots pendentes são cancelados e
// liberados em finalize(), antes de g_lua.terminate().
void fireSlot(const std::shared_ptr<Slot>& slot)
{
    slot->done = true;
    eraseSlot(slot);
    try {
        g_lua.getRef(slot->fnref);
        g_lua.signalCall(0);
    } catch (const std::exception& e) {
        std::cerr << "timer callback failed: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "timer callback failed (unknown)" << std::endl;
    }
    g_lua.unref(slot->fnref);
}

// Falha de validação de binding: sem throw (neste build LuaJIT o erro Lua
// vindo de um binding propaga como exceção estrangeira e termina em fatal).
// Diagnóstico + saída ordenada, exit 2.
int failUsage(const char* where, const char* msg)
{
    std::cerr << where << ": " << msg << std::endl;
    s_exitCode = 2;
    s_done = true;
    return 0;
}

int l_headlessModule(LuaInterface* lua)
{
    if (!lua->isString(-1)) return failUsage("dofile", "module path required");
    const auto path = lua->popString();
    if (path != "/scripts/runtime.lua" && path != "/scripts/powerdown.lua" &&
        path != "/scripts/trainer.lua" && path != "/scripts/net_reconnect.lua" &&
        path != "/scripts/training.lua")
        return failUsage("dofile", "module is outside the headless allowlist");
    try {
        lua->loadScript(path);
        return lua->safeCall(0, -1);
    } catch (const std::exception&) {
        return failUsage("dofile", "headless module failed to load");
    }
}

int l_log(LuaInterface* lua)
{
    std::cout << "[lua] " << lua->popString() << std::endl;
    return 0;
}

int l_now(LuaInterface* lua)
{
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - s_start).count();
    lua->pushInteger((long)ms);
    return 1;
}

int l_later(LuaInterface* lua)
{
    // Pilha: [delay, func]. Tudo validado ANTES de reter referência.
    if (!lua->isFunction(-1))
        return failUsage("later", "callback must be a function");
    if (!lua->isNumber(-2))
        return failUsage("later", "delay must be a number");
    const double d = lua->toNumber(-2);
    if (!(d >= 0) || d > INT_MAX || d != std::floor(d))
        return failUsage("later", "delay must be an integer in [0, 2147483647]");
    const int fnref = lua->ref();
    const int delay = lua->popInteger();
    // R5: o ScheduledEvent carimba g_clock.millis() + delay no momento da
    // criação; g_clock só é atualizado antes do poll, então após bootstrap
    // ou callback demorados o carimbo sairia defasado e o timer dispararia
    // cedo. Atualiza aqui: delay conta desde later(). Sem patch vendor
    // (API pública do Clock).
    g_clock.update();
    auto slot = std::make_shared<Slot>();
    slot->fnref = fnref;
    slot->ev = g_dispatcher.scheduleEventEx("later", [slot] { fireSlot(slot); }, delay);
    s_pending.push_back(slot);
    return 0;
}

// Timers diretos no core, SEM espelho later/ref: o callback invoca o hook
// Lua vivo no momento do disparo (ausente = no-op). Nada a liberar no
// finalize além do shutdown do dispatcher (cancela o pendente).
int l_coreSchedule(LuaInterface* lua)
{
    if (!lua->isNumber(-1))
        return failUsage("coreSchedule", "delay must be a number");
    const double d = lua->toNumber(-1);
    if (!(d >= 0) || d > INT_MAX || d != std::floor(d))
        return failUsage("coreSchedule", "delay must be an integer in [0, 2147483647]");
    const int delay = lua->popInteger();
    g_clock.update();
    g_dispatcher.scheduleEventEx("coreRaw", [] {
        g_lua.callGlobalField("g_nettest", "onRawTimer");
    }, delay);
    return 0;
}

ScheduledEventPtr s_cycle;

int l_coreCycle(LuaInterface* lua)
{
    if (!lua->isNumber(-1))
        return failUsage("coreCycle", "delay must be a number");
    const double d = lua->toNumber(-1);
    if (!(d >= 1) || d > INT_MAX || d != std::floor(d))
        return failUsage("coreCycle", "delay must be an integer in [1, 2147483647]");
    const int delay = lua->popInteger();
    g_clock.update();
    s_cycle = g_dispatcher.cycleEventEx("coreCycle", [] {
        g_lua.callGlobalField("g_nettest", "onCoreCycle");
    }, delay);
    return 0;
}

int l_coreCancel(LuaInterface* lua)
{
    (void)lua;
    if (s_cycle) {
        s_cycle->cancel();
        s_cycle.reset();
    }
    return 0;
}

int l_quit(LuaInterface* lua)
{
    (void)lua;
    s_done = true;
    return 0;
}

// --- Rede loopback (Connection original) ---

ConnectionPtr s_conn;
int s_connHook = -1; // onConnect (uso único)
int s_errHook = -1;  // onError (persistente até finalize)

struct NetRead {
    int fnref = -1;
    bool done = false;
};

std::vector<std::shared_ptr<NetRead>> s_reads;

void eraseRead(const std::shared_ptr<NetRead>& slot)
{
    for (auto it = s_reads.begin(); it != s_reads.end(); ++it) {
        if (*it == slot) {
            s_reads.erase(it);
            return;
        }
    }
}

void fireNetRead(const std::shared_ptr<NetRead>& slot, const std::string& data)
{
    slot->done = true;
    eraseRead(slot);
    try {
        g_lua.getRef(slot->fnref);
        g_lua.pushString(data);
        g_lua.signalCall(1);
    } catch (const std::exception& e) {
        std::cerr << "netread callback failed: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "netread callback failed (unknown)" << std::endl;
    }
    g_lua.unref(slot->fnref);
}

void fireNetError(const std::string& message)
{
    // Erro de conexão é terminal para reads pendentes (close anula o
    // callback no core): aposenta-os aqui para rediscagem limpa.
    for (const auto& r : s_reads) {
        if (!r->done) {
            r->done = true;
            g_lua.unref(r->fnref);
        }
    }
    s_reads.clear();
    if (s_errHook < 0) {
        std::cerr << "net-error (no hook): " << message << std::endl;
        return;
    }
    try {
        g_lua.getRef(s_errHook);
        g_lua.pushString(message);
        g_lua.signalCall(1);
    } catch (const std::exception& e) {
        std::cerr << "neterror callback failed: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "neterror callback failed (unknown)" << std::endl;
    }
}

int l_netConnect(LuaInterface* lua)
{
    // Pilha: [host, port, onConnect]. Uma conexão por execução.
    if (!lua->isFunction(-1))
        return failUsage("netConnect", "callback must be a function");
    if (!lua->isNumber(-2))
        return failUsage("netConnect", "port must be a number");
    if (!lua->isString(-3))
        return failUsage("netConnect", "host must be a string");
    const double p = lua->toNumber(-2);
    if (!(p >= 1) || p > 65535 || p != std::floor(p))
        return failUsage("netConnect", "port must be an integer in [1, 65535]");
    const std::string host = lua->toString(-3);
    if (host != "127.0.0.1")
        return failUsage("netConnect", "only loopback 127.0.0.1 is allowed");
    // Uma conexão VIVA por execução; morta anterior é descartada para
    // permitir rediscagem (backoff). Segura: close() já cancelou o asio.
    if (s_conn && (s_conn->isConnected() || s_conn->isConnecting()))
        return failUsage("netConnect", "already connected");
    s_conn.reset();
    if (s_connHook >= 0) {
        g_lua.unref(s_connHook);
        s_connHook = -1;
    }
    const int fnref = lua->ref();
    const int port = lua->popInteger();
    lua->popString();
    s_connHook = fnref;
    s_conn = ConnectionPtr(new Connection);
    s_conn->setErrorCallback([](const boost::system::error_code& ec) {
        fireNetError(ec.message());
    });
    s_conn->connect(host, (uint16)port, [] {
        if (s_connHook < 0)
            return;
        const int fnref = s_connHook;
        s_connHook = -1;
        try {
            g_lua.getRef(fnref);
            g_lua.signalCall(0);
        } catch (const std::exception& e) {
            std::cerr << "connect callback failed: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "connect callback failed (unknown)" << std::endl;
        }
        g_lua.unref(fnref);
    });
    return 0;
}

int l_netOnError(LuaInterface* lua)
{
    if (!lua->isFunction(-1))
        return failUsage("netOnError", "callback must be a function");
    if (s_errHook >= 0)
        g_lua.unref(s_errHook);
    s_errHook = lua->ref();
    return 0;
}

int l_netSend(LuaInterface* lua)
{
    if (!lua->isString(-1))
        return failUsage("netSend", "payload must be a string");
    const std::string data = lua->popString();
    if (!s_conn || !s_conn->isConnected())
        return failUsage("netSend", "not connected");
    s_conn->write((uint8*)data.data(), data.size());
    return 0;
}

int l_netRead(LuaInterface* lua)
{
    // Pilha: [nbytes, func]. Uma leitura pendente por vez (a Connection
    // original guarda um único m_recvCallback — mesma restrição do core).
    if (!lua->isFunction(-1))
        return failUsage("netRead", "callback must be a function");
    if (!lua->isNumber(-2))
        return failUsage("netRead", "nbytes must be a number");
    const double n = lua->toNumber(-2);
    if (!(n >= 1) || n > 327680 || n != std::floor(n))
        return failUsage("netRead", "nbytes must be an integer in [1, 327680]");
    if (!s_conn || !s_conn->isConnected())
        return failUsage("netRead", "not connected");
    for (const auto& r : s_reads) {
        if (!r->done)
            return failUsage("netRead", "read already pending");
    }
    const int fnref = lua->ref();
    const int nbytes = lua->popInteger();
    auto slot = std::make_shared<NetRead>();
    slot->fnref = fnref;
    s_conn->read((uint32)nbytes, [slot](uint8* buffer, uint32 size) {
        fireNetRead(slot, std::string((const char*)buffer, size));
    });
    s_reads.push_back(slot);
    return 0;
}

// --- Protocolo sintético (InputMessage/OutputMessage/Protocol originais) ---

TestProtocolPtr s_proto;
int s_pConnHook = -1, s_pRecvHook = -1, s_pErrHook = -1;

enum class FTag { U8, U16, U32, U64, STR };

struct Field {
    FTag tag;
    uint64 num = 0;
    std::string str;
};

bool tagOf(const std::string& name, FTag& tag)
{
    if (name == "u8") tag = FTag::U8;
    else if (name == "u16") tag = FTag::U16;
    else if (name == "u32") tag = FTag::U32;
    else if (name == "u64") tag = FTag::U64;
    else if (name == "str") tag = FTag::STR;
    else return false;
    return true;
}

// R2: valida array Lua denso 1..N no índice absoluto (sem buracos, extras
// ou chaves não inteiras) e retorna N. O consumo é por rawGeti(i) em ordem:
// lua_next não ordena e aceitaria mapas/esparsos silenciosamente.
bool checkDenseArray(LuaInterface* lua, int absIdx, int& n)
{
    n = 0;
    while (true) {
        lua->rawGeti(n + 1, absIdx);
        const bool isNil = lua->isNil(-1);
        lua->pop(1);
        if (isNil)
            break;
        ++n;
    }
    int keys = 0;
    lua->pushNil();
    while (lua->next(absIdx)) {
        // [..., key, value]: só a key interessa; pop só do valor,
        // a key fica para o próximo next()
        const bool numKey = lua->isNumber(-2);
        const double kd = numKey ? lua->toNumber(-2) : 0;
        lua->pop(1);
        if (!numKey || kd < 1 || kd > n || kd != std::floor(kd))
            return false;
        ++keys;
    }
    return keys == n;
}

// Lê array Lua de pares {"tag", valor} no topo. Falha de argumento = saída
// ordenada (failUsage); nunca throw através do binding.
bool readFields(LuaInterface* lua, std::vector<Field>& out)
{
    if (!lua->isTable(-1)) {
        failUsage("spec", "expected array of {tag, value}");
        return false;
    }
    const int absIdx = lua->stackSize();
    int n = 0;
    if (!checkDenseArray(lua, absIdx, n)) {
        failUsage("spec", "expected dense array 1..N");
        return false;
    }
    for (int i = 1; i <= n; ++i) {
        lua->rawGeti(i, absIdx);
        // entry = {"tag", valor}
        if (!lua->isTable(-1)) {
            failUsage("spec", "each entry must be {tag, value}");
            return false;
        }
        lua->rawGeti(1, -1);
        if (!lua->isString(-1)) {
            failUsage("spec", "tag must be a string");
            return false;
        }
        const std::string name = lua->popString();
        Field f;
        if (!tagOf(name, f.tag)) {
            failUsage("spec", "unknown tag (u8,u16,u32,u64,str)");
            return false;
        }
        lua->rawGeti(2, -1);
        if (f.tag == FTag::STR) {
            if (!lua->isString(-1)) {
                failUsage("spec", "str value must be a string");
                return false;
            }
            f.str = lua->popString();
            // R1: addString grava o comprimento em u16; 65536 enrolaria
            // para 0 (string vazia silenciosa no decode). Cap real: 65535.
            if (f.str.size() > 65535) {
                failUsage("spec", "str too long (max 65535 for u16 length)");
                return false;
            }
        } else {
            if (!lua->isNumber(-1)) {
                failUsage("spec", "numeric value must be a number");
                return false;
            }
            const double d = lua->toNumber(-1);
            lua->pop(1);
            uint64 max = 255;
            if (f.tag == FTag::U16) max = 65535;
            else if (f.tag == FTag::U32) max = 4294967295ULL;
            else if (f.tag == FTag::U64) max = 9007199254740991ULL; // double exato
            if (!(d >= 0) || d > (double)max || d != std::floor(d)) {
                failUsage("spec", "integer out of range for tag");
                return false;
            }
            f.num = (uint64)d;
        }
        out.push_back(f);
        lua->pop(1); // entry
    }
    return true;
}

// Lê array Lua de tags ["u8","str",...] no topo (para protoExpect).
bool readTags(LuaInterface* lua, std::vector<FTag>& out)
{
    if (!lua->isTable(-1)) {
        failUsage("expect", "expected array of tags");
        return false;
    }
    const int absIdx = lua->stackSize();
    int n = 0;
    if (!checkDenseArray(lua, absIdx, n)) {
        failUsage("expect", "expected dense array 1..N");
        return false;
    }
    for (int i = 1; i <= n; ++i) {
        lua->rawGeti(i, absIdx);
        if (!lua->isString(-1)) {
            failUsage("expect", "tag must be a string");
            return false;
        }
        FTag tag;
        if (!tagOf(lua->popString(), tag)) {
            failUsage("expect", "unknown tag (u8,u16,u32,u64,str)");
            return false;
        }
        out.push_back(tag);
    }
    return true;
}

std::vector<FTag> s_schema;

OutputMessagePtr buildMessage(const std::vector<Field>& fields)
{
    OutputMessagePtr msg(new OutputMessage);
    for (const auto& f : fields) {
        switch (f.tag) {
            case FTag::U8: msg->addU8((uint8)f.num); break;
            case FTag::U16: msg->addU16((uint16)f.num); break;
            case FTag::U32: msg->addU32((uint32)f.num); break;
            case FTag::U64: msg->addU64((uint64)f.num); break;
            case FTag::STR: msg->addString(f.str); break;
        }
    }
    return msg;
}

std::string hexOf(const uint8* data, size_t size)
{
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 15]);
    }
    return out;
}

// Empilha valores parseados de body pela schema. Truncamento lança
// stdext::exception (chamador decide: teste local retorna nil+erro;
// recv de rede falha ordenada).
void pushParsed(const std::string& body, const std::vector<FTag>& tags)
{
    InputMessage msg;
    msg.setBuffer(body);
    g_lua.newTable();
    int i = 1;
    for (const auto& tag : tags) {
        switch (tag) {
            case FTag::U8: g_lua.pushInteger((long)msg.getU8()); break;
            case FTag::U16: g_lua.pushInteger((long)msg.getU16()); break;
            case FTag::U32: g_lua.pushInteger((long)msg.getU32()); break;
            case FTag::U64: g_lua.pushNumber((double)msg.getU64()); break;
            case FTag::STR: g_lua.pushString(msg.getString()); break;
        }
        g_lua.rawSeti(i++, -2);
    }
}

void fireProtoRecv(const std::string& body)
{
    if (s_pRecvHook < 0)
        return;
    try {
        g_lua.getRef(s_pRecvHook);
        if (s_schema.empty()) {
            g_lua.pushString(body);
        } else {
            pushParsed(body, s_schema);
        }
        g_lua.signalCall(1);
    } catch (const std::exception& e) {
        std::cerr << "protorecv failed (truncated?): " << e.what() << std::endl;
        s_exitCode = 2;
        s_done = true;
    } catch (...) {
        std::cerr << "protorecv failed (unknown)" << std::endl;
        s_exitCode = 2;
        s_done = true;
    }
}

void fireProtoError(const std::string& message)
{
    if (s_pErrHook < 0) {
        std::cerr << "proto-error (no hook): " << message << std::endl;
        return;
    }
    try {
        g_lua.getRef(s_pErrHook);
        g_lua.pushString(message);
        g_lua.signalCall(1);
    } catch (const std::exception& e) {
        std::cerr << "protoerror callback failed: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "protoerror callback failed (unknown)" << std::endl;
    }
}

int l_protoConnect(LuaInterface* lua)
{
    if (!lua->isFunction(-1))
        return failUsage("protoConnect", "callback must be a function");
    if (!lua->isNumber(-2))
        return failUsage("protoConnect", "port must be a number");
    if (!lua->isString(-3))
        return failUsage("protoConnect", "host must be a string");
    const double p = lua->toNumber(-2);
    if (!(p >= 1) || p > 65535 || p != std::floor(p))
        return failUsage("protoConnect", "port must be an integer in [1, 65535]");
    const std::string host = lua->toString(-3);
    if (host != "127.0.0.1")
        return failUsage("protoConnect", "only loopback 127.0.0.1 is allowed");
    if (s_conn || (s_proto && (s_proto->isConnected() || s_proto->isConnecting())))
        return failUsage("protoConnect", "already connected");
    if (s_proto) {
        s_proto->disconnect();
        s_proto.reset();
    }
    if (s_pConnHook >= 0) {
        g_lua.unref(s_pConnHook);
        s_pConnHook = -1;
    }
    const int fnref = lua->ref();
    const int port = lua->popInteger();
    lua->popString();
    s_pConnHook = fnref;
    s_proto = TestProtocolPtr(new TestProtocol);
    s_proto->errorHook = [](const std::string& msg) { fireProtoError(msg); };
    s_proto->recvHook = [](const std::string& body) { fireProtoRecv(body); };
    s_proto->connectHook = [] {
        if (s_pConnHook < 0)
            return;
        const int fnref = s_pConnHook;
        s_pConnHook = -1;
        try {
            g_lua.getRef(fnref);
            g_lua.signalCall(0);
        } catch (const std::exception& e) {
            std::cerr << "protoconnect callback failed: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "protoconnect callback failed (unknown)" << std::endl;
        }
        g_lua.unref(fnref);
    };
    s_proto->connect(host, (uint16)port);
    return 0;
}

int l_protoOn(LuaInterface* lua)
{
    if (!lua->isFunction(-1))
        return failUsage("protoOn", "callback must be a function");
    if (!lua->isString(-2))
        return failUsage("protoOn", "event must be a string");
    const std::string evt = lua->toString(-2);
    if (evt != "connect" && evt != "recv" && evt != "error")
        return failUsage("protoOn", "event must be connect, recv or error");
    const int fnref = lua->ref();
    lua->popString();
    int* slot = nullptr;
    if (evt == "connect") slot = &s_pConnHook;
    else if (evt == "recv") slot = &s_pRecvHook;
    else slot = &s_pErrHook;
    if (*slot >= 0)
        g_lua.unref(*slot);
    *slot = fnref;
    return 0;
}

// Espera hook de connect registrado via protoOn("connect").
int l_protoSend(LuaInterface* lua)
{
    std::vector<Field> fields;
    if (!readFields(lua, fields))
        return 0;
    lua->pop(1); // spec
    if (!s_proto || !s_proto->isConnected())
        return failUsage("protoSend", "not connected");
    try {
        OutputMessagePtr msg = buildMessage(fields);
        // R1: framing simples é u16; o buffer físico (327680) NÃO é o
        // limite do frame. Rejeita antes do send; sem big packets.
        if (msg->getMessageSize() > 65535)
            return failUsage("protoSend", "frame body exceeds 65535 (u16 framing)");
        s_proto->send(msg);
    } catch (const std::exception& e) {
        return failUsage("protoSend", e.what());
    }
    return 0;
}

int l_protoExpect(LuaInterface* lua)
{
    std::vector<FTag> tags;
    if (!readTags(lua, tags))
        return 0;
    lua->pop(1); // spec
    s_schema = tags;
    return 0;
}

// Unidade local, sem socket: hex do corpo construído pelas APIs originais.
int l_msgEncode(LuaInterface* lua)
{
    std::vector<Field> fields;
    if (!readFields(lua, fields))
        return 0;
    lua->pop(1); // spec
    try {
        OutputMessagePtr msg = buildMessage(fields);
        const std::string body = msg->getBuffer();
        lua->pushString(hexOf((const uint8*)body.data(), body.size()));
    } catch (const std::exception& e) {
        return failUsage("msgEncode", e.what());
    }
    return 1;
}

// Unidade local: parse de bytes hex pelas APIs originais. Truncamento
// retorna (nil, erro) para asserção do teste — sem abort, sem exit.
int l_msgDecode(LuaInterface* lua)
{
    if (!lua->isString(-2) || !lua->isTable(-1))
        return failUsage("msgDecode", "expected (hexstring, tags)");
    const std::string hex = lua->toString(-2);
    if (hex.size() % 2 != 0)
        return failUsage("msgDecode", "odd hex length");
    std::string bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int hi = nib(hex[i]), lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0)
            return failUsage("msgDecode", "invalid hex digit");
        bytes.push_back((char)((hi << 4) | lo));
    }
    // spec no topo: array denso de tags (mesma regra R2)
    std::vector<FTag> tags;
    const int specIdx = lua->stackSize();
    int ntags = 0;
    if (!checkDenseArray(lua, specIdx, ntags))
        return failUsage("msgDecode", "expected dense array 1..N");
    for (int i = 1; i <= ntags; ++i) {
        lua->rawGeti(i, specIdx);
        if (!lua->isString(-1))
            return failUsage("msgDecode", "tag must be a string");
        FTag tag;
        if (!tagOf(lua->popString(), tag))
            return failUsage("msgDecode", "unknown tag");
        tags.push_back(tag);
    }
    lua->pop(2); // hex + spec
    try {
        pushParsed(bytes, tags);
    } catch (const std::exception& e) {
        g_lua.pushNil();
        g_lua.pushString(e.what());
        return 2;
    } catch (...) {
        g_lua.pushNil();
        g_lua.pushString("unknown decode error");
        return 2;
    }
    return 1;
}

// Drena o dispatcher e I/O, fecha a Connection ANTES de dispatcher/Lua,
// libera referências pendentes e encerra a VM. Caminho único de saída.
int finalize(int code)
{
    if (s_proto)
        s_proto->disconnect();
    if (s_conn)
        s_conn->close();
    g_dispatcher.shutdown();
    for (const auto& slot : s_pending) {
        if (!slot->done)
            g_lua.unref(slot->fnref);
    }
    s_pending.clear();
    for (const auto& r : s_reads) {
        if (!r->done)
            g_lua.unref(r->fnref);
    }
    s_reads.clear();
    if (s_connHook >= 0) {
        g_lua.unref(s_connHook);
        s_connHook = -1;
    }
    if (s_errHook >= 0) {
        g_lua.unref(s_errHook);
        s_errHook = -1;
    }
    if (s_pConnHook >= 0) {
        g_lua.unref(s_pConnHook);
        s_pConnHook = -1;
    }
    if (s_pRecvHook >= 0) {
        g_lua.unref(s_pRecvHook);
        s_pRecvHook = -1;
    }
    if (s_pErrHook >= 0) {
        g_lua.unref(s_pErrHook);
        s_pErrHook = -1;
    }
    if (s_cycle)
        s_cycle.reset();
    g_lua.terminate();
    g_resources.terminate();
    Connection::terminate();
    s_conn.reset();
    s_proto.reset();
    if (code == 0)
        std::cout << "shutdown ok wakeups=" << s_wakeups << std::endl;
    return code;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc>1 && std::string(argv[1])=="--login") return headless::runLoginProbe(argc,argv);
    headless::ProfileLock profileLock;
    headless::Options options;
    try { options = headless::parseOptions(argc, argv, profileLock); }
    catch (const std::exception& e) { std::cerr << e.what() << std::endl; return 2; }
    boost::asio::signal_set signals(g_ioService, SIGINT, SIGTERM);
    signals.async_wait([](const boost::system::error_code& ec, int) {
        if (!ec) s_done = true; // Asio dispatches on the owner thread, waking the wait.
    });
    std::ifstream in(options.script, std::ios::binary);
    if (!in) {
        std::cerr << "bootstrap não encontrado" << std::endl;
        return 2;
    }
    std::ostringstream ss;
    ss << in.rdbuf();

    // Mount only the selected script modules, never the client's UI/assets.
    g_resources.init(argv[0]);
    PHYSFS_permitSymbolicLinks(0);
    const auto modules = std::filesystem::path(options.modules);
    if (std::filesystem::is_directory(modules) && !PHYSFS_mount(modules.c_str(), "scripts", 1)) {
        g_resources.terminate();
        std::cerr << "cannot mount script modules" << std::endl;
        return 2;
    }

    try {
        s_start = std::chrono::steady_clock::now();
        g_lua.init();
        g_lua.bindGlobalFunction("dofile", l_headlessModule);
        g_lua.bindGlobalFunction("log", l_log);
        g_lua.bindGlobalFunction("now", l_now);
        g_lua.bindGlobalFunction("later", l_later);
        g_lua.bindGlobalFunction("quit", l_quit);
        g_lua.bindGlobalFunction("netConnect", l_netConnect);
        g_lua.bindGlobalFunction("netOnError", l_netOnError);
        g_lua.bindGlobalFunction("netSend", l_netSend);
        g_lua.bindGlobalFunction("netRead", l_netRead);
        g_lua.bindGlobalFunction("coreSchedule", l_coreSchedule);
        g_lua.bindGlobalFunction("coreCycle", l_coreCycle);
        g_lua.bindGlobalFunction("coreCancel", l_coreCancel);
        g_lua.bindGlobalFunction("protoConnect", l_protoConnect);
        g_lua.bindGlobalFunction("protoOn", l_protoOn);
        g_lua.bindGlobalFunction("protoSend", l_protoSend);
        g_lua.bindGlobalFunction("protoExpect", l_protoExpect);
        g_lua.bindGlobalFunction("msgEncode", l_msgEncode);
        g_lua.bindGlobalFunction("msgDecode", l_msgDecode);
        if (!options.host.empty()) {
            g_lua.pushString(options.host);
            g_lua.setGlobal("NET_HOST");
            g_lua.pushInteger(options.port);
            g_lua.setGlobal("NET_PORT");
        }
        g_lua.runBuffer(ss.str(), std::string("@") + options.script);
    } catch (const std::exception& e) {
        std::cerr << "bootstrap failed: " << e.what() << std::endl;
        return finalize(2);
    } catch (...) {
        std::cerr << "bootstrap failed (unknown)" << std::endl;
        return finalize(2);
    }

    try {
        g_clock.update();
        Connection::poll();
        g_dispatcher.poll();
        ++s_wakeups;
        // Work guard: sem ele, run_one_for retorna na hora quando o
        // io_service está sem trabalho pendente (comportamento documentado
        // do asio), virando busy loop. Com o guard, a espera bloqueia até
        // I/O real ou até a deadline — sem polling. Estático de função para
        // construir DEPOIS de g_ioService (TU distinta, sem fiasco de ordem).
        // finalize() chama Connection::terminate() (stop), após o loop.
        static const auto workGuard = asio::make_work_guard(g_ioService);
        (void)workGuard;
        while (!s_done) {
            // Fonte única: próximo prazo real do dispatcher (0 = imediato,
            // -1 = vazio). I/O acorda antes via run_one_for; fallback de
            // 100ms só sem trabalho algum. g_clock atualizado antes de
            // consultar (remainingTicks deriva dele) e antes de cada poll.
            g_clock.update();
            const ticks_t wait = g_dispatcher.nextWaitMs();
            auto dur = (wait < 0) ? std::chrono::milliseconds(100)
                                  : std::chrono::milliseconds(wait);
            g_ioService.reset();
            if (dur.count() > 0)
                g_ioService.run_one_for(dur);
            g_clock.update();
            Connection::poll();
            g_dispatcher.poll();
            ++s_wakeups;
        }
    } catch (const std::exception& e) {
        std::cerr << "loop failed: " << e.what() << std::endl;
        return finalize(3);
    } catch (...) {
        std::cerr << "loop failed (unknown)" << std::endl;
        return finalize(3);
    }
    return finalize(s_exitCode);
}
