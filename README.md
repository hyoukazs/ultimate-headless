# Ultimate Headless

Cliente de terminal para NTO Ultimate: login, seleção de personagem, treino Lua e reconnect, sem interface gráfica ou áudio.

**O código e o pacote incluem os recursos necessários. Não é preciso obter ou copiar arquivos de contexto/metadados separadamente.** Conta e senha são digitadas ao executar e nunca são distribuídas ou gravadas em profiles.

Ambiente validado: **Ubuntu 24.04 x86-64**. O pacote pronto funciona em VM de 1 GB sem compilação. Use uma conta GitHub com acesso para baixar ou clonar.

## Opção 1: baixar, enviar, extrair e executar

Baixe `ultimate-headless-linux-x86_64.tar.gz` na [release v1.1](https://github.com/hyoukazs/ultimate-headless/releases/tag/v1.1).

Envie para sua VM. Exemplo genérico, executado no diretório do download; substitua usuário e IP pelos seus valores:

```bash
scp ultimate-headless-linux-x86_64.tar.gz SEU_USUARIO@IP_DA_VM:~/
ssh SEU_USUARIO@IP_DA_VM
```

Se seu SSH exige uma chave específica, adicione `-i CAMINHO_DA_SUA_CHAVE` aos comandos. Nunca compartilhe a chave privada.

Na VM:

```bash
tar -xzf ~/ultimate-headless-linux-x86_64.tar.gz -C ~
cd ~/ultimate-headless
./run.sh
```

`run.sh` instala automaticamente as bibliotecas necessárias no Ubuntu se estiverem ausentes, usando sudo quando necessário. Na primeira execução, o sistema pode pedir sua senha de sudo; depois o programa solicita conta e senha do jogo.

Digite o número do personagem na lista recebida do servidor. Em seguida o programa pergunta `Este personagem tem buff de treino? 1 - Sim, 2 - Nao:` — responda `2` para seguir sem buff, ou `1` e digite o nome da skill de treino (ex.: `Byakugan Tenken`) para o personagem usá-la automaticamente. Posicione-o previamente na área de treino pelo cliente oficial. Após `DAEMON_STARTED`, pode fechar o SSH: o treino continua.

O checksum está disponível junto ao pacote. Para conferir, baixe também o `.sha256` no mesmo diretório e execute `sha256sum -c ultimate-headless-linux-x86_64.tar.gz.sha256` antes de extrair.

## Opção 2: clonar, compilar e executar ou enviar o pacote

Em Linux, com Git instalado e acesso ao repositório:

```bash
git clone https://github.com/hyoukazs/ultimate-headless.git
cd ultimate-headless
./build.sh
./run.sh
```

`build.sh` instala as dependências de desenvolvimento, compila com um job por padrão e gera o executável e `out/ultimate-headless-linux-x86_64.tar.gz`, com todos os recursos de runtime. Se você compilou em outra máquina Linux compatível, envie esse pacote para a VM e use os mesmos comandos de extração/execução da opção 1.

A compilação serial foi validada com limite de 768 MiB, sem swap e pico abaixo de 700 MiB. A RAM disponível depende também do sistema e de outras sessões. Usuários Windows podem usar diretamente o pacote Linux pronto; não precisam de servidor de build.

`UH_BUILD_JOBS` ajusta o paralelismo; `UH_BUILD_DIR` ajusta o diretório de build. O workflow manual **Build Linux package**, na aba Actions, também gera o pacote.

## Vários personagens

Repita `./run.sh`, autentique e selecione outro personagem. O profile é criado automaticamente após a seleção. Uma segunda sessão do mesmo personagem no mesmo diretório é recusada com `profile already running`.

`PROFILE_READY` mostra PID/log. `DAEMON_STARTED` mostra o PID do supervisor. Os arquivos em `run/` usam uma identidade SHA256 de servidor, conta e personagem; `.profile` contém o nome, `.pid` o supervisor e `.log` o log. Não há senha nos profiles.

## Consultar e encerrar

```bash
cd ~/ultimate-headless
grep -H '^character=' run/*.profile
ps -eo pid,ppid,stat,rss,args | grep '[u]ltimate-headless --login'

ou

cd ~/ultimate-headless
for p in run/*.profile; do
  printf '%-8s %s\n' "$(cat "${p%.profile}.pid" 2>/dev/null)" "$(sed -n 's/^character=//p' "$p")"
done
```

Cada sessão tem supervisor e worker. Para ler um log, substitua `IDENTIDADE` pelo valor mostrado em `PROFILE_READY`:

```bash
tail -f run/IDENTIDADE.log
```

Ctrl+C encerra apenas a leitura. Para parar definitivamente, use o PID do **supervisor**, substituindo `NUMERO_DO_PID`:

```bash
ps -p NUMERO_DO_PID -o pid,args
kill -TERM NUMERO_DO_PID
```

Encerrar uma sessão não encerra as outras. Matar somente o worker provoca reconnect. Confira o processo antes de usar um PID antigo. Antes de atualizar os arquivos, encerre as sessões da instalação.

## Macros e reconnect

- `!treinar`: a cada 100 ms até receber `on!` do próprio personagem. Os macros começam ativos; a lógica Lua prevê `Kai` se esse macro for desativado.
- `powerdown`: a cada 100 ms quando chakra/MP está acima de 45%.
- Trainer: reavalia a cada 200 ms; escolhe o Trainer mais próximo, no mesmo andar, até 7 tiles pela distância Chebyshev; desempate por ID. Em PZ segue; fora de PZ aproxima (follow) do mais próximo quando está além do alcance corpo a corpo e ataca ao chegar perto. Evita comandos repetidos sem mudança de alvo/modo.
- Trainers são compartilhados: outro jogador ao lado só exclui o candidato quando você não está ao lado dele (cabines individuais). Sem candidato, cancela suas ações. Follow pode mover o personagem; não há busca de caminho própria.
- Buff de treino (opcional): ao responder `1` na pergunta pós-seleção e digitar o nome da skill (ex.: `Byakugan Tenken`), um macro adicional usa o buff via `say` a cada 3 segundos enquanto o personagem estiver online. O nome é aparado e precisa ter 1–64 caracteres sem caracteres de controle. Responder `2` mantém o fluxo normal.

Após queda de conexão, server save, timeout ou crash do worker, o supervisor reconecta o mesmo personagem e recria os macros. O backoff começa em 5 s e chega a 60 s; respeita a espera informada pelo servidor. Credenciais inválidas, personagem ausente, protocolo incompatível ou erro Lua encerram as tentativas. Uma mensagem de manutenção desconhecida pode exigir ajuste.

As credenciais permanecem somente na memória. Se a VM reiniciar ou o supervisor morrer, execute `./run.sh` e autentique novamente. Não há início automático após reboot nem promessa de estabilidade de 24/72 horas ou capacidade máxima.

## Recursos e código

`resources/login-context.bin` é um template de compatibilidade do protocolo, sem o nome de usuário ou CPU do PC que originou a captura. Ao executar, `--local-platform-context` preenche esses campos com os dados reais da máquina atual. O campo opaco do preâmbulo e as assinaturas originais são preservados; não há replay de RSA, conta ou senha capturados.

`resources/metadata.json` contém atributos lógicos de itens e flags exportados pelas APIs Lua do cliente oficial, sem sprites ou recursos gráficos. Os recursos são específicos do cliente/protocolo NTO 860 validado; futuras atualizações do servidor podem exigir atualizar esse bundle.

`src/headless/`: C++; `scripts/`: macros/runtime Lua; `tools/`: execução/build; `cmake/`: target; `third_party/otcv8-dev/`: subset necessário do framework.

Código sob MIT: `LICENSE` e `third_party/otcv8-dev/LICENSE`. Base: [OTCv8/otcv8-dev](https://github.com/OTCv8/otcv8-dev), commit `3d32139512cc4576b105682c3579f18fe0d534e4`; avisos/copyrights preservados. Adaptações: dispatcher sem aplicação gráfica e com cálculo da próxima espera, logger sem include gráfico, relógio monotônico e exclusão do relançador gráfico/updater no target headless. Não distribui sprites ou scripts protegidos do NTO nem presume compatibilidade com outros servidores.
