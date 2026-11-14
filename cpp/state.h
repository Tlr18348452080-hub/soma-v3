#pragma once
#include "vec.h"
#include <string>
#include <cstdio>
#include <cstring>

/* ================= 身体状态总线 =================
   一条铁律：**所有**身体参数都在这里，一帧一份 JSON，60 次/秒重新装载。
   脊髓（矩阵层）读它、算它、写回它；渲染读它；大脑（LLM）只在被叫到时读它、回一句。

   为什么非要过 JSON：因为这就是主人要的那个"巨大的表单"。
   脊髓不持有任何隐藏状态 —— 它的全部输入输出都在这张表里，
   所以任何一秒的它都能被存下来、重放、喂给大脑看。
*/

struct BodyState {
  double t = 0;
  /* ---- 姿势：18 节点（骨盆/胸/颈/头/双臂/双腿）的世界坐标 ---- */
  V3 node[18];
  float nodeR[18] = {0};
  /* ---- 速度（每个节点）---- */
  V3 nodeV[18];
  /* ---- 接触：两只脚的世界位置 + 法向力 ---- */
  V3 foot[2];
  float footForce[2] = {0, 0};
  bool footContact[2] = {false, false};
  /* ---- 脊髓节律：两条腿的振荡器相位 ---- */
  float cpgPh[2] = {0, 3.14159265f};
  /* ---- 意图（这是大脑唯一能改的东西）---- */
  float vDes = 0;      // 想要多快（m/s）
  float wDes = 0;      // 想要转多快（rad/s，绕竖直轴）
  /* ---- 身体内部状态 ---- */
  float metab = 0, fat = 0, temp = 37.0f;
  /* ---- 世界 ---- */
  float mu = 0.9f;
  bool fallen = false;
  /* ---- 大脑通道 ---- */
  bool brainPending = false;   // 脊髓认为"这事我心里没数"，等大脑
  int brainTag = 0;            // 请求编号，大脑回来要带上同一个 tag

  std::string toJson() const {
    char buf[128];
    std::string s = "{\"t\":";
    std::snprintf(buf, sizeof buf, "%.4f", t); s += buf;
    s += ",\"nodes\":[";
    for (int i = 0; i < 18; ++i) {
      std::snprintf(buf, sizeof buf, "%s[%.4f,%.4f,%.4f,%.3f]", i ? "," : "",
                    node[i].x, node[i].y, node[i].z, nodeR[i]);
      s += buf;
    }
    s += "],\"vel\":[";
    for (int i = 0; i < 18; ++i) {
      std::snprintf(buf, sizeof buf, "%s[%.3f,%.3f,%.3f]", i ? "," : "",
                    nodeV[i].x, nodeV[i].y, nodeV[i].z);
      s += buf;
    }
    s += "],\"foot\":[";
    for (int i = 0; i < 2; ++i) {
      std::snprintf(buf, sizeof buf, "%s[%.4f,%.4f,%.4f,%.2f,%d]", i ? "," : "",
                    foot[i].x, foot[i].y, foot[i].z, footForce[i], footContact[i] ? 1 : 0);
      s += buf;
    }
    s += ",\"cpg\":[";
    std::snprintf(buf, sizeof buf, "%.4f,%.4f]", cpgPh[0], cpgPh[1]); s += buf;
    std::snprintf(buf, sizeof buf, ",\"intent\":{\"v\":%.3f,\"w\":%.3f}", vDes, wDes); s += buf;
    std::snprintf(buf, sizeof buf, ",\"bio\":{\"metab\":%.1f,\"fat\":%.3f,\"temp\":%.3f}", metab, fat, temp); s += buf;
    std::snprintf(buf, sizeof buf, ",\"world\":{\"mu\":%.2f}", mu); s += buf;
    std::snprintf(buf, sizeof buf, ",\"fallen\":%d,\"brain\":{\"pending\":%d,\"tag\":%d}}",
                  fallen ? 1 : 0, brainPending ? 1 : 0, brainTag);
    s += buf;
    return s;
  }
};

/* ---- 极简解析器：只认我们自己写出去的那张表 ----
   不引第三方库：这张表的结构是我们自己定的，键名固定、全部是数字和数组。
   解析失败就返回 false（脊髓不猜，坏了就让上层看见）。 */
#include <cstdlib>
namespace stjson {
inline const char* skip(const char* p) { while (*p && (*p == ' ' || *p == '\n' || *p == '\t')) ++p; return p; }
inline const char* findKey(const char* p, const char* key) {
  std::string pat = std::string("\"") + key + "\":";
  const char* q = std::strstr(p, pat.c_str());
  return q ? q + pat.size() : nullptr;
}
inline const char* readNum(const char* p, double& out) {
  p = skip(p); char* end = nullptr;
  out = std::strtod(p, &end);
  return end ? end : p;
}
inline bool readVec3(const char* p, V3& v) {
  p = skip(p); if (*p != '[') return false; ++p;
  double a, b, c;
  p = readNum(p, a); p = skip(p); if (*p == ',') ++p;
  p = readNum(p, b); p = skip(p); if (*p == ',') ++p;
  p = readNum(p, c);
  v = {(float)a, (float)b, (float)c};
  return true;
}
}  // namespace stjson

inline bool fromJson(const std::string& js, BodyState& st) {
  const char* p = js.c_str();
  const char* q = stjson::findKey(p, "t");
  if (!q) return false;
  double d; q = stjson::readNum(q, d); st.t = d;
  q = stjson::findKey(p, "nodes");
  if (!q) return false;
  q = stjson::skip(q);
  if (*q != '[') return false; ++q;
  for (int i = 0; i < 18; ++i) {
    q = stjson::skip(q);
    if (*q == ',') { ++q; q = stjson::skip(q); }
    if (*q != '[') return false;
    double a, b, c, r;
    ++q;
    q = stjson::readNum(q, a); q = stjson::skip(q); if (*q == ',') ++q;
    q = stjson::readNum(q, b); q = stjson::skip(q); if (*q == ',') ++q;
    q = stjson::readNum(q, c); q = stjson::skip(q); if (*q == ',') ++q;
    q = stjson::readNum(q, r); q = stjson::skip(q);
    if (*q == ']') ++q;
    st.node[i] = {(float)a, (float)b, (float)c};
    st.nodeR[i] = (float)r;
  }
  if ((q = stjson::findKey(p, "cpg"))) {
    double a, b;
    q = stjson::skip(q); if (*q == '[') ++q;
    q = stjson::readNum(q, a); q = stjson::skip(q); if (*q == ',') ++q;
    q = stjson::readNum(q, b);
    st.cpgPh[0] = (float)a; st.cpgPh[1] = (float)b;
  }
  if ((q = stjson::findKey(p, "v\""))) { }   // 占位：intent 单独解析
  if ((q = stjson::findKey(p, "intent"))) {
    const char* v = stjson::findKey(q, "v");
    const char* w = stjson::findKey(q, "w");
    if (v) { double x; stjson::readNum(v, x); st.vDes = (float)x; }
    if (w) { double x; stjson::readNum(w, x); st.wDes = (float)x; }
  }
  if ((q = stjson::findKey(p, "fallen"))) {
    double x; stjson::readNum(q, x); st.fallen = x > 0.5;
  }
  return true;
}
