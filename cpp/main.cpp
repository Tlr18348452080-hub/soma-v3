#include "walker.h"
#include "spinal_core.h"
#include "senses.h"
#include "poseprior.h"
#include "config.h"
#include "room.h"
#include "nav.h"      /* 屋子碰撞盒 + 触觉神经末梢 */
#include <utility>
#include "brain.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <sys/select.h>

#ifndef SOMA_NET
#define SOMA_NET 0
#endif

static void runSim(Walker& W, float sec, Frame* out, int stride = 0) {
  const float dt = 1.0f / 240.0f;
  int n = (int)(sec / dt);
  Frame last;
  for (int i = 0; i < n; ++i) {
    W.step(dt);
    if (stride && i % stride == 0) { W.pose(last); }
  }
  W.pose(last);
  if (out) *out = last;
}

static void printFrame(const Frame& f) {
  printf("t=%.2f speed=%.2f com=(%.3f,%.3f) cap=(%.3f,%.3f) zmp=(%.3f,%.3f) lean=(%.3f,%.3f) err=%.4f falls=%d duty=%.3f\n",
         f.t, f.speed, f.comX, f.comZ, f.capX, f.capZ, f.zmpX, f.zmpZ, f.leanX, f.leanZ, f.err, f.falls, f.dutyMeas);
}

static float P_HIP() { return 0.86f; }   /* Params::hipH 的默认值，测试里对一下就行 */

static int g_fail = 0;
static void checkv(const char* name, bool ok, const char* fmt, ...) {
  printf("%s %s  ", ok ? "PASS" : "FAIL", name);
  va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
  printf("\n");
  if (!ok) g_fail++;
}

static void check(const char* name, bool ok, const char* fmt, double a, double b) {
  printf("%s %s  ", ok ? "PASS" : "FAIL", name);
  printf(fmt, a, b);
  printf("\n");
  if (!ok) g_fail++;
}


/* ================= 实时服务模式：从 stdin 收指令，按墙钟 120Hz 推进，60Hz 出 JSON 帧 ============ */
static BrainChannel GB;
static Bio WBIO;

/* ---- 大脑给的意图（不是动作）----
    u = 速度 / 抬手 / 前倾 / 摆手频率。手最后长什么样，是模型算的，不是这里写的。 */
static PosePrior PP;
static int gDbgArms = 0;
static float gArmPhDbg = 0, gArmYDbg = 0, gArmXDbg = 0;
/* 大脑想要什么（JSON 里那几项）。它只写"目标"，
   真正的变化由矩阵每帧一点点推过去 —— 大脑不直接改身体。 */
struct AutoIntent { float vDes = 0.85f, armRaise = 0.0f, lean = 0.0f, waveHz = 0.0f; bool has = false; };
static AutoIntent AUTO;
static Senses SENS;              /* 视/听/触 + 碰撞 */
static OrganState ORG;           /* 心/肺（1 Hz 慢变量） */
static PlayerState PLR;          /* 主角在哪、跑多快（网页每 200 ms 推一次） */
static SenseCfg SC;
static OrganCfg OC;
static float gArmRaise = 0.0f, gWaveHz = 0.0f, gLean = 0.0f, gWavePh = 0.0f;          /* 最近一帧的生物层快照（emit 用，不参与物理） */
float DBGX[4] = {0,0,0,0};
static int VYSRC = 0;   /* 谁最后写的 vYawWant：调试用 */
static SpinalCore GSC;   /* 骨髓中枢：60 Hz，自己跑，只在看不懂时才抬头 */
static bool WANDER_OFF = false;   /* intent 命令/大脑给过速度之后，闲逛就别再插手 */
/* 大脑只想接管一阵子，不是一辈子。
   以前 answer 一进来就把 WANDER_OFF 永久置 true —— 她从此一直按那个速度走，
   再也不站住、idleT 永远为 0、于是**再也问不到第二次**。问一次就聋了。
   现在按秒交还：大脑说完话接管 8 s，之后她自己继续闲逛，站住了还会再想。 */
static float BRAIN_HOLD_T = 0.0f;

/* ---- 路线（房间动线）----
   前端把房间里的航点发过来（route add x z × N，然后 route on），她自己一路走过去。
   有路线的时候：不闲逛、也不做 8 米格子回绕 —— 她真的在房间里绕圈，
   画面上她的坐标就是房间坐标，不再需要前端"猜"她在哪。 */
static std::vector<float> ROUTE;      /* x,z 成对 */
static int ROUTE_I = 0;
/* goto：走一个目标点，到了就站住（可以再给一个朝向）。跟 route 的"一直绕圈"是两件事。 */
static bool GOTO_ON = false;
static int GOTO_PH = 0;              /* 0=在走 1=站定转身 2=完事 */
static float GOTO_X = 0, GOTO_Z = 0, GOTO_FACE = 0;
static bool GOTO_HASFACE = false;   /* goto 有没有带"到了朝哪边"这个参数 */
static bool ROUTE_ON = false;
static float ROUTE_V = 0.66f;         /* 巡航速度：屋子小了（走廊 2.2 m 宽），0.82 走不住 */

/* ---------- 内分泌：把"她想要什么"从阈值里长出来 ----------
   以前只有站太久（idle）会想到问大脑；饿了、渴了、累了全都不说话。
   这三样是慢变量，一整天都在漂，漂过线就该她做主了。 */
struct Endocrine {
  float hunger = 0, thirst = 0, tired = 0, stress = 0;   /* 0..1，越大越难受 */
  /* 情绪激素。不是装饰：它们直接改步态的振幅和步频（见 walker.h 的 dopaGain/cortGain），
     所以"心情好"和"心里堵"在她走路的样子上是看得见的。 */
  float dopa = 0.35f;      /* 多巴胺：看见人、吃到东西、被摸 → 涨；衰减快（20 s） */
  float cort = 0.20f;      /* 皮质醇：卡住、摔倒、太累 → 涨；衰减慢（90 s） */
  float pain = 0.0f;       /* 疼：摔一次 0.35，25 s 退一半 —— 走路会护着 */
  /* 三个原来缺的：没有它们，她就只有"饿/累/怕"三种状态，
     大脑收到的是一张干巴巴的表，当然也只会给出干巴巴的意图。 */
  float adr = 0.05f;       /* 肾上腺素：受惊/被推/摔倒 → 秒涨，30 s 回落；拉心率、缩反应时间 */
  float oxy = 0.30f;       /* 催产素：被说话、被摸、靠得近 → 缓涨（2 min），让她想凑近人 */
  float ser = 0.55f;       /* 血清素：情绪基线，睡好/吃好/没被吓 → 高；皮质醇压它 */
  float mel = 0.0f;        /* 褪黑素：跟着钟点和光走，睡前爬升；高了她会打哈欠、动作发软 */
  float glu = 0.90f;       /* 血糖：饿了掉、吃了涨；低了手抖、步速掉 */
  float melT = 0.0f;       /* 褪黑素目标值：由 main 循环按钟点写（夜 0.75、白天 0.05） */
  void setNight(float v) { melT = clampf(v, 0.0f, 1.0f); }
  void spikeDopa(float a) { dopa = clampf(dopa + a, 0.0f, 1.0f); }
  void spikeCort(float a) { cort = clampf(cort + a, 0.0f, 1.0f); }
  void spikePain(float a) { pain = clampf(pain + a, 0.0f, 1.0f); }
  void spikeAdr(float a)  { adr  = clampf(adr  + a, 0.0f, 1.0f); }
  void spikeOxy(float a)  { oxy  = clampf(oxy  + a, 0.0f, 1.0f); }
  float lastWhyT[6] = {0,0,0,0,0,0};                     /* 同一个理由别连着问 */
  void update(float kcal, float water, float sleep, float fat, bool stuck, float dt) {
    const float tgtH = clampf(1.0f - kcal, 0.0f, 1.0f);
    const float tgtT = clampf(1.0f - water, 0.0f, 1.0f);
    const float tgtTired = clampf(fmaxf(fat, 1.0f - sleep), 0.0f, 1.0f);
    /* 真的饿/渴是慢慢逼上来的，不能像开关一样跳 */
    hunger += (tgtH - hunger) * std::min(1.0f, dt / 6.0f);
    thirst += (tgtT - thirst) * std::min(1.0f, dt / 6.0f);
    tired  += (tgtTired - tired) * std::min(1.0f, dt / 10.0f);
    const float stg = stuck ? 1.0f : 0.0f;
    stress += (stg - stress) * std::min(1.0f, dt / 4.0f);
    /* 激素都是慢慢落回去的；跌得比涨得慢，就是"情绪有惯性" */
    const float dBase = 0.30f + 0.25f * (1.0f - tired);
    dopa += (dBase - dopa) * std::min(1.0f, dt / 20.0f);
    const float cBase = 0.10f + 0.50f * tired + 0.28f * stress;   /* 卡住别把皮质醇顶到满格 */
    cort += (cBase - cort) * std::min(1.0f, dt / 90.0f);
    pain *= std::exp(-dt / 22.0f);   /* 半衰期约 15 s */
    /* 肾上腺素：涨得快落得快（τ 30 s），受惊之后三五秒就能把心率顶上去 */
    adr += (0.03f - adr) * std::min(1.0f, dt / 30.0f);
    /* 血清素：睡够 + 不疼 + 不慌 才起得来，皮质醇一高压着它 */
    const float serT = clampf(0.35f + 0.45f * sleep - 0.35f * cort - 0.25f * pain, 0.0f, 1.0f);
    ser += (serT - ser) * std::min(1.0f, dt / 120.0f);
    /* 催产素：衰减慢（τ 150 s），"刚才有人跟我说话"这件事能在身上留几分钟 */
    oxy += (0.22f - oxy) * std::min(1.0f, dt / 150.0f);
    /* 褪黑素由外面的钟点灌进来（setNight），这里只负责跟着走 */
    mel += (melT - mel) * std::min(1.0f, dt / 60.0f);
    /* 血糖：饿得越狠掉得越快 */
    const float gluT = clampf(0.94f - 0.55f * hunger, 0.0f, 1.2f);
    glu += (gluT - glu) * std::min(1.0f, dt / 240.0f);
    for (int i = 0; i < 6; ++i) lastWhyT[i] += dt;
  }
};

/* ---------- 大脑缓存：问过一次的，下次自己就会了 ----------
   大脑给的意图按 why 分桶存起来。同一个理由再来，先看桶里有没有货：
   有就照着做，不问模型。这就是"主动动作被脊髓学成自动行为"，
   命中一次省一次 token —— 也让她越活越像自己，而不是每次都重新想。 */
struct BrainCache {
  struct Slot { bool has = false; float vDes = 0, armRaise = 0, lean = 0, waveHz = 0; int hits = 0; };
  Slot s[10];
  int nHit = 0, nMiss = 0;
  static int idx(const char* why) {
    if (!why) return 0;
    if (!strcmp(why, "idle"))   return 0;
    if (!strcmp(why, "hungry")) return 1;
    if (!strcmp(why, "thirsty"))return 2;
    if (!strcmp(why, "tired"))  return 3;
    if (!strcmp(why, "stuck"))  return 4;
    if (!strcmp(why, "fallen")) return 5;
    if (!strcmp(why, "spinal")) return 6;
    if (!strcmp(why, "see"))    return 7;   /* 看见人：跟"别的场合"分开存，不然学混了 */
    return 9;
  }
  bool get(const char* why, float& vDes, float& arm, float& lean, float& wave) {
    const Slot& e = s[idx(why)];
    if (!e.has) { ++nMiss; return false; }
    vDes = e.vDes; arm = e.armRaise; lean = e.lean; wave = e.waveHz;
    ++s[idx(why)].hits; ++nHit;
    return true;
  }
  /* 放进去的时候跟旧的做一下平滑：同一个场合大脑给的值每次略有差别，
     直接覆盖会让她的反应看起来神经质。学的是"平均做法"。 */
  void put(const char* why, float vDes, float arm, float lean, float wave) {
    Slot& e = s[idx(why)];
    const float k = e.has ? 0.45f : 1.0f;
    e.vDes     += (vDes - e.vDes) * k;
    e.armRaise += (arm  - e.armRaise) * k;
    e.lean     += (lean - e.lean) * k;
    e.waveHz   += (wave - e.waveHz) * k;
    e.has = true;
  }
  static const char* name(int i) {
    static const char* N[10] = {"idle","hungry","thirsty","tired","stuck","fallen","spinal","see","other","other"};
    return N[i];
  }
  /* ---- 持久化：这些是她真的学会的东西，重启不能丢 ---- */
  bool load(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    static char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f); buf[n] = 0; fclose(f);
    const char* p = buf;
    while ((p = strchr(p, '{'))) {
      int i = -1; float v = 0, a = 0, l = 0, w = 0; int h = 0;
      char why[20];
      /* save 写出来的行形状是 {"i":N,"why":"x","vDes":...} —— 少匹配那段 why 就一条也读不回来 */
      if (sscanf(p, "{\"i\":%d,\"why\":\"%19[^\"]\",\"vDes\":%f,\"arm\":%f,\"lean\":%f,\"wave\":%f,\"hits\":%d}", &i, why, &v, &a, &l, &w, &h) == 7 && i >= 0 && i < 10) {
        s[i].has = true; s[i].vDes = v; s[i].armRaise = a; s[i].lean = l; s[i].waveHz = w; s[i].hits = h;
      }
      ++p;
    }
    return true;
  }
  void save(const char* path) const {
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "{\n  \"note\": \"her reflex cache: what she has learned to do without thinking\",\n  \"slots\": [\n");
    bool first = true;
    for (int i = 0; i < 10; ++i) {
      if (!s[i].has) continue;
      fprintf(f, "%s    {\"i\":%d,\"why\":\"%s\",\"vDes\":%.4f,\"arm\":%.4f,\"lean\":%.4f,\"wave\":%.4f,\"hits\":%d}",
              first ? "" : ",\n", i, name(i), s[i].vDes, s[i].armRaise, s[i].lean, s[i].waveHz, s[i].hits);
      first = false;
    }
    fprintf(f, "\n  ]\n}\n");
    fclose(f);
  }
  /* 命中够多就"长在身上"了 —— 之后连问都不用问 */
  bool automatic(const char* why) const { return s[idx(why)].has && s[idx(why)].hits >= 6; }
};
static BrainCache BCACHE;
static float CACHE_SAVE_T = 0.0f;
/* 看见玩家的上升沿：她转头对上的那一瞬间才值得说一句，一直盯着不算 */
static bool SEEN_LAST = false;
static float SEE_EDGE = 0.0f;
static int LAST_FALLS = 0;
static Endocrine ENDO;
static const char* LAST_WHY = nullptr;

/* ---------- 屋子与触觉（每帧最多留 8 条接触） ---------- */
static RoomColl ROOM;
static bool ROOM_OK = false;
static const char* TOUCH_NM[8];
static float TOUCH_F[8], TOUCH_S[8];
static int TOUCH_N = 0;
static float BUMP_T = 0.0f;      /* 连续顶着家具的时间 */
/* 被家具顶住后放掉 goto，过一会儿再试 —— 目的是"绕过"而不是"放弃" */
static float GOTO_RETRY_T = 0.0f;
static NavGrid NAV;
static float NAV_WX = 0.0f, NAV_WZ = 0.0f;
static bool NAV_OK = false;
static float NAV_T = 0.0f;   /* 上次重算路径的时间 */
/* 撞上家具后往哪边绕：沿障碍法线转 90°，选离目标更近的那边。0.9 s 一换。 */
static float AVOID_T = 0.0f;
static float AVOID_SIGN = 1.0f;
static float GOTO_RX = 0.0f, GOTO_RZ = 0.0f;
static bool GOTO_RHASFACE = false;
/* 触觉不是"这一帧有没有"，是"刚才被碰了一下，余味还在"：
   家具推挤是一帧就弹开的，只按帧发的话外面根本看不到（实测 700 帧里 0 条）。
   所以每条接触带一个存活时间（0.7 s），新的接触覆盖旧的，力度取最大。 */
static float TOUCH_T[8] = {0, 0, 0, 0, 0, 0, 0, 0};
static void decayTouch(float dt) {
  for (int i = 0; i < TOUCH_N; ++i) TOUCH_T[i] -= dt;
  int w = 0;
  for (int i = 0; i < TOUCH_N; ++i) {
    if (TOUCH_T[i] > 0.0f) {
      TOUCH_NM[w] = TOUCH_NM[i]; TOUCH_F[w] = TOUCH_F[i];
      TOUCH_S[w] = TOUCH_S[i];   TOUCH_T[w] = TOUCH_T[i]; ++w;
    }
  }
  TOUCH_N = w;
}
static void pushTouch(const char* n, float force, float sens) {
  if (force <= 0.0f) return;
  for (int i = 0; i < TOUCH_N; ++i) {
    if (TOUCH_NM[i] == n) {                 /* 同一处：刷新存活时间，力度取大 */
      if (force > TOUCH_F[i]) TOUCH_F[i] = force;
      TOUCH_T[i] = 0.7f; return;
    }
  }
  if (TOUCH_N >= 8) return;
  TOUCH_NM[TOUCH_N] = n; TOUCH_F[TOUCH_N] = force; TOUCH_S[TOUCH_N] = sens;
  TOUCH_T[TOUCH_N] = 0.7f; ++TOUCH_N;
}

/* ---------------- 屋里怎么走：先找门 ----------------
   3 室 1 厅之后，直线到目标会撞墙：她在客厅 x=1.28，目标在走廊 z=6.5，
   直线穿过的是实墙（门在西边 0~±0.6）。
   不写寻路网格，只写"房间图 + 门"：6 个房间、5 道门，BFS 一跳就够。
   每帧只取路径上的下一道门当临时目标，过了门再重算 —— 这就是最省事的路由。 */
enum RoomId { RM_LIVING = 0, RM_HALL = 1, RM_A = 2, RM_B = 3, RM_K = 4, RM_BATH = 5, RM_N = 6 };
static RoomId roomOf(float x, float z) {
  if (z <= 3.25f) return RM_LIVING;
  if (x > -1.10f && x < 1.10f) return RM_HALL;
  if (x <= -1.10f) return (z <= 6.00f) ? RM_A : RM_B;
  return (z <= 6.40f) ? RM_K : RM_BATH;
}
struct DoorEdge { RoomId a, b; float x, z; };
static const DoorEdge DOORS[5] = {
  {RM_LIVING, RM_HALL, 0.00f, 3.25f},
  {RM_HALL,   RM_A,   -1.10f, 4.30f},
  {RM_HALL,   RM_B,   -1.10f, 7.30f},
  {RM_HALL,   RM_K,    1.10f, 4.30f},
  {RM_HALL,   RM_BATH, 1.10f, 7.30f},
};
/* 返回"下一道该穿的门"；不需要穿门就返回 false（可以直接走直线） */
static bool nextDoor(float px, float pz, float tx, float tz, float& ox, float& oz) {
  const RoomId from = roomOf(px, pz), to = roomOf(tx, tz);
  if (from == to) return false;
  /* BFS：房间图只有 6 个节点，手写邻接 */
  int prev[RM_N];
  for (int i = 0; i < RM_N; ++i) prev[i] = -1;
  prev[from] = (int)from;
  int q[RM_N], qh = 0, qt = 0;
  q[qt++] = (int)from;
  while (qh < qt) {
    const int cur = q[qh++];
    if (cur == (int)to) break;
    for (int i = 0; i < 5; ++i) {
      int nx = -1;
      if ((int)DOORS[i].a == cur) nx = (int)DOORS[i].b;
      else if ((int)DOORS[i].b == cur) nx = (int)DOORS[i].a;
      if (nx >= 0 && prev[nx] < 0) { prev[nx] = cur; q[qt++] = nx; }
    }
  }
  if (prev[(int)to] < 0) return false;
  /* 从目标往回追，找到"源头房间的下一间" */
  int step = (int)to;
  while (prev[step] != (int)from && prev[step] != -1) step = prev[step];
  if (prev[step] != (int)from) return false;
  for (int i = 0; i < 5; ++i) {
    const int a = (int)DOORS[i].a, b = (int)DOORS[i].b;
    if ((a == (int)from && b == step) || (b == (int)from && a == step)) {
      ox = DOORS[i].x; oz = DOORS[i].z;
      return true;
    }
  }
  return false;
}

static void emit(const Frame& f) {
  /* 触觉：这一帧身体上被压到的地方（名字, 力, 敏感度）。没有就是空数组。 */
  static char tbuf[640];
  { static int nt = 0; if (TOUCH_N > 0 && ++nt % 60 == 1) fprintf(stderr, "[touch] n=%d first=%s\n", TOUCH_N, TOUCH_NM[0]); }
  {
    size_t o = 0; tbuf[0] = '\0';   /* ★ 这里原来写成 tbuf[o++]='\0'，把 o 先推成 1，
                                       结果是"写在缓冲区第 1 字节、第 0 字节还是 0" ——
                                       触觉一直都在算，JSON 里永远是空数组。 */
    for (int i = 0; i < TOUCH_N; ++i) {
      o += (size_t)snprintf(tbuf + o, sizeof(tbuf) - o, "%s[\"%s\",%.2f,%.2f]",
                            i ? "," : "", TOUCH_NM[i], TOUCH_F[i], TOUCH_S[i]);
      if (o >= sizeof(tbuf) - 64) break;
    }
  }
  printf("{\"t\":%.3f,\"mode\":%d,\"falls\":%d,\"speed\":%.3f,\"yaw\":%.4f,\"vyaw\":%.3f,"
         "\"com\":[%.4f,%.4f],\"cap\":[%.4f,%.4f],"
         "\"zmp\":[%.4f,%.4f],\"lean\":[%.4f,%.4f],\"duty\":%.3f,\"err\":%.4f,\"nodes\":[",
         f.t,(int)f.mode,f.falls,f.speed,f.yaw,f.vYawOut,f.comX,f.comZ,f.capX,f.capZ,f.zmpX,f.zmpZ,f.leanX,f.leanZ,f.dutyMeas,f.err);
  (void)0;
  for (int i = 0; i < 18; ++i)
    printf("%s[%.4f,%.4f,%.4f,%.3f,%d]", i ? "," : "", f.nodes[i].x, f.nodes[i].y, f.nodes[i].z, f.radii[i], (int)f.contact[i]);
  printf("],\"comY\":%.4f,\"fallen\":%d,\"metab\":%.0f,\"fat\":%.3f,\"temp\":%.2f,"
         "\"bio\":{\"kcal\":%.4f,\"water\":%.4f,\"sleep\":%.4f,\"hunger\":%.4f,\"bladder\":%.4f},"
         "\"grfL\":[%.1f,%.1f,%.1f],\"grfR\":[%.1f,%.1f,%.1f],"
         "\"cop\":[%.4f,%.4f],\"mu\":%.3f,\"dbg\":[%.3f,%.3f,%.2f,%.2f],"
         "\"spinal\":{\"err\":%.4f,\"avg\":%.4f,\"doubt\":%.3f,\"reflex\":%.2f,\"roll\":%.4f,\"rollDeg\":%.2f,\"ticks\":%lld},\"touch\":[%s],\"senses\":{\"dist\":%.2f,\"bearing\":%.3f,\"sees\":%d,\"heard\":%d,\"touch\":%.0f,\"headYaw\":%.3f},\"horm\":{\"dopa\":%.3f,\"cort\":%.3f,\"pain\":%.3f,\"adr\":%.3f,\"oxy\":%.3f,\"ser\":%.3f,\"mel\":%.3f,\"glu\":%.3f},\"organs\":{\"hr\":%.1f,\"br\":%.1f,\"ox\":%.3f},\"brain\":{\"pending\":%d,\"tag\":%d,\"answered\":%d,\"dropped\":%d,\"timeout\":%d},\"bones\":[",
         f.comY, (int)f.fallen, f.metab, f.fat, f.temp,
         WBIO.kcal, WBIO.water, WBIO.sleep, WBIO.hunger, WBIO.bladder,
         f.grfL[0], f.grfL[1], f.grfL[2], f.grfR[0], f.grfR[1], f.grfR[2],
         f.copX, f.copZ, f.frictionUse, gArmPhDbg, gArmXDbg, DBGX[2], DBGX[3],
         GSC.err, GSC.errAvg, GSC.doubt(), GSC.reflexScale(), GSC.roll, GSC.rollAvg * 57.29578, (long long)GSC.ticks,
         tbuf,
         SENS.dist, SENS.bearing, SENS.sees?1:0, SENS.heard?1:0, SENS.touchN, SENS.headYaw,
         ENDO.dopa, ENDO.cort, ENDO.pain, ENDO.adr, ENDO.oxy, ENDO.ser, ENDO.mel, ENDO.glu,
         ORG.hr, ORG.br, ORG.oxygen,
         GB.pending()?1:0, GB.pendingTag, GB.answered, GB.dropped, GB.timedOut);
  for (int i = 0; i < 17; ++i) printf("%s[%d,%d]", i ? "," : "", f.bones[i][0], f.bones[i][1]);
  printf("]}\n");
}

static void handleCmd(Walker& W, const std::string& line) {
  char cmd[64]; float a=0,b=0,c=0;
  if (sscanf(line.c_str(), "%63s %f %f %f", cmd, &a, &b, &c) < 1) return;
  std::string k(cmd);
  if (k == "walk") W.setMode(1);
  else if (k == "run") W.setMode(2);
  else if (k == "stop") W.setMode(0);
  else if (k == "push") W.push(a, b, c);
  else if (k == "vdes") W.P.vDes = a < 0 ? 0 : (a > 2.0f ? 2.0f : a);
  else if (k == "intent") { WANDER_OFF = true; W.intentMode = true; W.P.vDes = a < 0 ? 0 : (a > 2.0f ? 2.0f : a); W.vDesWant = W.P.vDes; }
  else if (k == "reset") { W.reset(); W.setMode((int)a); }
  else if (k == "fall") { if (!W.fallen) W.push(0, 0, 3.0f); }   /* 强制侧向推倒 */
  else if (k == "mu") W.floorMu = a <= 0 ? 0.9f : (a > 1.5f ? 1.5f : a);
  else if (k == "route" || k == "goto") {
    /* goto X Z 是单独一条指令（大脑、习惯层、测试都这么发），以前只在 route 分支里认
       "route goto X Z"，于是所有 goto 都是**静默无效**的 —— 她收到的每一个目标点都被丢掉，
       只剩下闲逛在动。这就是"能动但不会做事"最大的一块。 */

    /* route clear | route add X Z | route poly x z x z ... | route on | route off | route speed V
       注意：这里**不往 stdout 打印**。server.js 把任何没带 "world" 的行都当最后一帧转发给前端，
       一条 {"route":..} 会让前端的 render() 拿到没有 nodes 的对象。调试走 stderr。 */
    std::string rest;
    if (k == "goto") {
      const size_t sp = line.find(' ');
      rest = (sp == std::string::npos) ? std::string("goto") : ("goto " + line.substr(sp + 1));
    } else {
      rest = line.substr(line.find("route") + 5);
    }
    char sub[32] = {0};
    float rx = 0, rz = 0;
    const int got = sscanf(rest.c_str(), "%31s %f %f", sub, &rx, &rz);
    const std::string sb(sub);
    if (got >= 1 && sb == "clear") { ROUTE.clear(); ROUTE_I = 0; ROUTE_ON = false; }
    else if (got >= 1 && sb == "off") { ROUTE_ON = false; W.vYawWant = 0.0f; }
    else if (got >= 1 && sb == "goto") {
      float gx = rx, gz = rz, gf = 0;
      const int g3 = sscanf(rest.c_str(), "%*s %f %f %f", &gx, &gz, &gf);
      /* goto X Z 就是"走过去"；第三个参数只是"到了以后朝哪个方向站定"，可省。
         以前写成 (g3>=3)?0:2 —— 两个参数时直接判定"已到达"，于是大脑的每个目标
         都变成"原地冻住"：她站在那儿不动，看起来像不会做事。 */
      ROUTE_ON = false; GOTO_ON = true; GOTO_PH = 0;
      GOTO_X = gx; GOTO_Z = gz; GOTO_FACE = gf; GOTO_HASFACE = (g3 >= 3);
      W.vDesWant = 0.0f; W.vYawWant = 0.0f; WANDER_OFF = true;
      fprintf(stderr, "[goto] %.2f %.2f ph=%d face=%d\n", gx, gz, GOTO_PH, (int)(g3 >= 3));
    }
    else if (got >= 1 && sb == "speed") { ROUTE_V = clampf(rx, 0.2f, 1.6f); }
    else if (got >= 3 && sb == "poly") {
      ROUTE.clear();
      const char* q = rest.c_str() + rest.find("poly") + 4;
      char* end = nullptr;
      while (true) {
        const double ax = strtod(q, &end);
        if (end == q) break;
        q = end;
        const double az = strtod(q, &end);
        if (end == q) break;
        q = end;
        ROUTE.push_back((float)ax); ROUTE.push_back((float)az);
      }
      ROUTE_I = 0;
    }
    else if (got >= 3 && (sb == "add" || sb == "at")) { ROUTE.push_back(rx); ROUTE.push_back(rz); }
    else if (got >= 3) { ROUTE.push_back(rx); ROUTE.push_back(rz); }
    else if (got >= 1 && sb == "on") {
      /* route on keep —— 别把她瞬移回第一个航点：刷新页面的时候她从原地走过去。
         默认（不带 keep）还是瞬移，selftest 和"从头来一遍"靠它。 */
      char keep[16] = {0};
      const int got2 = sscanf(rest.c_str(), "%31s %15s", sub, keep);
      const bool keepPos = (got2 >= 2 && std::string(keep) == "keep");
      ROUTE_ON = ROUTE.size() >= 6;
      ROUTE_I = 0;
      WANDER_OFF = ROUTE_ON;
      if (ROUTE_ON && !keepPos) {
        const float hx = ROUTE[2] - ROUTE[0], hz = ROUTE[3] - ROUTE[1];
        W.placeAt(ROUTE[0], ROUTE[1], std::atan2(hz, hx));
      }
    }
    fprintf(stderr, "[route] n=%d on=%d i=%d v=%.2f\n", (int)ROUTE.size() / 2, (int)ROUTE_ON, ROUTE_I, ROUTE_V);
  }
  else if (k == "fat") W.bio.fatAvg = a < 0 ? 0 : (a > 0.7f ? 0.7f : a);   /* 演示用：直接设疲劳 */
  else if (k == "getup") W.getup();
  else if (k == "motion") {                                       /* motion <名字> */
    char nm[48] = {0};
    if (sscanf(line.c_str(), "%*s %47s", nm) == 1) {
      for (const auto& m : W.clips) if (m.name == nm) { W.motion.play(m); break; }
    }
  }
  /* 躺下 / 跳一下 / 和主人玩：都走同一条"姿态叠加"通道（motion），
     不去写新的运动学函数 —— 物理该找平衡还是找平衡，这几个只是在上面盖一层姿态。 */
  else if (k == "lie" || k == "jump" || k == "play") {
    for (const auto& m : W.clips) if (m.name == k) { W.motion.play(m); break; }
  }
  else if (k == "mstop") W.motion.stop();
  /* hold / free：让习惯层能在"到了地方以后"把身体按住不必（不是 freeze 物理，
     只是把闲逛那一层的"想走"收掉），做完再还给她。没有新的运动能力，只有一个开关。 */
  /* 被摸：这是"别人的手"进入她身体的唯一入口（现在没有玩家碰撞体）。
     part 用 room.h 里 TOUCH_SITES 的名字：head / palm.L / chest …
     力按 0..1 给，默认 0.5。摸到的地方有末梢密度，所以头上的 0.7 和脚底的 0.85 反应不一样。 */
  else if (k == "pet") {
    char part[32] = "head"; float f = 0.5f;
    sscanf(line.c_str(), "%*s %31s %f", part, &f);
    if (f < 0) f = 0; if (f > 1) f = 1;
    const TouchSite* hit = nullptr;
    for (int i = 0; i < N_TOUCH; ++i) if (!strcmp(TOUCH_SITES[i].name, part)) { hit = &TOUCH_SITES[i]; break; }
    if (hit) {
      pushTouch(hit->name, 6.0f * f, hit->sens);
      ENDO.spikeOxy(0.10f + 0.22f * f * hit->sens);
      ENDO.spikeDopa(0.06f + 0.16f * f * hit->sens);
      ENDO.spikeCort(-0.10f * f);
      if (hit->sens > 0.75f) ENDO.spikeAdr(0.05f);   /* 太敏感的忽然被摸会一惊 */
      fprintf(stderr, "[pet] %s f=%.2f sens=%.2f\n", hit->name, f, hit->sens);
    }
  }
  else if (k == "hold") { WANDER_OFF = true; W.vDesWant = 0.0f; W.vYawWant = 0.0f; W.P.vDes = 0.0f; }
  else if (k == "free") { WANDER_OFF = false; }
  else if (k == "eat") {                                          /* 吃一口：默认一个苹果 ≈ 95 kcal */
    const float kcalIn = a > 0 ? a : 95.0f;
    W.bio.eat(kcalIn);
    printf("{\"ate\":%.0f,\"kcal\":%.3f}\n", kcalIn, W.bio.kcal); fflush(stdout);
  }
  else if (k == "pee") {                                          /* 去卫生间：憋得越久，松下来越舒服 */
    const float b0 = W.bio.bladder;
    W.bio.pee();
    if (b0 > 0.85f) { ENDO.spikePain(0.10f * (b0 - 0.85f) * 6.0f); ENDO.spikeCort(-0.18f); ENDO.spikeDopa(0.22f); }
    else { ENDO.spikeCort(-0.10f); ENDO.spikeDopa(0.12f); }
    printf("{\"pee\":%.3f,\"bladder\":%.3f}\n", W.bio.lastPeeL, W.bio.bladder); fflush(stdout);
  }
  else if (k == "drink") {                                        /* 喝一口：默认 250 ml */
    const float L = a > 0 ? a / 1000.0f : 0.25f;
    W.bio.drink(L);
    printf("{\"drank\":%.3f,\"water\":%.3f}\n", L, W.bio.water); fflush(stdout);
  }
  else if (k == "sleep") {                                        /* 睡 a 秒（默认 1800） */
    const float secs = a > 0 ? a : 1800.0f;
    W.bio.sleepFor(secs);
    W.bio.fatAvg *= std::exp(-secs / 240.0f);
    printf("{\"slept\":%.0f,\"sleep\":%.3f}\n", secs, W.bio.sleep); fflush(stdout);
  }
  else if (k == "spin") {                                         /* 脊髓矩阵：手动写盘 */
    const bool ok = GSC.save("assets/spinal_learned.json");
    printf("{\"spinalSaved\":%d}\n", ok ? 1 : 0); fflush(stdout);
  }
  /* 大脑只改意图：抬手多少、摆手多快、想前倾多少。具体手在哪，模型算。 */
  else if (k == "raise") { gArmRaise = clampf(a, 0.0f, 1.0f);
                           printf("{\"raise\":%.2f}\n", gArmRaise); fflush(stdout); }
  else if (k == "wave")  { gWaveHz = clampf(a, 0.0f, 3.0f);
                           printf("{\"wave\":%.2f}\n", gWaveHz); fflush(stdout); }
  else if (k == "lean")  { gLean = clampf(a, 0.0f, 1.0f);
                           printf("{\"lean\":%.2f}\n", gLean); fflush(stdout); }
  else if (k == "player") {                        /* player <x> <z> [speed] [yaw] */
    PLR.x = a; PLR.z = b; PLR.speed = c;
    float yy = 0; sscanf(line.c_str(), "%*s %*f %*f %*f %f", &yy); PLR.yaw = yy;
    PLR.valid = true;
  }
  else if (k == "brain") GB.ask();                                /* 测试钩子：脊髓挂一块"等大脑"的牌子 */
  else if (k == "answer") {   /* 大脑回来：answer <tag> false | {json} | vdes */
    const bool took = GB.answer((int)a);
    if (!took) return;
    /* 找到 tag 之后剩下的那段，看它是不是一张 JSON */
    const char* pz = line.c_str();
    int seen = 0;
    while (*pz && seen < 2) { if (*pz == ' ' && ++seen == 2) break; ++pz; }
    while (*pz == ' ') ++pz;
    if (*pz == '{') {
      std::string js(pz);
      JVal v; JParser jp; if (jp.parse(js.c_str(), v)) {
        AUTO.vDes    = (float)v.numOr("vDes", AUTO.vDes);
        AUTO.armRaise= (float)v.numOr("armRaise", AUTO.armRaise);
        AUTO.lean    = (float)v.numOr("lean", AUTO.lean);
        AUTO.waveHz  = (float)v.numOr("waveHz", AUTO.waveHz);
        AUTO.has = true;
        WANDER_OFF = true;        /* 大脑说话，插一段队 */
        BRAIN_HOLD_T = 8.0f;      /* 8 秒后把方向盘还给她自己 */
        BCACHE.put(LAST_WHY, AUTO.vDes, AUTO.armRaise, AUTO.lean, AUTO.waveHz);
      }
    } else if (b > 0) {          /* 兼容纯数字：最小改动 */
      AUTO.vDes = b > 2.0f ? 2.0f : b; AUTO.has = true;
    }
  }                               /* 起身（体模型自己站回来） */
  else if (k == "ff") {                                           /* 快进 a 秒（看疲劳/体温用） */
    float secs = a <= 0 ? 0 : (a > 3600 ? 3600 : a);
    int n = (int)(secs * 240.0f);
    for (int i = 0; i < n; ++i) W.step(1.0f / 240.0f);
    printf("{\"ff\":%.1f}\n", (double)secs);
    fflush(stdout);
  }
}

int serve(const char* cfgPath = nullptr) {
  Walker W; W.intentMode = true;
  SomaCfg C; bool loaded = false;
  if (cfgPath && *cfgPath) {
    Params P2; Bio B2;
    loadConfig(cfgPath, P2, B2, C, &SC, &OC);
    GSC.noiseDeg = SC.proprioNoiseDeg;          /* 本体感觉噪声：真脊髓也不是干净的 */
    GSC.driftDegPerMin = SC.proprioDriftDeg;
    if (C.ok) { W.P = P2; W.bio = B2; loaded = true; }
    else fprintf(stderr, "config: %s 读不出来，用内置默认\n", cfgPath);
  }
  /* 她自己会走路：站起来就在白空间里晃，不是等你按摇杆的模型。
     0.85 m/s 是"闲逛"的速度；大脑接管后可以改，脊髓不自信时会自动调慢。 */
  W.P.vDes = 0.85f; W.vDesWant = 0.85f; W.setMode(0);
  /* 脊髓上一次学到的矩阵：接上，而不是从零开始 */
  {
    const bool pok = PP.load("assets/poseprior.json") || PP.load("../assets/poseprior.json");
    fprintf(stderr, "poseprior: %s\n", pok ? "已加载（手臂由模型给）" : "没找到，手臂会退化成零长度");
  }
  /* 屋子：碰撞盒（墙 + 家具）。读不到就退回"四面墙"的老行为，但不静默装作成功。 */
  ROOM_OK = ROOM.load("assets/room.json") || ROOM.load("../assets/room.json");
  fprintf(stderr, "room: %s（%d 个碰撞盒）\n", ROOM_OK ? "已加载" : "没找到，家具不挡人", (int)ROOM.b.size());
  /* 寻路格子：按她的身体半径外扩障碍，建一次就够（家具不动） */
  if (ROOM_OK) { NAV.build(ROOM, 0.26f); fprintf(stderr, "nav: %dx%d 格（%.2f m）\n", NAV.nx, NAV.nz, NAV.cell); }
  if (GSC.load("assets/spinal_learned.json"))
    fprintf(stderr, "spinal: 接着上次学（%lld ticks, rmse %.2f°）\n", (long long)GSC.ticks, GSC.errAvg * 57.3);
  else
    fprintf(stderr, "spinal: 没有历史矩阵，从零学\n");
  if (loaded) W.clips = C.motions;
  /* 把上次学会的东西接回来 —— 她记得自己是怎么应对这些场合的 */
  if (BCACHE.load("assets/brain_cache.json")) {
    int n = 0; for (int i = 0; i < 10; ++i) if (BCACHE.s[i].has) ++n;
    fprintf(stderr, "brain cache: 接回 %d 条肌肉记忆\n", n);
  }
  /* 场景：优先用表里的 props；表里没有就退回一段 12 cm 的台子 */
  if (loaded) {
    for (const Prop& pr : C.props) if (pr.solid()) W.boxes.push_back(Box{pr.x0, pr.x1, pr.z0, pr.z1, pr.h});
  }
  if (W.boxes.empty()) W.boxes.push_back(Box{2.5f, 6.5f, -1.2f, 1.2f, 0.12f});
  printf("{\"world\":[");
  if (loaded) {
    for (size_t i = 0; i < C.props.size(); ++i) {
      const Prop& p = C.props[i];
      printf("%s{\"id\":\"%s\",\"kind\":\"%s\",\"x0\":%.3f,\"x1\":%.3f,\"z0\":%.3f,\"z1\":%.3f,"
             "\"h\":%.3f,\"x\":%.3f,\"z\":%.3f,\"w\":%.3f,\"d\":%.3f}",
             i ? "," : "", p.id.c_str(), p.kind.c_str(),
             p.x0, p.x1, p.z0, p.z1, p.h, p.x, p.z, p.w, p.d);
    }
  } else {
    for (size_t i = 0; i < W.boxes.size(); ++i) {
      const Box& b = W.boxes[i];
      printf("%s{\"x0\":%.3f,\"x1\":%.3f,\"z0\":%.3f,\"z1\":%.3f,\"h\":%.3f}", i ? "," : "", b.x0, b.x1, b.z0, b.z1, b.h);
    }
  }
  printf("]}\n");
  fflush(stdout);
  if (loaded) fprintf(stderr, "config ok: %d motions, %zu props\n", (int)C.motions.size(), C.props.size());
  const float dt = 1.0f / 120.0f;
  Frame f;
  int frameCount = 0;
  /* ---- 脊髓自主：什么时候叫醒大脑 ---- */
  float idleT = 0.0f, stuckT = 0.0f, fallT = 0.0f;
  float pxLast = W.p.x, pzLast = W.p.z;
  unsigned seed = 20261111u;
  auto rnd01 = [&seed]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; };
  auto t0 = std::chrono::steady_clock::now();
  double simTime = 0;
  double wanderT = 0;
  std::string buf;
  bool stdinOpen = true;
  while (true) {
    /* 非阻塞读 stdin。EOF 只表示"没人再下命令了"，不是该关机的信号——
       以前这里 n<=0 就 break，结果从管道/终端跑起来只吐一行 world 就死。 */
    if (stdinOpen) {
      fd_set rs; FD_ZERO(&rs); FD_SET(0, &rs);
      timeval tv{0, 0};
      if (select(1, &rs, nullptr, nullptr, &tv) > 0) {
        char tmp[512];
        ssize_t n = read(0, tmp, sizeof(tmp) - 1);
        if (n > 0) {
          tmp[n] = 0; buf += tmp;
          size_t pos;
          while ((pos = buf.find('\n')) != std::string::npos) {
            handleCmd(W, buf.substr(0, pos)); buf.erase(0, pos + 1);
          }
          /* 有些客户端（HTTP 代理那条路）不补换行：一次读进来的整段就当一条命令。
             不这么兜底的话，"player x z" 会一直躺在 buf 里等下一个换行。 */
          if (!buf.empty() && buf.size() > 3) {
            const size_t sp1 = buf.find(' ');
            if (sp1 != std::string::npos) {
              const std::string head = buf.substr(0, sp1);
              if (head == "player" || head == "intent" || head == "vdes" || head == "raise" ||
                  head == "wave" || head == "lean" || head == "eat" || head == "drink" ||
                  head == "sleep" || head == "answer" || head == "push") {
                handleCmd(W, buf); buf.clear();
              }
            }
          }
        } else {
          stdinOpen = false;   /* EOF / 出错：别再 select，否则空转 */
        }
      }
    }
    /* 白空间的格子在 x 上每 8 米重复一套（门/毯子/笔记本/灯）。
       她走过一整格，就把状态整体挪回来一格 —— 两边一模一样，画面上没有接缝。
       以前这里是 reset()，每 7 米闪一下，看着像在跑步机上。 */
    /* 没有路线的时候（页面还没连、或她说不转了）：在屋里待着，别顺着 +x 一直走到 15 米外去。
       旧版是两个世界共用的「格子重复」位移，房间只有 4 米见方，用不上。 */

    /* 脊髓自己判断：这一帧要不要叫醒大脑。
       不问的帧就照旧跑——大脑不是每帧都在，只有"心里没数"才挂一块牌子。 */
    {
      const float moved = std::fabs(W.p.x - pxLast) + std::fabs(W.p.z - pzLast);
      pxLast = W.p.x; pzLast = W.p.z;
      /* "卡住"要真的在想走：vDes 0.05 时她只是收尾慢，会被误判成卡着出不去，
         而 stuck 会把 stress 顶到 1、皮质醇一路 0.9 —— 脸上就永远是那副生气的样子。 */
      if (W.P.vDes > 0.18f && moved < 0.0006f) stuckT += dt;
      else stuckT = std::max(0.0f, stuckT - dt * 2.0f);
      if (W.P.vDes <= 0.05f && !W.fallen) idleT += dt; else idleT = 0.0f;

      ENDO.update(WBIO.kcal, WBIO.water, WBIO.sleep, W.bio.fatAvg, stuckT > 1.2f, dt);
    /* 憋着：不摔不疼，但人会烦、走不快 —— 这就是"不干要惩罚"的那一半 */
    if (W.bio.bladder > 0.80f) {
      const float k2 = (W.bio.bladder - 0.80f) * 5.0f;
      ENDO.spikeCort(0.010f * k2 * dt * 60.0f / 60.0f);
      ENDO.dopa = clampf(ENDO.dopa - 0.004f * k2 * dt, 0.0f, 1.0f);
      if (W.bio.bladder > 0.96f) ENDO.spikePain(0.002f * dt * 60.0f);
    }
      /* 摔一次疼一阵：疼和皮质醇都记在身体里，走路会自己护着，
         不用外面再下"你摔疼了要慢点"这种指令。 */
      if (W.falls > LAST_FALLS) {
        /* 摔一跤别把她钉在又疼又慌里三分钟：一次真的够疼，但幅度收小、退得快 */
        ENDO.spikePain(0.22f); ENDO.spikeCort(0.26f); ENDO.spikeAdr(0.42f); ENDO.spikeDopa(-0.14f);
        if (W.falls - LAST_FALLS > 1) { const int d = W.falls - LAST_FALLS;
          ENDO.spikePain(0.20f * (d - 1)); ENDO.spikeCort(0.18f * (d - 1)); }
        LAST_FALLS = W.falls;
      }
      if (stuckT > 1.2f) ENDO.spikeCort(0.008f * 60.0f * dt);   /* 卡着出不去：皮质醇慢慢积 */
      {   /* SOMA_DBG=1 时每 2 s 打一行心里的事：卡没卡、想不想走、疼不疼 */
        static const bool dbg = getenv("SOMA_DBG") && getenv("SOMA_DBG")[0] == '1';
        static float acc = 0.0f;
        if (dbg) { acc += dt; if (acc > 2.0f) { acc = 0.0f;
          fprintf(stderr, "[mind] stuck=%.2f vDes=%.2f v=%.2f cort=%.2f pain=%.2f adr=%.2f stress=%.2f tired=%.2f p=(%.2f,%.2f)\n",
                  stuckT, W.vDesWant, W.P.vDes, ENDO.cort, ENDO.pain, ENDO.adr, ENDO.stress, ENDO.tired, W.p.x, W.p.z); } }
      }
      W.bio.dopa = ENDO.dopa; W.bio.cort = ENDO.cort; W.bio.pain = ENDO.pain; W.bio.hunger = ENDO.hunger;
      /* 看见：视野里刚出现人/东西的那一帧，给她一次"注意到"的机会 */
      if (SENS.sees && !SEEN_LAST) { SEE_EDGE = 2.2f; ENDO.spikeDopa(0.22f); }
      else SEE_EDGE = std::max(0.0f, SEE_EDGE - dt);
      SEEN_LAST = SENS.sees;
      if (!GB.pending()) {
        const char* why = nullptr;
        float urg = 0.0f;
        if (W.fallen)           { urg = 1.00f; why = "fallen"; }
        else if (stuckT > 1.2f) { urg = 0.75f; why = "stuck";  }
        /* 身体自己有事要说的时候排在闲逛前面：饿、渴、累，
           这些是慢变量漂过线，不是随机，所以概率给得比 idle 高。 */
        /* 看见人这件事本身就有生理反应：催产素上去一点、多巴胺亮一下、肾上腺素轻抬。
           这不是装饰 —— 后面 vDesWant 和待机微动都读这些值。 */
        else if (SEE_EDGE > 0.0f)    { urg = 1.10f; why = "see";
          /* ★ 只在"刚看见"的那一下给激素，不能每帧给 ——
             60 Hz 每帧 +0.1 的话半秒就顶到 1.0，心率飙到 166，人就一直是"惊着"的状态。
             （这个坑当场踩到：horm 全是 0.9 以上，一眼假。） */
          static bool seePrev = false;
          const bool seeNow = (SENS.sees != 0);
          if (seeNow && !seePrev) { ENDO.spikeOxy(0.12f); ENDO.spikeDopa(0.10f); ENDO.spikeAdr(0.08f); }
          seePrev = seeNow; }
        else if (ENDO.hunger > 0.62f) { urg = 0.22f; why = "hungry";  }
        else if (ENDO.thirst > 0.62f) { urg = 0.26f; why = "thirsty"; }
        else if (ENDO.tired  > 0.78f) { urg = 0.20f; why = "tired";   }
        else if (idleT  > 3.0f) { urg = 0.16f; why = "idle";   }
        else if (GSC.doubt() > 0.45f) { urg = 0.60f; why = "spinal"; }
        if (why && rnd01() < urg * dt * 2.0f) {
          float cv = 0, ca = 0, cl = 0, cw = 0;
          if (BCACHE.get(why, cv, ca, cl, cw)) {   /* 命中：这已经是她的肌肉记忆 */
            /* 命中：她"想起来"上次这个场合该怎么办，不麻烦大脑 */
            AUTO.vDes = cv; AUTO.armRaise = ca; AUTO.lean = cl; AUTO.waveHz = cw; AUTO.has = true;
            printf("{\"cached\":\"%s\",\"hit\":%d}\n", why, BCACHE.nHit);
            fflush(stdout);
            idleT = 0.0f; stuckT = 0.0f;
          } else {
            LAST_WHY = why;
            const int tag = GB.ask();
            printf("{\"brain\":{\"tag\":%d,\"why\":\"%s\"}}\n", tag, why);
            fflush(stdout);
            idleT = 0.0f; stuckT = 0.0f;
          }
        }
      }
    }
    GB.tick(dt);   /* 超时就把牌子摘了，用自己的意图继续跑 */
    /* 褪黑素跟着钟点走：深夜 0.78、清晨 0.10。不是"到点就睡"，而是"夜里本来就困"，
       值本身会被游荡速度、动作幅度读到（见 vDesWant 那里）。 */
    {
      static float melClockT = 0.0f;
      melClockT += dt;
      if (melClockT > 20.0f) {
        melClockT = 0.0f;
        const time_t now = time(nullptr);
        const struct tm* lt = localtime(&now);
        const int h = lt ? lt->tm_hour : 12;
        const float night = (h >= 23 || h < 6) ? 0.78f : (h >= 20 || h < 8 ? 0.45f : 0.08f);
        ENDO.setNight(night);
      }
    }
    /* 每 60 秒把学到的东西落一次盘：她睡着、被关掉、机器重启都不该把这一夜忘掉 */
    CACHE_SAVE_T += dt;
    if (CACHE_SAVE_T > 60.0f) { CACHE_SAVE_T = 0.0f; BCACHE.save("assets/brain_cache.json"); }
    /* 她不是跑步机：走一阵、停一下、站两秒看看周围，再走。
       停站的窗口里人会真的站住（重心得收回到两脚中间），所以别切太短。
       intent 命令或大脑给过速度就听他们的（wanderOff）。 */
    /* 每帧先把转向指令清零：只有这一帧真的有人要她转，才写 vYawWant。
       以前是"谁写谁负责"，没人写就沿用上一帧 —— 于是她走着被限速到 0.45 之后，
       站住了再也没人清，就一直以 0.45 rad/s 原地转圈（用户看到的"站立转圈"）。 */
    W.vYawWant = 0.0f; VYSRC = 0;
    if (BRAIN_HOLD_T > 0.0f) {
      BRAIN_HOLD_T -= dt;
      if (BRAIN_HOLD_T <= 0.0f) { WANDER_OFF = false; wanderT = 0.0f; }   /* 交还给闲逛 */
    }
    if (!WANDER_OFF) {
      /* 骨头里的"想不想动"——不是节拍器。三个不同周期的慢正弦叠起来，
         再乘上身体自己的余力（fat 越高越不想动），所以永远走不成同一个循环。
         这一层是"伪 AI"：全都是矩阵/振荡器算出来的，不经过大模型。 */
      wanderT += dt;
      const float e = 1.0f - clampf(W.bio.fatAvg / 0.7f, 0.0f, 1.0f);   /* 余力 0..1 */
      const float mood = 0.50f * std::sin(wanderT * 0.211f) +
                         0.32f * std::sin(wanderT * 0.083f + 1.7f) +
                         0.18f * std::sin(wanderT * 0.037f + 0.4f);
      const float want = (mood > 0.10f) ? (0.30f + 0.62f * mood) : 0.0f;
      /* 激素真的改她走成什么样：
         · 褪黑素高（该睡了）→ 懒、慢、随时想坐；
         · 肾上腺素高（刚被吓/被推）→ 腿上有劲，走得比平时快；
         · 血清素低（心情压着）→ 步子提不起来。
         没有这三行，"激素"就只是 JSON 里几个好看的数。 */
      const float hormK = (1.0f - 0.45f * ENDO.mel) * (1.0f + 0.30f * ENDO.adr)
                        * (0.75f + 0.35f * ENDO.ser);
      W.vDesWant = want * (0.40f + 0.60f * e) * clampf(hormK, 0.30f, 1.45f);
      /* 想走的时候身子自己会偏一点：弧线，不是直线撞墙 */
      /* 弯一点就够了：曲率乘上当前速度占比，站着不动时不许拐（不然原地拧麻花会摔）。
         0.42 是在大房间里"散步"的曲率；房间只有 2.6 m 宽时，0.44 m/s 配 0.31 rad/s
         的转弯半径只有 1.4 m —— 她就会贴着墙一直绕圈，看着像"只会原地打转"。
         收到 0.16：转弯半径 2.7 m，正常房间里她基本是直着走，偶尔歪一点。 */
      if (want > 0.10f) {
        const float k = clampf(W.vDesWant / 0.6f, 0.0f, 1.0f);
        W.vYawWant = 0.16f * k * std::sin(wanderT * 0.129f + 1.1f); VYSRC = 1;
      }
    }

    /* 重试：上一次被家具挡住的目标，1.6 s 后再来一次 */
    if (!GOTO_ON && GOTO_RETRY_T > 0.0f) {
      GOTO_RETRY_T -= dt;
      if (GOTO_RETRY_T <= 0.0f) {
        GOTO_ON = true; GOTO_PH = 0; GOTO_X = GOTO_RX; GOTO_Z = GOTO_RZ;
        GOTO_HASFACE = GOTO_RHASFACE;
      }
    }
    /* 绕障计时：这 0.9 s 里额外给一点角速度，把"往里推"变成"顺着滑" */
    if (AVOID_T > 0.0f) {
      AVOID_T -= dt;
      if (W.vDesWant > 0.15f) {
        /* 顺着滑：转一点点、同时把速度压下来。转太多+走太快=螺旋，会摔。 */
        W.vYawWant = clampf(W.vYawWant + AVOID_SIGN * 0.55f, -1.2f, 1.2f); VYSRC = 4;
        W.vDesWant = std::min(W.vDesWant, 0.38f);
        if (W.vDesWant < 0.22f) W.vDesWant = 0.22f;
      }
    }
    /* ---- goto：走到一个点，站定，转个身 ---- */
    if (GOTO_ON && !W.fallen) {
      WANDER_OFF = true;
      /* 目标在别的房间：这一帧先奔门，过了门再奔真目标（门在路径上只取最近的一道） */
      float TX = GOTO_X, TZ = GOTO_Z;
      float dw = 0;
      if (GOTO_PH == 0 && NAV.ready) {
        /* 每 0.6 s 重算一次（目标变了就立刻算）：不逐帧跑 A*，手机上不划算 */
        static float lastX = 1e9f, lastZ = 1e9f;
        if (W.t - NAV_T > 0.6f || std::fabs(GOTO_X - lastX) > 1e-3f || std::fabs(GOTO_Z - lastZ) > 1e-3f) {
          NAV_T = W.t; lastX = GOTO_X; lastZ = GOTO_Z;
          float wx = 0, wz = 0;
          if (NAV.next(W.p.x, W.p.z, GOTO_X, GOTO_Z, wx, wz)) { NAV_WX = wx; NAV_WZ = wz; NAV_OK = true; }
          else NAV_OK = false;
        }
        if (NAV_OK) {
          TX = NAV_WX; TZ = NAV_WZ;
          dw = std::sqrt((TX - W.p.x) * (TX - W.p.x) + (TZ - W.p.z) * (TZ - W.p.z));
          /* 路点已经到了就当成"该重算" */
          if (dw < 0.22f) NAV_T = -1e9f;
        }
      }
      const float dx = TX - W.p.x, dz = TZ - W.p.z;
      const float d = std::sqrt(dx * dx + dz * dz);
      /* 到了没 = 离"真正的目标点"多远，不是离当前路点多远。
         以前这里拿 d（到路点的距离）判断，于是她走到第一个拐点就算"到达"，
         直接掉头回去闲逛 —— 客厅到门外走廊那条路就是这样被吃掉的。 */
      const float dGoal = std::sqrt((GOTO_X - W.p.x) * (GOTO_X - W.p.x) + (GOTO_Z - W.p.z) * (GOTO_Z - W.p.z));
      if (GOTO_PH == 0) {
        if (dGoal < 0.32f) {
          /* 到了。给了朝向就转过去站定，没给就直接把身子交还给闲逛 ——
             不然 GOTO_ON 一直挂着，她到了就永远站在那儿等下一个指令。 */
          if (GOTO_HASFACE) GOTO_PH = 1;
          else { GOTO_ON = false; WANDER_OFF = false; W.vDesWant = 0.0f; W.vYawWant = 0.0f; fprintf(stderr, "[goto] off: arrived (no face) at %.2f,%.2f\n", W.p.x, W.p.z); }
        }
        else {
          const float want = std::atan2(dz, dx);
          float err = want - W.yaw;
          while (err > 3.14159265f) err -= 6.28318531f;
          while (err < -3.14159265f) err += 6.28318531f;
          const float ae = std::fabs(err);
          /* 背对目标时先站住转身再走：一边慢慢挪一边拧 1.2 rad/s 只会原地画圈。
             这就是"原地打转"的另一半 —— 目标在背后，她永远绕不完那个圈。 */
          /* 拐弯要按"转弯半径"限速限角速度：捕获点在横向每帧多跑 v·ω·dt，
             实测 0.7 m/s 配 1.5 rad/s 会把她侧着甩出去（capL 1.04 → 判摔）。
             半径不小于 1.1 m，速度和角速度就自动配上了。 */
          const float vTar = ae > 0.55f ? 0.0f : (ae > 0.30f ? ROUTE_V * 0.55f : ROUTE_V);
          const float wMax = ae > 0.55f ? 1.5f : clampf(vTar / 1.1f, 0.35f, 1.5f);
          W.vYawWant = clampf(err * 2.4f, -wMax, wMax); VYSRC = 2;
          W.vDesWant = vTar;
        }
      } else if (GOTO_PH == 1) {
        float err = GOTO_FACE - W.yaw;
        while (err > 3.14159265f) err -= 6.28318531f;
        while (err < -3.14159265f) err += 6.28318531f;
        if (std::fabs(err) < 0.07f) {
          W.vYawWant = 0.0f; W.vDesWant = 0.0f;
          GOTO_ON = false; WANDER_OFF = false;   /* 站定了，把身子还给她自己 */
          fprintf(stderr, "[goto] off: faced+dropped at %.2f,%.2f\n", W.p.x, W.p.z);
        } else { W.vYawWant = clampf(err * 2.0f, -1.2f, 1.2f); W.vDesWant = 0.0f; }
      }
    }
    /* ---- 路线：纯追踪 + 朝向 ----
       目标点 = 当前航点，靠近了就切下一个；越近越早把目光放到下一个，转角才圆。
       朝向误差 → 想要的角速度；角度大就放慢，不然她会一边冲刺一边拐。 */
    if (ROUTE_ON && !W.fallen) {
      const int n = (int)ROUTE.size() / 2;
      if (n >= 3) {
        float tx = ROUTE[ROUTE_I * 2], tz = ROUTE[ROUTE_I * 2 + 1];
        float dx = tx - W.p.x, dz = tz - W.p.z;
        float d = std::sqrt(dx * dx + dz * dz);
        if (d < 0.45f) { ROUTE_I = (ROUTE_I + 1) % n; tx = ROUTE[ROUTE_I * 2]; tz = ROUTE[ROUTE_I * 2 + 1];
                         dx = tx - W.p.x; dz = tz - W.p.z; d = std::sqrt(dx * dx + dz * dz); }
        const int nx = (ROUTE_I + 1) % n;
        const float ax = ROUTE[nx * 2], az = ROUTE[nx * 2 + 1];
        const float look = 0.9f;
        const float wB = clampf((look - d) / look, 0.0f, 1.0f) * 0.85f;
        const float gx = tx + (ax - tx) * wB, gz = tz + (az - tz) * wB;
        const float want = std::atan2(gz - W.p.z, gx - W.p.x);
        float err = want - W.yaw;
        while (err > 3.14159265f) err -= 6.28318531f;
        while (err < -3.14159265f) err += 6.28318531f;
        const float ae = std::fabs(err);
        const float vTar = ae > 0.55f ? (ae > 1.2f ? 0.22f : 0.30f) : (ae > 0.30f ? ROUTE_V * 0.55f : ROUTE_V);
        /* 角速度上限跟着速度走：转弯半径 ≈ v/ω 不许小于 ~0.35 m，否则等于原地拧 */
        const float wMax = clampf(vTar / 1.1f, 0.35f, 1.5f);
        W.vYawWant = clampf(err * 2.4f, -wMax, wMax);
        W.vDesWant = vTar;
        WANDER_OFF = true;
      }
    } else if (ROUTE_ON) {
      W.vYawWant = 0.0f;   /* 路线关了才收角速度；没路线时的「朝屋里拐」不能在这里被清掉 */
    }
    /* 走路时不许猛拧：捕获点的横向速度是 v·ω，0.7 m/s 配 1.0 rad/s 就是 2.17 m/s，
       实测直接甩过 0.45 m 的横阈值判摔。站着可以拧得快（转身），动着只给 0.45。 */
    if (W.vDesWant > 0.30f) { W.vYawWant = clampf(W.vYawWant, -0.45f, 0.45f); VYSRC = 6; }

    /* 兜底：她就是个 46 kg 的姑娘，不该出现 2 m/s 的横向速度。
       动力学在角落里会自己把速度喂大（门口夹墙、绕障 + 转身同时发生），
       这一行是安全阀，不是控制律。 */
    {
      const float sp2 = std::sqrt(W.v.x * W.v.x + W.v.z * W.v.z);
      if (sp2 > 1.5f) { W.v.x *= 1.5f / sp2; W.v.z *= 1.5f / sp2; }
    }
    /* 站着不动的时候别用 1.7 rad/s 拧 —— 转身要慢一点，重心才跟得上 */
    if (W.vDesWant < 0.15f) W.vYawWant = clampf(W.vYawWant, -1.0f, 1.0f);

    /* 别走出屋子。这条必须放在最后：闲逛/goto/路线都会写 vYawWant，
       放在前面等于白写（以前就摆在 wander 前面，于是她一路走出墙外 3 米）。
       屋子是矩形的（web/room.js: x ∈ [-4,4]、z ∈ [-3.25,3.25]），不是半径 1.3 的圆 ——
       以前拿圆当边界，她其实被关在屋里一个 2.6 m 的小圈里，只能在中间打转。 */
    if (!ROUTE_ON && !GOTO_ON && !W.fallen) {
      /* 屋子扩成 3 室 1 厅：客厅 z∈[-3.25,3.25]，走廊 + 三间房一直到 z=8.20。
         边界跟着长，否则她永远走不出客厅。 */
      /* 屋子不是方的：客厅 x∈[-4,4] z∈[-3.25,3.25]，北厢再往北到 9.60。
         所以南边那堵墙在 z=-3.25，北边在 9.60 —— 之前用对称的 |z|>9.30，
         南墙根本没被拦，她一路走到 z=-2.9 贴墙，侧向捕获点 capL 到 -0.69，判摔。 */
      /* +x 不再是 3.55 —— 门外那条走廊把屋子拉到了 x=6.10（她可以推门出去站着）。 */
      const float LIM_XMIN = -3.55f, LIM_XMAX = 6.02f;
      const float LIM_ZMIN = -2.78f, LIM_ZMAX = 9.25f;
      /* 出界回拉带滞回：踩出去 0.08 m 才算越界，回到 0.30 m 以内才算收工。
         没有滞回时，她正好卡在边界上，目标点每帧在"屋里"和"她自己"之间跳，
         方向就左右翻 —— 于是她站着原地拧，越拧越贴墙，永远出不来。 */
      static bool BOUND_ON = false;
      const float overX = std::max(W.p.x - LIM_XMAX, LIM_XMIN - W.p.x);
      const float overZ = std::max(W.p.z - LIM_ZMAX, LIM_ZMIN - W.p.z);
      const float over = std::max(overX, overZ);
      if (!BOUND_ON && over > 0.08f) BOUND_ON = true;
      else if (BOUND_ON && over < -0.30f) BOUND_ON = false;
      if (BOUND_ON) {
        /* 目标点往里收 0.6 m，别只收到边界上（那样她走一步又越界，来回抖） */
        const float tx = clampf(W.p.x, LIM_XMIN + 0.6f, LIM_XMAX - 0.6f);
        const float tz = clampf(W.p.z, LIM_ZMIN + 0.6f, LIM_ZMAX - 0.6f);
        float back = std::atan2(tz - W.p.z, tx - W.p.x) - W.yaw;   /* 朝屋里 */
        while (back > 3.14159265f) back -= 6.28318531f;
        while (back < -3.14159265f) back += 6.28318531f;
        /* 慢慢转（1.2 rad/s 封顶）+ 先站住转身：贴墙时边走边拧会打小圈 */
        W.vYawWant = clampf(back * 1.4f, -0.9f, 0.9f); VYSRC = 5;
        /* 先站住转，别边走边拧：0.5 m/s 配 1.2 rad/s 的侧向速度会到 0.9 m/s，
           侧向捕获点 capL 冲到 0.72 就判摔（实测 1 分钟 8 次）。 */
        W.vDesWant = (std::fabs(back) > 0.35f) ? 0.0f : 0.45f;
      }
    }

    /* 摔了别摊在地上：她自己会爬起来（跟'肌肉把自己拽回来'的起身动画同一条路）。
       摔倒是给人看的，不是给人看的噩梦 —— 体模型摊在地上两秒就该收。 */
    if (W.fallen) {
      fallT += dt;
      if (fallT > 2.2f) { W.getup(); fallT = 0.0f; }
    } else fallT = 0.0f;

    /* 脊髓反射：越不自信，走越慢。这是身体自己的保护，不是大脑的决定。
       速度指令走一个 0.35 s 的一阶平滑：习惯层动不动作都会把 vDes 从 0.9 一步踩到 0
       （或反过来），周期 T 跟着一步跳 0.9→1.3 s，落脚点整段跟着挪 —— 踝和脚趾
       每帧能跳 0.1~0.4 m，看着就是脚在抽。慢半拍地起停，这个跳变就没了。 */
    /* 最后一道阀：她"站着却在转"超过 2.5 s 就掐掉转向，冷却 0.6 s。
       上游每一条我都修过了（清零、绕障看意图、边界滞回），但这台机器上的
       物理是浮点自激系统，永远可能从别的缝里漏出一点 —— 这一层不解释原因，
       只保证"站着转圈"这个现象不会再出现在人眼前。 */
    {
      static float SPIN_T = 0.0f, SPIN_CD = 0.0f;
      static float moveAcc = 0.0f, moveWin = 0.0f, lastX = 1e9f, lastZ = 1e9f;
      static bool STILL_WIN = false;      /* 上一秒是不是"想走但没挪动" */
      static float WEDGE_T = 0.0f;
      if (lastX < 1e8f) moveAcc += std::hypot(W.p.x - lastX, W.p.z - lastZ);
      lastX = W.p.x; lastZ = W.p.z;
      moveWin += dt;
      if (moveWin >= 1.0f) { STILL_WIN = (moveAcc < 0.06f); moveAcc = 0.0f; moveWin = 0.0f; }
      /* 想走却走不动 = 卡住了。卡住时闲逛还在"要 0.5 m/s"，限角就把这个愿望变成
         0.45 rad/s 的原地自转 —— 用户看到的"站着转圈"，八成是这一条。
         一秒内没挪动就让她歇 1.2 s，并换一个闲逛相位（换个方向再试）。 */
      if (W.vDesWant > 0.30f && STILL_WIN) {
        WEDGE_T += dt;
        /* 别一刀切到 0：步态是"想走多快"驱动的，0 会让落脚点瞬间冻住、平衡掉下去
           （实测一刀切之后一分钟多摔三次）。半秒内缓收，并换一个闲逛相位。 */
        if (WEDGE_T > 2.0f) {
          WEDGE_T = 0.0f;
          W.vDesWant *= 0.45f;
          W.vYawWant *= 0.45f;
          wanderT += 1.3f;
        }
      } else WEDGE_T = 0.0f;
      { static float dbgT = 0; static int cnt = 0; if (W.vDesWant > 0.30f && STILL_WIN) { dbgT += dt; if (dbgT > 1.0f && (++cnt % 12) == 1) fprintf(stderr, "[stuck] t=%.1f vDes=%.2f v=(%.2f,%.2f) mode=%d ph=%.2f fallen=%d p=(%.2f,%.2f)\n", W.t, W.vDesWant, W.v.x, W.v.z, -1, W.phase, (int)W.fallen, W.p.x, W.p.z); } else dbgT = 0.0f; }
      if (SPIN_CD > 0.0f) { SPIN_CD -= dt; W.vYawWant = 0.0f; SPIN_T = 0.0f; }
      else if (std::fabs(W.vYawWant) > 0.10f && STILL_WIN) {
        /* "站着"用位移判：原地转身时骨盆速度能有 0.1~0.2 m/s，速度阈值骗人；
           一秒内挪不到 6 cm 就是没在走路。 */
        SPIN_T += dt;
        if (SPIN_T > 2.2f) { SPIN_T = 0.0f; SPIN_CD = 0.5f; W.vYawWant *= 0.25f; }
      } else SPIN_T = 0.0f;
    }
    if (W.intentMode && !W.fallen) {
      static float vCmd = 0.85f;
      const float tv = W.vDesWant * GSC.reflexScale();
      const float kk = (dt > 0.0f) ? std::min(1.0f, (float)dt / 0.35f) : 1.0f;
      vCmd += (tv - vCmd) * kk;
      W.P.vDes = vCmd;
    }
    W.step(dt);
    WBIO = W.bio;
    /* 内脏得每帧跟着代谢走：以前只在 selftest 里 update，运行时永远 58 bpm */
    ORG.update(W.bio.metabTotal, W.bio.bodyMass * W.bio.bmrPerKg, dt, OC);
    /* 心率得跟着"她现在真的在干什么"走，不能由一个有量纲问题的代谢估算决定：
       原来走路时 hr 会顶到 160（= 人在冲刺），一眼就是假的。
       这里用一个生理上站得住的上下界把它夹住，再叠肾上腺素和疼痛。 */
    {
      const float spd = f.speed > 0 ? f.speed : 0.0f;
      const float hrCap = 72.0f + 46.0f * std::min(1.0f, spd / 1.5f) + 40.0f * ENDO.adr + 10.0f * ENDO.pain;
      ORG.hr = std::min(ORG.hr, hrCap);
      ORG.hr = std::fmaxf(ORG.hr, 60.0f + 46.0f * ENDO.adr);   /* 受惊时压不下来 */
      ORG.br = 12.0f + 14.0f * std::min(1.0f, spd / 1.4f) + 6.0f * ENDO.adr;
    }
    W.bio.oxygen = ORG.oxygen;
    /* 手臂：意图(u) + 步态相位 → 模型给偏移 → walker 在 pose 里落位 */
    {
      const float uu[4] = {W.P.vDes / 2.0f, gArmRaise, gLean, gWaveHz / 3.2f};
      /* 相位必须用走路自己积出来的那个（W.phase），不能用 t/cycleT()：
         cycleT 随速度连续变化，而 t 已经几百秒 —— T 抖 0.01 s，t/T 就跳掉大半个周期，
         手会在一帧里瞬移半个身位（实测单帧最大位移 0.47 m = 28 m/s）。 */
      const float armPh = W.phase + gWavePh;
      const float wv[2] = {std::sin(6.2831853f * armPh), std::cos(6.2831853f * armPh)};
      float apx[PosePrior::F]; PosePrior::feats(uu, wv, apx);
      float apy[PosePrior::O]; PP.eval(apx, apy);
      gArmPhDbg = armPh; gArmXDbg = W.P.armOv[0][3]; gArmYDbg = apy[3];
      for (int i = 0; i < PosePrior::O; ++i) if (!std::isfinite(apy[i])) apy[i] = 0.0f;
      for (int k = 0; k < 2; ++k) {
        V3 e{apy[k*6+3], apy[k*6+4], apy[k*6+5]};
        V3 h{apy[k*6+0], apy[k*6+1], apy[k*6+2]};
        const float le = std::sqrt(e.x*e.x + e.y*e.y + e.z*e.z);
        const float lh = std::sqrt(h.x*h.x + h.y*h.y + h.z*h.z);
        /* 长度只许"收"，不许"撑"。以前是无条件归一化到 0.28 / 0.54 —— 手臂永远伸直到底，
           再乘 2.2 的增益，手能甩到离肩 1.19 m 的地方（臂长只有 0.54），于是四肢乱飞。
           现在：模型给多长就用多长，超过臂长才压回来。 */
        if (le > 0.30f) { const float t = 0.30f / le; e.x *= t; e.y *= t; e.z *= t; }
        else if (le < 1e-3f) e = V3{0.f, -0.26f, 0.f};
        h.x *= 1.25f;                    /* 只放大前后摆：真人走路手前后 0.2~0.3 m */
        const float lh2 = std::sqrt(h.x*h.x + h.y*h.y + h.z*h.z);
        if (lh2 > 0.52f) { const float t = 0.52f / lh2; h.x *= t; h.y *= t; h.z *= t; }
        else if (lh2 < 1e-3f) h = V3{0.f, -0.50f, 0.f};
        W.P.armOv[k][0] = e.x; W.P.armOv[k][1] = e.y; W.P.armOv[k][2] = e.z;
        W.P.armOv[k][3] = h.x; W.P.armOv[k][4] = h.y; W.P.armOv[k][5] = h.z;
      }
    }
    /* ---------- 家具：她真的会撞上去，撞上真的疼 ----------
       以前屋子里对她来说只有"四面墙 + 一个矩形边界"，床、桌子、柜子都不存在，
       所以她会整个身子插进床里走过去 —— 用户看到的"穿模"最严重的一类就是这个。
       这份碰撞盒和 web/room.js 摆的是同一套坐标（assets/room.json 导出的），
       不是我在物理层重新编一遍屋子。 */
    decayTouch(dt);
    if (ROOM_OK && !W.fallen) {
      /* 1) 身子（骨盆）先推出去：这样她不会停在家具里面，也不会一直顶着抖 */
      /* 身子不是一个点：桌子只有 0.42 m 高，骨盆在 0.9 m 上根本碰不到它，
         可她的小腿会整条穿过去 —— 所以要按"腿-腰-胸"三个高度各查一次，取最深的那个。 */
      float nx = 0, nz = 0;
      float depth = 0.0f;
      for (float hy : {0.18f, 0.42f, 0.72f, 0.92f}) {
        float ax = 0, az = 0;
        const float d = ROOM.contact(W.p.x, W.p.z, hy, 0.24f, ax, az);
        if (d > depth) { depth = d; nx = ax; nz = az; }
      }
      if (depth > 0.0f) {
        { static int np = 0; if (++np % 200 == 1) fprintf(stderr, "[push] d=%.3f n=(%.2f,%.2f) p=(%.2f,%.2f)\n", depth, nx, nz, W.p.x, W.p.z); }
        /* 一帧最多推开 5 cm：她可能因为体重/姿态瞬间"陷进"家具几十厘米，
           一次推出去会让骨盆速度在 1 帧内到 1.2 m/s（捕获点 capL 0.97 → 判摔，
           实测就是这样一分钟摔十次）。慢慢推，物理层来得及接管。 */
        const float pushD = std::min(depth, 0.05f);
        W.p.x += nx * pushD; W.p.z += nz * pushD;
        const float vn = W.v.x * nx + W.v.z * nz;
        if (vn < 0.0f) { W.v.x -= vn * nx; W.v.z -= vn * nz; }
        /* 撞一下会把速度削掉大半：只削法向分量不够 ——
           门口那种两堵墙夹出来的角，两个法向交替推，动力学里会把速度越积越大
           （实测 v 冲到 2.4 m/s，捕获点横飞 1.93 m，直接判摔）。 */
        W.v.x *= 0.55f; W.v.z *= 0.55f;
        {
          const float sp2 = std::sqrt(W.v.x * W.v.x + W.v.z * W.v.z);
          if (sp2 > 1.6f) { W.v.x *= 1.6f / sp2; W.v.z *= 1.6f / sp2; }
        }
        const float impact = std::max(0.0f, -vn);          /* 撞上去的法向速度 */
        /* 撞得越快越疼；轻轻蹭一下只是"被碰到"，不该有惩罚 */
        /* 撞一次疼一下，但贴着家具磨的时候每帧都算"撞"，会把疼和肾上腺素顶到满格
           （实测站着不动 pain 0.55 / adr 0.71，脸上一直挂着怒色）。加 0.9 s 冷却。 */
        static float PAIN_CD = 0.0f;
        PAIN_CD = std::max(0.0f, PAIN_CD - dt);
        if (impact > 0.30f && PAIN_CD <= 0.0f) {
          PAIN_CD = 2.5f;
          ENDO.spikePain(std::min(0.22f, 0.04f + 0.34f * (impact - 0.12f)));
          ENDO.spikeCort(0.025f * (impact > 0.5f ? 1.0f : 0.4f));
          ENDO.spikeAdr(0.12f * std::min(1.0f, impact));
        }
        pushTouch("waist", 6.0f * impact + 12.0f * depth, 0.5f);
        /* 绕障：撞到东西就顺着它滑，而不是一直往里推。
           方向取"离目标更近的那一侧"——叉乘判左右，不用再问目标在哪。 */
        {
          const float tx0 = (GOTO_ON ? GOTO_X : W.p.x + 1.0f) - W.p.x;
          const float tz0 = (GOTO_ON ? GOTO_Z : W.p.z + 1.0f) - W.p.z;
          const float cr = nx * tz0 - nz * tx0;      /* >0：目标在法线左侧 */
          /* 只有"她本来想走"的时候才绕：站着贴在家具上（比如站在书桌边）
             每帧都算一次接触，如果照样武装绕障，0.9 s 一到再一接触，就无限接力 ——
             表现出来就是"站立时原地转圈"。这是本轮的第二个转圈源。 */
          /* 只看"想不想走"，不看当前速度：原地转身时脚也在动，速度>0.12 会再次武装绕障，
             0.85 rad/s 转 → 更贴家具 → 再武装 …… 自激成正反馈，这就是站立转圈。 */
          /* 只有"真的撞上去"才绕：贴着家具蹭过去（impact≈0）不算。
             以前只要接触就武装，结果她在门口以 0.9 m/s 一边走一边被转 0.45 rad/s，
             轨迹打成螺旋、捕获点横飞 → 同一个角落反复摔。 */
          const bool moving = (W.vDesWant > 0.15f) && (impact > 0.10f);
          if (moving && AVOID_T <= 0.0f) { AVOID_SIGN = (cr >= 0.0f) ? 1.0f : -1.0f; AVOID_T = 0.6f; }
        }
        { static int nb = 0; if (++nb % 120 == 1) fprintf(stderr, "[bump] depth=%.3f vn=%.2f at (%.2f,%.2f)\n", depth, impact, W.p.x, W.p.z); }
        /* 顶着家具走超过半秒 = 这个目标点根本去不了：把 goto 放掉，让她自己转身走开。
           不这么做她会一直朝柜子推、被推出来、再推回去（"贴着家具原地磨"）。 */
        BUMP_T += dt;
        /* 门槛从 1.2 s 放到 2.6 s，并且要"真的没往前走"才放掉：
           从客厅穿门去走廊，第一段贴着茶几蹭过去要一秒多 —— 以前会在门口
           把 goto 丢掉、变成闲逛，看起来像"她不想出门"。 */
        const float dNow = std::sqrt((GOTO_X - W.p.x) * (GOTO_X - W.p.x) + (GOTO_Z - W.p.z) * (GOTO_Z - W.p.z));
        static float dBest = 1e9f;
        if (dNow < dBest - 0.12f) { dBest = dNow; BUMP_T = 0.0f; }   /* 有推进就不算卡住 */
        if (!GOTO_ON) dBest = 1e9f;
        if (BUMP_T > 2.6f) {
          /* 顶着家具走太久 = 这一步过不去。放开 goto 让她自己挪两步再试 ——
             直接放弃的话，沙发角这种小东西就能让她永远到不了门。 */
          if (GOTO_ON) {
            GOTO_RETRY_T = 1.6f;
            GOTO_RX = GOTO_X; GOTO_RZ = GOTO_Z;      /* 记住原目标：重试回到它，不是回 (0,0) */
            GOTO_RHASFACE = GOTO_HASFACE;
            fprintf(stderr, "[bump] 目标点被家具挡住，放掉 goto，1.6 s 后重试\n");
            GOTO_ON = false; GOTO_PH = 0;
          }
          WANDER_OFF = false;
          BUMP_T = 0.0f;
        }

      }
      /* 2) 末端（手/膝/肩/头）再逐点查：抬手会戳进柜子、蹲下会穿进床沿。
            每条触觉点就是"固定方位 + 接触半径 + 末梢密度 + 敏感度"，
            见 room.h 的 TOUCH_SITES —— 只写真有神经末梢的地方，不逐部位铺。 */
      for (int i = 0; i < N_TOUCH; ++i) {
        const TouchSite& t = TOUCH_SITES[i];
        const V3& nd = f.nodes[t.node];
        const float px = nd.x + t.ox, py = nd.y + t.oy, pz = nd.z + t.oz;
        float sx = 0, sz = 0;
        const float dep = ROOM.contact(px, pz, py, t.r, sx, sz);
        if (dep > 0.0f) {
          const float force = dep * 60.0f * t.sens;      /* 压进去越深，力越大 */
          pushTouch(t.name, force, t.sens);
          /* 手/肩贴着墙走的时候，每一帧都"碰到" —— 按帧给疼会一秒顶到 1.0
             （实测走路时 pain=1.00、脸上永远是那副忍着疼的样子）。
             疼是"事件"，不是"状态"：同一个部位 1.2 s 内只疼一次，且单次幅度小。 */
          static float PAIN_CD2[16] = {0};
          PAIN_CD2[i] = std::max(0.0f, PAIN_CD2[i] - dt);
          if (force > 2.2f && py > 0.25f && PAIN_CD2[i] <= 0.0f) {
            PAIN_CD2[i] = 1.2f;
            ENDO.spikePain(std::min(0.05f, 0.010f * force * t.sens));
            ENDO.spikeCort(0.0015f * force);
          }
        }
      }
    }
    W.pose(f);                 /* 这一行之前被误删过：没有它 f 永远是初始帧（t=0） */
    /* 感知每帧跑一次：看到/听到玩家就把头转过去，靠得太近就吃推力 —— 以前只在 selftest 里跑过 */
    SENS.update(f, PLR, SC, dt);
    if (std::fabs(SENS.headYaw) > 1e-4f) {
      const V3 nk = f.nodes[2], hd = f.nodes[3];
      const float dxh = hd.x - nk.x, dzh = hd.z - nk.z;
      const float cy = std::cos(SENS.headYaw), sy = std::sin(SENS.headYaw);
      f.nodes[3].x = nk.x + dxh * cy - dzh * sy;
      f.nodes[3].z = nk.z + dxh * sy + dzh * cy;
    }
    GSC.step(f);
    if (GSC.ticks - GSC.savedAt > 6000) {   /* 每 ~100 秒写一次学到的矩阵 */
      if (GSC.save("assets/spinal_learned.json")) GSC.savedAt = GSC.ticks;
    }
    simTime += dt;
    DBGX[0] = W.standing ? 1.0f : 0.0f;
    DBGX[1] = W.catchT;
    DBGX[2] = (float)GOTO_PH + (ROUTE_ON ? 10.0f : 0.0f) + (GOTO_ON ? 20.0f : 0.0f);
    DBGX[3] = W.vYawWant;   /* 相位/手臂调试早就够用了，这两格现在盯"谁在下转向指令" */
    { static int sd = 0; if (++sd % 60 == 1) fprintf(stderr, "[steer] goto=%d ph=%d route=%d wanderOff=%d vDes=%.2f vYawWant=%.2f p=(%.2f,%.2f)\n",
        (int)GOTO_ON, GOTO_PH, (int)ROUTE_ON, (int)WANDER_OFF, W.vDesWant, W.vYawWant, W.p.x, W.p.z);
      fprintf(stderr, "  vy src=%d\n", VYSRC);
      if (GOTO_ON) fprintf(stderr, "[steer2] tgt=(%.2f,%.2f) d=%.2f yaw=%.2f\n", GOTO_X, GOTO_Z,
          std::sqrt((GOTO_X-W.p.x)*(GOTO_X-W.p.x)+(GOTO_Z-W.p.z)*(GOTO_Z-W.p.z)), W.yaw); }
    if (++frameCount % 2 == 0) {
      /* 大脑写目标，矩阵负责过程：每帧只朝目标挪一点点（限速 3/s）。
         所以"走快点/抬点手"不会让身体瞬移，也不会把脊髓的预测打乱。 */
      if (AUTO.has) {
        const float k = std::min(1.0f, (float)dt * 3.0f);
        gArmRaise  += (AUTO.armRaise - gArmRaise) * k;
        gLean      += (AUTO.lean - gLean) * k;
        gWaveHz    += (AUTO.waveHz - gWaveHz) * k;
        W.vDesWant += (AUTO.vDes - W.vDesWant) * k;
        if (W.vDesWant < 0) W.vDesWant = 0;
        if (W.vDesWant > 2.0f) W.vDesWant = 2.0f;
        W.P.vDes = W.vDesWant;
        /* 注意：这里**不能**再写 WANDER_OFF / BRAIN_HOLD_T。
           AUTO.has 一旦为真就永不清零，这两行会被每帧重设一次 ——
           接管时间永远停在 8 秒，wander 永远回不来，她就再也没停过、
           也就再也没问过第二次。接管计时只在收到新 answer 那一刻起算。 */
      }
      {
        const float uu[MatrixSpinal::U] = {W.P.vDes / 2.0f, gArmRaise, gLean, gWaveHz / 3.2f};
        GSC.setIntent(uu);
      }
      emit(f); fflush(stdout);
    }
    /* 追墙钟，最多补 4 帧，别追过头 */
    auto now = std::chrono::steady_clock::now();
    double wall = std::chrono::duration<double>(now - t0).count();
    if (simTime > wall + 0.004) {
      double nap = simTime - wall;
      /* 以前这里是 "落后太多就把 simTime 清零"，结果时钟一清零就再也没长回去，
         所有帧的 t 永远是 0（看起来像仿真死了）。现在只把这一觉睡短一点。 */
      if (nap > 0.05) nap = 0.05;
      std::this_thread::sleep_for(std::chrono::microseconds((long)(nap * 1e6)));
    }
  }
  return 0;
}

/* --navtest：只验寻路，不跑物理、不进 60 Hz —— 出问题一眼看到是哪一格。 */
/* 调试：打印从 (x,z) 到 (tx,tz) 的路点序列 —— 走不通的时候看这个 */
int navpath(float sx, float sz, float tx, float tz) {
  ROOM_OK = ROOM.load("assets/room.json") || ROOM.load("../assets/room.json");
  if (!ROOM_OK) { printf("nav: 没有碰撞盒\n"); return 1; }
  NAV.build(ROOM, 0.26f);
  float px = sx, pz = sz;
  printf("grid %dx%d\n", NAV.nx, NAV.nz);
  for (int i = 0; i < 40; ++i) {
    float wx = 0, wz = 0;
    if (!NAV.next(px, pz, tx, tz, wx, wz)) { printf("  no path (from %.2f,%.2f)\n", px, pz); return 2; }
    printf("  %2d  (%.2f,%.2f) -> (%.2f,%.2f)  free=%d\n", i, px, pz, wx, wz, (int)NAV.freeAt(wx, wz));
    px = wx; pz = wz;
    if (std::sqrt((px-tx)*(px-tx)+(pz-tz)*(pz-tz)) < 0.32f) { printf("  arrived\n"); return 0; }
  }
  return 0;
}

int navtest() {
  ROOM_OK = ROOM.load("assets/room.json") || ROOM.load("../assets/room.json");
  if (!ROOM_OK) { printf("nav: 没有碰撞盒\n"); return 1; }
  NAV.build(ROOM, 0.26f);
  printf("grid %dx%d cell %.2f\n", NAV.nx, NAV.nz, NAV.cell);
  struct Case { float x, z, tx, tz; const char* name; };
  const Case cs[] = {
    {0.0f, 0.0f, 0.0f, 6.5f, "客厅中心 -> 走廊深处"},
    {2.4f, -2.2f, 0.0f, 6.5f, "客厅东北 -> 走廊深处"},
    {1.2f, 0.2f, 0.0f, 6.5f, "客厅中部 -> 走廊深处"},
    {0.0f, 6.5f, -2.20f, 4.20f, "走廊 -> 卧 A"},
    {0.0f, 6.5f, 2.2f, 8.2f, "走廊 -> 卫浴"},
    {-2.20f, 4.20f, 1.80f, 4.60f, "卧 A -> 厨房"},
    {-1.80f, 7.70f, 2.2f, 8.2f, "卧 B -> 卫浴"},
    {2.00f, 0.90f, 5.60f, 0.90f, "客厅 -> 门外走廊"},
    {5.60f, 0.90f, 0.30f, 0.15f, "门外走廊 -> 客厅"},
  };
  printf("freeAt(0,0)=%d freeAt(0,3.0)=%d freeAt(0,6.5)=%d ix(0)=%d iz(0)=%d\n",
         (int)NAV.freeAt(0,0), (int)NAV.freeAt(0,3.0f), (int)NAV.freeAt(0,6.5f), NAV.ix(0), NAV.iz(0));
  int bad = 0;
  for (const auto& c : cs) {
    float px = c.x, pz = c.z;
    int steps = 0, ok = 0;
    while (steps < 400) {
      float wx = 0, wz = 0;
      if (!NAV.next(px, pz, c.tx, c.tz, wx, wz)) break;
      if (!NAV.freeAt(wx, wz)) { printf("  !! 路点落在障碍里 (%.2f,%.2f)\n", wx, wz); break; }
      px = wx; pz = wz; ++steps;
      if (std::sqrt((px - c.tx) * (px - c.tx) + (pz - c.tz) * (pz - c.tz)) < 0.32f) { ok = 1; break; }
    }
    const float dd = std::sqrt((px - c.tx) * (px - c.tx) + (pz - c.tz) * (pz - c.tz));
    printf("%s %-24s 步 %3d  终点 (%.2f,%.2f) 差 %.2f\n", ok ? "OK  " : "FAIL", c.name, steps, px, pz, dd);
    if (!ok) ++bad;
  }
  printf("%s（%d 条失败）\n", bad ? "有路走不通" : "全通", bad);
  return bad ? 1 : 0;
}

int selftest() {
  /* 1. 站立 */
  {
    Walker W; W.setMode(0);
    Frame f; runSim(W, 6, &f);
    printf("--- 站立 6s: "); printFrame(f);
    check("站立不摔", f.falls == 0 && !W.fallen, "falls=%g drift=%.4f", f.falls, std::fabs(W.p.x));
    check("站立不漂", std::fabs(W.p.x) < 0.02 && std::fabs(W.p.z) < 0.02, "x=%.4f z=%.4f", W.p.x, W.p.z);
  }
  /* 2. 走 */
  {
    Walker W; W.setMode(1); W.P.vDes = 0.85f;
    Frame f; runSim(W, 10, &f);
    printf("--- 走路 10s: "); printFrame(f);
    float spd = W.p.x / 10.0f;
    check("走路不摔", f.falls == 0, "falls=%g steps=%.0f", f.falls, 0.0);
    check("走路速度达标", std::fabs(spd - 0.85f) < 0.25f, "实际 %.3f / 目标 %.2f", spd, 0.85);
    check("走路侧摆不过分", std::fabs(f.capZ) < 0.35f, "capZ=%.3f 限 %.2f", f.capZ, 0.35);
    check("骨长自洽", f.err < 0.02f, "err=%.4f 限 %.2f", f.err, 0.02);
  }
  /* 3. 跑 */
  {
    Walker W; W.setMode(2); W.P.vDes = 1.9f; W.P.Tstep = 0.34f;
    Frame f; runSim(W, 8, &f);
    printf("--- 跑 8s: "); printFrame(f);
    check("跑步不摔", f.falls == 0, "falls=%g v=%.2f", f.falls, W.v.x);
  }
  /* 4. 侧推 */
  {
    Walker W; W.setMode(1); W.P.vDes = 0.85f;
    Frame f; runSim(W, 3, &f);
    Ward:;
    W.push(0, 0, 0.9f);           // 侧向 0.9 m/s 冲击
    float maxZ = 0;
    for (int i = 0; i < 240 * 6; ++i) { W.step(1.0f / 240.0f); maxZ = std::max(maxZ, std::fabs(W.p.z)); }
    W.pose(f);
    printf("--- 侧推 0.9m/s 后 6s: "); printFrame(f);
    check("侧推后站住", f.falls == 0, "falls=%g 侧移峰值 %.3f", f.falls, maxZ);
    check("侧推后回中", std::fabs(W.p.z) < 0.25f, "z=%.3f 限 %.2f", W.p.z, 0.25);
  }
  /* 5. 前推 */
  {
    Walker W; W.setMode(1);
    Frame f; runSim(W, 3, &f);
    W.push(0.8f, 0, 0);
    for (int i = 0; i < 240 * 5; ++i) W.step(1.0f / 240.0f);
    W.pose(f);
    printf("--- 前推 0.8m/s 后 5s: "); printFrame(f);
    check("前推后不摔", f.falls == 0, "falls=%g", f.falls, 0);
  }
  /* 6. 确定性 */
  {
    Walker A, B; A.setMode(1); B.setMode(1);
    for (int i = 0; i < 2400; ++i) { A.step(1.0f / 240.0f); B.step(1.0f / 240.0f); }
    check("确定性", A.p.x == B.p.x && A.p.z == B.p.z, "x=%.6f", A.p.x, B.p.x);
  }
  /* 7. 脚不入地 */
  {
    Walker W; W.setMode(1);
    float lowest = 1e9f;
    for (int i = 0; i < 240 * 8; ++i) {
      W.step(1.0f / 240.0f);
      for (int k = 0; k < 2; ++k) lowest = std::min(lowest, W.feet[k].ank.y);
    }
    check("脚不乱入地", lowest > W.P.ankleH - 0.004f, "最低踝高 %.4f / 标准 %.3f", lowest, W.P.ankleH);
  }
  /* 7. 速度扫描：0.4~1.9 m/s 全程不摔、骨长不崩 */
  {
    int bad = 0;
    for (float v : {0.4f, 0.6f, 0.85f, 1.1f, 1.3f, 1.6f, 1.9f}) {
      Walker W; W.setMode(v > 1.2f ? 2 : 1); W.P.vWalk = v; W.P.vRun = v; W.P.vDes = v;
      Frame f; double me = 0; float xMid = 0;
      for (int i = 0; i < 60 * 12; ++i) {
        W.step(1.0f / 60.0f);
        if (i == 60 * 6) xMid = W.p.x;          /* 起步加速段不算 */
        if (i % 6 == 0) { W.pose(f); me = std::max(me, (double)f.err); }
      }
      float got = (W.p.x - xMid) / 6.0f;
      if (W.falls || me > 0.02 || std::fabs(got - v) > 0.12f) { bad++; printf("  速度 %.2f: 实得 %.2f err %.4f falls %d\n", v, got, me, W.falls); }
    }
    check("0.4~1.9 m/s 全程稳定", bad == 0, "bad=%g", (double)bad, 0);
  }
  /* 8. 体模型：质量 / 摔倒 / 躺平 / 骨长守恒 / 起身 */
  {
    float m[18]; Ragdoll::masses(m);
    double sum = 0; for (int i = 0; i < 18; ++i) sum += m[i];
    check("总质量 70kg", std::fabs(sum - 70.0) < 0.01, "%.3f kg", sum, 70.0);

    Walker W; W.setMode(1);
    Frame f; runSim(W, 2, &f);
    float comStand = f.comY;
    check("站立质心高度", comStand > 0.80f && comStand < 1.05f, "%.3f m", comStand, 0.95);

    W.push(0, 0, 3.0f);
    int i = 0;
    for (; i < 240 * 10 && !W.fallen; ++i) W.step(1.0f / 240.0f);
    for (; i < 240 * 10; ++i) W.step(1.0f / 240.0f);   /* 摔了之后再滚 4~5 s */
    W.pose(f);
    printf("--- 摔倒: fallen=%d 质心 %.3f m 骨长 %.4f settled=%d falls=%d\n",
           (int)W.fallen, f.comY, f.boneErrRagdoll, (int)W.rd.settled(), f.falls);
    check("被推倒", W.fallen == true, "fallen=%g", (double)W.fallen, 1);
    check("躺平(质心掉到 0.6 以下)", f.comY < 0.60f, "%.3f m", f.comY, 0.60);
    float low = 1e9f;
    for (int k = 0; k < 18; ++k) low = std::min(low, f.nodes[k].y - f.radii[k]);
    check("身体不穿地", low > -0.02f, "最低 %.4f m", low, 0);
    check("骨长守恒(体模型)", f.boneErrRagdoll < 0.02f, "%.4f", f.boneErrRagdoll, 0.02);
    check("摔完静止", W.rd.settled(), "settled=%g", (double)W.rd.settled(), 1);
    {   /* 自碰撞：手不插进胸腔、膝盖不穿对侧腿 */
      static const int PR[6][2] = {{5,1},{8,1},{4,1},{7,1},{11,15},{10,14}};
      float worst = 9;
      for (int k = 0; k < 6; ++k) {
        int a = PR[k][0], b = PR[k][1];
        float d = len(W.rd.x[a] - W.rd.x[b]);
        worst = std::min(worst, d / std::max(1e-4f, W.rd.r[a] + W.rd.r[b]));
      }
      check("自碰撞不穿模", worst > 0.85f, "最紧 %.2f×半径和", worst, 0.94);
    }

    W.setMode(1);                     /* 躺着按走路 = 起来 */
    Frame g; runSim(W, 2, &g);
    check("起身后恢复站立", !W.fallen && g.comY > 0.80f, "comY=%.3f", g.comY, 0.95);

    /* 确定性：同一次摔倒跑两遍必须逐位相同 */
    Walker A, B; A.setMode(1); B.setMode(1);
    for (int k = 0; k < 240 * 2; ++k) { A.step(1.0f / 240.0f); B.step(1.0f / 240.0f); }
    A.push(0, 0, 3.0f); B.push(0, 0, 3.0f);
    for (int k = 0; k < 240 * 6; ++k) { A.step(1.0f / 240.0f); B.step(1.0f / 240.0f); }
    bool same = true;
    for (int k = 0; k < 18; ++k) if (A.rd.x[k].y != B.rd.x[k].y || A.rd.x[k].z != B.rd.x[k].z) same = false;
    check("摔倒确定性", same, "ok=%g", (double)same, 1);
    /* 地面摩擦：同样的推，冰面上躺着的身体会滑出去，水泥地几乎不动 */
    auto slideOn = [&](float mu) {
      Walker W; W.floorMu = mu; W.setMode(1);
      for (int i = 0; i < 240 * 3; ++i) W.step(1.0f / 240.0f);
      /* 推得足够狠，保证它是"躺下去滑"而不是"迈一步救回来" */
      W.push(0, 0, 6.0f);
      for (int i = 0; i < 240 * 6; ++i) W.step(1.0f / 240.0f);
      float z0 = W.p.z;
      for (int i = 0; i < 18; ++i) W.rd.xp[i].z -= 0.40f / 240.0f;   /* 横向初速 ~0.4 m/s（xp 是上一帧位置，差一帧就是速度） */
      for (int i = 0; i < 240 * 6; ++i) W.step(1.0f / 240.0f);
      return W.p.z - z0;
    };
    /* 摩擦直接量约束本身：让一个贴地球体"这一步想滑 10 cm"，看 solveGround 留下多少。
       为什么不用"推倒后滑多远"来量：那个数被翻滚、摔姿、借步一起搅和，
       高摩擦时身体绕着支点翻，位移反而更大，测出来的东西跟摩擦没关系。 */
    auto slideStep = [](float mu) {
      Walker W; W.setMode(0);
      Frame f; W.pose(f);                       /* pose 之后才有真实节点与半径 */
      W.rd.init(f.nodes, f.radii, 1.0f / 240.0f);
      const int i = 0;
      float gy = W.rd.ground ? W.rd.ground(0.0f, 0.0f) : 0.0f;
      W.rd.x[i] = V3{0.0f, gy - 0.001f, 0.0f};   /* 强行压进地面，保证进入接触分支 */
      W.rd.xp[i] = W.rd.x[i];
      W.rd.x[i].z = 0.10f;                      /* 本步想滑 10 cm */
      W.rd.solveGround(mu);
      return W.rd.x[i].z;
    };
    float slipIce = slideStep(0.05f), slipConc = slideStep(0.9f);
    printf("--- 摩擦: 冰面留 %.3f m / 水泥留 %.3f m (都从 0.10 m 起)\n", slipIce, slipConc);
    check("冰面上会打滑", slipIce > 0.085f, "%.3f m", (double)slipIce, 0.085);
    check("水泥地上不滑", slipConc < 0.025f, "%.3f m", (double)slipConc, 0.025);
  }

  /* 9. 地形：台阶 / 墙 / 在台子上摔倒 */
  {
    for (float h : {0.08f, 0.18f}) {
      Walker W; W.setMode(1);
      W.boxes.push_back({1.0f, 9.0f, -1.5f, 1.5f, h});
      float me = 0, low = 9;   /* low = 摆动脚离它正下方地面的最小净高 */
      for (int i = 0; i < 240 * 8; ++i) {
        W.step(1.0f / 240.0f);
        if (i % 8 == 0) { Frame f; W.pose(f); me = std::max(me, f.err); }
        for (int k = 0; k < 2; ++k) {
          if (!W.feet[k].swing) continue;
          const V3& a = W.feet[k].ank;
          low = std::min(low, a.y - W.groundAt(a.x, a.z));
        }
      }
      printf("--- 台阶 %.2f m: x=%.2f 骨盆高 %.3f err=%.4f falls=%d 最低踝 %.3f\n",
             h, W.p.x, W.p.y, me, W.falls, low);
      char name[64]; snprintf(name, sizeof name, "上 %.2f m 台阶", h);
      check(name, W.p.x > 2.0f && W.falls == 0 && me < 0.02f, "x=%.2f err=%.4f", (double)W.p.x, (double)me);
      /* 摆动脚不能从台阶侧面穿过去：每一步都检查脚底离脚下地面的距离 */
      char nm2[64]; snprintf(nm2, sizeof nm2, "摆动脚不穿 %.2f m 台阶", h);
      /* 正常摆动抬脚净高 ≈ ankleH(0.08) + lift，穿模的话会掉到 0.03 以下 */
      check(nm2, low > 0.035f, "摆动净高 %.3f m", (double)low, 0.035);
      check("骨盆抬到台阶高度", std::fabs(W.p.y - (P_HIP() + h)) < 0.06f, "y=%.3f 期望 %.3f", W.p.y, P_HIP() + h);
    }
    {   /* 墙：走得过去就说明没挡 */
      Walker W; W.setMode(1);
      W.boxes.push_back({2.0f, 2.3f, -1.5f, 1.5f, 1.6f});
      for (int i = 0; i < 240 * 8; ++i) W.step(1.0f / 240.0f);
      printf("--- 墙: x=%.2f vx=%.2f falls=%d\n", W.p.x, W.v.x, W.falls);
      check("被墙挡住", W.p.x < 2.05f && W.falls == 0, "x=%.3f", W.p.x, 2.05);
    }
    {   /* 在 0.18 m 台子上推倒：身体得躺在台面上，不能掉到地面 */
      Walker W; W.setMode(1);
      W.boxes.push_back({0.0f, 40.0f, -1.5f, 1.5f, 0.18f});
      for (int i = 0; i < 240 * 3; ++i) W.step(1.0f / 240.0f);
      W.push(0, 0, 3.0f);
      for (int i = 0; i < 240 * 8; ++i) W.step(1.0f / 240.0f);
      Frame f; W.pose(f);
      printf("--- 台上摔倒: fallen=%d comY=%.3f\n", (int)W.fallen, f.comY);
      check("台子上躺平", W.fallen && f.comY > 0.18f && f.comY < 0.45f, "comY=%.3f", f.comY, 0.3);
    }
  }

  /* 10. 地面反作用力（GRF） */
  {
    const float mg = 70.0f * 9.81f;
    { Walker W; W.setMode(0); for (int i = 0; i < 240 * 3; ++i) W.step(1.0f / 240.0f);
      Frame f; W.pose(f);
      float fz = f.grfL[1] + f.grfR[1];
      printf("--- GRF 站立: Fz=%.1f N (体重 %.1f) Fx=%.1f\n", fz, mg, f.grfL[0] + f.grfR[0]);
      check("站着 Fz = 体重", std::fabs(fz - mg) < mg * 0.01f, "Fz=%.1f / %.1f", fz, mg); }
    { Walker W; W.setMode(1);
      for (int i = 0; i < 240 * 3; ++i) W.step(1.0f / 240.0f);   /* 先过起步瞬态 */
      float sum = 0, mx = 0, frMax = 0; int n = 0;
      for (int i = 0; i < 240 * 4; ++i) {
        W.step(1.0f / 240.0f); Frame f; W.pose(f);
        float fz = f.grfL[1] + f.grfR[1];
        sum += fz; mx = std::max(mx, fz); frMax = std::max(frMax, f.frictionUse); n++;
      }
      printf("--- GRF 走路稳态: 平均 %.1f (%.2f 体重) 峰 %.1f (%.2f 体重) 摩擦峰 %.2f\n",
             sum / n, sum / n / mg, mx, mx / mg, frMax);
      check("走起来平均 Fz ≈ 体重", std::fabs(sum / n - mg) < mg * 0.05f, "%.1f / %.1f", sum / n, mg);
      check("Fz 峰值 1.0~1.4 体重", mx > mg && mx < mg * 1.4f, "%.2f 体重", mx / mg, 1.2);
      check("不打滑（摩擦利用率 < 0.9）", frMax < 0.9f, "%.2f", frMax, 0.9);
    }
    { Walker W; W.setMode(2); W.P.vDes = 1.9f;
      int air = 0;
      for (int i = 0; i < 240 * 6; ++i) { W.step(1.0f / 240.0f); Frame f; W.pose(f);
        if (f.grfL[1] + f.grfR[1] < 1.0f) air++; }
      printf("--- GRF 跑步: 腾空帧 %d / %d\n", air, 240 * 6);
      check("跑步有腾空相（反作用力=0）", air > 0, "air=%.0f", (double)air, 1);
    }
  }

  /* 11. 起身：躺着的体模型靠"肌肉"把自己拽起来 */
  {
    const float dt = 1.0f / 240.0f;
    Walker W; W.setMode(1);
    for (int i = 0; i < 240 * 2; ++i) W.step(dt);
    W.push(0, 0, 3.0f);
    for (int i = 0; i < 240 * 5; ++i) W.step(dt);
    float lowY = W.comY;
    int f0 = W.falls;
    W.getup();
    for (int i = 0; i < 240 * 3; ++i) W.step(dt);
    printf("--- 起身: 躺 %.3f → 站 %.3f (fallen=%d)\n", lowY, W.comY, (int)W.fallen);
    check("推倒后质心贴地", lowY < 0.30f, "comY=%.3f", lowY, 0.1);
    check("起身站回来", !W.fallen && W.comY > 0.82f && W.comY < 1.05f, "comY=%.3f", W.comY, 0.9);
    check("起身不算新摔", W.falls == f0, "falls %.0f→%.0f", (double)f0, (double)W.falls);
    float x0 = W.p.x;
    for (int i = 0; i < 240 * 3; ++i) W.step(dt);
    printf("--- 起身后继续走: x %.2f → %.2f\n", x0, W.p.x);
    check("起身后还能走", W.p.x - x0 > 0.8f && W.falls == f0, "dx=%.2f", W.p.x - x0, 0.8);

    /* 确定性：同样的输入必须落到同样的姿态 */
    Walker A, B;
    for (Walker* w : {&A, &B}) {
      w->setMode(1);
      for (int i = 0; i < 240 * 2; ++i) w->step(dt);
      w->push(0, 0, 3.0f);
      for (int i = 0; i < 240 * 5; ++i) w->step(dt);
      w->getup();
      for (int i = 0; i < 240 * 3; ++i) w->step(dt);
    }
    check("起身确定性", std::fabs(A.comY - B.comY) < 1e-4f && std::fabs(A.p.x - B.p.x) < 1e-4f,
          "dy=%.6f", (double)std::fabs(A.comY - B.comY), 0);
  }

  /* ================= §12 生物层：肌肉 → 代谢 → 疲劳 → 体温 ================= */
  printf("\n== §12 生物层（Hill 肌肉 / 代谢 / 疲劳 / 体温） ==\n");
  {
    const float dt = 1.0f / 240.0f;

    /* 12.1 静息代谢：70 kg × 1.2 W/kg = 84 W */
    Walker R; R.bio.bodyMass = R.totalMass;
    float zt[6] = {0,0,0,0,0,0}, zw[6] = {0,0,0,0,0,0};
    for (int i = 0; i < 240 * 5; ++i) { R.bio.step(dt, zt, zw); }
    check("静息代谢 84W", std::fabs(R.bio.metabRest - 84.0f) < 1.0f, "metabRest=%.1f", R.bio.metabRest, 0);

    /* 12.2 站立 / 走路 / 跑步的代谢阶梯 */
    /* 代谢是逐帧波动的（摆动相/双支撑差很多），要取一段时间的平均，不能采一帧 */
    auto avgMetab = [&](Walker& W, float secs) {
      for (int i = 0; i < 240 * 10; ++i) W.step(dt);          /* 先热身 10 s */
      double acc = 0; int n = 0;
      for (int i = 0; i < (int)(240 * secs); ++i) { W.step(dt); acc += W.bio.metabTotal; n++; }
      return (float)(acc / n);
    };
    Walker A; A.setMode(0); float mStand = avgMetab(A, 4.0f);
    Walker B; B.setMode(1); float mWalk = avgMetab(B, 4.0f);
    Walker C; C.setMode(2); float mRun = avgMetab(C, 4.0f);
    check("站立代谢 150~260W", mStand > 150 && mStand < 260, "%.0fW (%.2f W/kg)", (double)mStand, (double)(mStand / 70));
    checkv("走 > 站", mWalk > mStand * 1.25f, "走 %.0fW / 站 %.0fW", mWalk, mStand, 0);
    checkv("跑 > 走", mRun > mWalk * 1.5f, "跑 %.0fW / 走 %.0fW", mRun, mWalk, 0);
    /* 真实跑步约 10~12 W/kg。我们有两点系统性偏低：小腿/髋的力矩被准静态近似低估，
       腾空相不做肌肉功。所以这条只卡"跑步确实比走路贵得多"，不硬凑真人数字。 */
    check("跑步 5~9 W/kg", mRun / 70 > 5.0f && mRun / 70 < 9.0f, "%.2f W/kg", (double)(mRun / 70), 0);

    /* 12.3 跑 10 min：疲劳积累 + 体温上升 */
    Walker D; D.setMode(2);
    for (int i = 0; i < 240 * 600; ++i) D.step(dt);
    check("跑 10min 疲劳 0.20~0.55", D.bio.fatAvg > 0.20f && D.bio.fatAvg < 0.55f, "fat=%.3f", D.bio.fatAvg, 0);
    check("跑 10min 体温 37.2~38.2", D.bio.temp > 37.2 && D.bio.temp < 38.2, "T=%.3f", (float)D.bio.temp, 0);
    checkv("跑 10min 不摔", D.falls == 0 && !D.fallen, "falls=%d", (int)D.falls);

    /* 12.4 急停 → 站定：不能摔（这是"站定后侧向漂移"那个 bug 的回归测试） */
    D.setMode(0);
    for (int i = 0; i < 240 * 60; ++i) D.step(dt);
    checkv("跑→站定不摔", !D.fallen && D.falls == 0, "fallen=%d falls=%d", (int)D.fallen, (int)D.falls);
    check("站 1min 后侧向回正", std::fabs(D.p.z) < 0.20f, "z=%.3f", D.p.z, 0);

    /* 12.5 站 5 min：疲劳恢复 + 体温回落 */
    float fat0 = D.bio.fatAvg, t0 = (float)D.bio.temp;
    for (int i = 0; i < 240 * 240; ++i) D.step(dt);
    check("站 4min 疲劳回 <0.05", D.bio.fatAvg < 0.05f, "%.3f → %.3f", fat0, D.bio.fatAvg);
    check("站 4min 体温回落", (float)D.bio.temp < t0 - 0.03f, "%.3f → %.3f", t0, (float)D.bio.temp);

    /* 12.6 体温的时间分辨率：1 分钟内必须有可测变化（float 舍入会把它吃掉） */
    Walker E; E.setMode(1);
    for (int i = 0; i < 240 * 30; ++i) E.step(dt);
    double te0 = E.bio.temp;
    for (int i = 0; i < 240 * 60; ++i) E.step(dt);
    check("体温 1min 可分辨", std::fabs(E.bio.temp - te0) > 0.002, "dT=%.5f", (float)std::fabs(E.bio.temp - te0), 0);

    /* 12.7 摔倒之后生物层还在跑（躺着也代谢，不能冻住） */
    Walker F; F.setMode(1);
    for (int i = 0; i < 240 * 5; ++i) F.step(dt);
    F.push(0, 0, 3.5f);
    for (int i = 0; i < 240 * 6; ++i) F.step(dt);
    float h1 = F.bio.heatProd;
    for (int i = 0; i < 240 * 10; ++i) F.step(dt);
    checkv("摔倒后仍代谢", F.fallen && h1 > 50.0f && std::fabs(F.bio.heatProd - h1) < 60.0f,
          "fallen=%d heat %.0f→%.0f", (int)F.fallen, (double)h1, (double)F.bio.heatProd);
  }

  /* ================= §13 借步恢复：站定被推歪时迈一步把重心接回来 ================= */
  printf("\n== §13 借步恢复（站立被推） ==\n");
  {
    struct R { float push, peak; bool fell; };
    auto shove = [&](float push) {
      Walker W; W.setMode(0);
      for (int i = 0; i < 240 * 2; ++i) W.step(1.0f / 240.0f);
      W.push(0, 0, push);
      float peak = 0;
      for (int i = 0; i < 240 * 4; ++i) {
        W.step(1.0f / 240.0f);
        peak = std::max(peak, std::fabs(W.p.z));
        if (W.fallen) break;
      }
      return R{push, peak, W.fallen};
    };
    R r1 = shove(1.4f);
    printf("--- 侧推 1.4 m/s: 峰值 %.3f m, 摔倒=%d\n", r1.peak, (int)r1.fell);
    check("1.4 m/s 侧推能借步站住", !r1.fell, "fell=%d", (double)r1.fell, 0);
    check("借步后侧移 <0.5 m", r1.peak < 0.5f, "%.3f m", r1.peak, 0.5);
    R r2 = shove(3.0f);
    printf("--- 侧推 3.0 m/s: 峰值 %.3f m, 摔倒=%d\n", r2.peak, (int)r2.fell);
    check("3.0 m/s 侧推该倒", r2.fell, "fell=%d", (double)r2.fell, 1);
  }

  /* ================= §14 脊髓层：CPG 自己产生节律 ================= */
  printf("\n== §14 脊髓层（CPG 节律 / 触地重同步 / 步频跟随） ==\n");
  {
    Walker W; W.setMode(1);
    float dt = 1.0f / 240.0f;
    float maxGap = 0;
    int touch = 0;
    bool prevContact[2] = {true, true};
    float badResync = 0;
    for (int i = 0; i < 240 * 8; ++i) {
      W.step(dt);
      if (i > 240) maxGap = std::max(maxGap, std::fabs(3.14159265f - W.cpg.phaseGap()));
      for (int k = 0; k < 2; ++k) {
        bool c = W.feet[k].contact;
        if (c && !prevContact[k]) {
          ++touch;
          /* 触地那一瞬，这条腿的振荡器应该被拉回支撑相起点（≈0） */
          float ph = std::fabs(W.cpg.ph[k]);
          if (std::min(ph, std::fabs(ph - 6.28318531f)) > 0.30f) badResync += 1;
        }
        prevContact[k] = c;
      }
    }
    float freq = touch / 2.0f / 8.0f;       /* 6 s 内落地次数 / 2 次每周期 */
    printf("--- CPG 走路 8 s: 反相偏差峰 %.3f rad, 落地 %d 次 (步频 %.2f Hz), 目标 %.2f Hz, 重同步偏差帧 %d\n",
           maxGap, touch, freq, W.cpgFreq(), (int)badResync);
    check("两腿反相稳定 (|π-gap|<0.35)", maxGap < 0.35f, "%.3f", maxGap, 0.35);
    check("步频跟随意图 (±25%)", std::fabs(freq - W.cpgFreq()) < 0.25f * W.cpgFreq(),
          "%.2f vs %.2f", freq, W.cpgFreq());
    check("触地重同步到位", badResync == 0, "%d 帧", (double)badResync, 0);

    /* 对照：关掉脊髓层，老的相位计数器也得能走（不然是"换了个坏"） */
    Walker L; L.spinal = false; L.setMode(1);
    for (int i = 0; i < 240 * 10; ++i) L.step(dt);
    printf("--- 旧相位计数器对照: falls=%d x=%.2f\n", L.falls, L.p.x);
    check("旧模式仍然能走（对照）", L.falls == 0 && L.p.x > 5.0f, "falls=%d x=%.2f",
          (double)L.falls, 0);
  }

  /* 15. 大脑通道：异步、带 tag、会超时 */
  {
    BrainChannel B;
    int tag = B.ask();
    check("挂起后 pending", B.pending(), "%d", (double)B.pendingTag, 1);
    check("重复 ask 不换 tag", B.ask() == tag, "tag=%d", (double)tag, 0);
    check("错 tag 被丢弃", !B.answer(tag + 1) && B.dropped == 1, "dropped=%d", (double)B.dropped, 0);
    check("丢错 tag 后仍挂着", B.pending(), "%d", (double)B.pendingTag, 1);
    bool timedOut = false;
    /* 真模型的往返是 0.5–3 s，等 4 s 才自决 —— 所以这里要跑满 4 秒 */
    for (int i = 0; i < 500 && !timedOut; ++i) timedOut = B.tick(0.01f);
    printf("--- 大脑超时: timeout=%d pending=%d\n", B.timedOut, (int)B.pending());
    check("等不到就自己拿主意 (4 s)", timedOut && !B.pending(), "timeout=%d", (double)B.timedOut, 1);
    int t2 = B.ask();
    check("答对 tag 生效", B.answer(t2) && B.answered == 1 && !B.pending(), "answered=%d", (double)B.answered, 1);
  }

  /* 16. 意图驱动：只有"想走多快"，站/走/跑由身体自己切 */
  {
    Walker W; W.intentMode = true; W.P.vDes = 0.0f;
    Frame f;
    for (int i = 0; i < 480; ++i) W.step(1.0f / 240.0f);
    W.pose(f);
    check("意图 0 = 站着", f.mode == 0 && std::fabs(f.speed) < 0.02f, "mode=%d speed=%.3f", (double)f.mode, 0);
    W.P.vDes = 0.60f;
    for (int i = 0; i < 240 * 6; ++i) W.step(1.0f / 240.0f);
    W.pose(f);
    printf("--- 意图 0.60: mode=%d speed=%.3f\n", (int)f.mode, f.speed);
    check("意图 0.60 = 走", f.mode == 1 && std::fabs(f.speed - 0.60f) < 0.10f, "speed=%.3f", (double)f.speed, 0.60);
    W.P.vDes = 1.60f;
    for (int i = 0; i < 240 * 6; ++i) W.step(1.0f / 240.0f);
    W.pose(f);
    printf("--- 意图 1.60: mode=%d speed=%.3f\n", (int)f.mode, f.speed);
    check("意图 1.60 自己变成跑", f.mode == 2 && f.speed > 1.30f, "mode=%d speed=%.3f", (double)f.mode, 1.30);
    check("切跑步没摔", W.falls == 0 && !W.fallen, "falls=%d", (double)W.falls, 0);
    W.P.vDes = 0.0f;
    for (int i = 0; i < 240 * 6; ++i) W.step(1.0f / 240.0f);
    W.pose(f);
    printf("--- 意图 0: mode=%d speed=%.3f\n", (int)f.mode, f.speed);
    check("意图 0 自己停下站好", f.mode == 0 && std::fabs(f.speed) < 0.05f, "mode=%d speed=%.3f", (double)f.mode, 0.05);
    W.P.vDes = 0.90f;
    for (int i = 0; i < 240 * 5; ++i) W.step(1.0f / 240.0f);
    W.pose(f);
    check("停下后还能自己起步", f.mode == 1 && f.speed > 0.70f, "mode=%d speed=%.3f", (double)f.mode, 0.70);
  }

  /* 17. 生物层压脊髓输出：累了以后步态自己变样（步频降、抬脚低），
         而不是外面把目标速度调小 —— 速度是意图，步态是身体。 */
  {
    const float P_ANKLEH = 0.08f;
    auto gaitAt = [P_ANKLEH](float fat, float& liftMax) {
      Walker W; W.intentMode = true; W.P.vDes = 0.80f;
      W.bio.fatAvg = fat;
      liftMax = 0;
      for (int i = 0; i < 240 * 6; ++i) {
        W.step(1.0f / 240.0f);
        for (int k = 0; k < 2; ++k)
          if (W.feet[k].swing)
            liftMax = std::max(liftMax, W.feet[k].ank.y - P_ANKLEH - W.groundAt(W.feet[k].ank.x, W.feet[k].ank.z));
      }
      Frame f; W.pose(f);
      return std::make_pair(W.cycleT(), f.speed);
    };
    float l0 = 0, l6 = 0;
    auto g0 = gaitAt(0.0f, l0);
    auto g6 = gaitAt(0.60f, l6);
    printf("--- 疲劳压步态: 不累 T=%.3f 抬脚 %.3f / 累 T=%.3f 抬脚 %.3f\n",
           g0.first, l0, g6.first, l6);
    check("累了步频降 >12%", g6.first > g0.first * 1.12f, "T %.3f → %.3f", g0.first, g6.first);
    check("累了抬脚低 >15%", l6 < l0 * 0.85f, "lift %.3f → %.3f", l0, l6);
    check("累了还能走不摔", g6.second > 0.40f, "speed=%.3f", g6.second, 0.40);
  }

  /* 18. 脊髓 10 关节：躯干/颈/摆臂也要真的被量出来，而且能学、能存盘。 */
  {
    printf("\n== §15 脊髓 10 关节 + 学到的矩阵 ==\n");
    Walker W; W.intentMode = true; W.P.vDes = 0.85f;
    Frame f;
    SpinalCore S;
    /* 产品里 main.cpp 启动时会 load 这份先验；自检也得带上它，
       否则测的是一个用户永远见不到的"从零开始"的脊髓。 */
    const bool priorOk = S.load("assets/spinal_learned.json") || S.load("../assets/spinal_learned.json");
    float q[S.D];
    double errEarly = 0, errLate = 0, qTrunk = 0, qNeck = 0, qArm = 0;
    for (int i = 0; i < 240 * 40; ++i) {
      W.step(1.0f / 240.0f);
      if (i % 4 == 0) {
      W.pose(f);
        S.step(f);
        if (i == 240 * 4) errEarly = S.errAvg;
        if (i == 240 * 38) errLate = S.errAvg;
        if (i > 240 * 30 && i % 8 == 0) {
          /* 单帧可能正好落在摆动的中性点上，所以取一段窗口里的最大值 */
          float qq[SpinalCore::D];
          SpinalCore::measure(f, qq);
          qTrunk = qq[6]; qNeck = qq[8];
          if (std::fabs(qq[9]) > std::fabs(qArm)) qArm = qq[9];

        }
      }
    }
    printf("--- 脊髓: ticks=%lld err 早 %.4f → 晚 %.4f rad, doubt=%.2f reflex=%.2f\n",
           (long long)S.ticks, errEarly, errLate, S.doubt(), S.reflexScale());
    printf("--- 量的上肢: 躯干俯仰 %.3f rad, 颈 %.3f, 摆臂 %.3f\n", qTrunk, qNeck, qArm);
    check("脊髓跑满 40 s", S.ticks > 2000, "ticks=%d", (double)S.ticks, 0);
    check("预测误差在合理范围", errLate < 0.25 && errLate > 0.0, "err=%.4f", errLate, 0);
    check("走路时摆臂被量到", std::fabs(qArm) > 0.15f, "arm=%.3f rad", (double)qArm, 0);
    check("躯干没歪（正常走路）", std::fabs(qTrunk) < 0.25f, "trunk=%.3f", (double)qTrunk, 0);
    check("反射不压满血脊髓", S.reflexScale() > 0.85f, "reflex=%.2f", (double)S.reflexScale(), 0);
    printf("--- 多步 rollout（24 步 = 0.4 s）: %.4f rad = %.2f°\n", S.roll, S.roll * 57.29578);
    check("24 步 rollout 有数", S.roll >= 0.0, "roll=%.4f", S.roll, 0);
    /* 0.12 rad 是旧步态（周期 0.90 s）下卡的门槛；换成真人表的 0.98 s 周期后
       同样 0.4 s 的盲推变成 0.148 rad —— 单步误差没变（0.0111 vs 0.0112 rad），
       是轨迹本身更不对称了。门槛跟着放到 0.17，并把实测值打在检查名里。 */
    check("rollout 预测够准（< 0.17 rad）", S.roll < 0.17, "roll=%.4f rad", S.roll, 0);
    /* 336,874 帧自身数据训出来的先验，价值就体现在这里：
       24 步盲推 8.50° → 2.34°（从零开始学也能到 0.75° 单步，但 0.4 s 的盲推学不到）。 */
    /* 带先验时 5.15°，不带是 8.50° —— 单步误差两边都是 0.011 rad，
       差别全在 0.4 s 的盲推上，这正是 predictHorizon 取的那条路。 */
    /* 118 万帧（3000 段）重训后盲推从 5.15° 降到 3.24°，门槛跟着收到 4° */
    check("自身数据先验把 24 步盲推压到 4° 内", priorOk && S.roll < 0.070, "roll=%.2f°",
          S.roll * 57.29578, 0);

    const char* tmp = "/tmp/soma_spinal_test.json";
    const bool sv = S.save(tmp);
    SpinalCore L2;
    const bool ld = L2.load(tmp);
    double same = 0;
    for (int i = 0; i < MatrixSpinal::DX; ++i)
      for (int k = 0; k < MatrixSpinal::D; ++k) same += std::fabs(S.sp.A[i][k] - L2.sp.A[i][k]);
    check("矩阵能写盘", sv, "path=%s", 0, 0);
    check("写盘能读回（数值一致）", ld && same < 1e-5, "diff=%.6f", same, 0);

    /* 19. 循环帧 + 预测：模型能往前推，手也能被相位举起来（不是关键帧表） */
    {
      float pr[SpinalCore::D];
      float q0[SpinalCore::D];
      SpinalCore::measure(f, q0);
      S.predictHorizon(6, pr);
      double diff = 0;
      for (int i = 0; i < SpinalCore::D; ++i) diff += std::fabs(pr[i] - q0[i]);
      check("预测能往前推 6 帧（且不是原地不动）", diff > 1e-3 && diff < 6.0, "diff=%.4f", diff, 0);

      /* 姿态先验：u 变一下，手就该到不同的地方 —— 而且中间是连续的 */
      PosePrior P;
      const bool ok = P.load("assets/poseprior.json") || P.load("../assets/poseprior.json");
      check("姿态先验能从盘上读回来", ok, "path=%s", 0, 0);
      if (ok) {
        /* 相位是有影响的（摆手就是跟着相位走的），所以按一圈相位取平均再比 */
        const float uDown[4] = {0.42f, 0.0f, 0.3f, 0.5f};
        const float uUp[4]   = {0.42f, 1.0f, 0.3f, 0.5f};
        float lo = 0, hi = 0;
        for (int k = 0; k < 12; ++k) {
          const float ph = k / 12.0f;
          const float wv[2] = {(float)std::sin(6.2831853f * ph), (float)std::cos(6.2831853f * ph)};
          float x[PosePrior::F], y[PosePrior::O];
          PosePrior::feats(uDown, wv, x); P.eval(x, y); lo += y[1] / 12.0f;
          PosePrior::feats(uUp, wv, x);   P.eval(x, y); hi += y[1] / 12.0f;
        }
        printf("--- 先验: 手相对肩（一圈相位平均） 抬手0 → %.3f m, 抬手1 → %.3f m\n", lo, hi);
        check("抬手 u 变大会把手抬起来", hi - lo > 0.06f, "dy=%.3f", hi - lo, 0);
        /* 连续性：u 从 0 走到 1，手的高度不能跳 */
        float maxJump = 0, prevY = lo;
        float x2[PosePrior::F];
        const float wv2[2] = {0.0f, 1.0f};
        for (int i = 1; i <= 10; ++i) {
          const float uu[4] = {0.42f, i / 10.0f, 0.3f, 0.5f};
          float y[PosePrior::O]; PosePrior::feats(uu, wv2, x2); P.eval(x2, y);
          maxJump = std::fmax(maxJump, std::fabs(y[1] - prevY)); prevY = y[1];
        }
        check("意图是连续的（没有档位跳变）", maxJump < 0.06f, "maxJump=%.3f", maxJump, 0);
      }
    }
  }

  /* 19. 生物慢变量：吃/喝/睡真的改账本，走路真的在烧。 */
  {
    printf("\n== §16 慢变量（血糖 / 水 / 清醒） ==\n");
    Bio B; B.init();
    check("满状态可出力 = 1", std::fabs(B.conditionK() - 1.0f) < 1e-4f, "K=%.4f", (double)B.conditionK(), 0);
    B.kcal = 0.20f; B.sleep = 0.30f;
    check("饿+困压出力", B.conditionK() < 0.86f, "K=%.4f", (double)B.conditionK(), 0);
    B.eat(95.0f);
    check("吃一个苹果血糖升", B.kcal > 0.20f, "kcal=%.4f", (double)B.kcal, 0);
    B.water = 0.50f;
    const float w0 = B.water;
    B.drink(0.25f);
    check("喝水水合升", B.water > w0, "water %.3f → %.3f", (double)w0, (double)B.water);
    const float s0 = B.sleep;
    B.sleepFor(1800.0f);
    check("睡半小时清醒升", B.sleep > s0, "sleep %.3f → %.3f", (double)s0, (double)B.sleep);

    Bio C; C.init();
    Walker W; W.intentMode = true; W.P.vDes = 1.0f;
    const float k0 = W.bio.kcal;
    for (int i = 0; i < 240 * 120; ++i) W.step(1.0f / 240.0f);
    printf("--- 走 2 min: kcal %.5f → %.5f, water %.5f, sleep %.5f\n",
           k0, W.bio.kcal, W.bio.water, W.bio.sleep);
    check("走路在烧糖", W.bio.kcal < k0, "kcal %.5f → %.5f", (double)k0, (double)W.bio.kcal);
    check("出汗在失水", W.bio.water < 1.0f, "water=%.5f", (double)W.bio.water, 0);
    check("清醒度在掉", W.bio.sleep < 1.0f, "sleep=%.5f", (double)W.bio.sleep, 0);
    check("2 分钟不至于饿死", W.bio.kcal > 0.9f, "kcal=%.5f", (double)W.bio.kcal, 0);
  }


  /* ------------------------- 17. 感知：视 / 听 / 触 / 碰撞 ------------------------- */
  {
    Walker W; W.setMode(1);
    Frame f; runSim(W, 40, &f);
    SenseCfg SC; OrganCfg OC;
    loadConfig("assets/soma.json", W.P, W.bio, *(new SomaCfg()), &SC, &OC);
    Senses S; PlayerState P;

    P.valid = true; P.x = f.comX + 3.0f; P.z = f.comZ + 3.0f; P.speed = 0.0f;   /* 侧前 45° */
    S.update(f, P, SC, 0.016f);
    const bool seeFront = S.sees;
    S.update(f, P, SC, 1.0f);
    const float yawFront = S.headYaw;

    P.x = f.comX - 3.0f; P.z = f.comZ - 3.0f;
    S.update(f, P, SC, 1.0f);
    const bool seeBack = S.sees;

    P.x = f.comX + 12.0f; P.speed = 0.0f;   /* 站着一动不动：听不到 */
    S.update(f, P, SC, 0.5f);
    const bool hearStill = S.heard;
    P.speed = 1.6f;                          /* 跑起来：听得到 */
    S.update(f, P, SC, 0.5f);
    const bool hearRun = S.heard;

    P.x = f.comX + 0.15f; P.z = f.comZ; P.speed = 0.0f;
    S.update(f, P, SC, 0.016f);
    const float touch = S.touchN;
    const float push = std::sqrt(S.pushX * S.pushX + S.pushZ * S.pushZ);

    printf("--- 17. 感知: 看见(前)=%d 看见(后)=%d 头转=%.2frad 听见(站)=%d 听见(跑)=%d 触碰=%.0fN 推开=%.3fm\n",
           (int)seeFront, (int)seeBack, yawFront, (int)hearStill, (int)hearRun, touch, push);
    bool ok = seeFront && !seeBack && hearRun && !hearStill && touch > 50.0f && push > 0.01f && push < 0.16f;
    printf("%s 感知：看得见前面、看不见背后、跑动才听得见、贴上会被推开\n", ok ? "PASS" : "FAIL");
    if (!ok) ++g_fail;
  }

  /* ------------------------- 18. 内脏：心率/呼吸跟着代谢走 ------------------------- */
  {
    SenseCfg SC2; OrganCfg OC;
    { SomaCfg C; Params P; Bio B; loadConfig("assets/soma.json", P, B, C, &SC2, &OC); }
    OrganState O;
    const float bmr = 80.0f;              /* 70 kg 静息代谢 ≈ 80 W */
    for (int i = 0; i < 120; ++i) O.update(700.0f, bmr, 0.5f, OC);   /* 快走 1 分钟 */
    const float hrHigh = O.hr;
    for (int i = 0; i < 120; ++i) O.update(150.0f, bmr, 0.5f, OC);   /* 歇 1 分钟 */
    const float hrLow = O.hr;
    printf("--- 18. 内脏: 走时 %.0f bpm → 歇时 %.0f bpm, 呼吸 %.1f 次/分, 氧 %.2f\n",
           hrHigh, hrLow, O.br, O.oxygen);
    bool ok = hrHigh > 95.0f && hrLow < hrHigh - 5.0f && hrLow > 40.0f;
    printf("%s 内脏：心率随代谢上去、歇下来会回落\n", ok ? "PASS" : "FAIL");
    if (!ok) ++g_fail;
  }

  /* ------------------------- 19. 转身：真的把身体转过去 -------------------------
     以前"朝向"只是前端把模型转一下，物理里她永远朝 +x。这一版 yaw 进了动力学：
     体坐标随 yaw 转，落脚点/CoP/判摔全在体坐标里算，所以她走路是沿弧线拐的。 */
  {
    Walker W; W.setMode(1); W.P.vDes = 0.85f;
    for (int i = 0; i < 240 * 3; ++i) W.step(1.0f / 240.0f);   /* 先直走 3 s 站稳节奏 */
    const float x0 = W.p.x, z0 = W.p.z, yaw0 = W.yaw;
    W.vYawWant = 1.0f;                                          /* 拧 1 rad/s */
    for (int i = 0; i < 240 * 3; ++i) W.step(1.0f / 240.0f);
    W.vYawWant = 0.0f;
    Frame f; W.pose(f);
    const float dx = W.p.x - x0, dz = W.p.z - z0;
    const float dist = std::sqrt(dx * dx + dz * dz);
    const float dYaw = W.yaw - yaw0;
    /* 位移方向也必须真的偏出去：只在原地转（重心没动）不算转身 */
    const float headingDeg = std::atan2(dz, dx) * 57.29578f;
    printf("--- 19. 转身: Δyaw=%.2f rad (%.0f°), 位移 %.2f m 方向 %.0f°, 前向速度 %.2f m/s, 摔 %d 次\n",
           dYaw, dYaw * 57.29578f, dist, headingDeg, f.speed, W.falls);
    check("转身：身体真的转了（yaw 走起来）", dYaw > 1.5f, "dYaw=%.2f", dYaw, 2.5);
    check("转身：边走边拐不摔", W.falls == 0 && !W.fallen, "falls=%d", (double)W.falls, 0);
    check("转身：还保持走路速度（不是减速换向）", f.speed > 0.4f, "speed=%.2f", f.speed, 0.7);
    check("转身：走的是弧线（位移方向偏出 30°+）", headingDeg > 30.0f, "heading=%.0f", headingDeg, 60);
    check("转身：不是原地拧（还是走了 1 m+）", dist > 1.0f, "dist=%.2f", dist, 2.0);
    /* 转身之后她仍然是同一个"体坐标"世界：正前方 / 左手边定义要自洽 */
    V3 fw, lf; W.axes(fw, lf);
    const float orth = fw.x * lf.x + fw.z * lf.z;
    check("转身：体坐标轴仍然正交", std::fabs(orth) < 1e-5f, "dot=%.6f", (double)orth, 0);
  }

  printf("\n%s  (%d 条失败)\n", g_fail ? "有红" : "全绿", g_fail);
  return g_fail ? 1 : 0;
}

static int qdump() {
  /* 把运行时真正量到的 10 个关节角打出来：
     训练数据(mocap)能不能直接当先验，取决于两边角度约定是否一致。 */
  const float speeds[3] = {0.6f, 0.9f, 1.4f};
  for (int m = 0; m < 3; ++m) {
    Walker W; W.setMode(m == 2 ? 2 : 1); W.P.vDes = speeds[m];
    if (m == 2) W.P.Tstep = 0.36f;
    W.vDesWant = speeds[m];
    Frame f;
    const float dt = 1.0f / 240.0f;
    for (int i = 0; i < 240 * 14; ++i) {
      W.step(dt);
      if (i % 4) continue;
      W.pose(f);
      if (i < 240 * 6) continue;                 /* 前 6 s 让她进稳态 */
      float q[SpinalCore::D];
      SpinalCore::measure(f, q);
      printf("Q %d %.5f", m, (double)f.phase);
      for (int k = 0; k < SpinalCore::D; ++k) printf(" %.6f", (double)q[k]);
      printf("\n");
    }
  }
  return 0;
}

static int seqdump(int episodes, const char* outPath) {
  /* 用她自己的身体生成训练集：脊髓层要预测的是**她自己的动力学**，
     不是 CMU 的骨架。这里把 240Hz 的物理跑成 60Hz 的 (u, φ, q) 序列，
     离线再算 dq / delta。多速度、多转向、抬手、前倾都覆盖到。 */
  FILE* o = fopen(outPath, "w");
  if (!o) { fprintf(stderr, "cannot write %s\n", outPath); return 1; }
  unsigned seed = 20261113u;
  auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return ((seed >> 8) & 0xFFFF) / 65535.0f; };
  const float dt = 1.0f / 240.0f;
  long long wrote = 0;
  for (int ep = 0; ep < episodes; ++ep) {
    Walker W;
    const int mode = (rnd() < 0.78f) ? 1 : ((rnd() < 0.6f) ? 2 : 0);
    W.setMode(mode);
    const float v0 = 0.35f + 1.55f * rnd();
    W.P.vDes = v0; W.vDesWant = v0;
    gArmRaise = rnd() * 0.55f;
    gLean = rnd() * 0.45f;
    gWaveHz = rnd() * 2.2f;
    gWavePh = rnd();
    float yawWant = 0.0f;
    Frame f;
    const int n = (int)(240 * (7.0f + 5.0f * rnd()));
    for (int i = 0; i < n; ++i) {
      /* 每 ~1.5 s 换一次意图：速度小幅漂移 + 偶尔转向 */
      if (i % 360 == 0) {
        const float v = clampf(v0 + (rnd() - 0.5f) * 0.5f, 0.0f, 2.0f);
        W.P.vDes = v; W.vDesWant = v;
        yawWant = (rnd() < 0.45f) ? (rnd() - 0.5f) * 1.6f : 0.0f;
        W.vYawWant = yawWant;
        gArmRaise = clampf(gArmRaise + (rnd() - 0.5f) * 0.35f, 0.0f, 0.9f);
        gLean = clampf(gLean + (rnd() - 0.5f) * 0.3f, 0.0f, 0.8f);
        gWaveHz = clampf(gWaveHz + (rnd() - 0.5f) * 1.2f, 0.0f, 3.0f);
      }
      W.step(dt);
      if (i % 4) continue;
      W.pose(f);
      if (i < 240 * 3) continue;                  /* 前 3 s 是启动瞬态 */
      float q[SpinalCore::D];
      SpinalCore::measure(f, q);
      bool bad = !std::isfinite(f.phase);
      for (int k = 0; k < SpinalCore::D; ++k) if (!std::isfinite(q[k])) bad = true;
      if (bad || W.fallen) continue;
      fprintf(o, "%d %.5f %.5f %.4f %.4f %.4f", ep, (double)f.phase,
              (double)(W.P.vDes / 2.0f), (double)gArmRaise, (double)gLean, (double)(gWaveHz / 3.2f));
      for (int k = 0; k < SpinalCore::D; ++k) fprintf(o, " %.6f", (double)q[k]);
      fprintf(o, "\n");
      ++wrote;
    }
  }
  fclose(o);
  printf("seqdump: %d 段, %lld 帧 (60Hz) → %s\n", episodes, wrote, outPath);
  return 0;
}

static int spinaleval(const char* prior) {
  /* 先验到底有没有用：同一段走路，从零开始 vs 带着盘上的矩阵开始。
     看的是 errAvg（单步对账）和 roll（24 步闭环），以及它有没有被每 60 tick 的清零救命。 */
  for (int variant = 0; variant < 2; ++variant) {
    Walker W; W.setMode(1); W.P.vDes = 0.85f; W.vDesWant = 0.85f;
    SpinalCore S;
    bool ok = false;
    if (variant == 1 && prior) ok = S.load(prior);
    Frame f;
    double early = -1, mid = -1;
    for (int i = 0; i < 240 * 40; ++i) {
      W.step(1.0f / 240.0f);
      if (i % 4) continue;
      W.pose(f);
      S.step(f);
      if (S.ticks == 240) early = S.errAvg;
      if (S.ticks == 1200) mid = S.errAvg;
    }
    printf("  起步漂移%.0f° 到 %.0f° | 4s err %.4f rad (%.2f°) | 20s err %.4f (%.2f°) | roll24 %.2f° doubt %.2f reflex %.2f\n",
           early * 57.29578, mid * 57.29578, early, early * 57.29578, mid, mid * 57.29578,
           S.roll * 57.29578, S.doubt(), S.reflexScale());
  }
  return 0;
}

static int gaitdump() {
  /* 她自己的步态表：速度 → 周期 / 步幅 / 抬脚高度。
     拿它跟 mocap 量出来的真人表对（tools/mocap/gaitstats.txt），才知道哪里不像人。 */
  printf("v     T      stride  stepH   实际v  模式 ikErr 摔\n");
  for (float v = 0.40f; v <= 1.91f; v += 0.10f) {
    Walker W; W.setMode(1);
    W.P.vDes = v; W.vDesWant = v;
    float prevPh = 0; double lastWrap = -1, sumT = 0; int nCyc = 0;
    double lift = 0, lmin[2] = {1e9, 1e9}, err = 0; int falls = 0;
    Frame frG;
    double vsum = 0, vn = 0;
    const int N = (int)(240 * 24);
    for (int i = 0; i < N; ++i) {
      W.step(1.0f / 240.0f);
      if (i % 4) continue;
      if (i < 240 * 4) { prevPh = W.phase; lastWrap = W.t; continue; }
      if (W.phase < prevPh) {              /* 相位回绕 = 一个周期 */
        if (lastWrap > 0) { sumT += W.t - lastWrap; ++nCyc; }
        lastWrap = W.t;
      }
      prevPh = W.phase;
      for (int k = 0; k < 2; ++k) lmin[k] = std::fmin(lmin[k], W.feet[k].ank.y);
      for (int k = 0; k < 2; ++k) lift = std::fmax(lift, W.feet[k].ank.y - lmin[k]);
      W.pose(frG);
      err = std::fmax(err, frG.err);
      if (W.fallen) ++falls;
      vsum += std::fabs(W.vRef); ++vn;
    }
    const double T = nCyc ? sumT / nCyc : 0;
    printf("%.2f  %.3f  %.3f   %.3f   %.2f   %-4s ik=%.3f 摔%d\n", v, T, T * v, lift, vn ? vsum / vn : 0.0,
           W.P.run ? "run" : "walk", err, falls);
  }
  return 0;
}

int main(int argc, char** argv) {
  bool st = false, bench = false, sv = false, qd = false, sd = false;
  const char* seqOut = "/tmp/selfseq.txt"; int nEp = 60;
  bool se = false; const char* priorPath = nullptr;
  bool gd = false, nt = false;
  const char* cfg = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--selftest")) st = true;
    else if (!strcmp(argv[i], "--bench")) bench = true;
    else if (!strcmp(argv[i], "--qdump")) qd = true;
    else if (!strcmp(argv[i], "--gaitdump")) gd = true;
    else if (!strcmp(argv[i], "--navtest")) nt = true;
    else if (!strcmp(argv[i], "--navpath") && i + 4 < argc) {
      return navpath((float)atof(argv[i + 1]), (float)atof(argv[i + 2]),
                     (float)atof(argv[i + 3]), (float)atof(argv[i + 4]));
    }
    else if (!strcmp(argv[i], "--spinaleval")) { se = true; priorPath = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : nullptr; }
    else if (!strcmp(argv[i], "--seqdump") && i + 1 < argc) { sd = true; seqOut = argv[++i]; }
    else if (!strcmp(argv[i], "--episodes") && i + 1 < argc) nEp = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--serve")) sv = true;
    else if (!strcmp(argv[i], "--config") && i + 1 < argc) cfg = argv[++i];
  }
  if (sd) return seqdump(nEp, seqOut);
  if (nt) return navtest();
  if (gd) return gaitdump();
  if (se) return spinaleval(priorPath);
  if (qd) return qdump();
  if (sv) return serve(cfg);
  if (st) return selftest();
  if (bench) {
    Walker W; W.setMode(1); Frame f;
    clock_t c0 = clock();
    for (int i = 0; i < 240 * 60; ++i) W.step(1.0f / 240.0f);
    double ms = 1000.0 * (clock() - c0) / CLOCKS_PER_SEC;
    printf("60s 物理 %.2f ms (%.3f ms/帧@240Hz)\n", ms, ms / (240 * 60));
    return 0;
  }
  fprintf(stderr, "usage: soma3d --serve [--config assets/soma.json] | --selftest | --navtest | --bench\n");
  return 2;
}
