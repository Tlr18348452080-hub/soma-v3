#pragma once
#include <cmath>
#include <algorithm>

struct V3 {
  float x{0}, y{0}, z{0};
  V3() = default;
  V3(float a, float b, float c) : x(a), y(b), z(c) {}
  V3 operator+(const V3& o) const { return {x + o.x, y + o.y, z + o.z}; }
  V3 operator-(const V3& o) const { return {x - o.x, y - o.y, z - o.z}; }
  V3 operator*(float s) const { return {x * s, y * s, z * s}; }
  V3& operator+=(const V3& o) { x += o.x; y += o.y; z += o.z; return *this; }
  V3& operator-=(const V3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
  V3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
};
inline float dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(const V3& a, const V3& b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float len(const V3& a) { return std::sqrt(dot(a, a)); }
inline V3 norm(const V3& a) { float l = len(a); return l > 1e-9f ? a * (1.0f / l) : V3{0, 1, 0}; }
inline V3 lerp(const V3& a, const V3& b, float t) { return a + (b - a) * t; }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
/* 水平面距离的平方（y 不参与） */
inline float distXZ(const V3& a, const V3& b) { return std::sqrt((a.x-b.x)*(a.x-b.x) + (a.z-b.z)*(a.z-b.z)); }
