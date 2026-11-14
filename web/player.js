/* ================= 玩家替身 =================
   她自己那套是物理算出来的；玩家这套是"想走就给我走"，不做力平衡。
   两套共用同一个模型和同一种姿态接口，所以她俩站在一起不会像两个游戏。
*/
import { createWhaleGirl } from './whale.js';

export function createPlayer(THREE) {
  const girl = createWhaleGirl(THREE, {
    hair: 0x6b7480, hairLt: 0xa3acb7,
    dress: 0x262a31, dress2: 0x353a43,
    white: 0xeeeeea, sock: 0xf3f3ef, shoe: 0x1a1c20,
    tail: 0x8a9bb0, fin: 0xc3d0de,
    eye: 0x272c33, blush: 0xd7a6a6, emissive: 0x0e1116,
  });

  const nodes = Array.from({ length: 18 }, () => new THREE.Vector3());
  const radii = new Array(18).fill(0.042);
  let ph = 0, cur = 0;

  const THIGH = 0.45, SHANK = 0.45;

  function pose(speed) {
    const sw = Math.min(1, speed / 1.4);
    const hipH = 0.895;
    const bob = Math.abs(Math.sin(ph * 2)) * 0.016 * sw;

    nodes[0].set(0, hipH + bob, 0);
    nodes[1].set(0.008, hipH + 0.285 + bob, 0);
    nodes[2].set(0.018, hipH + 0.525 + bob, 0);
    nodes[3].set(0.028, hipH + 0.645 + bob, 0);

    for (let i = 0; i < 2; i++) {
      const sgn = i === 0 ? 1 : -1;
      const p = ph + i * Math.PI;
      const b = 10 + i * 4;
      const hz = sgn * 0.085;
      const hipA = Math.sin(p) * 0.50 * sw;
      const kneeBend = Math.max(0, Math.sin(p + 1.9)) * 0.95 * sw;

      nodes[b].set(0, hipH + bob, hz);
      const kx = Math.sin(hipA) * THIGH;
      const ky = hipH + bob - Math.cos(hipA) * THIGH;
      nodes[b + 1].set(kx, ky, hz);

      const a2 = hipA - kneeBend;
      const ax = kx + Math.sin(a2) * SHANK;
      const ay = ky - Math.cos(a2) * SHANK;
      nodes[b + 2].set(ax, ay, hz);
      nodes[b + 3].set(ax + 0.17, Math.max(0.015, ay - 0.055), hz);
    }

    for (let i = 0; i < 2; i++) {
      const sgn = i === 0 ? 1 : -1;
      const p = ph + i * Math.PI;
      const b = 4 + i * 3;
      const sy = hipH + 0.44 + bob, sz = sgn * 0.19;
      const armA = -Math.sin(p) * 0.42 * sw;
      const bend = Math.max(0, Math.sin(p)) * 0.30 * sw;

      nodes[b].set(0.008, sy, sz);
      const ex = 0.008 + Math.sin(armA) * 0.28;
      const ey = sy - Math.cos(armA) * 0.28;
      nodes[b + 1].set(ex, ey, sz);
      nodes[b + 2].set(ex + Math.sin(armA - bend) * 0.26, ey - Math.cos(armA - bend) * 0.26, sz);
    }
  }

  return {
    group: girl.group,
    /* speed: 目标速度 m/s（0 = 站住）。朝向由外部改 group.rotation.y。 */
    update(dt, speed) {
      cur += (speed - cur) * Math.min(1, dt * 9);
      if (cur > 0.02) ph += dt * (5.0 + cur * 2.0);
      pose(cur);
      girl.update(nodes, radii, dt, ph);
    },
  };
}
