# Notas de engenharia reversa

Tudo aqui vale para o `MaxPayne.exe` **1.0.4.0** (32 bits, base `0x400000`, sem ASLR). O título da janela mostra "Max Payne v1.05".
Os endereços foram achados por análise estática do executável (disassembly com Capstone, RTTI das classes) e por leitura de memória com o jogo rodando.

As structs do motor (MAX-FX) são **packed**: muitos campos ficam em offsets ímpares, como `+0x2B3`.

## Visão geral do trainer

```
MaxPayneExternal.exe (processo próprio)
 ├─ OpenProcess + Read/WriteProcessMemory no MaxPayne.exe
 ├─ VirtualAllocEx: 1 página de código (caves) + 1 página de dados compartilhados (D)
 ├─ 2 jmps de 5 bytes + 2 trocas de ponteiro (escritos com o processo suspenso)
 ├─ loop a cada ~8 ms: lê personagens/câmera, aplica cheats, desenha o overlay
 └─ ao sair: restaura bytes/ponteiros originais e valores alterados
```

O código das caves é montado em tempo de execução por um mini-assembler (`build_caves()`).
`MaxPayneExternal.exe --dump` grava `caves.bin` para conferir o código num disassembler.

## Hooks

| Onde | Tipo | O que a cave faz |
|---|---|---|
| `0x6D71A0` setter de vida (`mov eax,[esp+4]; mov [ecx+85h],eax; ret 4`) | jmp de 5 bytes | jogador + god: valor menor que o atual vira a vida máxima. Outros + hit kill: valor menor vira `0.0` |
| `0x757F50` tiro (`dec [ecx+1CBh]; inc [ecx+223h]; ret`) | jmp de 5 bytes | pula o decremento do pente quando `ecx` é o weaponset do jogador |
| vtable `0x7D595C` (slot 3 da vtable secundária de `X_Character`, original `0x6911E0`) | ponteiro | anota `this` num anel de 256 entradas, que vira a lista de personagens ativos |
| IAT `0x7CA524` (`P_Scene::render(P_Camera*)`, em `e2mfc.dll`) | ponteiro | anota a câmera num anel de 8 entradas, que vira a câmera do ESP |

Antes de instalar, o trainer confere os bytes originais de cada ponto. Se algum não bate, aquele hook fica desligado e um aviso aparece no console.
Se o setter de vida já começa com `E9` (patch de outra execução que não saiu direito), o trainer se recusa a rodar.

Área de dados `D` (cave + `0x1000`):

| Offset | Conteúdo |
|---|---|
| `+0x00` | componente de vida do jogador (comparado com `ecx` no setter) |
| `+0x04` / `+0x08` | flags god / hit kill |
| `+0x0C` | vida do god (float) |
| `+0x10` / `+0x1C` | contadores de chamadas (diagnóstico) |
| `+0x14` / `+0x18` | weaponset do jogador / flag sem recarga |
| `+0x20` / `+0x24` | índices dos anéis |
| `+0x28` / `+0x2C` | ponteiros originais (render / update) |
| `+0x40` | anel de personagens (256 × 4) |
| `+0x440` | anel de câmeras (8 × 4) |

## X_Character

| Offset | Campo |
|---|---|
| `+0x000` | vtable `0x7D59D0` |
| `+0x01E` | vtable secundária `0x7D5950` |
| `+0x22E` | componente de vida/atributos (`ecx` do setter `0x6D71A0`); `+0x85` HP, `+0x8A` multiplicador de cadência |
| `+0x2B3` | HP (float; é o mesmo `+0x22E+0x85`) |
| `+0x2F9` | `X_CharacterInput` embutido |
| `+0x2FE` | `MaxPayne_LocalCharacterInput*`; `[+0x32]` tem vtable `0x7CF974` (`MaxPayne_PlayerInput`) **só no jogador** |
| `+0x398` | `X_WeaponArsenal*` |
| `+0x3A6` | nó do motor (vtable `0x7D1074`): `+0x5C` matriz de mundo 4×3, **`+0x80` = pés em coordenadas de mundo** |
| `+0x594` | posição **local**, relativa à sala. Só bate com o mundo na sala da câmera; não serve para o ESP |
| `+0x718` | banco do tipo de personagem (ver abaixo) |

`[0x8B709C]` guarda o último personagem criado. O trainer usa esse valor como fallback para achar o jogador.

### Banco do tipo de personagem (`char+0x718`)

Existe um por tipo de personagem, e o Max tem o dele. Os nomes vêm da tabela de propriedades do parser (`0x52F430`):

| Offset | Propriedade |
|---|---|
| `+0x44` / `+0x48` / `+0x4C` | CapsuleTop / CapsuleBottom / CapsuleRadius |
| `+0x51` | AirborneSpeed |
| `+0x5D` | MaximumHealth (int; 60 no Max, 30 nos inimigos comuns) |
| `+0x61` | WoundedHealth |
| `+0x71` | AutoAimTarget |
| `+0x72` | ShootingFrequencyMultiplier (Max 1.0, inimigos 0.4) |

## Arsenal (`X_WeaponArsenal`, vtable `0x7D8C10`)

| Offset | Campo |
|---|---|
| `+0x04` | → → banco de armas: `+4` quantidade, `+8` `X_Weapon**` |
| `+0x08` | id da arma na mão |
| `+0x0C` | vetor de 2 `X_WeaponSet` (mão direita/esquerda), `0x26D` bytes cada: `+0x1CB` pente, `+0x223` tiros disparados |
| `+0x10` | **ponteiro** para o vetor de munição, 30 bytes por tipo |

Entrada de munição (30 bytes):

| Offset | Campo |
|---|---|
| `+0x00` | reserva |
| `+0x04` | pente |
| `+0x08` | máximo |
| `+0x0C` | flag de munição infinita (byte) |
| `+0x11` | `std::map<int idArma, bool possui>` (MSVC 6). Nó: `+0` esq, `+4` pai, `+8` dir, `+0xC` chave, `+0x10` possui. Sentinela `_Nil` em `[0x8B7314]` |

O construtor do arsenal já cria um nó para cada arma do banco. Por isso "liberar todas as armas" é só marcar o byte `possui` de cada nó, sem alocar nada.

Arma (`X_Weapon`, do banco): `+0x04` nome, `+0x3D` tipo de munição, `+0x41` id, `+0x45` ClipSize, `+0x5D` MaximumShootingFrequency, `+0x61` DefaultShootingFrequency, `+0x65` ShootingFrequencyMultiplier.

| id | arma | id | arma |
|---|---|---|---|
| 0 | empty | 9 | ingramdual |
| 1 | leadpipe | 10 | (Colt Commando) |
| 2 | baseballbat | 11 | jackhammer |
| 3 | beretta | 12 | molotov |
| 4 | berettadual | 13 | grenade |
| 5 | deserteagle | 14 | (lança-granadas) |
| 6 | sawedshotgun | 15 | sniper |
| 7 | pumpshotgun | 16 | painkiller |
| 8 | ingram | | |

### Cadência

`componente(char+0x22E)+0x8A` é lido em `0x766950` junto com a `MaximumShootingFrequency` da arma.
Na prática ele **escala o intervalo** entre tiros: 4.0 deixou o Max mais lento e 0.25 deu cerca de 4× mais tiros. O trainer escreve 0.25 a cada tick e, ao desligar, devolve o valor do banco (`+0x718+0x72`).
O valor é recalculado em `0x691A80`, por exemplo ao entrar e sair do bullet time.

## Câmera e projeção

- `X_Camera`, vtable `0x7D3BF4`: `+0x5C` matriz câmera→mundo, `+0x8C` matriz 4×3 mundo→câmera, `+0x200` tanx, `+0x204` tany.
- A câmera certa é a que chega em `P_Scene::render`, com matriz diferente da identidade (a do menu de pausa é identidade).
- Tela virtual em `e2mfc.dll`: `+0x49DF8` = meia largura (320), `+0x49DF0` = meia altura (240, ou ~180 com widescreen fix).

```
cx,cy,cz = p · mundo→câmera
sx = (cx/cz · K1/tanx + K1) · W / (2·K1)
sy = (K2 − cy/cz · K2/tany) · H / (2·K2)
```

## Aimbot

O aimbot não escreve ângulos na memória. Ele injeta movimento relativo de mouse (`SendInput`), que o DirectInput do jogo aceita.
A cada tick ele calcula o ângulo até o alvo no espaço da câmera e move `ângulo / 0,0042 rad × 0,35`. É um controle proporcional: converge mesmo com erro na sensibilidade e não oscila.
O valor de `0,0042 rad por contagem` foi medido assim: 400 contagens giraram o Max 96°. Depende da sensibilidade de mouse configurada no jogo (`AIM_RAD_PER_COUNT` no código).

Outros achados que não foram usados:
- O jogo tem auto-aim nativo: flag `[0x89AECD]`, aplicada em `0x6D8BA0`. Ele aumenta a cápsula de colisão dos inimigos para os tiros do jogador, usando os multiplicadores `AutoAimCapsule*Multiplier` da dificuldade.
- O yaw do Max aparece em radianos em `char+0x295`.

## Pendências / ideias

- **Velocidade e super pulo só do Max**: o movimento vem da animação (root motion) mais um vetor de gravidade global da fase (`Gravity`, banco da fase `+0x40`). Falta achar o vetor de velocidade do personagem; o caminho mais curto é gravar o struct do Max enquanto ele corre e pula e correlacionar com a derivada da posição.
- **Checagem de linha de visão no aimbot**: hoje ele mira inimigos atrás de paredes que estejam dentro do círculo.
