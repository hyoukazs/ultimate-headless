// Adapter de teste, distinto de ProtocolGame: sobrescreve os callbacks
// protegidos e usa send()/recv() originais. Payloads sintéticos; sem login,
// sem game state. Sem reter referências Lua (hooks vivos via callGlobalField;
// ausente = no-op). Seguro contra VM destruída: só dispara durante poll ou
// shutdown do dispatcher, sempre antes de g_lua.terminate().
#ifndef PROTO_TEST_H
#define PROTO_TEST_H

#include <framework/net/protocol.h>

class TestProtocol : public Protocol {
public:
    std::function<void()> connectHook;
    std::function<void(const std::string&)> errorHook;
    std::function<void(const std::string&)> recvHook;

protected:
    void onConnect() override
    {
        recv(); // rearma como ProtocolGame
        if (connectHook)
            connectHook();
    }
    void onRecv(const InputMessagePtr& message) override
    {
        if (recvHook)
            recvHook(message->getBodyBuffer());
        recv(); // rearma como ProtocolGame
    }
    void onError(const boost::system::error_code& ec) override
    {
        if (errorHook)
            errorHook(ec.message());
    }
};

typedef stdext::shared_object_ptr<TestProtocol> TestProtocolPtr;

#endif
