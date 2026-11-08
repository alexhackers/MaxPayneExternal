# MaxPayneExternal

Trainer **externo** para o **Max Payne 1** (PC, 2001), escrito em C puro, feito para estudo de engenharia reversa e para se divertir no single-player.

Ele roda como um programa separado. Lê e escreve a memória do jogo pelo lado de fora, sem injetar DLL, e desenha o ESP numa janela transparente por cima do jogo. Ao sair, tudo o que foi alterado volta ao original.

> **Aviso:** projeto educacional, para uso **offline/single-player**. Não tem relação com a Remedy, a Rockstar ou a Take-Two. O repositório não traz nenhum arquivo do jogo: você precisa ter a sua cópia.

*English summary: an external, single-file C trainer for Max Payne 1 (MaxPayne.exe 1.0.4.0) with god mode, infinite ammo, no reload, one-hit kill, unlock all weapons, rapid fire, a GDI overlay ESP and a mouse-based aimbot. Everything is restored on exit. Reverse-engineering notes are in [`docs/ENGENHARIA_REVERSA.md`](docs/ENGENHARIA_REVERSA.md).*

---

## Funções

| Tecla | Função |
|---|---|
| **NUM1** | God mode: a vida do Max não diminui |
| **NUM2** | Munição infinita, usando a flag nativa do jogo por tipo de munição. O contador de reserva some do HUD |
| **NUM3** | Sem recarga: o pente do Max não diminui |
| **NUM4** | ESP: caixa, linha até o inimigo, barra e número de vida, distância e arma dele. Inimigos fora da tela viram marcadores na borda. **Já começa ligado** |
| **NUM5** | Hit kill: qualquer dano em outro personagem mata na hora |
| **NUM6** | Libera **todas as armas** e enche a reserva de munição (ação única) |
| **NUM7** | Tiro rápido: cerca de 4× a cadência, **só o Max** |
| **NUM8** | Aimbot (ver abaixo) |
| **NUM0** | Encher a vida agora |
| **END** | Sair e restaurar o jogo |

As teclas funcionam com o jogo em foco. Com o NumLock desligado, o NUM5 também é aceito.
Os cheats são reaplicados sozinhos depois de carregar um save ou mudar de fase.

### Aimbot

Com o aimbot ligado, aparece um círculo roxo em volta da mira. Enquanto você **segura o botão esquerdo** (atirar) ou um **botão lateral** do mouse, a mira é puxada para o inimigo vivo mais perto do centro dentro do círculo, e esse alvo fica destacado em roxo no ESP.
O aimbot não escreve ângulos na memória: ele move o mouse (`SendInput`), como um jogador faria.

---

## Requisitos

- Windows 10/11.
- Max Payne 1 com o `MaxPayne.exe` **versão 1.0.4.0** (patch 1.04; o título da janela mostra "Max Payne v1.05").
  - Testado com o executável de SHA-256 `723650A5B4241C60EE61568C2B128706FDD5C0408EACD71B46054A6B571923FA`.
  - Em outro executável, o trainer confere os bytes antes de alterar qualquer coisa. Se não reconhecer a versão, ele avisa e não mexe em nada.
- Jogo em **modo janela** para ver o ESP. O overlay não aparece por cima de tela cheia exclusiva. Funciona com widescreen fix.
- Se o jogo estiver rodando como administrador, rode o trainer como administrador também.

## Download

Quem não quiser compilar pode baixar o executável pronto (Windows x86) na página de [Releases](https://github.com/alexhackers/MaxPayneExternal/releases/latest). O SHA-256 de cada binário está nas notas da release.
Antivírus podem sinalizar o arquivo, porque ele lê e escreve a memória de outro processo. O código-fonte completo está em [`src/MaxPayneExternal.c`](src/MaxPayneExternal.c) para quem preferir compilar.

## Como usar

1. Abra o jogo em janela. Use [`tools/MaxPayne_janela.bat`](tools/MaxPayne_janela.bat), que passa `-window`: copie o arquivo para a pasta do jogo ou passe o caminho dela como argumento.
2. Carregue um save.
3. Rode o `MaxPayneExternal.exe` (baixado da release ou compilado em `bin\`). Se o jogo ainda não estiver aberto, ele espera.
4. Use as teclas da tabela. O console mostra o estado de cada cheat e dos hooks.
5. Para sair, aperte **END** ou feche a janela do console. As duas formas restauram o jogo.

## Compilação

O projeto é um único arquivo C, sem dependências além da API do Windows (`gdi32`, `user32`). Precisa ser compilado em **32 bits**, porque o jogo é x86.

Com o [Zig](https://ziglang.org/) como compilador C (o jeito mais simples é `pip install ziglang`):

```bat
build.bat
```

ou, à mão:

```bat
zig cc -target x86-windows-gnu -O2 src/MaxPayneExternal.c -o bin/MaxPayneExternal.exe -lgdi32 -luser32
```

Outros compiladores C para Windows 32 bits devem funcionar (por exemplo, MinGW-w64 i686: `i686-w64-mingw32-gcc -O2 src/MaxPayneExternal.c -o bin/MaxPayneExternal.exe -lgdi32 -luser32`), mas só o Zig foi testado.

`bin\MaxPayneExternal.exe --dump` grava `caves.bin` com o código de máquina das caves, para conferir num disassembler.

---

## Como funciona

```
MaxPayneExternal.exe ──OpenProcess──► MaxPayne.exe
   │                                   ├─ cave: setter de vida (god / hit kill)
   │  Read/WriteProcessMemory          ├─ cave: decremento do pente (sem recarga)
   │                                   ├─ cave: update dos personagens → lista de ativos
   │                                   └─ cave: P_Scene::render → câmera atual
   └─ janela transparente (GDI, topmost, click-through) → ESP
```

- **Hooks mínimos:** dois `jmp` de 5 bytes e duas trocas de ponteiro (uma vtable e uma entrada da IAT). Tudo é escrito com o processo suspenso e desfeito ao sair. O código das caves é gerado em tempo de execução por um mini-assembler, e as caves só leem flags de uma área de dados compartilhada.
- **Lista de personagens e câmera:** as caves anotam, num anel, cada personagem atualizado e cada câmera renderizada. O trainer lê esses anéis para saber quem está ativo e com que matriz projetar.
- **Posição de mundo:** a posição usada pelo ESP vem do nó do motor gráfico (`char+0x3A6` → `+0x80`). O campo de posição "óbvio" do personagem (`+0x594`) é relativo à sala e daria o lugar errado.
- **Munição, armas e cadência:** aproveitam estruturas do próprio jogo:
  - a flag nativa de munição infinita;
  - o `std::map` de armas possuídas de cada tipo de munição;
  - o multiplicador de cadência individual do personagem, que existe só no Max e não afeta os inimigos.

Os detalhes (endereços, offsets das structs, layout das caves, fórmula de projeção) estão em [`docs/ENGENHARIA_REVERSA.md`](docs/ENGENHARIA_REVERSA.md).

## Limitações conhecidas

- Só funciona com o `MaxPayne.exe` 1.0.4.0.
- O ESP não aparece com o jogo em tela cheia exclusiva.
- O aimbot não checa linha de visão: ele também escolhe inimigos atrás de paredes se estiverem dentro do círculo.
- A velocidade do aimbot foi calibrada para uma sensibilidade de mouse (`AIM_RAD_PER_COUNT` e `AIM_GAIN` no código). Com uma sensibilidade muito diferente, ajuste esses valores.
- Com sniper e escopeta de bomba, o tiro rápido muda pouco, porque a animação de engatilhar limita o ritmo.
- Super pulo e velocidade só do Max ainda não foram feitos (ver "Pendências" nas notas de engenharia reversa).

## Estrutura

```
src/MaxPayneExternal.c       o trainer inteiro
build.bat                    compila para bin/
tools/MaxPayne_janela.bat    abre o jogo em modo janela
docs/ENGENHARIA_REVERSA.md   hooks, offsets e como cada coisa foi achada
```

## Contribuindo

Issues e pull requests são bem-vindos, principalmente para suporte a outras versões do executável, velocidade e pulo só do Max, e linha de visão no aimbot.
Ao mudar offsets ou hooks, atualize também o `docs/ENGENHARIA_REVERSA.md`.

## Licença

[MIT](LICENSE).
