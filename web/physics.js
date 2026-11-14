/* ================= 物理 · 就一层薄薄的 =================
   不引物理引擎：这屋里要处理的东西一共两种 ——
     1) 一个站着走路的人（圆盘，半径 0.26，不会跳不会叠罗汉）；
     2) 一堆不会动的盒子（墙、床、桌、沙发、书架）。
   那就写解析解，别为两个圆和一个矩形引 200 KB 的 ODE。

   规则：先按轴分开推进，每一轴推完立刻把圆从盒子里挤出来。
   分轴推进的好处是贴着墙走会自动"滑"过去，而不是卡住 —— 第一版试过整体推进，
   走到墙角就变成粘鼠板。
*/

const EPS = 1e-4;

/* 圆 vs 轴对齐盒：把圆心推到盒外。返回推出后的 [x, z]，没接触就原样返回。 */
export function pushOutOfBox(px, pz, r, b) {
  /* 最近点 */
  const cx = Math.max(b.x0, Math.min(b.x1, px));
  const cz = Math.max(b.z0, Math.min(b.z1, pz));
  let dx = px - cx, dz = pz - cz;
  const d2 = dx * dx + dz * dz;

  if (d2 > r * r - EPS) return null;   /* 没碰上 */

  if (d2 > EPS * EPS) {
    /* 圆心在盒外：沿最近点方向推到 r 处 */
    const d = Math.sqrt(d2);
    const k = (r - d) / d;
    return [px + dx * k, pz + dz * k];
  }
  /* 圆心在盒内（或者正好压在边上）：找最近的那一面，从那儿推出去 */
  const dl = px - b.x0, dr = b.x1 - px;
  const db = pz - b.z0, df = b.z1 - pz;
  const m = Math.min(dl, dr, db, df);
  if (m === dl) return [b.x0 - r, pz];
  if (m === dr) return [b.x1 + r, pz];
  if (m === db) return [px, b.z0 - r];
  return [px, b.z1 + r];
}

/* 一个圆盘沿 (dx,dz) 走一步，被 colliders 挡住就贴着滑。
   返回 [{x,z}, hit] —— hit 是撞到的面法线方向（给音效/镜头抖动用）。 */
export function moveCircle(px, pz, dx, dz, r, colliders, bounds) {
  let x = px, z = pz;
  let hit = null;

  /* 先 x 后 z：贴墙走时另一个轴还能动 */
  x += dx;
  for (let i = 0; i < colliders.length; i++) {
    const b = colliders[i];
    const out = pushOutOfBox(x, z, r, b);
    if (out) { x = out[0]; z = out[1]; hit = hit || (Math.abs(out[0] - (px + dx)) > Math.abs(out[1] - pz) ? 'x' : 'z'); }
  }
  z += dz;
  for (let i = 0; i < colliders.length; i++) {
    const b = colliders[i];
    const out = pushOutOfBox(x, z, r, b);
    if (out) { x = out[0]; z = out[1]; hit = hit || (Math.abs(out[1] - (pz + dz)) > Math.abs(out[0] - x) ? 'z' : 'x'); }
  }
  /* 再扫一遍：刚才的推出可能又挤进另一个盒子（墙角） */
  for (let pass = 0; pass < 2; pass++) {
    let moved = false;
    for (let i = 0; i < colliders.length; i++) {
      const out = pushOutOfBox(x, z, r, colliders[i]);
      if (out) { x = out[0]; z = out[1]; moved = true; }
    }
    if (!moved) break;
  }

  if (bounds) {
    if (x < bounds.x0 + r) { x = bounds.x0 + r; hit = hit || 'x'; }
    if (x > bounds.x1 - r) { x = bounds.x1 - r; hit = hit || 'x'; }
    if (z < bounds.z0 + r) { z = bounds.z0 + r; hit = hit || 'z'; }
    if (z > bounds.z1 - r) { z = bounds.z1 - r; hit = hit || 'z'; }
  }
  return { x, z, hit };
}

/* 相机不要穿墙：从 target 朝相机拉一条线，撞到盒面就停在前面。
   没有 raycast 库，就直接拿盒子的六个面做一次线段-盒求交（够用了，
   因为相机只会在房间里，最多穿过一面内墙）。 */
export function clampCamera(target, wanted, colliders, bounds, pad = 0.22) {
  const dx = wanted.x - target.x, dy = wanted.y - target.y, dz = wanted.z - target.z;
  let tHit = 1;

  /* 房间边界先算（把相机挡在墙里侧） */
  if (bounds) {
    const lim = { x0: bounds.x0 + pad, x1: bounds.x1 - pad, z0: bounds.z0 + pad, z1: bounds.z1 - pad };
    if (dx > 0) tHit = Math.min(tHit, (lim.x1 - target.x) / dx);
    if (dx < 0) tHit = Math.min(tHit, (lim.x0 - target.x) / dx);
    if (dz > 0) tHit = Math.min(tHit, (lim.z1 - target.z) / dz);
    if (dz < 0) tHit = Math.min(tHit, (lim.z0 - target.z) / dz);
    if (wanted.y > 2.72) tHit = Math.min(tHit, (2.72 - target.y) / dy);
  }
  /* 高的家具（书架、衣柜）也会挡相机 */
  for (let i = 0; i < colliders.length; i++) {
    const b = colliders[i];
    if (b.h < 1.2) continue;                 /* 矮的不挡镜头，挡了反而奇怪 */
    const t = rayBox(target, dx, dy, dz, b, pad);
    if (t < tHit) tHit = t;
  }
  tHit = Math.max(0.18, Math.min(1, tHit));
  return {
    x: target.x + dx * tHit,
    y: target.y + dy * tHit,
    z: target.z + dz * tHit,
    t: tHit,
  };
}

/* 线段 (origin + s*dir, s∈[0,1]) 与盒（外扩 pad）的进入参数。 */
function rayBox(o, dx, dy, dz, b, pad) {
  let tmin = 0, tmax = 1;
  const lo = [b.x0 - pad, 0 - pad, b.z0 - pad];
  const hi = [b.x1 + pad, b.h + pad, b.z1 + pad];
  const ori = [o.x, o.y, o.z];
  const dir = [dx, dy, dz];
  for (let i = 0; i < 3; i++) {
    if (Math.abs(dir[i]) < 1e-6) {
      if (ori[i] < lo[i] || ori[i] > hi[i]) return 2;   /* 平行且在盒外 */
      continue;
    }
    let t1 = (lo[i] - ori[i]) / dir[i];
    let t2 = (hi[i] - ori[i]) / dir[i];
    if (t1 > t2) { const tt = t1; t1 = t2; t2 = tt; }
    if (t1 > tmin) tmin = t1;
    if (t2 < tmax) tmax = t2;
    if (tmin > tmax) return 2;
  }
  return tmin;
}
