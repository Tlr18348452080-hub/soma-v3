/* ================= 门外那截小走廊 =================
   客厅的 +x 墙上原来有扇关着的门（装饰用）。现在把门打开、外面接一条 2.2 m 的走廊：
   鞋柜、衣帽钩、地垫、尽头一扇小窗 —— 出门这件事在画面上有个"去处"，不然推开门是虚空。

   尺寸（和 room.js 的 +x 门对齐，门洞 z∈[0.44, 1.36]，门宽 0.92）：
     x ∈ [4.00, 6.20]   z ∈ [-0.10, 1.90]   层高沿用 2.90
   一件家具 = 一个盒子 + 一个碰撞盒（h ≤ 0.18 的不挡路）。 */
import { loadProp } from './room.js';

export function createHall(THREE, MATS) {
  const g = new THREE.Group();
  const colliders = [];
  const H = 2.90;
  const X0 = 4.00, X1 = 6.20, Z0 = -0.10, Z1 = 1.90;

  const mk = MATS ? MATS.mk : (o) => new THREE.MeshStandardMaterial({ color: o.color, roughness: o.rough });
  const matWall = mk({ map: 'wall', rep: [3, 1.4], color: 0xe9e2d6, rough: 0.95 });
  const matFloor = mk({ map: 'tile', rep: [3, 2], color: 0xdfe2e4, rough: 0.5, metal: 0.02 });
  const matWood = mk({ map: 'wood', rep: [2, 1.2], color: 0xffffff, rough: 0.5 });
  const matWhite = mk({ color: 0xf4f1ea, rough: 0.6 });
  const matMetal = mk({ color: 0xb9bcc0, rough: 0.35, metal: 0.7 });
  const matFabric = mk({ map: 'fabric', rep: [2, 2], color: 0xd9d0c2, rough: 1.0 });
  const matLamp = mk({ color: 0xfff4e2, emissive: 0xffe9c8, emissiveIntensity: 0.5, rough: 0.8 });
  let hallLamp = null;

  function box(w, h, d, x, y, z, mat, opt = {}) {
    const m = new THREE.Mesh(new THREE.BoxGeometry(w, h, d), mat);
    m.position.set(x, y, z);
    m.castShadow = opt.cast !== false;
    m.receiveShadow = true;
    g.add(m);
    return m;
  }
  function collider(x0, x1, z0, z1, h) {
    colliders.push({ x0: Math.min(x0, x1), x1: Math.max(x0, x1), z0: Math.min(z0, z1), z1: Math.max(z0, z1), h });
  }

  /* 地板 / 天花 */
  {
    const f = new THREE.Mesh(new THREE.PlaneGeometry(X1 - X0, Z1 - Z0), matFloor);
    f.rotation.x = -Math.PI / 2; f.position.set((X0 + X1) / 2, 0.001, (Z0 + Z1) / 2);
    f.receiveShadow = true; g.add(f);
    const c = new THREE.Mesh(new THREE.PlaneGeometry(X1 - X0, Z1 - Z0), matWhite);
    c.rotation.x = Math.PI / 2; c.position.set((X0 + X1) / 2, H, (Z0 + Z1) / 2);
    g.add(c);
  }

  /* 墙：尽头（+x）、两侧（±z）。靠客厅那面不建 —— 那是客厅的右墙，已经拆出门洞。 */
  const wallSeg = (w, x, z, rotY) => {
    const m = new THREE.Mesh(new THREE.PlaneGeometry(w, H), matWall);
    m.position.set(x, H / 2, z); m.rotation.y = rotY; m.receiveShadow = true; g.add(m);
  };
  wallSeg(Z1 - Z0, X1, (Z0 + Z1) / 2, -Math.PI / 2);      /* 尽头 */
  wallSeg(X1 - X0, (X0 + X1) / 2, Z0, 0);                  /* 北侧 */
  wallSeg(X1 - X0, (X0 + X1) / 2, Z1, Math.PI);            /* 南侧 */
  /* 尽头墙上一扇小窗（只是个凹进去的亮块，光从那边来） */
  {
    const win = new THREE.Mesh(new THREE.PlaneGeometry(0.72, 0.86),
      mk({ color: 0xe8f3f6, emissive: 0xdcefff, emissiveIntensity: 0.5, rough: 0.3, transparent: true, opacity: 0.8 }));
    win.position.set(X1 - 0.01, 1.72, 0.9); win.rotation.y = -Math.PI / 2;
    g.add(win);
    box(0.05, 0.06, 0.80, X1 - 0.03, 1.27, 0.9, matWhite, { cast: false });
    box(0.05, 0.06, 0.80, X1 - 0.03, 2.17, 0.9, matWhite, { cast: false });
  }
  collider(X1 - 0.04, X1 + 0.30, Z0 - 0.30, Z1 + 0.30, H);          /* 尽头墙 */
  collider(X0 - 0.30, X1 + 0.30, Z0 - 0.22, Z0 + 0.02, H);          /* 北墙 */
  collider(X0 - 0.30, X1 + 0.30, Z1 - 0.02, Z1 + 0.22, H);          /* 南墙 */

  /* 地垫：进门第一脚（很矮，不挡路） */
  box(1.15, 0.012, 0.72, 4.55, 0.006, 0.90, matFabric, { cast: false });

  /* 鞋柜：尽头左手边，柜上摆个钥匙盘 */
  {
    box(0.92, 0.62, 0.34, 5.70, 0.31, 1.66, matWood);
    box(0.96, 0.04, 0.38, 5.70, 0.64, 1.66, matWhite, { cast: false });
    collider(5.24, 6.16, 1.49, 1.83, 0.66);
    const tray = box(0.26, 0.02, 0.18, 5.62, 0.67, 1.66, matMetal, { cast: false });
    tray.rotation.y = 0.08;
    /* 柜前一双鞋（她的，拖鞋） */
    for (const [dx, dz, r] of [[-0.16, -0.24, 0.22], [0.06, -0.20, -0.12]]) {
      box(0.10, 0.03, 0.24, 5.66 + dx, 0.017, 1.42 + dz, matFabric).rotation.y = r;
    }
  }

  /* 衣帽钩：南墙上一条板 + 三个钩 + 一件挂着的风衣 */
  {
    box(0.86, 0.07, 0.05, 4.95, 1.72, 1.86, matWood);
    for (const dx of [-0.28, 0, 0.28]) {
      const hook = new THREE.Mesh(new THREE.CylinderGeometry(0.012, 0.012, 0.09, 8), matMetal);
      hook.rotation.x = Math.PI / 2; hook.position.set(4.95 + dx, 1.68, 1.80);
      hook.castShadow = true; g.add(hook);
    }
    /* 衣：一整片垂下来的布（双面），肩那里宽一点 */
    const coatMat = mk({ map: 'fabric', rep: [1.4, 2.2], color: 0x5d6a7a, rough: 0.95 });
    const coat = new THREE.Mesh(new THREE.PlaneGeometry(0.52, 0.98, 4, 8), coatMat);
    coat.position.set(4.95, 1.16, 1.80); coat.castShadow = true; coat.receiveShadow = true;
    const pos = coat.geometry.attributes.position;
    for (let i = 0; i < pos.count; i++) {
      const y = pos.getY(i) / 0.98 + 0.5;                 /* 0 底 → 1 顶 */
      const w = 0.55 + 0.45 * y;                           /* 上面宽、下面收 */
      pos.setX(i, pos.getX(i) * w);
      pos.setZ(i, Math.sin(y * 6.0 + pos.getX(i) * 4.0) * 0.022);
    }
    coat.geometry.computeVertexNormals();
    g.add(coat);
  }

  /* 一小盆绿植：走廊尽头那点生气 */
  {
    const pot = new THREE.Mesh(new THREE.CylinderGeometry(0.13, 0.10, 0.22, 12), matWhite);
    pot.position.set(5.86, 0.11, 0.28); pot.castShadow = true; g.add(pot);
    for (let i = 0; i < 6; i++) {
      const a = i / 6 * Math.PI * 2;
      const leaf = new THREE.Mesh(new THREE.ConeGeometry(0.05, 0.34, 6),
        mk({ color: 0x5f7f52, rough: 0.9 }));
      leaf.position.set(5.86 + Math.cos(a) * 0.06, 0.38, 0.28 + Math.sin(a) * 0.06);
      leaf.rotation.z = Math.cos(a) * 0.34; leaf.rotation.x = Math.sin(a) * 0.34;
      leaf.castShadow = true; g.add(leaf);
    }
    collider(5.72, 6.00, 0.14, 0.42, 0.30);
  }

  /* 顶灯 */
  {
    const p = new THREE.PointLight(0xffe6c4, 0.42, 4.6, 2);
    p.position.set(5.05, H - 0.30, 0.90); g.add(p);
    const shade = new THREE.Mesh(new THREE.SphereGeometry(0.10, 12, 8), matLamp);
    shade.position.copy(p.position); g.add(shade);
    hallLamp = p;
  }

  function setDaylight(t) { if (hallLamp) hallLamp.intensity = 0.16 + 0.34 * t; }

  return {
    group: g,
    colliders,
    rooms: { hallOut: { x: 5.05, z: 0.90, name: '门外走廊' } },
    bounds: { x0: X0, x1: X1, z0: Z0, z1: Z1 },
    setDaylight,
  };
}
