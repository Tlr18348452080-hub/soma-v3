#pragma once
#include "vec.h"
#include <cmath>
#include <functional>
#include <algorithm>

/* ================= 体模型：约束粒子（PBD） =================
   为什么要有它：
   以前"摔倒"只是个布尔量，身体照样按步态表站着走 —— 那没有体积、没有质量分布，
   也就谈不上碰撞。这里把 18 个节点当成有质量的球，17 根骨当成刚性距离约束，
   地面当成平面，用位置型动力学（PBD/Verlet）求解。
   结果：重心一旦跑出支撑多边形，身体是真的会倒下去、砸到地上、躺平的。

   质量按 Dempster 人体段比例分（总质量归一到 70 kg），只用于
   - 约束投影里的质量加权（腿不能把躯干拽飞）
   - 质心 / 惯量的统计（后面接生物学要用）
*/

struct Ragdoll {
  static const int N = 18, NB = 17;
  /* 0 骨盆 1 胸 2 颈 3 头 | 4-6 左肩肘手 | 7-9 右肩肘手 | 10-13 左髋膝踝趾 | 14-17 右 */
  static constexpr float RAW[N] = {
      10.5f, 22.0f, 1.0f, 4.5f,
      2.0f, 2.4f, 0.7f,
      2.0f, 2.4f, 0.7f,
      9.5f, 3.7f, 1.4f, 0.4f,
      9.5f, 3.7f, 1.4f, 0.4f};
  inline static constexpr int BONE[NB][2] = {{0,1},{1,2},{2,3},{1,4},{4,5},{5,6},{1,7},{7,8},{8,9},
                                  {0,10},{10,11},{11,12},{12,13},{0,14},{14,15},{15,16},{16,17}};

  V3 x[N], xp[N];          // 当前位置 / 上一步位置（Verlet 的隐式速度）
  float m[N], r[N];
  float blen[NB];
  bool active = false;
  float mu = 0.9f;          /* 地面切向摩擦系数：冰面 0.05、水泥 0.9 */
  std::function<float(float, float)> ground;   // 地面高度函数（台阶/平台），空 = 平地
  float settleT = 0;       // 连续低速时间
  float age = 0;
  float totalMass = 0;

  static void masses(float out[N]) {
    float s = 0;
    for (int i = 0; i < N; ++i) s += RAW[i];
    const float k = 70.0f / s;
    for (int i = 0; i < N; ++i) out[i] = RAW[i] * k;
  }

  V3 com() const {
    V3 c{0, 0, 0};
    float s = 0;
    for (int i = 0; i < N; ++i) { c = c + x[i] * m[i]; s += m[i]; }
    return s > 0 ? c * (1.0f / s) : c;
  }

  void init(const V3 pos[N], const float rad[N], float dt) {
    masses(m);
    totalMass = 0;
    for (int i = 0; i < N; ++i) {
      totalMass += m[i];
      x[i] = pos[i];
      xp[i] = pos[i];
      r[i] = rad[i];
    }
    for (int b = 0; b < NB; ++b) {
      float L = len(x[BONE[b][1]] - x[BONE[b][0]]);
      blen[b] = L > 1e-4f ? L : 1e-4f;
    }
    settleT = 0; age = 0;
    active = true;
    (void)dt;
  }

  void stop() { active = false; settleT = 0; age = 0; }

  /* 一根骨：按质量反比推开，纯位置投影（不显式算速度） */
  void solveBone(int b) {
    int a = BONE[b][0], c = BONE[b][1];
    V3 d = x[c] - x[a];
    float dist = len(d);
    if (dist < 1e-6f) return;
    float wa = 1.0f / m[a], wc = 1.0f / m[c];
    float diff = (dist - blen[b]) / dist / (wa + wc);
    x[a] = x[a] + d * (diff * wa);
    x[c] = x[c] - d * (diff * wc);
  }

  /* 关节限位：膝、肘不能反折（用两根骨的长度和当上限，是必要不充分条件，
     但足以挡住 PBD 最常见的"小腿穿进大腿"） */
  void solveLimits() {
    struct L { int a, b; float maxLen; } L3[4] = {
        {10, 12, blen[10] + blen[11] * 0.995f},    /* 左髋-左踝 ≤ 大腿+小腿 */
        {14, 16, blen[14] + blen[15] * 0.995f},    /* 右髋-右踝 */
        {4, 6, blen[3] + blen[4] * 0.995f},        /* 左肩-左手 */
        {7, 9, blen[7] + blen[8] * 0.995f}};       /* 右肩-右手 */
    for (int i = 0; i < 4; ++i) {
      V3 d = x[L3[i].b] - x[L3[i].a];
      float dist = len(d);
      if (dist <= L3[i].maxLen || dist < 1e-6f) continue;
      float wa = 1.0f / m[L3[i].a], wc = 1.0f / m[L3[i].b];
      float diff = (dist - L3[i].maxLen) / dist / (wa + wc);
      x[L3[i].a] = x[L3[i].a] + d * (diff * wa);
      x[L3[i].b] = x[L3[i].b] - d * (diff * wc);
    }
  }

  /* 弯折限位（等效"肌肉张力"）：没有它，躯干会像面条一样卷成一团 ——
     纯距离骨只能保证"长度不变"，管不了连接处的角度。
     这里用"隔一个节点的最小距离"来近似角度下限：
     |躯干起点 - 颈| 不许太短，膝盖不许折过头，肘子同理。 */
  void solveBend() {
    struct B { int a, c; float minLen; } BEND[6] = {
        {0, 2, 0.92f * (blen[0] + blen[1])},   /* 骨盆-颈：脊柱别塌 */
        {1, 3, 0.80f * (blen[1] + blen[2])},   /* 胸-头：脖子别折 180° */
        {10, 12, 0.34f * (blen[10] + blen[11])},  /* 左膝：最多屈到 ~110° */
        {14, 16, 0.34f * (blen[14] + blen[15])},  /* 右膝 */
        {4, 6, 0.55f * (blen[3] + blen[4])},   /* 左肘 */
        {7, 9, 0.55f * (blen[7] + blen[8])}};  /* 右肘 */
    for (int i = 0; i < 6; ++i) {
      V3 d = x[BEND[i].c] - x[BEND[i].a];
      float dist = len(d);
      if (dist >= BEND[i].minLen || dist < 1e-6f) continue;
      float wa = 1.0f / m[BEND[i].a], wc = 1.0f / m[BEND[i].c];
      float diff = (dist - BEND[i].minLen) / dist / (wa + wc);
      x[BEND[i].a] = x[BEND[i].a] + d * (diff * wa);
      x[BEND[i].c] = x[BEND[i].c] - d * (diff * wc);
    }
  }

  /* 自碰撞：全部球对（18 个球 = 153 对，每步 8 次迭代也够便宜）。
     以前只写死 6 对手/胸、膝/髋，摔倒时小臂能直接穿过胸腔和大腿 —— 看着像纸片人。
     跳过规则：同一根骨头连着的、以及隔一节的（相邻关节本来就贴在一起）。 */
  void solveSelf() {
    for (int a = 0; a < N; ++a) {
      for (int b = a + 1; b < N; ++b) {
        if (adjacent(a, b)) continue;
        V3 d = x[b] - x[a];
        float dist = len(d);
        float minD = (r[a] + r[b]) * 0.94f;
        if (dist >= minD || dist < 1e-6f) continue;
        float wa = 1.0f / m[a], wc = 1.0f / m[b];
        float diff = (dist - minD) / dist / (wa + wc);
        x[a] = x[a] + d * (diff * wa);
        x[b] = x[b] - d * (diff * wc);
      }
    }
  }

  /* a、b 是否"本来就该贴着"：直接相连，或者隔着一节（共享一个邻居） */
  static bool adjacent(int a, int b) {
    bool na[N] = {false}, nb[N] = {false};
    for (int i = 0; i < NB; ++i) {
      int p = BONE[i][0], q = BONE[i][1];
      if (p == a) na[q] = true;
      if (q == a) na[p] = true;
      if (p == b) nb[q] = true;
      if (q == b) nb[p] = true;
    }
    /* 只跳过直接相连的：隔一节的（比如肩-肘、髋-膝）虽然近，但那是真的会互相挤的，
       以前把"隔一节"也跳过，结果摔倒时小臂能陷进胸腔里。 */
    return na[b];
  }

  /* ---- 关节马达（脊髓运动神经元的最简等价物）----
     注意：不能直接"把下游骨段转过去" —— 那是运动学摆姿势，
     Verlet 会把位移当速度，一帧就炸（实测质心飞到 4 m 高）。
     正确做法是给角加速度：力矩 → 切向速度增量 → 改 xp。
     转出来的运动照样过距离约束、地面接触、自碰撞 —— 撑不住就倒。 */
  int parent[N];
  bool treeBuilt = false;
  void buildTree() {
    for (int i = 0; i < N; ++i) parent[i] = -1;
    for (int b = 0; b < NB; ++b) {
      int p = BONE[b][0], c = BONE[b][1];
      if (parent[c] < 0) parent[c] = p;
    }
    treeBuilt = true;
  }
  bool inSubtree(int node, int root) const {
    int cur = node;
    for (int guard = 0; guard < N && cur >= 0; ++guard) {
      if (cur == root) return true;
      cur = parent[cur];
    }
    return false;
  }
  /* 绕 z 轴的角加速度：alpha 单位 rad/s²，限于 [root] 的子树 */
  void jointTorque(int root, V3 pivot, float alpha, float dt) {
    if (!treeBuilt) buildTree();
    for (int i = 0; i < N; ++i) {
      if (!inSubtree(i, root)) continue;
      float rx = x[i].x - pivot.x, ry = x[i].y - pivot.y;
      float rr = std::sqrt(rx * rx + ry * ry);
      if (rr < 1e-4f) continue;
      /* 切向单位向量（绕 z 右手） */
      float tx = -ry / rr, ty = rx / rr;
      float dv = alpha * rr * dt;          // 速度增量 m/s
      xp[i].x -= tx * dv * dt;             // Verlet 里速度 = (x-xp)/dt
      xp[i].y -= ty * dv * dt;
    }
  }
  /* 关节角（两段骨之间的夹角，矢状面，弧度） */
  float jointAngle(int a, int b, int c) const {
    V3 u = x[a] - x[b], v = x[c] - x[b];
    float lu = len(u), lv = len(v);
    if (lu < 1e-6f || lv < 1e-6f) return 0;
    float cs = (u.x * v.x + u.y * v.y) / (lu * lv);
    cs = std::max(-1.0f, std::min(1.0f, cs));
    return std::acos(cs);
  }

  /* 地面：球体对平面 y=0，法向贴合 + 切向摩擦（切向位移按系数打折，等效静摩擦） */
  void solveGround(float mu) {
    for (int i = 0; i < N; ++i) {
      float g = ground ? ground(x[i].x, x[i].z) : 0.0f;
      if (x[i].y < g + r[i]) {
        x[i].y = g + r[i];
        /* 切向：把这一步的滑动吃掉一部分（脚掌/躯干贴地就不会一直滑） */
        x[i].x = xp[i].x + (x[i].x - xp[i].x) * (1.0f - mu);
        x[i].z = xp[i].z + (x[i].z - xp[i].z) * (1.0f - mu);
        /* 撞击耗散：竖直速度直接吃掉 */
        xp[i].y = x[i].y;
      }
    }
  }

  /* 一个物理子步（建议 dt=1/240） */
  void substep(float dt, const V3& g) {
    const float damp = 0.9992f;
    for (int i = 0; i < N; ++i) {
      V3 vel = (x[i] - xp[i]) * damp;
      xp[i] = x[i];
      V3 a = g;
      if (haveTgt && drive > 0) {
        /* 弹簧 + 阻尼，限幅：慢了就多使劲，快了就收力 —— 这是"肌肉"在干的事 */
        V3 e = tgt[i] - x[i];
        V3 vv = vel * (1.0f / std::max(1e-4f, dt));
        V3 f = e * (kSpring * drive) - vv * (cDamp * drive);
        float fl = len(f), cap = aMax * drive;
        if (fl > cap) f = f * (cap / fl);
        a = g + f;
      }
      x[i] = x[i] + vel + a * (dt * dt);
    }
    for (int it = 0; it < 8; ++it) {
      for (int b = 0; b < NB; ++b) solveBone(b);
      solveLimits();
      solveBend();
      solveSelf();
      solveGround(mu);
    }
  }

  /* ---- 起身：用"肌肉力"把各节点往目标姿态拽，约束照常兜底 ----
     drive 从 0 涨到 1（代表发力），aMax 限制单点加速度（肌肉功率上限）。 */
  V3 tgt[N];
  bool haveTgt = false;
  float drive = 0, aMax = 45.0f, kSpring = 130.0f, cDamp = 14.0f;

  void setTarget(const V3 t[N]) { for (int i = 0; i < N; ++i) tgt[i] = t[i]; haveTgt = true; drive = 0; }
  void clearTarget() { haveTgt = false; drive = 0; }

  bool settled() const { return settleT > 0.8f; }

  float maxSpeed(float dt) const {
    float mx = 0;
    for (int i = 0; i < N; ++i) mx = std::max(mx, len(x[i] - xp[i]) / std::max(1e-5f, dt));
    return mx;
  }

  void step(float dt) {
    if (!active) return;
    const int SUB = 4;
    const float h = dt / SUB;
    V3 g{0, -9.81f, 0};
    for (int s = 0; s < SUB; ++s) substep(h, g);
    age += dt;
    float ms = maxSpeed(dt);
    if (ms < 0.25f) settleT += dt; else settleT = 0;
  }
};
