#pragma once
#include "vec.h"
#include "ragdoll.h"
#include "bio.h"
#include "cpg.h"
#include <cstring>
#include <string>
#include <vector>

/* ================= SOMA 3D 内核 =================
   模型：3D 骨架 + LIPM（线性倒立摆）水平动力学。
   - 骨盆水平位置 p(x,z)、速度 v(x,z) 是真积分状态：ẍ = ω²(x - zmp)
   - zmp = 支撑脚接触中心 + 踝策略修正（限制在支撑多边形内）
   - 落脚点 = 捕获点 ξ = p + v/ω + 目标速度补偿（Raibert 式）
   - 竖直方向按步频起伏（约束在髋高上，LIPM 的标准假设）
   - 腿用 2-link 解析 IK，膝朝向由前向 pole 向量决定
   这样侧向是**有动力学的**：侧推一下，捕获点偏了，下一脚就必须去接住。 */

struct Params {
  float g = 9.81f;
  float hipH = 0.895f;          // 骨盆离地高
  float thigh = 0.45f, shank = 0.45f;
  float ankleH = 0.08f;
  float hipHalfW = 0.085f;
  float footLen = 0.25f, footW = 0.095f;
  float torso1 = 0.34f, torso2 = 0.12f, headR = 0.105f, neckLen = 0.12f;
  float upperArm = 0.28f, foreArm = 0.26f, shoulderHalfW = 0.19f;
  float armLift = 0.0f;   /* 肩角整体抬高（rad）：这是身体参数，不是动作 */
  float armOv[2][6] = {{0}};   /* 外部接管：每只手 {elbow xyz, hand xyz} 相对肩的偏移（米） */
  float vDes = 0.85f;          // 目标速度 m/s
  float vWalk = 0.85f, vRun = 1.9f;
  float Tstep = 0.56f;         // 单支撑时长的标称值（秒）
  float duty = 0.62f;          // 单脚支撑相占比
  float liftWalk = 0.07f, liftRun = 0.12f;
  float runDuty = 0.40f;
  float sagK = 0.55f;          // 落脚超前量增益
  float hipKp = 7.0f;          // 髋策略刚度
  float hipKd = 3.2f;          // 髋策略阻尼
  float latK = 1.0f;          // 侧向落脚增益（越大越保守）
  float ankleKp = 7.0f, ankleKd = 0.55f;   // 踝策略增益
  float latStiff = 1.0f;
  float leadBias = 0.02f;
  float alphaCap = 0.35f;      // 捕获点权重
  float betaStep = 0.55f;      // 步长 = beta·v·T      // 落脚点相对重心的固定前偏（小步起步用）
  float toePush = 0.075f;      // 蹬地时 CoP 前移到脚掌/脚趾的量
  float heelBrake = 0.045f;    // 脚跟着地时 CoP 后移的量（减速，跟蹬地一起形成步态）
  float zmpLimX = 0.42f, zmpLimZ = 0.40f;  // 占脚掌半长的比例
  float bounce = 0.018f;
  float armAmp = 0.55f;
  bool run = false;
  bool freezeYaw = true;
};

/* 世界里的一个方块：台阶、平台、墙都是它。
   h 低 = 能踩上去（落脚高度跟着变）；h 高 = 墙，挡住身子不许过。 */
struct Box {
  float x0 = 0, x1 = 0, z0 = 0, z1 = 0, h = 0;
  bool wall() const { return h > 0.24f; }      /* 超过 24 cm 一脚跨不上去 */
  bool contains(float x, float z) const {
    return x >= x0 && x <= x1 && z >= z0 && z <= z1;
  }
};

struct FootState {
  V3 ank;        // 当前踝位置（世界坐标）
  V3 plant;      // 支撑时的踝位置
  V3 from, to;   // 摆动起终点
  float t0 = 0, t1 = 0;
  bool swing = false;
  bool contact = false;
  int id = 0;    // 0=left(+z), 1=right(-z)
};

struct Frame {                 // 交给渲染器的一帧
  float t = 0;
  V3 nodes[18];
  float radii[18];
  unsigned char contact[18];
  float speed = 0, comX = 0, comZ = 0, capX = 0, capZ = 0;
  float yaw = 0, vYawOut = 0;                   /* 朝向（rad）与角速度：前端拿它转模型 */
  float zmpX = 0, zmpZ = 0, err = 0, dutyMeas = 0, leanX = 0, leanZ = 0;
  float phase = 0;               /* 步态相位 0..1：脊髓拿它当"这是循环里的第几帧" */
  int falls = 0;
  unsigned char mode = 0;
  unsigned char fallen = 0;      /* 1 = 现在是一具躺在地上的身体（体模型接管） */
  float comY = 0;                /* 质心高度：摔倒后应该掉到 0.2 m 上下 */
  float metab = 0, fat = 0, temp = 37.0f;   /* 代谢功率 W / 疲劳 0~1 / 体温 °C */
  float boneErrRagdoll = 0;      /* 体模型帧的骨长残差（0 = 约束收敛） */
  float grfL[3] = {0,0,0}, grfR[3] = {0,0,0};   /* 两只脚的地面反作用力（N） */
  float copX = 0, copZ = 0;                     /* 压力中心（世界坐标） */
  float frictionUse = 0;                        /* 用掉多少可用摩擦（|F_h|/(μF_z)） */
  int bones[17][2];
};

#include "motion.h"

class Walker {
  float latRef = 0.0f;          /* 侧向目标位置：站着时锁在支撑中点，不是绝对 0 */
  float stopGateT = 0.0f;       /* 急停等待侧向稳定用的计时 */
 public:
  Params P;
  V3 p, v;                 // 骨盆（y 另算）
  float y = 0, t = 0, phase = 0;
  FootState feet[2];
  int falls = 0;
  bool fallen = false;
  float dutyAcc = 0, dutyN = 0;
  float lastZmpX = 0, lastZmpZ = 0;
  float leanSmX = 0, leanSmZ = 0;   /* 躯干倾角（平滑过）：直接跟 ZMP 走会一步一跳 */
  float vRef = 0;
  float lostT = 0;
  float vPush = 0;             /* 前后向推力脉冲（指数衰减），前后是伺服，只能这么注入 */
  float xRef = 0;              /* 前后参考：按指令速度平移的“虚拟目标” */              /* 速度指令：从 0 按加速度限幅爬到 vDes，第一脚才不会跨太大 */
  float accLim = 1.1f;         /* 起步加速度上限 m/s² */
  float swingLift = 0.07f;
  /* 朝向：身体是有方向的刚体，不是永远朝 +x 的跑步机。
     yaw 绕 +y 转，0 = 面朝 +x（所有老断言都在这条线上）。
     体坐标：fwd=(cos,sin)，left=(-sin,cos)；fwdBase/latBase 是体坐标原点的世界位置，
     转身时跟着转，参照线（中线/前向参考）才不会留在世界原点上。 */
  float yaw = 0, vYaw = 0;
  float vYawWant = 0.0f;        /* 想要的转向角速度（rad/s），由路线/意图给 */
  float turnAcc = 7.0f;         /* 角加速度上限 rad/s²：身体不会瞬间拧过去 */
  float fwdBase = 0.0f, latBase = 0.0f;
  bool standing = true;
  /* 意图模式：不按按钮，只给一个「想走多快」，站/走/跑由身体自己决定 */
  bool intentMode = false;
  float vDesWant = 0.0f;    /* 意图速度（未经脊髓反射折算） */
  float intentRunSpeed = 1.15f;
  int modeCode = 0;          /* 0 站 1 走 2 跑，复位时保留意图 */
  V3 lastPush;
  float boneErr = 0;
  std::vector<Box> boxes;      /* 场景里的方块（台阶/平台/墙） */
  float totalMass = 70.0f;     /* kg，和体模型同一张质量表 */
  float dtLast = 1.0f / 240.0f;
  float vxPrev = 0, vzPrev = 0, axPrev = 0, azPrev = 0;
  float y1 = 0, y2 = 0, ayPrev = 0;   /* 骨盆竖直方向的二阶差分（用来算 Fz 的起伏） */
  float prevAng[6] = {0, 0, 0, 0, 0, 0};   /* 上一帧关节角（算角速度用） */
  float grf[3] = {0, 0, 0};    /* 合地面反作用力（N），竖直分量平时≈体重 */
  float muGround = 0.8f;       /* 脚底摩擦系数（GRF 指标用） */
  float floorMu = 0.9f;        /* 地面摩擦：摔倒后滑多远取决于它（冰面 0.05） */
  float gy = 0;                /* 骨盆脚下的地面高度（平滑过） */
  Ragdoll rd;                 /* 体模型：摔倒之后由它接管 */
  MotionPlayer motion;        /* 动作通道：在物理结果之上叠一层姿态偏移 */
  std::vector<Motion> clips;  /* 从 soma.json 载入的动作剪辑（名字 → 关键帧） */
  float mass[18];             /* 各节点质量（kg），总质量 70 */
  float comY = 0;

  Walker() {
    Ragdoll::masses(mass);
    rd.ground = [this](float x, float z) { return groundAt(x, z); };
    reset();
  }

  float omega() const { return std::sqrt(P.g / P.hipH); }

  /* ---- 体坐标系 ----
     fwd = 她面朝的方向，left = 她的左手边。yaw=0 时就是老的 (x, z)，所以
     "没有转身"这条路上每一个算式都和以前一模一样 —— 老断言是回归网，不许动。
     重心位置 p/v 仍然是**世界坐标**（体模型、碰撞、渲染都在世界里）；
     只是落脚、ZMP、判摔这些"跟她自己有关的量"全部换算到体坐标里算。 */
  void axes(V3& fw, V3& lf) const {
    const float c = std::cos(yaw), s = std::sin(yaw);
    fw = V3{c, 0, s}; lf = V3{-s, 0, c};
  }
  float fwdOf(const V3& q) const { return q.x * std::cos(yaw) + q.z * std::sin(yaw) - fwdBase; }
  float latOf(const V3& q) const { return -q.x * std::sin(yaw) + q.z * std::cos(yaw) - latBase; }
  /* 体坐标 → 世界（y 直通） */
  V3 fromBody(float f, float l, float yy) const {
    const float c = std::cos(yaw), s = std::sin(yaw);
    const float F = f + fwdBase, L = l + latBase;
    return V3{F * c - L * s, yy, F * s + L * c};
  }
  /* 绕着重心把整个体坐标拧 dth：重心不动、速度跟着转、前向/侧向参考不动。
     转身时重心不该被"甩"出去 —— 甩出去的是脚，不是重心。 */
  void yawBy(float dth) {
    if (!std::isfinite(dth) || std::fabs(dth) < 1e-7f) return;
    const float pF = fwdOf(p), pL = latOf(p);
    const float c = std::cos(dth), s = std::sin(dth);
    v = V3{v.x * c - v.z * s, v.y, v.x * s + v.z * c};
    yaw += dth;
    /* yaw 收在 ±π（cos/sin 是周期函数，体坐标不受影响）：转一整天也不会掉精度 */
    if (yaw > 3.14159265f) yaw -= 6.28318531f;
    else if (yaw < -3.14159265f) yaw += 6.28318531f;
    fwdBase = p.x * std::cos(yaw) + p.z * std::sin(yaw) - pF;
    latBase = -p.x * std::sin(yaw) + p.z * std::cos(yaw) - pL;
  }

  /* 把整个世界相对她平移 dx：走过一整格（8 m）就把状态挪回来一格。
     格子和格子长得一模一样，所以这一步在画面上是看不见的 —— 以前是"闪现回起点"。
     摔倒时体模型的质量球也要一起挪，不然一爬起来就穿模。 */
  void shiftX(float dx) {
    p.x += dx; lastZmpX += dx;
    /* 体坐标原点跟着平移，xRef/latRef 这些体坐标量才不用动 */
    fwdBase += dx * std::cos(yaw);
    latBase -= dx * std::sin(yaw);
    for (int i = 0; i < 2; ++i) {
      feet[i].ank.x += dx; feet[i].plant.x += dx;
      feet[i].from.x += dx; feet[i].to.x += dx;
    }
    if (rd.active) { for (int i = 0; i < Ragdoll::N; ++i) { rd.x[i].x += dx; rd.xp[i].x += dx; } }
  }

  /* 把她整个放到世界的 (x,z)、朝 yawRad 方向。路线开始时用 ——
     不然她会从世界原点斜着穿过整个房间走到第一个航点上。 */
  void placeAt(float x, float z, float yawRad) {
    reset();
    yaw = yawRad; vYaw = 0.0f; vYawWant = 0.0f;
    p = V3{x, 0.0f, z};
    /* 让 fwdOf(p)=latOf(p)=0：体坐标原点就在她脚下 */
    fwdBase = x * std::cos(yaw) + z * std::sin(yaw);
    latBase = -x * std::sin(yaw) + z * std::cos(yaw);
    p.y = groundAt(x, z) + P.hipH;
    xRef = 0.0f; latRef = 0.0f;
    lastZmpX = x; lastZmpZ = z;
    for (int i = 0; i < 2; ++i) {
      const float sgn = (i == 0 ? 1.0f : -1.0f);
      V3 a = fromBody(0.0f, sgn * P.hipHalfW, 0.0f);
      a.y = groundAt(a.x, a.z) + P.ankleH;
      feet[i].ank = a; feet[i].plant = a; feet[i].from = a; feet[i].to = a;
      feet[i].contact = true; feet[i].swing = false;
    }
    standing = true;
  }

  /* 某个点脚下的地面高度：可踩的方块取最高的那个，踩不上去的（墙）当不存在 */
  float groundAt(float x, float z) const {
    float g = 0;
    for (const Box& b : boxes) if (!b.wall() && b.contains(x, z)) g = std::max(g, b.h);
    return g;
  }
  /* 点的水平投影是否落在墙里（走路时不许穿墙） */
  bool inWall(float x, float z) const {
    for (const Box& b : boxes) if (b.wall() && b.contains(x, z)) return true;
    return false;
  }

  void reset() {
    p = {0, P.hipH, 0};
    v = {0, 0, 0};
    t = 0; phase = 0; falls = 0; fallen = false;
    dutyAcc = dutyN = 0;
    yaw = vYaw = vYawWant = 0; fwdBase = latBase = 0;
    lastZmpX = lastZmpZ = 0;
    leanSmX = leanSmZ = 0;
    for (int i = 0; i < 2; ++i) {
      FootState& f = feet[i];
      f.id = i;
      float z = (i == 0 ? 1.0f : -1.0f) * P.hipHalfW;
      f.ank = {0, groundAt(0, z) + P.ankleH, z};
      f.plant = f.ank; f.from = f.ank; f.to = f.ank;
      f.swing = false; f.contact = true;
      f.t0 = f.t1 = 0;
    }
    standing = (modeCode == 0);
    xRef = fwdOf(p); vRef = 0;
    rd.stop();
    bio.reset();
    comY = 0;
    y1 = y2 = p.y; ayPrev = 0;
    vxPrev = vzPrev = axPrev = azPrev = 0;
    grf[0] = grf[1] = grf[2] = 0;
  }

  /* 一个完整周期（左右各一步）。纯按表走的话，速度快了身体会在一次支撑内跑出
     腿的可及范围，IK 只能把骨头压短（err 变大）。所以周期跟着速度收：
     v·(T/2) 不超过 ~0.40 m 的横向可及量。 */
  /* ---- 脊髓层：节律由 CPG 自己产生，不由 dt/cycleT 的外部计数器推 ---- */
  CPG cpg;
  bool spinal = true;
  float catchT = 0.0f;         /* 借步抢救期（>0 时判摔宽限） */          // 关掉 = 退回旧的相位计数器（给对照测试用）
  float cpgFreq() const { return 1.0f / std::max(0.20f, cycleT()); }

  /* 累了以后步态自己变样：步子变慢、抬脚变低。这是生物层压"脊髓输出"的地方，
     不是靠外面把目标速度调小 —— 速度是意图，步态是身体。 */
  float fatGain() const { return 1.0f - 0.45f * clampf(bio.fatAvg, 0.0f, 0.9f); }
  float heatGain() const { return bio.temp > 39.5f ? 0.80f : 1.0f; }
  /* 情绪和疼也压步态：多巴胺高步子轻快、皮质醇高步子拖、疼了会护着那条腿。
     这不是动画，是"身体状态 → 运动输出"的最后一米。 */
  float dopaGain() const { return 1.0f + 0.10f * clampf(bio.dopa - 0.35f, -0.35f, 0.65f); }
  float cortGain() const { return 1.0f - 0.16f * clampf(bio.cort - 0.20f, 0.0f, 0.80f); }
  float painGain() const { return 1.0f - 0.22f * clampf(bio.pain, 0.0f, 1.0f); }
  /* 饿到一定程度步子就发虚：不是"走得慢"，是每一步都短一点、软一点 */
  float lowSugarGain() const {
    /* 只有真饿了才改步态：血糖还够的时候不该莫名其妙变样，
       否则整段学习曲线都被这个"其实还没饿"的系数污染。 */
    const float h = clampf((bio.hunger - 0.45f) / 0.55f, 0.0f, 1.0f);
    return 1.0f - 0.16f * h;
  }

  float phaseOut() const { return phase; }
  float cycleT() const {
    float v = std::max(0.25f, std::fabs(vRef));
    /* 0.8/v 那条限制会把步子压得太短（0.85 m/s 只剩 0.4 m 一步），
       人走 0.85 m/s 大约是 0.5 m 一步、55 个循环/分钟。 */
    /* 真人表（mocap 34 段走路/跑步，见 mocap/gaitstats.txt）：
       周期在走路区间几乎是个常数 —— v 0.5~0.8 是 1.25 s（96 步/分），0.8~1.1 是 1.10 s，
       1.1~1.4 是 1.04 s。旧公式的 2*Tstep=1.12 s 只在 0.71 m/s 以上才起作用，
       慢走时周期偏快（1.095 s vs 真人 1.25 s）。高速那边保留 0.80/v：她的腿长
       （大腿 0.45 + 小腿 0.45、骨盆高 0.895）决定落脚点最远只能放到前方 0.29 m，
       步子再长 IK 只能压短骨头（实测步幅 1.05 m 时 err 0.012、1.41 m 时 0.096）。
       要让步子像真人那么长，得先改腿长（那是 v4 的事）。 */
    float T = std::min(1.30f - 0.20f * v, 0.80f / v);
    T /= std::max(0.35f, fatGain() * heatGain() * dopaGain() * cortGain() * painGain() * lowSugarGain());
    return std::max(0.30f, T);
  }
  float dutyNow() const { return P.run ? P.runDuty : P.duty; }

  /* 支撑脚 -> CoP 基准。双支撑期按“重心正在转移”的比例加权：
     刚落地那只脚的权重从 0 涨到 1，要起脚那只从 1 掉到 0。
     直接取两脚中点是不对的 —— 它会凭空产生一个把身体往回推的 CoP。 */
  /* 两只脚各承担多少体重：刚落地那只从 0 涨到 1，要起脚那只从 1 掉到 0 */
  void footWeights(float& w0, float& w1) const {
    const float d = dutyNow();
    const float ramp = std::max(0.06f, 2.0f * d - 1.0f);
    float w[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
      float q = phase - (i == 0 ? 0.0f : 0.5f);
      if (q < 0) q += 1.0f;
      if (q >= d) q = d - 1e-4f;
      w[i] = std::min(1.0f, q / ramp) * std::min(1.0f, (d - q) / ramp);
    }
    float sum = w[0] + w[1];
    if (sum < 1e-5f) {                 /* 单支撑：全给踩着的那只 */
      int k = 0;
      for (int i = 0; i < 2; ++i) if (feet[i].contact && !feet[i].swing) k = i;
      w[0] = (k == 0); w[1] = (k == 1);
      sum = 1;
    }
    w0 = w[0] / sum; w1 = w[1] / sum;
  }

  V3 supportCenter() const {
    const float d = dutyNow();
    const float ramp = std::max(0.06f, 2.0f * d - 1.0f);
    V3 acc{0, 0, 0}; float wsum = 0;
    for (int i = 0; i < 2; ++i) {
      if (!feet[i].contact || feet[i].swing) continue;
      float q = phase - (i == 0 ? 0.0f : 0.5f);
      if (q < 0) q += 1.0f;
      if (q >= d) q = d - 1e-4f;
      float w = std::min(1.0f, q / ramp) * std::min(1.0f, (d - q) / ramp);
      acc += feet[i].ank * std::max(w, 0.0f);
      wsum += std::max(w, 0.0f);
    }
    if (wsum < 1e-5f) {
      for (int i = 0; i < 2; ++i) if (feet[i].contact && !feet[i].swing) return feet[i].ank;
      return {p.x, groundAt(p.x, p.z) + P.ankleH, p.z};
    }
    return acc * (1.0f / wsum);
  }

  /* 支撑多边形：支撑脚踝的包围盒（简化，够用）。
     从"会转身"这一版起给的是**体坐标**范围（前 f / 左 l）：脚掌是跟身体一起转的，
     世界轴包围盒一转身就把脚算成斜的，CoP 会甩到脚掌外面去。 */
  void supportBox(float& f0, float& f1, float& l0, float& l1) const {
    f0 = 1e9f; f1 = -1e9f; l0 = 1e9f; l1 = -1e9f;
    int n = 0;
    for (int i = 0; i < 2; ++i) if (feet[i].contact && !feet[i].swing) {
      const float af = fwdOf(feet[i].ank), al = latOf(feet[i].ank);
      f0 = std::min(f0, af - P.footLen * 0.45f); f1 = std::max(f1, af + P.footLen * 0.45f);
      l0 = std::min(l0, al - P.footW * 0.5f);    l1 = std::max(l1, al + P.footW * 0.5f);
      n++;
    }
    if (!n) {
      const float pf = fwdOf(p), pl = latOf(p);
      f0 = pf - 0.05f; f1 = pf + 0.05f; l0 = pl - 0.05f; l1 = pl + 0.05f;
    }
  }

  /* 当前主要承重的脚（双支撑期取权重大的那只） */
  int bestFoot() const {
    const float d = dutyNow();
    float best = -1; int bi = 0;
    for (int i = 0; i < 2; ++i) {
      if (!feet[i].contact || feet[i].swing) continue;
      float q = phase - (i == 0 ? 0.0f : 0.5f); if (q < 0) q += 1.0f;
      float w = std::min(q, std::max(0.0f, d - q));
      if (w > best) { best = w; bi = i; }
    }
    return bi;
  }

  /* 摆动相目标落脚点：捕获点 + 速度补偿。全程在体坐标里算 ——
     她一转身，落脚点跟着转到新的前方/左手边，迈出去的就是弧线上的那一步。 */
  V3 footTarget(int i) const {
    float w = omega();
    V3 fw, lf; axes(fw, lf);
    const float pF = fwdOf(p), pL = latOf(p);
    const float vF = dot(v, fw), vL = dot(v, lf);
    float sign = (i == 0 ? 1.0f : -1.0f);
    /* 落脚点 = “这一脚落地时重心大概会到哪” + 速度误差修正：
       p + vRef*Tstep 是下一支撑期的重心预计位置，再按 (vRef-v) 微调。
       这样从静止起步时第一脚只迈一小步，不会一步跨 25cm 把自己蹬回去。 */
    float Tx = P.Tstep * (P.run ? 0.62f : 1.0f);
    /* 借步时把脚直接放到捕获点上（还会再限幅），不然 1.4 m/s 的侧推根本接不住 */
    float latK = (catchT > 0.0f) ? 1.0f : P.latK;
    const float capF = pF + vF / w, capL = pL + vL / w;
    float tgF = pF + vF * Tx * 0.5f + P.sagK * (vRef - vF) * Tx * 0.5f + P.leadBias;
    float tgL = sign * P.hipHalfW + latK * (capL - sign * P.hipHalfW);
    /* 侧向落脚不许贴到中线，也不许劈太大 */
    /* 侧向落脚上限从 2.6 抬到 4.2 髋宽（≈0.36 m）：原来最多只能把脚放到 0.22 m，
       捕获点横向漂到 0.5 m 时根本接不住，只能倒。 */
    float lmin = 0.055f, lmax = P.hipHalfW * 4.2f;
    float lm = std::fabs(tgL);
    lm = clampf(lm, lmin, lmax);
    tgL = (tgL >= 0 ? 1.0f : -1.0f) * lm;
    /* 别让落脚点超出腿够得着的范围（否则 IK 只能压短骨长） */
    float reachX = std::sqrt(std::max(0.0f, (P.thigh + P.shank) * (P.thigh + P.shank)
                        - (P.hipH - P.ankleH) * (P.hipH - P.ankleH))) - 0.03f;
    tgF = clampf(tgF, pF - reachX, pF + reachX);
    (void)capF;
    V3 tg = fromBody(tgF, tgL, 0.0f);
    tg.y = groundAt(tg.x, tg.z) + P.ankleH;
    return tg;
  }

  void startSwing(int i) {
    FootState& f = feet[i];
    f.from = f.ank;
    f.to = footTarget(i);
    f.t0 = t;
    f.t1 = t + ((catchT > 0.0f) ? 0.22f : (1.0f - dutyNow()) * cycleT());
    f.swing = true;
    f.contact = false;
    standing = false;
  }

  void step(float dt) {
    dtLast = dt;
    if (dt <= 0) return;
    t += dt;
    motion.step(dt);           /* 动作通道自己计时：物理怎么晃都不影响它走时间 */
    if (fallen) { stepFallen(dt); return; }

    /* ---- 朝向：想要的角速度按角加速度爬坡，再绕着重心把体坐标拧过去 ----
       拧的是"她自己的坐标系"，不是模型上的一层壳：下一秒的落脚点就落在新的前方，
       所以这是真转身（弧线走），不是把模型转过去糊弄镜头。 */
    {
      vYawWant = clampf(vYawWant, -2.0f, 2.0f);
      vYaw += clampf(vYawWant - vYaw, -turnAcc * dt, turnAcc * dt);
      if (std::fabs(vYawWant) < 1e-4f && std::fabs(vYaw) < 1e-3f) vYaw = 0.0f;
      yawBy(vYaw * dt);
    }

    /* ---- 意图 → 步态：没有「走路/跑步」按钮，只有「想走多快」 ---- */
    if (intentMode) {
      float vInt = clampf(P.vDes, 0.0f, 2.0f);
      bool wantRun = vInt > intentRunSpeed;
      if (vInt > 0.05f && standing) {
        standing = false; latRef = 0.0f; phase = 0.0f;
        P.run = wantRun; swingLift = P.run ? P.liftRun : P.liftWalk;
        modeCode = P.run ? 2 : 1;
      }
      if (!standing && wantRun != P.run) { P.run = wantRun; swingLift = P.run ? P.liftRun : P.liftWalk; }
      if (!standing) modeCode = P.run ? 2 : 1;
      if (vInt <= 0.05f && !standing) { P.run = false; modeCode = 0; }
    }
    const float w = omega();
    if (justStood > 0) { justStood -= dt; vRef = 0; xRef = p.x; }
    if (catchT > 0.0f) catchT = std::max(0.0f, catchT - dt);
    /* 速度指令爬坡：目标速度靠“每一脚落得更远”逐步达成，不靠瞬间推力 */
    {
      /* 疲劳/热应激会压速度：累到 0.7 时只剩 65%，体温过 39.5 再砍一半。
         这就是"身体"在管行为，而不只是画面上多一条读数。 */
      float fatK = 1.0f - 0.50f * clampf(bio.fatAvg, 0.0f, 0.9f);
      float heatK = (bio.temp > 39.5) ? 0.55f : 1.0f;
      float vWant = P.vDes * fatK * heatK;
      float dv = vWant - vRef;
      float lim = accLim * dt;
      vRef += clampf(dv, -lim * 1.6f, lim);
      if (std::fabs(vWant - vRef) < 1e-4f) vRef = vWant;
      xRef += vRef * dt;
      if (p.x - xRef > 0.30f) xRef = p.x - 0.30f;    /* 防积分饱和：参考别把重心甩太远 */
      if (xRef - p.x > 0.30f) xRef = p.x + 0.30f;
    }

    /* ---- 相位推进：两只脚交替，谁该抬脚由相位窗口决定 ---- */
    float d = dutyNow();
    if (!standing) {
      if (spinal) {
        /* 节律来自 CPG：两条腿各自一个振荡器，耦合维持反相。
           相位不是"过了多少秒"，是动力学自己跑出来的 —— 身体一动，
           相位就被带着走（下面触地时还会重同步）。 */
        cpg.intent(cpgFreq(), 1.0f);
        cpg.step(dt);
        /* 相位从 CPG 读回来：动作层（手臂、脊髓）拿它当"第几帧"。
           以前这里只推进了 cpg，phase 一直停在 0 —— 手臂就永远是同一帧。 */
        phase = cpg.ph[0] / 6.28318531f;
        if (phase < 0) phase += 1.0f;
        if (phase >= 1.0f) phase -= 1.0f;
      } else {
        phase += dt / cycleT();
        if (phase >= 1.0f) phase -= 1.0f;
      }
      for (int i = 0; i < 2; ++i) {
        /* 每只脚自己的周期进度：CPG 给的就是"这条腿的相位" */
        float q;
        if (spinal) {
          q = cpg.ph[i] / 6.28318531f;
          q -= std::floor(q);
        } else {
          float off = (i == 0 ? 0.0f : 0.5f);
          q = phase - off; if (q < 0) q += 1.0f;
        }
        if (i == 0 && spinal) phase = q;
        bool shouldStance = (q < d);
        FootState& f = feet[i];
        if (shouldStance && f.swing && !f.contact) {
          /* 该落地了：收尾（迟到的摆动直接吸附到目标） */
          f.ank = f.to; f.plant = f.to; f.swing = false; f.contact = true;
          /* 触地重同步：这是脊髓机制，不是补丁 —— 脚一沾地，
             那条腿的振荡器就被拉回支撑相起点，节律永远咬住身体，
             而不是咬住一个和身体无关的秒表。 */
          if (spinal) cpg.ph[i] = 0.0f;
        } else if (!shouldStance && !f.swing) {
          startSwing(i);
        }
      }
    } else {
      phase = 0;
      if (spinal) { cpg.intent(cpgFreq(), 0.0f); cpg.step(dt); }
    }

    /* ---- 摆动相轨迹：抛物线抬脚 + 直线前进 ---- */
    for (int i = 0; i < 2; ++i) {
      FootState& f = feet[i];
      if (!f.swing) continue;
      float s = (t - f.t0) / std::max(1e-4f, (f.t1 - f.t0));
      s = clampf(s, 0.0f, 1.0f);
      f.to = footTarget(i);          /* 摆动相每帧重规划：捕获点会随着身体漂移，落脚点必须跟着改 */
      V3 lin = lerp(f.from, f.to, s);
      float lift = swingLift * fatGain() * std::sin(3.14159265f * s);   /* 腿沉了抬不高 */
      /* 台阶：起脚和落脚的地面高度不同，摆动中段线性过渡。
         但如果路径中间有更高的东西（台阶边缘/矮坎），线性插值会让脚直接穿过去 ——
         所以再沿路径采样取最高地面，让脚"从上面跨过去"。 */
      float gA = groundAt(f.from.x, f.from.z), gB = groundAt(f.to.x, f.to.z);
      float gL = gA + (gB - gA) * s;
      {
        const int N = 5;
        float gMax = gL;
        for (int k = 0; k <= N; ++k) {
          float sk = (float)k / N;
          float gx = f.from.x + (f.to.x - f.from.x) * sk;
          float gz = f.from.z + (f.to.z - f.from.z) * sk;
          gMax = std::max(gMax, groundAt(gx, gz));
        }
        /* 只在后半程才抬到坎上，前半程还在起脚侧 —— 用 sin 包络平滑过渡 */
        gL = gL + (gMax - gL) * std::sin(3.14159265f * s);
      }
      f.ank = {lin.x, gL + P.ankleH + lift, lin.z};
      f.contact = false;
    }

    /* ---- ZMP：支撑中心 + 踝策略（限幅在支撑多边形内） ---- */
    V3 cen = supportCenter();
    const float w2 = w * w;
    /* 踝策略（真正的 ZMP 控制律）：ẍ = ω²(p - zmp)，想让 p 收敛到参考点，
       就取 zmp = p + (kp·(p-ref) + kd·v)/ω²。
       侧向参考永远在中线（ref.l=0），前后参考就是“踩在脚下”的支撑中心。 */
    /* 前后：CoP 在脚掌内前后扫 —— 着地时压在脚跟（刹车），离地前压到脚趾（蹬地）。
       这一前一后就是步行的推进力来源，没有它只能原地踏步。
       和侧向用同一套“参考跟踪”律，参考点 xRef 以指令速度平移：
       前后是二阶伺服（不会被一步跨太大蹬回去），侧向仍是真倒立摆动力学。 */
    /* 体坐标：前/左各一份，ZMP 也在这里算完再回世界 —— 转身时 CoP 才不会横穿脚掌 */
    V3 fw, lf; axes(fw, lf);
    const float pF = fwdOf(p), pL = latOf(p);
    const float vF = dot(v, fw), vL = dot(v, lf);
    float zmpF = pF;
    if (modeCode != 0) {                        /* 蹬地/脚跟制动：只有已经在走才给，站着不推 */
      float q = phase - (bestFoot() == 0 ? 0.0f : 0.5f);
      if (q < 0) q += 1.0f;
      const float u = clampf(q / dutyNow(), 0.0f, 1.0f);
      const float gate = clampf((vF - 0.05f) / 0.30f, 0.0f, 1.0f);
      zmpF += gate * (P.toePush * std::pow(clampf((u - 0.6f) / 0.4f, 0.0f, 1.0f), 2.0f)
                    - P.heelBrake * std::cos(3.14159265f * std::min(1.0f, u * 2.2f)));
    }
    /* 侧向：参照中线，用踝策略主动收敛（脚掌窄，必须靠 CoP 横向挪动来稳） */
    float zmpL = pL + (P.ankleKp * P.latStiff * (pL - latRef) + P.ankleKd * P.ankleKp * vL) / w2;
    float b0, b1, c0, c1; supportBox(b0, b1, c0, c1);
    zmpF = clampf(zmpF, b0, b1);
    zmpL = clampf(zmpL, c0, c1);
    V3 zmp = fromBody(zmpF, zmpL, p.y);
    lastZmpX = zmp.x; lastZmpZ = zmp.z;

    /* ---- 前后：速度伺服（与 2D 同款，稳、速度精确）；侧向：真倒立摆动力学 ---- */
    vPush *= std::exp(-dt / 0.38f);            /* 推力脉冲衰减 */
    const float vFr = vRef + vPush;            /* 前向 = 速度伺服（沿她面朝的方向） */
    float az = w * w * (pL - zmpL);
    /* 髋/踝联合策略：关节力矩有限，给侧向加一段饱和的 PD 恢复力（物理上就是
       “脚掌扭一下 + 髋往外顶”，不是无限大的外力）。没有它，CoP 一旦被钳在脚掌
       内、重心跑出脚掌就再也没有恢复力矩了。 */
    /* 借步冲刺期允许更大的髋/踝发力（那是"拼命救"的阶段，不是常态） */
    const float hipMax = (catchT > 0.0f) ? 7.0f : 3.0f;
    float hip = -(P.hipKp * (pL - latRef) + P.hipKd * vL);
    az += clampf(hip, -hipMax, hipMax);
    const float vL2 = vL + az * dt;
    /* 前/侧向合成回世界速度：v.y 不碰（推力脉冲只在水平面上） */
    v.x = fw.x * vFr + lf.x * vL2;
    v.z = fw.z * vFr + lf.z * vL2;
    p.x += v.x * dt;
    p.z += v.z * dt;
    {   /* 骨盆跟着脚下地面抬。必须限速：上台阶那一瞬如果骨盆猛抬，
           后脚还踩在低处，那条腿会被拉到 0.9 m 之外（IK 只能把骨拉长）。 */
      float gT = groundAt(p.x, p.z);
      float dg = clampf(gT - gy, -0.60f * dt, 0.22f * dt);
      gy += dg;
      p.y = gy + P.hipH + P.bounce * std::cos(4.0f * 3.14159265f * phase);
    }
    y = p.y;

    /* ---- 摔倒判定：捕获点跑出可达范围 ---- */
    V3 cap{p.x + v.x / w, 0, p.z + v.z / w};
    /* 判摔要相对支撑中心，不能拿绝对坐标 —— 人往前走 2 m，|cap.x| 必然超过 1.6，
       以前那版这么写，靠的是"累加器每帧被清零"才没乱摔（也就从来没摔过）。
       阈值也换成体坐标：0.45 是"横向出脚"，0.90 是"前后出脚"，她转身时不变。 */
    const float capF2 = fwdOf(cap) - fwdOf(cen), capL2 = latOf(cap) - latOf(cen);
    const float vFBody = dot(v, V3{std::cos(yaw), 0.0f, std::sin(yaw)});   /* 只给日志看 */
    float dxc = capF2, dzc = capL2;
    bool lost = std::fabs(dzc) > 0.55f || std::fabs(dxc) > 0.95f || std::fabs(vL2) > 2.6f;
    lostT = lost ? lostT + dt : 0.0f;          /* 瞬时越界不算摔：要连续 0.15s 才算 */
    /* 借步抢救期内宽限：捕获点跑出去 + 一条腿正在朝那边落 = 正在救，不是已经倒了。
       抢救期结束（0.6 s）还不稳，照样判摔。 */
    {
      float thr = (catchT > 0.0f) ? 0.60f : 0.15f;
      if (lostT > thr && !fallen && justStood <= 0) {
        fallen = true; falls++;
        fprintf(stderr, "[fall] t=%.1f capF=%.2f capL=%.2f vL=%.2f vF=%.2f spd=%.2f yaw=%.2f vYaw=%.2f catch=%.2f v=(%.2f,%.2f) p=(%.2f,%.2f)\n",
                t, capF2, capL2, vL2, vFBody, vRef, yaw, vYaw, catchT, v.x, v.z, p.x, p.z);
      }
    }
    /* 注意：这里以前还有一行 if(!fallen) lostT=0; —— 它会让累加器永远归零，
       摔倒判定从来没被触发过，只有 |p.z|>5 那条兜底的 nan 检查偶尔生效。已删。 */

    /* 墙：身子到了墙边就停住（脚会被落点规划带着走，v 清零但不判摔） */
    /* 墙：身子进了墙里就往**最近的出口**推出去。
       原来只往 -x 推 —— 那是"世界只有一堵 x 墙"的旧假设；转身之后她可能面朝任何方向，
       沿墙走的时候被硬拽 x 会瞬间把捕获点甩出去，接着就判摔（实测摔过一次）。
       现在按体坐标前后左右四个方向找第一个不撞墙的方向，人和脚一起挪。 */
    if (inWall(p.x, p.z)) {
      V3 fw, lf; axes(fw, lf);
      const float steps[4] = { -1.0f, 1.0f, -1.0f, 1.0f };   /* -前 前 -左 左 */
      V3 out = p; bool got = false;
      for (float k = 0.02f; k <= 0.42f && !got; k += 0.02f) {
        for (int i = 0; i < 4; ++i) {
          const V3 dir = (i < 2) ? fw : lf;
          V3 q{ p.x + dir.x * steps[i] * k, p.y, p.z + dir.z * steps[i] * k };
          if (!inWall(q.x, q.z)) { out = q; got = true; break; }
        }
      }
      const float dx = out.x - p.x, dz = out.z - p.z;
      p.x = out.x; p.z = out.z;
      for (int i = 0; i < 2; ++i) {
        feet[i].ank.x += dx; feet[i].ank.z += dz;
        feet[i].from.x += dx; feet[i].from.z += dz;
        feet[i].to.x += dx; feet[i].to.z += dz;
        feet[i].plant.x += dx; feet[i].plant.z += dz;
      }
      v.x = 0; v.z = 0; vPush = 0;
      latRef = latOf(p); xRef = fwdOf(p); vRef = 0;
    }

    /* 世界边界：走了 40 m 就把整条腿的历史一起平移回来，不算摔 */
    if (std::isfinite(p.x) && p.x > 40.0f) {
      /* 整具身子平移 40 m：人和脚一起挪，体坐标基准按新位置重算（否则 fwdOf/latOf 会跳） */
      const float pF = fwdOf(p), pL = latOf(p);
      p.x -= 40.0f;
      for (int i = 0; i < 2; ++i) {
        feet[i].ank.x -= 40.0f; feet[i].from.x -= 40.0f; feet[i].to.x -= 40.0f; feet[i].plant.x -= 40.0f;
      }
      lastZmpX -= 40.0f;
      const float c2 = std::cos(yaw), s2 = std::sin(yaw);
      fwdBase = p.x * c2 + p.z * s2 - pF;
      latBase = -p.x * s2 + p.z * c2 - pL;
      xRef = pF; latRef = pL;
    }
    if (!std::isfinite(p.x) || !std::isfinite(p.z) || !std::isfinite(v.x) || !std::isfinite(v.z) ||
        std::fabs(p.z) > 5.0f) {
      falls++; fallen = true; { int fs = falls; reset(); falls = fs; }
    }

    /* ---- 生物层：先算这一帧的几何/反作用力，再映射到肌肉 ---- */
    /* （放在 GRF 之前会用到上一帧的 grf，所以放到后面统一更新，见 step 末尾） */

    /* ---- 地面反作用力：F = m·a（a 是这一步的加速度），按支撑权重分给两只脚 ---- */
    {
      float axNow = (v.x - vxPrev) / dt;
      float azNow = az;
      /* 竖直：模型里 CoM 起伏是运动学的（bounce 项），但渲染出来就是真在起伏，
         所以照样取二阶差分当 a_y —— 步行时 Fz 才会像真人那样在 1±0.2 体重之间晃。 */
      float ayNow = clampf((p.y - 2.0f * y1 + y2) / (dt * dt), -P.g * 0.8f, P.g * 1.2f);
      y2 = y1; y1 = p.y;
      ayPrev = ayPrev + (ayNow - ayPrev) * 0.35f;
      axPrev = axPrev + (axNow - axPrev) * 0.25f;      /* 轻微低通，免得单帧噪声吓人 */
      azPrev = azPrev + (azNow - azPrev) * 0.25f;
      vxPrev = v.x; vzPrev = v.z;
      grf[0] = totalMass * axPrev;
      grf[1] = totalMass * (P.g + std::max(-P.g * 0.9f, ayPrev));
      grf[2] = totalMass * azPrev;
      float w0, w1; footWeights(w0, w1);
      bool anyContact = feet[0].contact || feet[1].contact;
      float k = anyContact ? 1.0f : 0.0f;
      /* 腾空相没有反作用力 */
      grf[0] *= k; grf[1] *= k; grf[2] *= k;
    }

    /* 急停：光速度掉下来不够 —— 侧向还在摆（v.z 能到 0.3），这时候把相位停住
       就等于让重心带着侧向速度飞出去，CoP 一被钳进脚掌就再也拉不回来。
       要等侧向速度过零、重心落回支撑范围内，才算真正站定。 */
    /* 站着被推歪了：别硬撑 —— 迈一步把重心接回来。人是这么干的，
       不迈步光靠踝/髋，重心一出脚掌就必倒（CoP 钳不到脚外）。 */
    if (modeCode == 0 && standing) {
      V3 cc = supportCenter();
      /* 站着的时候压力中心就是两脚之间的中点：把 ZMP 钉在这儿，
         不然它会停在最后一步的旧值上，躯干就会一直朝后仰着（视觉上像要倒）。 */
      lastZmpX = cc.x; lastZmpZ = cc.z;
      /* 体坐标：偏了多少、往哪边偏，都相对"她自己的左手边"量 —— 转身之后照样成立 */
      V3 fw, lf; axes(fw, lf);
      const float vLc = dot(v, lf);
      const float dLc = latOf(p) - latOf(cc);
      if (std::fabs(dLc) > 0.13f || std::fabs(vLc) > 0.35f) {
        standing = false;
        latRef = 0.0f;
        stopGateT = 0.0f;
        /* 借步是惊吓反应：节律立刻上电，并且直接发到"要迈的那条腿" */
        cpg.catchPhase((vLc >= 0.0f || dLc >= 0.0f) ? 0 : 1, dutyNow());
        catchT = 0.6f;
      }
    }
    /* 站定后把两只脚慢慢收回中立站姿：不是"冻结在最后一步的位置"，
       那样停下来会像被人按了暂停键（脚还一前一后）。 */
    if (std::getenv("SOMA_DBG") && modeCode == 0 && standing && ((int)(t*2)%2==0)) {
      static int dbgN = 0;
      if (dbgN++ % 60 == 0)
        std::fprintf(stderr, "[dbg] standing p.x=%.3f feet=%.3f/%.3f sw=%d/%d catchT=%.2f err=%.3f\n",
                     p.x, feet[0].ank.x, feet[1].ank.x, (int)feet[0].swing, (int)feet[1].swing, catchT,
                     std::fabs(feet[0].ank.x - p.x));
    }
    if (modeCode == 0 && standing && catchT <= 0.0f) {
      for (int i = 0; i < 2; ++i) {
        FootState& f = feet[i];
        if (f.swing) continue;      /* 借步/起步时还在迈腿，别抢它的脚 */
        float sign = (i == 0 ? 1.0f : -1.0f);
        /* 中立站姿也走体坐标：人站着转身，脚就跟着挪到新的左右两侧 */
        V3 want = fromBody(fwdOf(p) + 0.02f, sign * P.hipHalfW, 0.0f);
        want.y = groundAt(want.x, want.z) + P.ankleH;
        float k = 1.0f - std::exp(-dt * 2.5f);
        f.ank.x += (want.x - f.ank.x) * k;
        f.ank.y += (want.y - f.ank.y) * k;
        f.ank.z += (want.z - f.ank.z) * k;
        f.contact = true;
      }
    }
    if (modeCode == 0 && !standing) {
      stopGateT += dt;
      V3 cc = supportCenter();
      V3 fw, lf; axes(fw, lf);
      const float vFb = dot(v, fw), vLb = dot(v, lf);
      const float pFb = fwdOf(p), pLb = latOf(p);
      bool settled = std::fabs(vFb) < 0.20f && std::fabs(vLb) < 0.10f &&
                     std::fabs(pLb - latOf(cc)) < 0.12f;
      if (settled || stopGateT > 3.0f) {
        standing = true; phase = 0.0f;
        v.x = 0.0f; v.z = 0.0f;
        latRef = latOf(cc);     /* 把支撑中点当侧向目标，别硬拉回绝对中线 */
        xRef = fwdOf(cc);       /* 前后也一样：站定后重心应该落在两脚中间，不是留在最后一步的位置 */
        const float nf = pFb, nl = latOf(cc) + clampf(pLb - latOf(cc), -0.06f, 0.06f);
        V3 q = fromBody(nf, nl, p.y);
        p.x = q.x; p.z = q.z;
      }
    }

    /* ---- 躯干倾角：只跟"重心相对压力中心"的慢变化 ----
       ZMP 每换一只承重脚就从后脚跳到前脚（前后差到 0.35 m），倾角被钳在 ±0.12 m，
       于是一步一跳、方向还来回反 —— 画面上就是"上半身每走一步抖一下"，手臂跟着甩。
       人的躯干没这么快，用 0.25 s 跟一下，抖动变成自然的前后随动。 */
    {
      const float kk = (dt > 0) ? std::min(1.0f, dt / 0.25f) : 1.0f;
      const float tx = (fwdOf(p) - fwdOf(V3{lastZmpX, 0.0f, lastZmpZ})) * 0.35f;
      const float tz = (latOf(p) - latOf(V3{lastZmpX, 0.0f, lastZmpZ})) * 0.75f;
      leanSmX += (clampf(tx, -0.12f, 0.12f) - leanSmX) * kk;
      leanSmZ += (clampf(tz, -0.10f, 0.10f) - leanSmZ) * kk;
    }

    /* ---- 生物层：关节力矩 → 肌肉激活 → 耗能/疲劳/体温 ---- */
    {
      Frame bf; pose(bf);
      float tq[6] = {0,0,0,0,0,0}, wq[6] = {0,0,0,0,0,0};   /* 必须清零：以前漏写会吃栈垃圾，再经疲劳反馈放大成不确定性 */
      computeTorque(bf, tq, wq);
      bio.bodyMass = totalMass;
      bio.step(dt, tq, wq);
      bio.tempStep(dt);
      bio.slowStep(dt);          /* 血糖 / 水 / 清醒度：小时级的账，每秒记一笔 */
    }

    /* ---- 指标 ---- */
    dutyN += dt;
    if (feet[0].contact || feet[1].contact) dutyAcc += dt;
  }

  /* ---- 骨架正解：18 节点 ---- */
  void pose(Frame& fr) {
    fr.metab = bio.metabTotal; fr.fat = bio.fatAvg; fr.temp = (float)bio.temp;
    if (rd.active) { poseRagdoll(fr); return; }
    float c = std::cos(yaw), s = std::sin(yaw);
    /* 前向/侧向分量：所有"相对她自己"的判断都用这两个，不用世界 x/z */
    const float vFw = v.x * c + v.z * s, vLw = -v.x * s + v.z * c;
    auto rot = [&](V3 q) { return V3{q.x * c - q.z * s, q.y, q.x * s + q.z * c}; };
    auto W = [&](V3 base, V3 off) { return base + rot(off); };

    V3 pelvis{p.x, p.y, p.z};
    /* 躯干倾斜：跟着 (p - zmp) 走，纯视觉 */
    /* 倾斜也在体坐标里：往她自己面朝的方向倒 / 往她左手边倒。
       用差值投影，所以 p 那一项自动抵消，yaw=0 时和原来逐位相同。
       值来自 step() 里平滑过的 leanSm —— 这里不再直接读 ZMP，否则一步一跳。 */
    V3 lean{leanSmX, 0, leanSmZ};
    V3 chest = W(pelvis, {lean.x, P.torso1, lean.z});
    V3 neck = W(chest, {lean.x * 0.5f, P.torso2, lean.z * 0.5f});
    /* 头是稳的：胸前怎么晃，颈子反着补一点，眼睛才不至于跟着上下摇 */
    V3 head = W(neck, {-lean.x * 0.35f, P.neckLen + P.headR * 0.2f, -lean.z * 0.35f});

    fr.nodes[0] = pelvis;
    fr.nodes[1] = chest;
    fr.nodes[2] = neck;
    fr.nodes[3] = head;

    /* 动作通道（motion clip）在手臂之前叠：手臂最后由模型接管，
       放在后面会把模型的结果盖掉 —— 之前就是这里，让手看起来死在一个固定姿势上。 */
    if (!fallen) motion.applyTo(fr.nodes);

    /* 肩 / 臂
       摆臂不再"套一条正弦"：直接取同侧脚相对骨盆的前后位置，取反就是手臂该去的地方。
       脚在前面 → 那只手在后面。这是从姿态里读出来的，不是编排出来的动作。 */
    for (int i = 0; i < 2; ++i) {
      float sign = (i == 0 ? 1.0f : -1.0f);
      int base = 4 + i * 3;
      V3 sh = W(chest, {0, 0.02f, sign * P.shoulderHalfW});
      /* 摆臂跟步态周期走：phase 就是"这一帧在整步里的第几帧"。
         不去读脚的位置（脚在体坐标系里，减去 p 会抵消成 0），也不套手势。
         注意别写成 fmod(t / cycleT(), 1)：t 是几百秒，cycleT 一变，相位就跳掉半圈。 */
      const float cph = phase;
      const float vAbs = std::fmax(std::fabs(vFw), std::fmax(std::fabs(P.vDes), std::fabs(vDesWant)));
      const float vk = clampf(vAbs / 0.5f, 0.0f, 1.0f);
      const float sw = std::sin(6.2831853f * (cph + (i == 0 ? 0.0f : 0.5f))) * vk;
      /* 走路时肘微屈；停下时手臂自然下垂，不摆 */
      float elb = 0.18f + 0.22f * clampf(std::fabs(P.vDes) / 0.9f, 0.0f, 1.0f);
      V3 elbowPt = W(sh, {sw * P.armAmp * 0.22f, -P.upperArm * 0.92f, sign * 0.02f});
      V3 handPt = W(sh, {sw * P.armAmp * 0.42f - elb * 0.12f,
                         -P.upperArm - P.foreArm * (1.0f - elb * 0.55f), sign * 0.03f});
      /* 肩角整体抬高：绕肩关节、在矢状面里转（armLift 是身体参数） */
      if (P.armLift != 0.0f) {
        const float cl = std::cos(P.armLift), sl = std::sin(P.armLift);
        auto lift = [&](const V3& q) {
          const float dxq = q.x - sh.x, dyq = q.y - sh.y;
          return V3{sh.x + dxq * cl - dyq * sl, sh.y + dxq * sl + dyq * cl, q.z};
        };
        elbowPt = lift(elbowPt);
        handPt = lift(handPt);
      }
      fr.nodes[base] = sh;
      /* 手臂外部接管：模型给的偏移（相对肩，米）直接落进来。
         放在 walker 里，任何一条产出 Frame 的路径都会带上它。 */
      const float* ov = P.armOv[i];
      if (ov[0] != 0.0f || ov[1] != 0.0f || ov[2] != 0.0f ||
          ov[3] != 0.0f || ov[4] != 0.0f || ov[5] != 0.0f) {
        /* ov 是体坐标（模型按"往前/往上/往左"学出来的），落位必须跟 yaw 一起转。
           以前直接加到世界坐标上：人一转，手臂就甩到背后去了 —— 就是这个 bug。 */
        const V3 eo = rot(V3{ov[0], ov[1], ov[2]});
        const V3 ho = rot(V3{ov[3], ov[4], ov[5]});
        elbowPt = V3{sh.x + eo.x, sh.y + eo.y, sh.z + eo.z};
        handPt  = V3{sh.x + ho.x, sh.y + ho.y, sh.z + ho.z};
      }
      fr.nodes[base + 1] = elbowPt;
      fr.nodes[base + 2] = handPt;
    }

    /* 腿：2-link 解析 IK，膝朝向 +x */
    for (int i = 0; i < 2; ++i) {
      float sign = (i == 0 ? 1.0f : -1.0f);
      int base = 10 + i * 4;
      /* 骨盆侧倾（Trendelenburg）：摆动侧那半边胯会沉下去一点。
         纯视觉，但"两胯一样高"是纸片人和活人最大的差别之一。 */
      float ql = phase - (i == 0 ? 0.0f : 0.5f); if (ql < 0) ql += 1.0f;
      float hipDip = (ql < dutyNow()) ? 0.0f : -0.016f * std::sin(3.14159265f * (ql - dutyNow()) / std::max(1e-3f, 1.0f - dutyNow()));
      V3 hip = W(pelvis, {0, hipDip, sign * P.hipHalfW});
      V3 ank = feet[i].ank;
      V3 pole = rot(V3{1, 0, 0});
      V3 d = ank - hip;
      float dist = len(d);
      float L1 = P.thigh, L2 = P.shank;
      dist = clampf(dist, std::fabs(L1 - L2) + 1e-3f, L1 + L2 - 1e-3f);
      V3 u = norm(d);
      float a = (L1 * L1 - L2 * L2 + dist * dist) / (2 * dist);
      float hgt = std::sqrt(std::max(0.0f, L1 * L1 - a * a));
      V3 perp = pole - u * dot(pole, u);
      if (len(perp) < 1e-5f) perp = V3{1, 0, 0};
      perp = norm(perp);
      V3 knee = hip + u * a + perp * hgt;
      /* 脚掌滚动：脚不是一块平板，它绕踝关节前后翻。
         着地：脚尖先翘起来（脚跟着地），压平；离地：脚跟抬起、压到脚趾蹬出去。
         没有这一下，看的人第一眼就会说"走路姿势不对"。 */
      float qf = phase - (i == 0 ? 0.0f : 0.5f); if (qf < 0) qf += 1.0f;
      float pitch;                       /* 正值＝脚跟抬、脚尖压地 */
      if (!feet[i].swing && feet[i].contact) {
        float u = clampf(qf / std::max(1e-3f, dutyNow()), 0.0f, 1.0f);
        float heel = 1.0f - clampf(u / 0.22f, 0.0f, 1.0f);          /* 落地瞬间脚跟先吃 */
        float toe = clampf((u - 0.55f) / 0.45f, 0.0f, 1.0f);        /* 离地前压脚趾 */
        pitch = 0.30f * toe * toe - 0.22f * heel;
      } else {
        float sp = (qf - dutyNow()) / std::max(1e-3f, 1.0f - dutyNow());
        sp = clampf(sp, 0.0f, 1.0f);
        /* 离地时还是蹬地的角度，到落地前收回脚尖朝上 */
        pitch = 0.30f * (1.0f - sp) * (1.0f - sp) - 0.22f * sp * sp;
      }
      pitch *= clampf(std::fabs(vFw) / 0.35f, 0.0f, 1.0f);          /* 站着不翻脚 */
      float cp = std::cos(pitch), sp2 = std::sin(pitch);
      float fx = P.footLen * 0.55f, fy = -P.ankleH;
      V3 toe = ank + rot(V3{fx * cp + fy * sp2, fx * sp2 + fy * cp, 0});
      fr.nodes[base] = hip;
      fr.nodes[base + 1] = knee;
      fr.nodes[base + 2] = ank;
      fr.nodes[base + 3] = toe;
    }

    static const float R[18] = {0.085f, 0.10f, 0.055f, 0.095f, 0.045f, 0.038f, 0.032f,
                                0.045f, 0.038f, 0.032f, 0.070f, 0.055f, 0.042f, 0.040f,
                                0.070f, 0.055f, 0.042f, 0.040f};
    for (int i = 0; i < 18; ++i) {
      fr.radii[i] = R[i];
      fr.contact[i] = 0;
    }
    fr.contact[13] = feet[0].contact ? 1 : 0;
    fr.contact[17] = feet[1].contact ? 1 : 0;


    fr.t = t;
    fr.speed = vFw;                          /* 前向速度：转身不改变"走多快" */
    fr.yaw = yaw; fr.vYawOut = vYaw;
    fr.comX = p.x; fr.comZ = p.z;
    fr.capX = p.x + v.x / omega();
    fr.capZ = p.z + v.z / omega();
    fr.zmpX = lastZmpX; fr.zmpZ = lastZmpZ;
    fr.leanX = lean.x; fr.leanZ = lean.z;
    fr.phase = phase;
    fr.falls = falls;
    fr.mode = (unsigned char)(P.run ? 2 : (standing ? 0 : 1));
    fr.fallen = 0;
    {   /* 质心高度：按节点质量加权（体模型的质量表） */
      float sw = 0, sy = 0;
      for (int i = 0; i < 18; ++i) { sw += mass[i]; sy += mass[i] * fr.nodes[i].y; }
      comY = sw > 0 ? sy / sw : 0;
      fr.comY = comY;
    }
    fr.dutyMeas = dutyN > 0 ? dutyAcc / dutyN : 0;
    {
      float w0, w1; footWeights(w0, w1);
      for (int k = 0; k < 3; ++k) {
        fr.grfL[k] = grf[k] * w0;
        fr.grfR[k] = grf[k] * w1;
      }
      fr.copX = lastZmpX; fr.copZ = lastZmpZ;
      float fh = std::sqrt(grf[0] * grf[0] + grf[2] * grf[2]);
      fr.frictionUse = grf[1] > 1.0f ? fh / (muGround * grf[1]) : 0.0f;
    }

    /* 骨长残差（自检用） */
    static const int B[17][2] = {{0,1},{1,2},{2,3},{1,4},{4,5},{5,6},{1,7},{7,8},{8,9},
                                 {0,10},{10,11},{11,12},{12,13},{0,14},{14,15},{15,16},{16,17}};
    boneErr = 0;
    for (int i = 0; i < 17; ++i) {
      fr.bones[i][0] = B[i][0]; fr.bones[i][1] = B[i][1];
      float dd = len(fr.nodes[B[i][1]] - fr.nodes[B[i][0]]);
      float ref;
      switch (i) {
        case 0: ref = P.torso1; break;
        case 1: ref = P.torso2; break;
        case 2: ref = P.neckLen + P.headR * 0.2f; break;
        case 3: case 6: ref = P.shoulderHalfW; break;
        case 4: case 7: ref = P.upperArm * 0.45f; break;
        case 5: case 8: ref = P.upperArm * 0.45f; break;
        case 9: case 13: ref = P.hipHalfW; break;
        case 10: case 14: ref = P.thigh; break;
        case 11: case 15: ref = P.shank; break;
        case 12: case 16: ref = std::sqrt(std::pow(P.footLen * 0.55f, 2) + P.ankleH * P.ankleH); break;
        default: ref = 1;
      }
      if (i >= 10 && i != 12 && i != 16) boneErr = std::max(boneErr, std::fabs(dd - ref));
    }
    fr.err = boneErr;
  }

  /* ---- 起身：给体模型一个"站姿目标"，靠肌肉弹簧把自己拽起来 ---- */
  bool gettingUp = false;
  float getupT = 0;
  float justStood = 0;   /* 起身后的缓冲时间，判摔暂时豁免 */
  V3 standTgt[18];

  void buildStandTarget() {
    const float gy = groundAt(p.x, p.z);
    V3 pel{p.x, gy + P.hipH, p.z};
    standTgt[0] = pel;
    standTgt[1] = pel + V3{0, rd.blen[0], 0};
    standTgt[2] = standTgt[1] + V3{0, rd.blen[1], 0};
    standTgt[3] = standTgt[2] + V3{0, rd.blen[2], 0};
    for (int i = 0; i < 2; ++i) {
      const float sg = i == 0 ? 1.0f : -1.0f;
      int sh = 4 + i * 3, el = sh + 1, hd = sh + 2;
      standTgt[sh] = standTgt[1] + V3{0, 0, sg * rd.blen[3 + i * 3]};
      standTgt[el] = standTgt[sh] + V3{0, -rd.blen[4 + i * 3], 0};
      standTgt[hd] = standTgt[el] + V3{0, -rd.blen[5 + i * 3], 0};
      int hip = 10 + i * 4, kn = hip + 1, an = hip + 2, to = hip + 3;
      standTgt[hip] = pel + V3{0, 0, sg * rd.blen[9]};
      standTgt[kn] = standTgt[hip] + V3{0.02f, -rd.blen[10 + i * 4], 0};
      standTgt[an] = standTgt[kn] + V3{-0.02f, -rd.blen[11 + i * 4], 0};
      standTgt[to] = standTgt[an] + V3{rd.blen[12 + i * 4] * 0.95f, -P.ankleH * 0.9f, 0};
    }
  }

  void getup() {
    if (!fallen) return;
    if (!rd.active) {
      Frame tmp; pose(tmp);
      rd.init(tmp.nodes, tmp.radii, 1.0f / 240.0f);
    }
    buildStandTarget();
    rd.setTarget(standTgt);
    gettingUp = true; getupT = 0; falls_ = falls;
  }
  int falls_ = 0;

  /* ---- 准静态逆动力学：由地面反作用力往上推关节力矩 ----
     只算矢状面（x 前后 / y 竖直）。正力矩 = 伸展。
     每只脚的压力中心用"踝 + 限幅后的偏移"近似：模型里没有足底压力分布，
     直接用全局 CoP 会得到 (cop-ankle) 几十厘米的力臂，力矩会大到离谱（实测 ±240 N·m）。 */
  void computeTorque(const Frame& fr, float tq[6], float w[6]) {
    const float mF = 1.0f, mS = 3.7f, mT = 9.5f;
    const float G = P.g;
    /* 必须是成员：以前写成函数内 static，两个 Walker 会共用同一份关节角历史，
       速度就串味了 —— 同进程跑两条腿彼此污染，确定性测试也过不了。 */
    for (int i = 0; i < 2; ++i) {
      const int hipN = 10 + i * 4, knN = hipN + 1, anN = hipN + 2, toN = hipN + 3;
      const V3 an = fr.nodes[anN], kn = fr.nodes[knN], hp = fr.nodes[hipN], to = fr.nodes[toN];
      const float* g = i == 0 ? fr.grfL : fr.grfR;
      const float gfx = g[0], gfy = g[1];
      /* 矢状面力矩必须投到"她的前后方向"上：世界 x 在一转身就变成斜的了。
         用投影差值，yaw=0 时和原来的 an.x 逐位相同。 */
      const float cY = std::cos(yaw), sY = std::sin(yaw);
      auto hz = [&](const V3& q) { return q.x * cY + q.z * sY; };
      const float anF = hz(an), knF = hz(kn), hpF = hz(hp), toF = hz(to);
      /* 地面力也按同样的方向分解（法向力方向不变，切向分量取前向投影） */
      const float fx = gfx * cY + g[2] * sY, fy = gfy;
      /* 该脚自己的压力中心：踝前后 ±6/10 cm（足长 25 cm，重心偏前） */
      float off = hz(V3{fr.copX, 0.0f, fr.copZ}) - anF;
      off = std::max(-0.06f, std::min(0.10f, off));
      const float copF = anF + off;

      auto cr = [](float ax, float ay, float bx, float by) { return ax * by - ay * bx; };
      /* 足段：地面力 + 自重，踝关节反力 Fa、力矩 Ma */
      const float Ma = -(cr(copF - anF, 0.0f - an.y, fx, fy) + 0.0f);
      const float Fa_x = -fx, Fa_y = -(fy - mF * G);
      /* 小腿段：踝关节传来 -Fa / -Ma，自重 mS·g */
      const float comS_x = (knF + anF) * 0.5f;
      const float Mk = Ma - cr(anF - knF, an.y - kn.y, -Fa_x, -Fa_y)
                          - cr(comS_x - knF, 0.0f, 0.0f, -mS * G);
      const float Fk_x = Fa_x, Fk_y = Fa_y + mS * G;
      /* 大腿段 */
      const float comT_x = (hpF + knF) * 0.5f;
      const float Mh = Mk - cr(knF - hpF, kn.y - hp.y, -Fk_x, -Fk_y)
                          - cr(comT_x - hpF, 0.0f, 0.0f, -mT * G);
      tq[i * 3 + 0] = Mh;
      tq[i * 3 + 1] = Mk;
      tq[i * 3 + 2] = Ma;

      float ah = std::atan2(kn.y - hp.y, knF - hpF);
      float ak = std::atan2(an.y - kn.y, anF - knF);
      float aa = std::atan2(to.y - an.y, toF - anF);
      if (prevAng[i * 3] == 0 && prevAng[i * 3 + 1] == 0 && prevAng[i * 3 + 2] == 0) {
        prevAng[i * 3] = ah; prevAng[i * 3 + 1] = ak; prevAng[i * 3 + 2] = aa;
      }
      w[i * 3 + 0] = (ah - prevAng[i * 3 + 0]) / DT_;
      w[i * 3 + 1] = (ak - prevAng[i * 3 + 1]) / DT_;
      w[i * 3 + 2] = (aa - prevAng[i * 3 + 2]) / DT_;
      prevAng[i * 3 + 0] = ah; prevAng[i * 3 + 1] = ak; prevAng[i * 3 + 2] = aa;
    }
  }
  float DT_ = 1.0f / 120.0f;
  Bio bio;

  /* ---- 摔倒之后：体模型接管 ---- */
  void stepFallen(float dt) {
    if (!rd.active) {
      Frame tmp;
      pose(tmp);                       /* 此刻 rd.active 还是 false，拿到的是摔倒前那一帧 */
      rd.init(tmp.nodes, tmp.radii, dt);
      V3 c = rd.com();
      comY = c.y;
    }
    if (gettingUp) {
      getupT += dt;
      /* 发力曲线：0.25 s 内从 0 涨到 1，起来之后收力 */
      rd.drive = std::min(1.0f, getupT / 0.25f);
      if (getupT > 1.10f && rd.drive > 0.92f) rd.drive = 0.92f;
      bool upOk = getupT > 1.20f && comY > 0.80f;
      if (upOk || getupT > 2.60f) {
        Frame tmp; tmp.falls = falls;
        rd.clearTarget();
        /* 站起来了：把运动学状态挪到当前骨盆位置 */
        p = V3{rd.x[0].x, rd.x[0].y, rd.x[0].z};
        gy = groundAt(p.x, p.z);
        p.y = gy + P.hipH;
        v = V3{0, 0, 0}; xRef = p.x; vRef = 0;
        for (int i = 0; i < 2; ++i) {
          FootState& f = feet[i];
          float z = (i == 0 ? 1.0f : -1.0f) * P.hipHalfW;
          f.ank = V3{p.x, groundAt(p.x, p.z + z) + P.ankleH, p.z + z};
          f.plant = f.ank; f.from = f.ank; f.to = f.ank;
          f.swing = false; f.contact = true; f.t0 = f.t1 = 0;
        }
        rd.stop();
        fallen = false; gettingUp = false; getupT = 0;
        phase = 0; standing = (modeCode == 0);
        justStood = 0.8f;   /* 刚站起来的一小段：别马上按走路判摔 */
        int fs = falls; falls = fs;   /* 摔的次数不清零 */
      }
    }
    rd.mu = floorMu;
    rd.step(dt);
    {
      /* 躺着也在代谢：力矩给 0，光算基础代谢 + 体温调节 */
      float tq0[6] = {0, 0, 0, 0, 0, 0}, w0[6] = {0, 0, 0, 0, 0, 0};
      bio.bodyMass = totalMass;
      bio.step(dt, tq0, w0);
      bio.tempStep(dt);
    }
    V3 c = rd.com();
    comY = c.y;
    p = V3{rd.x[0].x, rd.x[0].y, rd.x[0].z};
    v = V3{0, 0, 0};
    y = p.y;
  }

  /* 体模型帧：直接把 18 个球端出去 */
  void poseRagdoll(Frame& fr) {
    for (int i = 0; i < 18; ++i) {
      fr.nodes[i] = rd.x[i];
      fr.radii[i] = rd.r[i];
      bool touch = rd.x[i].y - rd.r[i] < 0.006f;
      fr.contact[i] = touch ? 1 : 0;
    }
    static const int B[17][2] = {{0,1},{1,2},{2,3},{1,4},{4,5},{5,6},{1,7},{7,8},{8,9},
                                 {0,10},{10,11},{11,12},{12,13},{0,14},{14,15},{15,16},{16,17}};
    float mx = 0;
    for (int b = 0; b < 17; ++b) {
      fr.bones[b][0] = B[b][0]; fr.bones[b][1] = B[b][1];
      float dd = len(fr.nodes[B[b][1]] - fr.nodes[B[b][0]]);
      if (b >= 10 && b != 12 && b != 16) mx = std::max(mx, std::fabs(dd - rd.blen[b]));
    }
    fr.err = mx;
    fr.boneErrRagdoll = mx;
    V3 c = rd.com();
    fr.t = t;
    fr.speed = 0; fr.yaw = yaw; fr.vYawOut = 0.0f;
    fr.comX = c.x; fr.comZ = c.z; fr.comY = c.y;
    fr.capX = c.x; fr.capZ = c.z;
    fr.zmpX = c.x; fr.zmpZ = c.z;
    fr.leanX = fr.leanZ = 0;
    fr.falls = falls;
    fr.mode = 3;                 /* 3 = 摔倒 */
    fr.fallen = 1;
    fr.dutyMeas = dutyN > 0 ? dutyAcc / dutyN : 0;
  }

  void push(float fx, float fy, float fz) {
    vPush += fx; v.y += fy; v.z += fz;
  }
  void setMode(int m) {
    if (fallen) { int fs = falls; reset(); falls = fs; if (m == 0) return; }  /* 躺着按走路 = 起来 */
    if (m == modeCode && m != 0) { P.vDes = clampf(P.run ? P.vRun : P.vWalk, 0.0f, 2.0f); return; }
    modeCode = m;
    P.run = (m == 2);
    swingLift = P.run ? P.liftRun : P.liftWalk;
    if (m == 0) {
      /* 从跑步急停不能一步站住：先减速、脚下继续迈，等速度掉下来再定住。
         直接 standing=true 会让相位停住但身体还在往前冲 → 捕获点越界 → 判摔。 */
      P.vDes = 0.0f;
      { const float vFw0 = v.x * std::cos(yaw) + v.z * std::sin(yaw);
        standing = (std::fabs(vFw0) < 0.25f); }
      stopGateT = 0.0f;
      return;
    }
    standing = false;
    latRef = 0.0f;   /* 走起来就以中线为参考 */
    P.vDes = clampf(P.run ? P.vRun : P.vWalk, 0.0f, 2.0f);   /* 换模式就是换目标速度，别继承上一个模式的 */
    phase = 0.0f;   /* 起手双支撑，相位走到 d 之后左脚自然离地 */
  }
};
