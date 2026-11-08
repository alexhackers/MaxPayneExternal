// Max Payne 1 (MaxPayne.exe 1.0.4.0) - trainer EXTERNO para estudo/single-player.
// Programa separado: le/escreve a memoria do jogo via Read/WriteProcessMemory.
// Nao cria dispositivo D3D nem roda codigo C dentro do jogo; instala apenas
// pequenos trechos de maquina ("caves") em memoria alocada no processo,
// e restaura os bytes originais ao sair (tecla END ou fechar o console).
//
// Projeto: veja README.md (uso) e docs/ENGENHARIA_REVERSA.md (hooks e offsets).
// Licenca: MIT (arquivo LICENSE).
//
// Build (x86):  build.bat
//   ou:  zig cc -target x86-windows-gnu -O2 src/MaxPayneExternal.c -o bin/MaxPayneExternal.exe -lgdi32 -luser32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

typedef unsigned char u8; typedef unsigned int u32;

// ---------------------------------------------------------------- enderecos (MaxPayne.exe 1.04)
#define VT_CHAR          0x7D59D0   // X_Character
#define VT_CHAR2         0x7D5950   // vtable secundaria em char+0x1E
#define SLOT_CHAR_UPDATE 0x7D595C   // [3] da secundaria -> 0x6911E0 (1x/frame/personagem ativo)
#define FN_CHAR_UPDATE   0x6911E0
#define VT_PLAYER_INPUT  0x7CF974   // MaxPayne_PlayerInput
#define VT_XCAMERA       0x7D3BF4   // X_Camera
#define IAT_SCENE_RENDER 0x7CA524   // P_Scene::render(P_Camera*)
#define FN_HEALTH_SET    0x6D71A0   // mov eax,[esp+4]; mov [ecx+85h],eax; ret 4
#define FN_CLIP_SHOT     0x757F50   // dec [ecx+1CBh]; inc [ecx+223h]; ret
#define G_LAST_CHAR      0x8B709C

#define CH_HEALTHCOMP 0x22E
#define CH_HP         0x2B3
#define CH_INPUT      0x2FE
#define CH_ARSENAL    0x398
#define CH_POS        0x594      // posicao LOCAL (relativa a sala) - nao serve para o ESP
#define CH_NODE       0x3A6      // ponteiro p/ no do motor (vtable 0x7D1074)
#define VT_CHAR_NODE  0x7D1074
#define NODE_WORLD_T  0x80       // traducao da matriz de mundo (no+0x5C) = pes, em coords de mundo
#define CH_MAXHPOBJ   0x718
#define AR_WEAPONSET  0x0C
#define AR_AMMO       0x10
#define AMMO_STRIDE   30
#define AMMO_INF      0x0C
#define AMMO_TYPES    14
#define AR_CURWEAPON  0x08       // id da arma na mao (indice no banco de armas)
#define AR_DB         0x04       // ponteiro -> ponteiro -> {?, n, X_Weapon**}
#define AMMO_OWNED_MAP 0x11      // std::map<int arma, bool possui> (cabeca da arvore)
#define G_MAP_NIL     0x8B7314   // no sentinela (_Nil) dos std::map dessas arvores
#define HC_SHOOTFREQ  0x8A       // componente(char+0x22E)+0x8A: multiplicador de cadencia (Max 1.0, inimigos 0.4)
#define CAM_VIEW 0x8C
#define CAM_TANX 0x200
#define CAM_TANY 0x204
#define E2_K1 0x49DF8
#define E2_K2 0x49DF0

static const u8 ORIG_HEALTH[13] = { 0x8B,0x44,0x24,0x04,0x89,0x81,0x85,0x00,0x00,0x00,0xC2,0x04,0x00 };
static const u8 ORIG_CLIP[13]   = { 0xFF,0x89,0xCB,0x01,0x00,0x00,0xFF,0x81,0x23,0x02,0x00,0x00,0xC3 };

// ---------------------------------------------------------------- area de dados da cave (offsets em D)
#define D_PLAYER_HC  0x00
#define D_GOD        0x04
#define D_ONEHIT     0x08
#define D_GODHP      0x0C
#define D_CNT_HEALTH 0x10
#define D_PLAYER_WS  0x14
#define D_NORELOAD   0x18
#define D_CNT_CLIP   0x1C
#define D_UPD_IDX    0x20
#define D_CAM_IDX    0x24
#define D_ORIG_REND  0x28
#define D_ORIG_UPD   0x2C
#define D_UPD_RING   0x40      // 256 * 4
#define UPD_RING_N   256
#define D_CAM_RING   0x440     // 8 * 4
#define CAM_RING_N   8
#define D_SIZE       0x460

static HANDLE g_h; static DWORD g_pid; static u32 g_e2;
static u32 g_cave, g_D;      // endereco remoto do codigo e dos dados
static u32 g_hcHealth, g_hcClip, g_hcUpd, g_hcRend;
static int g_patHealth, g_patClip, g_patUpd, g_patRend;

static int  god = 0, ammo = 0, noreload = 0, esp = 1, onehit = 0, rapid = 0, aimbot = 0;
#define RAPID_MULT  0.25f       // multiplica o INTERVALO entre tiros (0.25 = 4x mais rapido)
#define AIM_FOV_PX  260.f       // raio (px em 1080p) em volta da mira para pegar alvo
#define AIM_HEIGHT  1.55f       // altura do ponto de mira acima dos pes (peito/pescoco)
#define AIM_RAD_PER_COUNT 0.0042f // medido: 400 contagens de mouse = 96 graus
#define AIM_GAIN    0.35f

// ---------------------------------------------------------------- memoria remota
static int rd(u32 a, void *b, u32 n) { SIZE_T r = 0; return ReadProcessMemory(g_h, (LPCVOID)(uintptr_t)a, b, n, &r) && r == n; }
static int wr(u32 a, const void *b, u32 n) { SIZE_T r = 0; return WriteProcessMemory(g_h, (LPVOID)(uintptr_t)a, b, n, &r) && r == n; }
static u32 r32(u32 a) { u32 v = 0; rd(a, &v, 4); return v; }
static float rf(u32 a) { float v = 0; rd(a, &v, 4); return v; }
static void w32(u32 a, u32 v) { wr(a, &v, 4); }
static int wr_prot(u32 a, const void *b, u32 n) {
    DWORD old; if (!VirtualProtectEx(g_h, (LPVOID)(uintptr_t)a, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    int ok = wr(a, b, n); VirtualProtectEx(g_h, (LPVOID)(uintptr_t)a, n, old, &old);
    FlushInstructionCache(g_h, (LPCVOID)(uintptr_t)a, n); return ok;
}
typedef LONG (NTAPI *PFN_NtProc)(HANDLE);
static void suspend(int s) {
    static PFN_NtProc ps, pr;
    if (!ps) { HMODULE n = GetModuleHandleA("ntdll.dll"); ps = (PFN_NtProc)GetProcAddress(n, "NtSuspendProcess"); pr = (PFN_NtProc)GetProcAddress(n, "NtResumeProcess"); }
    if (s) { if (ps) ps(g_h); } else { if (pr) pr(g_h); }
}

// ---------------------------------------------------------------- mini-assembler
typedef struct { u8 b[512]; int n; } Asm;
static void E(Asm *a, const char *hex) { // "FF 05" etc
    while (*hex) { while (*hex == ' ') hex++; if (!*hex) break; unsigned v; sscanf(hex, "%2x", &v); a->b[a->n++] = (u8)v; hex += 2; }
}
static void I32(Asm *a, u32 v) { memcpy(a->b + a->n, &v, 4); a->n += 4; }
static int J8(Asm *a, const char *op) { E(a, op); a->b[a->n++] = 0; return a->n - 1; }      // retorna pos do rel8
static void BIND(Asm *a, int p) { a->b[p] = (u8)(a->n - (p + 1)); }                         // label = aqui
static void J8TO(Asm *a, const char *op, int target) { E(a, op); a->b[a->n] = (u8)(target - (a->n + 1)); a->n++; }

static u8 *g_local; // modo --dump: monta num buffer local
static u32 emit(u32 base, Asm *a, u32 *cursor) { // copia para a cave remota e retorna endereco
    u32 at = base + *cursor;
    if (g_local) memcpy(g_local + *cursor, a->b, a->n); else wr(at, a->b, a->n);
    *cursor += (a->n + 15) & ~15; return at;
}

static void build_caves(u32 base) {
    u32 D = g_D, cur = 0; Asm a;
    // --- health setter: god (player) / one-hit (outros)
    memset(&a, 0, sizeof a);
    E(&a, "FF 05"); I32(&a, D + D_CNT_HEALTH);           // inc [cnt]
    E(&a, "8B 44 24 04");                                // mov eax,[esp+4]
    E(&a, "3B 0D"); I32(&a, D + D_PLAYER_HC);            // cmp ecx,[playerHC]
    int jEnemy = J8(&a, "75");                           // jne ENEMY
    E(&a, "83 3D"); I32(&a, D + D_GOD); E(&a, "00");     // cmp [god],0
    int jS1 = J8(&a, "74");                              // je STORE
    E(&a, "3B 81 85 00 00 00");                          // cmp eax,[ecx+85]
    int jS2 = J8(&a, "7D");                              // jge STORE
    E(&a, "A1"); I32(&a, D + D_GODHP);                   // mov eax,[godhp]
    int STORE = a.n; BIND(&a, jS1); BIND(&a, jS2);
    E(&a, "89 81 85 00 00 00");                          // mov [ecx+85],eax
    E(&a, "C2 04 00");                                   // ret 4
    BIND(&a, jEnemy);
    E(&a, "83 3D"); I32(&a, D + D_ONEHIT); E(&a, "00");  // cmp [onehit],0
    J8TO(&a, "74", STORE);                               // je STORE
    E(&a, "3B 81 85 00 00 00");                          // cmp eax,[ecx+85]
    J8TO(&a, "7D", STORE);                               // jge STORE
    E(&a, "31 C0");                                      // xor eax,eax (0.0f)
    J8TO(&a, "EB", STORE);                               // jmp STORE
    g_hcHealth = emit(base, &a, &cur);

    // --- clip: nao decrementa o pente do jogador
    memset(&a, 0, sizeof a);
    E(&a, "FF 05"); I32(&a, D + D_CNT_CLIP);
    E(&a, "83 3D"); I32(&a, D + D_NORELOAD); E(&a, "00");
    int jN = J8(&a, "74");                               // je NORMAL
    E(&a, "3B 0D"); I32(&a, D + D_PLAYER_WS);            // cmp ecx,[playerWS]
    int jDone = J8(&a, "74");                            // je DONE
    BIND(&a, jN);
    E(&a, "FF 89 CB 01 00 00 FF 81 23 02 00 00");        // codigo original
    BIND(&a, jDone);
    E(&a, "C3");
    g_hcClip = emit(base, &a, &cur);

    // --- update de personagem: anota 'this' num anel (lista de personagens ativos)
    memset(&a, 0, sizeof a);
    E(&a, "50");                                         // push eax
    E(&a, "A1"); I32(&a, D + D_UPD_IDX);                 // mov eax,[idx]
    E(&a, "25"); I32(&a, UPD_RING_N - 1);                // and eax,N-1
    E(&a, "89 0C 85"); I32(&a, D + D_UPD_RING);          // mov [ring+eax*4],ecx
    E(&a, "FF 05"); I32(&a, D + D_UPD_IDX);              // inc [idx]
    E(&a, "58");                                         // pop eax
    E(&a, "FF 25"); I32(&a, D + D_ORIG_UPD);             // jmp [orig]
    g_hcUpd = emit(base, &a, &cur);

    // --- P_Scene::render(cam): anota a camera num anel
    memset(&a, 0, sizeof a);
    E(&a, "50 52");                                      // push eax; push edx
    E(&a, "A1"); I32(&a, D + D_CAM_IDX);
    E(&a, "83 E0"); a.b[a.n++] = CAM_RING_N - 1;         // and eax,7
    E(&a, "8B 54 24 0C");                                // mov edx,[esp+0C] (arg cam)
    E(&a, "89 14 85"); I32(&a, D + D_CAM_RING);          // mov [ring+eax*4],edx
    E(&a, "FF 05"); I32(&a, D + D_CAM_IDX);
    E(&a, "5A 58");                                      // pop edx; pop eax
    E(&a, "FF 25"); I32(&a, D + D_ORIG_REND);            // jmp [orig]
    g_hcRend = emit(base, &a, &cur);
}

static int patch_jmp(u32 at, u32 to) {
    u8 j[5] = { 0xE9 }; *(int *)(j + 1) = (int)(to - (at + 5)); return wr_prot(at, j, 5);
}

static void uninstall(void) {
    if (!g_h) return;
    suspend(1);
    if (g_patHealth) { wr_prot(FN_HEALTH_SET, ORIG_HEALTH, 5); g_patHealth = 0; }
    if (g_patClip)   { wr_prot(FN_CLIP_SHOT, ORIG_CLIP, 5); g_patClip = 0; }
    if (g_patUpd)    { u32 v = FN_CHAR_UPDATE; wr_prot(SLOT_CHAR_UPDATE, &v, 4); g_patUpd = 0; }
    if (g_patRend)   { u32 v = r32(g_D + D_ORIG_REND); wr_prot(IAT_SCENE_RENDER, &v, 4); g_patRend = 0; }
    suspend(0);
    // a cave fica alocada (algum thread pode estar dentro dela) - inofensivo
}

static void install(void) {
    u8 cur[13];
    g_cave = (u32)(uintptr_t)VirtualAllocEx(g_h, NULL, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_cave) { printf("VirtualAllocEx falhou (%lu)\n", GetLastError()); return; }
    g_D = g_cave + 0x1000;
    u8 zero[D_SIZE] = { 0 }; wr(g_D, zero, D_SIZE);
    u32 origRend = r32(IAT_SCENE_RENDER);
    w32(g_D + D_ORIG_REND, origRend);
    w32(g_D + D_ORIG_UPD, FN_CHAR_UPDATE);
    build_caves(g_cave);

    suspend(1);
    if (rd(FN_HEALTH_SET, cur, 13) && !memcmp(cur, ORIG_HEALTH, 13)) g_patHealth = patch_jmp(FN_HEALTH_SET, g_hcHealth);
    else printf("  [!] health setter com bytes inesperados - god/one-hit via cave desativados\n");
    if (rd(FN_CLIP_SHOT, cur, 13) && !memcmp(cur, ORIG_CLIP, 13)) g_patClip = patch_jmp(FN_CLIP_SHOT, g_hcClip);
    else printf("  [!] funcao do pente com bytes inesperados - sem recarga desativado\n");
    if (r32(SLOT_CHAR_UPDATE) == FN_CHAR_UPDATE) { u32 v = g_hcUpd; g_patUpd = wr_prot(SLOT_CHAR_UPDATE, &v, 4); }
    else printf("  [!] vtable de update inesperada - ESP/lista de personagens desativados\n");
    if (g_e2 && origRend >= g_e2 && origRend < g_e2 + 0x200000) { u32 v = g_hcRend; g_patRend = wr_prot(IAT_SCENE_RENDER, &v, 4); }
    else printf("  [!] IAT de render inesperada (%08X) - camera do ESP desativada\n", origRend);
    suspend(0);
}

// ---------------------------------------------------------------- processo
static DWORD find_pid(void) {
    PROCESSENTRY32 pe = { sizeof pe }; DWORD pid = 0;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return 0;
    for (BOOL ok = Process32First(s, &pe); ok; ok = Process32Next(s, &pe))
        if (!_stricmp(pe.szExeFile, "MaxPayne.exe")) { pid = pe.th32ProcessID; break; }
    CloseHandle(s); return pid;
}
static u32 find_module(const char *name) {
    MODULEENTRY32 me = { sizeof me }; u32 base = 0;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, g_pid);
    if (s == INVALID_HANDLE_VALUE) return 0;
    for (BOOL ok = Module32First(s, &me); ok; ok = Module32Next(s, &me))
        if (!_stricmp(me.szModule, name)) { base = (u32)(uintptr_t)me.modBaseAddr; break; }
    CloseHandle(s); return base;
}
static HWND g_game;
static BOOL CALLBACK enum_cb(HWND w, LPARAM l) {
    DWORD p; GetWindowThreadProcessId(w, &p);
    if (p == g_pid && IsWindowVisible(w) && !GetWindow(w, GW_OWNER)) { g_game = w; return FALSE; }
    return TRUE;
}

// ---------------------------------------------------------------- estado do jogo
typedef struct { u32 p; float hp, pos[3]; int weapon; } Ch;
static Ch g_ch[64]; static int g_nch;
static u32 g_player; static float g_php, g_pmax; static int g_clip = -1;
static float g_view[12], g_tanx, g_tany, g_K1 = 320, g_K2 = 240; static int g_camOk;
static u8 g_ammoOrig[AMMO_TYPES]; static u32 g_ammoArs;

// posicao dos pes em coordenadas de mundo (mesmo espaco da camera)
static int world_pos(u32 c, float *o) {
    u32 n = r32(c + CH_NODE);
    if (n && r32(n) == VT_CHAR_NODE && rd(n + NODE_WORLD_T, o, 12)) return 1;
    return 0;
}

static int is_player(u32 c) {
    u32 li = r32(c + CH_INPUT); if (!li) return 0;
    u32 in = r32(li + 0x32);    if (!in) return 0;
    return r32(in) == VT_PLAYER_INPUT;
}

static void refresh_chars(void) {
    u32 ring[UPD_RING_N]; g_nch = 0;
    if (!g_patUpd || !rd(g_D + D_UPD_RING, ring, sizeof ring)) return;
    for (int i = 0; i < UPD_RING_N; i++) {
        if (!ring[i]) continue;
        u32 c = ring[i] - 0x1E; int dup = 0;
        for (int j = 0; j < g_nch; j++) if (g_ch[j].p == c) { dup = 1; break; }
        if (dup || g_nch >= 64) continue;
        if (r32(c) != VT_CHAR) continue;
        Ch *x = &g_ch[g_nch];
        x->p = c; x->hp = rf(c + CH_HP); if (!world_pos(c, x->pos)) continue;
        { u32 ars = r32(c + CH_ARSENAL); x->weapon = ars ? (int)r32(ars + AR_CURWEAPON) : -1; }
        if (is_player(c)) g_player = c;
        g_nch++;
    }
}

static void refresh_camera(void) {
    u32 ring[CAM_RING_N]; g_camOk = 0;
    if (!g_patRend || !rd(g_D + D_CAM_RING, ring, sizeof ring)) return;
    for (int i = 0; i < CAM_RING_N; i++) {
        u32 c = ring[i]; if (!c || r32(c) != VT_XCAMERA) continue;
        float m[12]; if (!rd(c + CAM_VIEW, m, sizeof m)) continue;
        int ident = fabsf(m[0] - 1) < 1e-5f && fabsf(m[4] - 1) < 1e-5f && fabsf(m[8] - 1) < 1e-5f &&
                    fabsf(m[9]) < 1e-5f && fabsf(m[10]) < 1e-5f && fabsf(m[11]) < 1e-5f;
        float tx = rf(c + CAM_TANX), ty = rf(c + CAM_TANY);
        if (ident || !(tx > 0.01f && tx < 100.f) || !(ty > 0.01f && ty < 100.f)) continue;
        memcpy(g_view, m, sizeof m); g_tanx = tx; g_tany = ty; g_camOk = 1; break;
    }
    if (g_e2) { float k1 = rf(g_e2 + E2_K1), k2 = rf(g_e2 + E2_K2); if (k1 > 1 && k2 > 1) { g_K1 = k1; g_K2 = k2; } }
}

// arsenal+0x10 e um PONTEIRO para o vetor de municao (30 bytes por tipo).
// So escreve com o cheat ligado; ao desligar devolve as flags originais uma vez.
static int g_ammoApplied;
static void apply_ammo(void) {
    u32 ars = g_player ? r32(g_player + CH_ARSENAL) : 0;
    u32 arr = ars ? r32(ars + AR_AMMO) : 0;
    if (!arr) return;
    if (arr != g_ammoArs) { g_ammoArs = arr; g_ammoApplied = 0; } // save carregado / fase nova
    if (ammo) {
        for (int i = 0; i < AMMO_TYPES; i++) {
            u32 e = arr + i * AMMO_STRIDE; int mx = (int)r32(e + 8); u8 cur;
            if (mx <= 0 || mx > 100000 || !rd(e + AMMO_INF, &cur, 1)) continue;
            if (!g_ammoApplied) g_ammoOrig[i] = cur;
            if (cur != 1) { u8 one = 1; wr(e + AMMO_INF, &one, 1); }
        }
        g_ammoApplied = 1;
    } else if (g_ammoApplied) {
        for (int i = 0; i < AMMO_TYPES; i++) {
            u32 e = arr + i * AMMO_STRIDE; int mx = (int)r32(e + 8);
            if (mx > 0 && mx <= 100000) wr(e + AMMO_INF, &g_ammoOrig[i], 1);
        }
        g_ammoApplied = 0;
    }
}

// ---------------------------------------------------------------- armas
// Cada tipo de municao tem um std::map<idArma, bool possui>. O construtor do arsenal ja cria
// um no para cada arma do banco, entao "dar todas" e so marcar o bool dos nos (sem alocar nada).
static int give_weapons(void) {
    u32 ars = g_player ? r32(g_player + CH_ARSENAL) : 0, arr = ars ? r32(ars + AR_AMMO) : 0;
    if (!arr) return 0;
    u32 nil = r32(G_MAP_NIL); int n = 0;
    for (int i = 0; i < AMMO_TYPES; i++) {
        u32 e = arr + i * AMMO_STRIDE; int mx = (int)r32(e + 8);
        if (mx <= 0 || mx > 100000) continue;
        u32 head = r32(e + AMMO_OWNED_MAP); if (!head) continue;
        u32 st[32]; int sp = 0, guard = 0;
        u32 root = r32(head + 4); if (root && root != nil && root != head) st[sp++] = root;
        while (sp && guard++ < 64) {
            u32 nd = st[--sp]; int key = (int)r32(nd + 0xC); u8 cur = 1;
            if (key > 0 && key < 64 && rd(nd + 0x10, &cur, 1) && !cur) { u8 one = 1; wr(nd + 0x10, &one, 1); n++; }
            u32 l = r32(nd), r = r32(nd + 8);
            if (l && l != nil && l != head && sp < 32) st[sp++] = l;
            if (r && r != nil && r != head && sp < 32) st[sp++] = r;
        }
        if ((int)r32(e) < mx) w32(e, (u32)mx);   // reserva cheia
    }
    return n;
}

// nomes das armas do banco do jogo (id -> "beretta", "pumpshotgun"...)
static char g_wname[32][20]; static u32 g_wnameDb;
static const char *weapon_name(u32 ars, int id) {
    if (id < 0 || id >= 32) return "";
    u32 db = r32(r32(ars + AR_DB));
    if (db != g_wnameDb) { memset(g_wname, 0, sizeof g_wname); g_wnameDb = db; }
    if (!g_wname[id][0]) {
        int n = (int)r32(db + 4); u32 a = r32(db + 8), w = (id < n && a) ? r32(a + 4 * id) : 0;
        u32 s = w ? r32(w + 4) : 0; char b[20] = { 0 };
        if (s && rd(s, b, 19)) { for (int k = 0; k < 19 && b[k]; k++) if (b[k] < 32 || b[k] > 126) { b[k] = 0; break; } }
        if (!b[0]) sprintf(b, "#%d", id);
        strcpy(g_wname[id], b);
    }
    return g_wname[id];
}

// cadencia: componente+0x8A escala o intervalo entre tiros (testado: 4.0 deixou mais lento; inimigos usam 0.4).
// E por personagem, entao so afeta o Max.
static u32 g_rapidChar; static float g_rapidOrig; static int g_rapidOn;
static void apply_rapid(void) {
    if (g_player != g_rapidChar) { g_rapidChar = g_player; g_rapidOn = 0; }
    if (!g_player) return;
    u32 a = g_player + CH_HEALTHCOMP + HC_SHOOTFREQ;
    if (rapid) {
        float cur = rf(a);
        if (!g_rapidOn) { g_rapidOrig = (cur > 0.05f && cur < 3.9f) ? cur : 1.f; g_rapidOn = 1; }
        if (cur != RAPID_MULT) { float v = RAPID_MULT; wr(a, &v, 4); }
    } else if (g_rapidOn) {
        u32 db = r32(g_player + CH_MAXHPOBJ); float v = db ? rf(db + 0x72) : g_rapidOrig; // valor do banco do Max
        if (!(v > 0.05f && v < 3.9f)) v = g_rapidOrig;
        wr(a, &v, 4); g_rapidOn = 0;
    }
}

static void tick(void) {
    if (g_player && r32(g_player) != VT_CHAR) g_player = 0;
    refresh_chars();
    if (!g_player) { u32 c = r32(G_LAST_CHAR); if (c && r32(c) == VT_CHAR && is_player(c)) g_player = c; }
    refresh_camera();

    u32 hc = 0, ws = 0; g_php = 0; g_pmax = 0; g_clip = -1;
    if (g_player) {
        hc = g_player + CH_HEALTHCOMP;
        g_php = rf(g_player + CH_HP);
        u32 mo = r32(g_player + CH_MAXHPOBJ); int mx = mo ? (int)r32(mo + 0x5D) : 0;
        g_pmax = (mx > 0 && mx < 10000) ? (float)mx : 60.f;
        u32 ars = r32(g_player + CH_ARSENAL); ws = ars ? r32(ars + AR_WEAPONSET) : 0;
        if (ws) g_clip = (int)r32(ws + 0x1CB);
        if (god && g_php > 0 && g_php < g_pmax) { float f = g_pmax; wr(g_player + CH_HP, &f, 4); } // reforco por polling
        apply_ammo();
    }
    apply_rapid();
    u32 d[8] = { hc, (u32)god, (u32)onehit, 0, 0, ws, (u32)noreload, 0 };
    float gh = g_pmax > 0 ? g_pmax : 60.f; memcpy(&d[3], &gh, 4);
    wr(g_D + D_PLAYER_HC, &d[0], 16);            // hc, god, onehit, godhp
    wr(g_D + D_PLAYER_WS, &d[5], 8);             // ws, noreload
}

// ---------------------------------------------------------------- overlay (ESP)
static int g_row;
static HWND g_ov; static int g_W = 1920, g_H = 1080, g_X, g_Y;
static HDC g_mdc; static HBITMAP g_bmp; static int g_bw, g_bh;
static int g_PW = 1920, g_PH = 1080;   // tamanho da area cliente usado na projecao
static int g_aimTarget = -1;           // indice em g_ch do alvo atual do aimbot

static void cam_space(const float *v, float *o) {
    const float *m = g_view;
    o[0] = v[0] * m[0] + v[1] * m[3] + v[2] * m[6] + m[9];
    o[1] = v[0] * m[1] + v[1] * m[4] + v[2] * m[7] + m[10];
    o[2] = v[0] * m[2] + v[1] * m[5] + v[2] * m[8] + m[11];
}
static int project(const float *v, float *sx, float *sy) {
    const float *m = g_view;
    float cx = v[0] * m[0] + v[1] * m[3] + v[2] * m[6] + m[9];
    float cy = v[0] * m[1] + v[1] * m[4] + v[2] * m[7] + m[10];
    float cz = v[0] * m[2] + v[1] * m[5] + v[2] * m[8] + m[11];
    if (cz < 0.1f) return 0;
    float vx = cx / cz * g_K1 / g_tanx + g_K1;
    float vy = g_K2 - cy / cz * g_K2 / g_tany;
    *sx = vx * g_PW / (2 * g_K1); *sy = vy * g_PH / (2 * g_K2);
    return 1;
}

// ---------------------------------------------------------------- aimbot
// Escolhe o inimigo vivo mais perto da mira (dentro do FOV) e, enquanto o botao esquerdo
// (ou um botao lateral do mouse) estiver apertado, injeta movimento relativo de mouse
// proporcional ao angulo ate ele. O jogo le o mouse via DirectInput, que aceita SendInput.
static float g_aimAccX, g_aimAccY;
static void aim_step(void) {
    g_aimTarget = -1;
    if (!aimbot || !g_camOk || !g_game || GetForegroundWindow() != g_game) { g_aimAccX = g_aimAccY = 0; return; }
    RECT rc; GetClientRect(g_game, &rc); if (rc.right < 64 || rc.bottom < 64) return;
    g_PW = rc.right; g_PH = rc.bottom;
    float cxs = g_PW / 2.f, cys = g_PH / 2.f, best = AIM_FOV_PX * g_PH / 1080.f; best *= best;
    float aimc[3] = { 0 };
    for (int i = 0; i < g_nch; i++) {
        Ch *c = &g_ch[i];
        if (c->p == g_player || !(c->hp > 0)) continue;
        float p[3] = { c->pos[0], c->pos[1] + AIM_HEIGHT, c->pos[2] }, sx, sy;
        if (!project(p, &sx, &sy)) continue;
        float d2 = (sx - cxs) * (sx - cxs) + (sy - cys) * (sy - cys);
        if (d2 < best) { best = d2; g_aimTarget = i; cam_space(p, aimc); }
    }
    int hold = ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_XBUTTON1) | GetAsyncKeyState(VK_XBUTTON2)) & 0x8000) != 0;
    if (g_aimTarget < 0 || !hold || aimc[2] < 0.1f) { g_aimAccX = g_aimAccY = 0; return; }
    float ax = atanf(aimc[0] / aimc[2]), ay = atanf(aimc[1] / aimc[2]);
    float dx = ax / AIM_RAD_PER_COUNT * AIM_GAIN + g_aimAccX, dy = -ay / AIM_RAD_PER_COUNT * AIM_GAIN + g_aimAccY;
    int ix = (int)dx, iy = (int)dy; g_aimAccX = dx - ix; g_aimAccY = dy - iy;
    if (ix > 80) ix = 80; if (ix < -80) ix = -80; if (iy > 80) iy = 80; if (iy < -80) iy = -80;
    if (ix || iy) {
        INPUT in = { 0 }; in.type = INPUT_MOUSE; in.mi.dx = ix; in.mi.dy = iy; in.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &in, sizeof in);
    }
}

static LRESULT CALLBACK ov_proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcA(w, m, wp, lp);
}
static void ov_create(void) {
    WNDCLASSA wc = { 0 }; wc.lpfnWndProc = ov_proc; wc.hInstance = GetModuleHandleA(0);
    wc.lpszClassName = "MPExtOverlay"; wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);
    g_ov = CreateWindowExA(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                           wc.lpszClassName, "", WS_POPUP, 0, 0, g_W, g_H, 0, 0, wc.hInstance, 0);
    SetLayeredWindowAttributes(g_ov, RGB(0, 0, 0), 0, LWA_COLORKEY);
}

static void ov_draw(void) {
    int show = esp && g_game && !IsIconic(g_game);
    HWND fg = GetForegroundWindow();
    if (fg != g_game && fg != g_ov) show = 0;
    if (!show) { if (IsWindowVisible(g_ov)) ShowWindow(g_ov, SW_HIDE); return; }

    RECT rc; POINT o = { 0, 0 };
    GetClientRect(g_game, &rc); ClientToScreen(g_game, &o);
    int W = rc.right, H = rc.bottom; if (W < 64 || H < 64) return;
    if (W != g_W || H != g_H || o.x != g_X || o.y != g_Y || !IsWindowVisible(g_ov)) {
        g_W = W; g_H = H; g_X = o.x; g_Y = o.y;
        SetWindowPos(g_ov, HWND_TOPMOST, o.x, o.y, W, H, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    HDC wdc = GetDC(g_ov);
    if (!g_mdc || g_bw != W || g_bh != H) {
        if (g_mdc) { DeleteDC(g_mdc); DeleteObject(g_bmp); }
        g_mdc = CreateCompatibleDC(wdc); g_bmp = CreateCompatibleBitmap(wdc, W, H); SelectObject(g_mdc, g_bmp);
        g_bw = W; g_bh = H;
        SelectObject(g_mdc, GetStockObject(NULL_BRUSH));
        SetBkMode(g_mdc, TRANSPARENT);
        HFONT f = CreateFontA(-15, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, NONANTIALIASED_QUALITY, 0, "Consolas");
        SelectObject(g_mdc, f);
    }
    RECT all = { 0, 0, W, H }; FillRect(g_mdc, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
    g_PW = W; g_PH = H;

    int alive = 0, onscreen = 0;
    if (g_camOk) {
        float pp[3] = { 0 }; if (g_player) world_pos(g_player, pp);
        for (int i = 0; i < g_nch; i++) {
            Ch *c = &g_ch[i];
            if (c->p == g_player || !(c->hp > 0)) continue;
            alive++;
            float dx = c->pos[0] - pp[0], dy = c->pos[1] - pp[1], dz = c->pos[2] - pp[2];
            float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            COLORREF col = dist < 10 ? RGB(255, 40, 40) : dist < 25 ? RGB(255, 170, 0) : RGB(255, 255, 60);
            char s[32]; sprintf(s, "%.0fm", dist);
            SetTextColor(g_mdc, col);
            float feet[3] = { c->pos[0], c->pos[1], c->pos[2] };
            float head[3] = { c->pos[0], c->pos[1] + 1.85f, c->pos[2] };
            float fx = 0, fy = 0, hx = 0, hy = 0;
            int vis = project(feet, &fx, &fy) && project(head, &hx, &hy);
            float h = vis ? fy - hy : 0; if (h < 6) h = 6;
            float w = h * 0.45f;
            int x0 = (int)(fx - w / 2), y0 = (int)hy, x1 = (int)(fx + w / 2), y1 = (int)fy;
            if (vis && (h > H * 3 || x1 < 0 || x0 > W || y1 < 0 || y0 > H)) vis = 0;
            if (!vis) {
                // fora da tela / atras: marcador na borda apontando a direcao
                float cc[3]; cam_space(feet, cc);
                float ex = cc[0], ey = -cc[1];
                if (cc[2] < 0 && fabsf(ex) < 0.01f) ex = 0.01f;
                float len = sqrtf(ex * ex + ey * ey); if (len < 1e-4f) { ex = 0; ey = 1; len = 1; }
                ex /= len; ey /= len;
                float bx = W / 2.f - 28, by = H / 2.f - 28;
                float t = fabsf(ex) * by > fabsf(ey) * bx ? bx / fabsf(ex) : by / fabsf(ey);
                int mx = (int)(W / 2.f + ex * t), my = (int)(H / 2.f + ey * t);
                HBRUSH br = CreateSolidBrush(col), ob = SelectObject(g_mdc, br);
                HPEN pn = CreatePen(PS_SOLID, 1, RGB(1, 1, 1)), op = SelectObject(g_mdc, pn);
                Ellipse(g_mdc, mx - 9, my - 9, mx + 9, my + 9);
                SelectObject(g_mdc, ob); SelectObject(g_mdc, op); DeleteObject(br); DeleteObject(pn);
                SelectObject(g_mdc, GetStockObject(NULL_BRUSH));
                int tx = mx + (ex < 0 ? 12 : -12 - 8 * (int)strlen(s)), ty = my + (ey < 0 ? 10 : -26);
                TextOutA(g_mdc, tx, ty, s, (int)strlen(s));
                continue;
            }
            onscreen++;
            int target = (i == g_aimTarget);
            { // linha do pe da tela ate o inimigo
                HPEN lp = CreatePen(PS_SOLID, 1, target ? RGB(255, 0, 255) : col), op = SelectObject(g_mdc, lp);
                MoveToEx(g_mdc, W / 2, H - 2, NULL); LineTo(g_mdc, (int)fx, y1);
                SelectObject(g_mdc, op); DeleteObject(lp);
            }
            HPEN pen = CreatePen(PS_SOLID, target ? 3 : 2, target ? RGB(255, 0, 255) : col), op = SelectObject(g_mdc, pen);
            Rectangle(g_mdc, x0, y0, x1, y1);
            SelectObject(g_mdc, op); DeleteObject(pen);
            // barra de vida (max 30 p/ inimigos)
            float frac = c->hp / 30.f; if (frac > 1) frac = 1;
            RECT bg = { x0 - 6, y0, x0 - 3, y1 }, fgR = { x0 - 6, y1 - (int)((y1 - y0) * frac), x0 - 3, y1 };
            HBRUSH bb = CreateSolidBrush(RGB(60, 60, 60)), gb = CreateSolidBrush(frac > 0.5f ? RGB(40, 230, 40) : frac > 0.25f ? RGB(240, 200, 0) : RGB(240, 40, 40));
            FillRect(g_mdc, &bg, bb); FillRect(g_mdc, &fgR, gb); DeleteObject(bb); DeleteObject(gb);
            char hp[16]; sprintf(hp, "%.0f hp", c->hp);
            SetTextColor(g_mdc, RGB(230, 230, 230)); TextOutA(g_mdc, x0, y0 - 17, hp, (int)strlen(hp));
            u32 ars = r32(c->p + CH_ARSENAL);
            char lab[64]; sprintf(lab, "%s  %s", s, ars ? weapon_name(ars, c->weapon) : "");
            SetTextColor(g_mdc, col); TextOutA(g_mdc, x0, y1 + 2, lab, (int)strlen(lab));
        }
    }
    if (aimbot) { // circulo do FOV do aimbot
        int r = (int)(AIM_FOV_PX * H / 1080.f);
        HPEN cp = CreatePen(PS_DOT, 1, RGB(255, 0, 255)), op = SelectObject(g_mdc, cp);
        SetBkMode(g_mdc, TRANSPARENT);
        Ellipse(g_mdc, W / 2 - r, H / 2 - r, W / 2 + r, H / 2 + r);
        SelectObject(g_mdc, op); DeleteObject(cp);
    }
    { // indicador de que o overlay esta ativo
        char s[128]; sprintf(s, "ESP  vivos:%d  na tela:%d  camera:%s%s", alive, onscreen, g_camOk ? "ok" : "--",
                             aimbot ? (g_aimTarget >= 0 ? "  AIMBOT: alvo" : "  AIMBOT: sem alvo") : "");
        SetTextColor(g_mdc, RGB(0, 255, 120)); TextOutA(g_mdc, 10, 8, s, (int)strlen(s));
    }
    BitBlt(wdc, 0, 0, W, H, g_mdc, 0, 0, SRCCOPY);
    ReleaseDC(g_ov, wdc);
}

// ---------------------------------------------------------------- console
static char g_msg[96];
static void status(void) {
    HANDLE o = GetStdHandle(STD_OUTPUT_HANDLE); COORD c = { 0, (SHORT)g_row }; SetConsoleCursorPosition(o, c);
    int alive = 0; for (int i = 0; i < g_nch; i++) if (g_ch[i].p != g_player && g_ch[i].hp > 0) alive++;
    printf("  [NUM1] God mode ......... %-3s   \n", god ? "ON" : "off");
    printf("  [NUM2] Municao infinita . %-3s   \n", ammo ? "ON" : "off");
    printf("  [NUM3] Sem recarga ...... %-3s   \n", noreload ? "ON" : "off");
    printf("  [NUM4] ESP .............. %-3s   \n", esp ? "ON" : "off");
    printf("  [NUM5] Hit kill ......... %-3s   \n", onehit ? "ON" : "off");
    printf("  [NUM6] Liberar todas as armas (+ municao cheia)          \n");
    printf("  [NUM7] Tiro rapido (x%.0f) . %-3s   \n", 1.f / RAPID_MULT, rapid ? "ON" : "off");
    printf("  [NUM8] Aimbot ........... %-3s   (mira enquanto segura o botao esquerdo ou lateral do mouse)\n", aimbot ? "ON" : "off");
    printf("  [NUM0] Encher vida agora      [END] sair e restaurar\n\n");
    if (g_msg[0]) printf("  %-70s\n", g_msg); else printf("  %-70s\n", "");
    printf("  Jogador: %08X  HP %5.1f/%-4.0f  pente %-4d          \n", g_player, g_php, g_pmax, g_clip);
    printf("  Personagens ativos: %-3d  inimigos vivos: %-3d  camera: %-3s   \n", g_nch, alive, g_camOk ? "ok" : "--");
    printf("  Hooks: vida=%d pente=%d update=%d render=%d   chamadas vida=%u pente=%u      \n",
           g_patHealth, g_patClip, g_patUpd, g_patRend, r32(g_D + D_CNT_HEALTH), r32(g_D + D_CNT_CLIP));
}

static void tick(void);
static BOOL WINAPI on_ctrl(DWORD t) { (void)t; god = ammo = noreload = onehit = rapid = aimbot = 0; tick(); uninstall(); return FALSE; } // devolve municao/cadencia e remove hooks

// borda de descida pelo estado real da tecla (o bit 0 do GetAsyncKeyState e compartilhado e perde toques)
static u8 g_keyDown[256];
static int pressed(int vk) {
    int down = (GetAsyncKeyState(vk) & 0x8000) != 0, hit = down && !g_keyDown[vk];
    g_keyDown[vk] = (u8)down; return hit;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--dump")) { // so para conferir o codigo gerado
        static u8 buf[0x1000]; g_local = buf; g_cave = 0x20000000; g_D = g_cave + 0x1000; build_caves(g_cave);
        FILE *f = fopen("caves.bin", "wb"); fwrite(buf, 1, sizeof buf, f); fclose(f);
        printf("health=%08X clip=%08X upd=%08X rend=%08X\n", g_hcHealth, g_hcClip, g_hcUpd, g_hcRend); return 0;
    }
    SetConsoleTitleA("Max Payne - trainer externo");
    printf("Max Payne 1 (v1.04) - trainer externo\n");
    printf("Aguardando MaxPayne.exe...\n");
    while (!(g_pid = find_pid())) Sleep(500);
    g_h = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION | PROCESS_SUSPEND_RESUME, FALSE, g_pid);
    if (!g_h) { printf("OpenProcess falhou (%lu). Rode como admin se o jogo estiver como admin.\n", GetLastError()); getchar(); return 1; }
    u8 chk[13];
    if (!rd(FN_HEALTH_SET, chk, 13) || (memcmp(chk, ORIG_HEALTH, 13) && chk[0] != 0xE9)) {
        printf("Versao do MaxPayne.exe nao reconhecida (esperado 1.04). Nada foi alterado.\n"); getchar(); return 1;
    }
    if (chk[0] == 0xE9) { printf("O jogo ja tem patches de outra execucao do trainer. Reinicie o jogo.\n"); getchar(); return 1; }
    for (int i = 0; i < 40 && !(g_e2 = find_module("e2mfc.dll")); i++) Sleep(250);
    printf("PID %lu  e2mfc=%08X\n", g_pid, g_e2);
    install();
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    ov_create();
    { CONSOLE_SCREEN_BUFFER_INFO ci; GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &ci); g_row = ci.dwCursorPosition.Y + 1; }
    for (int i = 0; i < 10; i++) pressed(VK_NUMPAD0 + i); pressed(VK_END); pressed(VK_CLEAR);

    DWORD lastStatus = 0;
    for (;;) {
        MSG msg; while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        DWORD code = 0;
        if (!GetExitCodeProcess(g_h, &code) || code != STILL_ACTIVE) { printf("\nO jogo fechou.\n"); break; }
        if (pressed(VK_END)) break;
        if (pressed(VK_NUMPAD1)) god ^= 1;
        if (pressed(VK_NUMPAD2)) ammo ^= 1;
        if (pressed(VK_NUMPAD3)) noreload ^= 1;
        if (pressed(VK_NUMPAD4)) esp ^= 1;
        if (pressed(VK_NUMPAD5) | pressed(VK_CLEAR)) onehit ^= 1; // VK_CLEAR = NUM5 com NumLock desligado
        if (pressed(VK_NUMPAD7)) rapid ^= 1;
        if (pressed(VK_NUMPAD8)) aimbot ^= 1;
        if (!g_game || !IsWindow(g_game)) { g_game = 0; EnumWindows(enum_cb, 0); }
        tick();
        if (pressed(VK_NUMPAD6)) {
            if (g_player) { int n = give_weapons(); sprintf(g_msg, "Armas liberadas: %d nova(s), municao no maximo.", n); }
            else strcpy(g_msg, "Jogador nao encontrado - entre no jogo e tente de novo.");
        }
        aim_step();
        if (pressed(VK_NUMPAD0) && g_player) { float f = g_pmax; wr(g_player + CH_HP, &f, 4); }
        ov_draw();
        if (GetTickCount() - lastStatus > 250) { status(); lastStatus = GetTickCount(); }
        Sleep(8);
    }
    god = ammo = noreload = onehit = rapid = aimbot = 0; tick(); // devolve flags de municao / cadencia originais
    uninstall();
    printf("\nPatches restaurados. Pode fechar.\n");
    Sleep(1500);
    return 0;
}
