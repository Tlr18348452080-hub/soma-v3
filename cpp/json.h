#pragma once
/* ================= 极简 JSON =================
   为什么自己写：参数量已经大到不能用 strstr 抠了（spinal.h 里那套是给单个文件用的）。
   这里只要 DOM：读 + 按 key 取 + 带默认值。
   不支持：注释、NaN、\u 代理对（遇到就原样留着，正文里的中文都是 UTF-8 直出）。
*/
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <utility>

struct JVal {
  enum T { NUL, BOOL, NUM, STR, ARR, OBJ } t = NUL;
  bool b = false;
  double num = 0;
  std::string str;
  std::vector<JVal> arr;
  std::vector<std::pair<std::string, JVal>> obj;

  const JVal* get(const char* k) const {
    if (t != OBJ) return nullptr;
    for (auto& kv : obj) if (kv.first == k) return &kv.second;
    return nullptr;
  }
  bool has(const char* k) const { return get(k) != nullptr; }
  double numOr(const char* k, double d) const { const JVal* v = get(k); return (v && v->t == NUM) ? v->num : d; }
  bool boolOr(const char* k, bool d) const { const JVal* v = get(k); return (v && v->t == BOOL) ? v->b : d; }
  const char* strOr(const char* k, const char* d) const { const JVal* v = get(k); return (v && v->t == STR) ? v->str.c_str() : d; }
  int size() const { return (int)arr.size(); }
};

struct JParser {
  const char* p;
  bool ok = true;

  void ws() { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p; }
  bool eat(char c) { ws(); if (*p == c) { ++p; return true; } return false; }

  bool fail() { ok = false; return false; }

  static void putUtf8(std::string& s, unsigned cp) {
    if (cp < 0x80) s += (char)cp;
    else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
    else { s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
  }

  bool parseString(std::string& out) {
    if (!eat('"')) return fail();
    out.clear();
    while (*p && *p != '"') {
      if (*p != '\\') { out += *p++; continue; }
      ++p;
      switch (*p) {
        case 'n': out += '\n'; ++p; break;
        case 't': out += '\t'; ++p; break;
        case 'r': out += '\r'; ++p; break;
        case 'b': out += '\b'; ++p; break;
        case 'f': out += '\f'; ++p; break;
        case '/': out += '/'; ++p; break;
        case '"': out += '"'; ++p; break;
        case '\\': out += '\\'; ++p; break;
        case 'u': {
          ++p;
          unsigned cp = 0;
          for (int i = 0; i < 4 && *p; ++i, ++p) {
            char c = *p; cp <<= 4;
            if (c >= '0' && c <= '9') cp |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') cp |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp |= (unsigned)(c - 'A' + 10);
          }
          putUtf8(out, cp);
          break;
        }
        default: out += *p++; break;      /* 不认识的转义：原样吐出来 */
      }
    }
    if (*p != '"') return fail();
    ++p;
    return true;
  }

  bool parseValue(JVal& v) {
    ws();
    if (!*p) return fail();
    switch (*p) {
      case '{': {
        ++p; v.t = JVal::OBJ;
        ws();
        if (eat('}')) return true;
        while (true) {
          std::string k;
          if (!parseString(k)) return false;
          if (!eat(':')) return fail();
          JVal child;
          if (!parseValue(child)) return false;
          v.obj.emplace_back(std::move(k), std::move(child));
          if (eat(',')) continue;
          return eat('}') ? true : fail();
        }
      }
      case '[': {
        ++p; v.t = JVal::ARR;
        ws();
        if (eat(']')) return true;
        while (true) {
          JVal child;
          if (!parseValue(child)) return false;
          v.arr.emplace_back(std::move(child));
          if (eat(',')) continue;
          return eat(']') ? true : fail();
        }
      }
      case '"': v.t = JVal::STR; return parseString(v.str);
      case 't': if (!strncmp(p, "true", 4)) { p += 4; v.t = JVal::BOOL; v.b = true; return true; } return fail();
      case 'f': if (!strncmp(p, "false", 5)) { p += 5; v.t = JVal::BOOL; v.b = false; return true; } return fail();
      case 'n': if (!strncmp(p, "null", 4)) { p += 4; v.t = JVal::NUL; return true; } return fail();
      default: {
        char* end = nullptr;
        double d = std::strtod(p, &end);
        if (end == p) return fail();
        p = end; v.t = JVal::NUM; v.num = d; return true;
      }
    }
  }

  bool parse(const char* s, JVal& out) { p = s; ok = true; return parseValue(out) && ok; }
};

inline bool jsonLoad(const char* path, JVal& out) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  std::string s;
  char buf[8192]; size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
  std::fclose(f);
  JParser jp;
  return jp.parse(s.c_str(), out);
}
