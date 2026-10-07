# Segurança e auditoria

Este documento explica o que o programa faz com sua conta e como verificar cada
afirmação sem precisar confiar no autor.

## Conta e senha

- **Entrada:** lidas do terminal (`/dev/tty`) com o eco desligado. Não existe
  opção de linha de comando, variável de ambiente ou arquivo para fornecê-las;
  qualquer opção desconhecida é recusada. Ver `class Tty` e `configOf` em
  [`src/headless/login.cpp`](src/headless/login.cpp).
- **Destino:** enviadas somente nos pacotes de login do protocolo do jogo, para
  `cliente.ntoultimate.com.br:7173` e para o servidor de jogo que esse login
  indica. Outro host é recusado. O código de login não contém HTTP, webhook,
  telemetria nem envio para o autor ou para terceiros (procure por `connect(`
  e `addString(credentials` em `login.cpp`).
- **Disco:** nada é gravado. Em `run/` ficam o `.profile` (só
  `character="Nome"`), `.pid`, `.lock` e `.log`. O nome desses arquivos é um
  SHA-256 de servidor, porta, conta e personagem; ele não revela a conta, mas
  quem já souber o nome da conta consegue confirmar que é ela.
- **Logs:** conta e senha são substituídas por `[redacted]` em mensagens do
  servidor.
- **Memória:** a senha fica na memória do supervisor enquanto ele roda, porque
  é necessária para o reconnect automático, e é apagada (`OPENSSL_cleanse`) ao
  encerrar. Quem tiver root na máquina pode lê-la durante a execução. Use uma VM
  só sua.

## Limitações do protocolo do jogo

- O login é cifrado com RSA usando a chave pública padrão do OTServ, a mesma do
  [The Forgotten Server](https://github.com/otland/forgottenserver/blob/master/key.pem),
  cuja chave privada é pública. Isso **não protege** a senha contra quem
  intercepta a rede. É uma característica do servidor, que aceita essa chave;
  este programa não piora nem melhora a situação.
- **Use uma senha exclusiva para o jogo.**

## Dados da máquina enviados ao servidor

O cliente oficial envia no login o nome de usuário do Windows e o modelo da
CPU. Para manter o mesmo formato, `run.sh` usa `--local-platform-context`, que
envia ao servidor do jogo:

- o nome do usuário Linux que executa o programa;
- os primeiros 20 caracteres do modelo da CPU (`/proc/cpuinfo`).

Nada além disso sobre a máquina é enviado.

## Como verificar

| O quê | Como |
|---|---|
| Código C++ e Lua | Tudo o que roda está em `src/headless/` e `scripts/`. O binário só carrega `runtime.lua`, `training.lua` e `live_training.lua`, e só aceita enviar ao chat `!treinar`, `powerdown` e `Kai`. |
| `third_party/otcv8-dev` | `tools/verify-third-party.sh` baixa o [OTCv8](https://github.com/OTCv8/otcv8-dev) no commit fixado e compara arquivo por arquivo. Só podem diferir 5 arquivos, com patches marcados `HEADLESS-PATCH`; use `--show-diff` para vê-los. O CI roda essa verificação. |
| `resources/login-context.bin` | `python3 tools/inspect-login-context.py` mostra todos os campos: preâmbulo do protocolo, campos de usuário/CPU vazios e assinaturas dos arquivos do cliente. Não há credenciais. |
| `resources/metadata.json` | Somente dados de itens (atributos e flags), lidos como JSON. Não é executado. |
| Pacote pronto | Releases são gerados apenas pelo workflow [`release.yml`](.github/workflows/release.yml), com atestado de proveniência. Verifique: `gh attestation verify ultimate-headless-linux-x86_64.tar.gz --repo hyoukazs/ultimate-headless`. |
| Mais seguro | Clone e compile você mesmo com `./build.sh`. |

## Reportar um problema

Abra uma issue. Se envolver risco para contas, não publique detalhes
exploráveis na issue; peça um canal privado.
