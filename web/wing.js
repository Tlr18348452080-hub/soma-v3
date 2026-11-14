/* ================= 北厢：走廊 + 三间卧室 + 卫浴 + 厨房 =================
   原来的客厅（room.js）是一间 8×6.5 的屋子，x∈[-4,4]、z∈[-3.25,3.25]。
   这里从它的 +z 那面墙开一道门，接出一条走廊和四间房：

        z=8.20  ┌──────┬──────────┬──────┐
                │ 卧 B │  卫 浴    │ 卧 C │
        z=5.60  ├──────┤          ├──────┤
                │ 卧 A │  走 廊    │ 厨房 │
        z=3.25  └──┬───┴────┬─────┴──┬───┘
                  │   客 厅（原房间）    │
                x=-4              x=4

   所有几何程序化生成（和 room.js 一个路子：没有外部贴图，离线也能开）。
   每件家具同时产出一个碰撞盒 —— 物理和画面用同一份数字，不再各写一遍。
*/
import { loadProp } from './room.js';

export function createWing(THREE, MATS) {
  const g = new THREE.Group();
  const colliders = [];

  const H = 2.90;
  const Z0 = 3.25, Z1 = 9.60;          /* 走廊起点 / 最北 */
  const HW = 1.10;                     /* 走廊半宽 */
  const X0 = -4.0, X1 = 4.0;
  const ZA = 6.00;                     /* 西侧 A/B 的分界 */
  const ZB = 6.40;                     /* 东侧 厨房/卫浴 的分界 */
  const DOOR = 0.55;                   /* 门洞半宽 */

  const mk = MATS ? MATS.mk : (o) => new THREE.MeshStandardMaterial({ color: o.color, roughness: o.rough, metalness: o.metal });
  const matWall = mk({ map: 'wall', rep: [4, 1.5], color: 0xf4efe6, rough: 0.95 });
  const matWallWarm = mk({ map: 'wall', rep: [4, 1.5], color: 0xe8ddca, rough: 0.95 });
  const matFloor = mk({ map: 'floor', rep: [3, 3], color: 0xd8c8b0, rough: 0.82 });
  const matTile = mk({ map: 'tile', rep: [3, 3], color: 0xe8ecef, rough: 0.55, metal: 0.02 });
  const matWood = mk({ map: 'wood', rep: [2, 1.2], color: 0xffffff, rough: 0.5 });
  const matWoodDark = mk({ map: 'wood', rep: [2, 1.2], color: 0x9a7a52, rough: 0.55 });
  const matFabric = mk({ map: 'fabric', rep: [2, 2], color: 0xf0e8db, rough: 1.0 });
  const matFabric2 = mk({ map: 'fabric', rep: [2, 2], color: 0xd8cdb9, rough: 1.0 });
  const matWhite = mk({ color: 0xf6f4ef, rough: 0.55 });
  const matGlass = mk({ color: 0xcfe3ea, rough: 0.18, metal: 0.1, transparent: true, opacity: 0.34 });
  const matMetal = mk({ color: 0xb9bcc0, rough: 0.38, metal: 0.7 });
  const matCounter = mk({ map: 'kitchen', rep: [3, 2], color: 0xffffff, rough: 0.42 });
  const matLamp = mk({ color: 0xfff4e2, emissive: 0xffe9c8, emissiveIntensity: 0.55, rough: 0.8 });

  function box(w, h, d, x, y, z, mat, opt) {
    const m = new THREE.Mesh(new THREE.BoxGeometry(w, h, d), mat);
    m.position.set(x, y, z);
    m.castShadow = !(opt && opt.cast === false);
    m.receiveShadow = true;
    g.add(m);
    return m;
  }
  function collider(x0, x1, z0, z1, h) {
    colliders.push({ x0: Math.min(x0, x1), x1: Math.max(x0, x1), z0: Math.min(z0, z1), z1: Math.max(z0, z1), h });
  }
  /* 一件家具 = 一个盒子 + 一个碰撞盒 */
  function furn(w, h, d, x, z, mat, opt) {
    const y = (opt && opt.y != null) ? opt.y + h / 2 : h / 2;
    const m = box(w, h, d, x, y, z, mat, opt);
    if (!(opt && opt.ghost)) collider(x - w / 2, x + w / 2, z - d / 2, z + d / 2, (opt && opt.y ? opt.y : 0) + h);
    return m;
  }

  /* ---------------- 地板 / 天花 ---------------- */
  {
    const f1 = new THREE.Mesh(new THREE.PlaneGeometry(X1 - X0, Z1 - Z0), matFloor);
    f1.rotation.x = -Math.PI / 2; f1.position.set(0, 0, (Z0 + Z1) / 2);
    f1.receiveShadow = true; g.add(f1);
    /* 卫浴那块贴瓷砖，压在木地板上一点点 */
    const tile = new THREE.Mesh(new THREE.PlaneGeometry(X1 - HW - 0.02, Z1 - ZB - 0.02), matTile);
    tile.rotation.x = -Math.PI / 2; tile.position.set((HW + X1) / 2, 0.004, (ZB + Z1) / 2);
    tile.receiveShadow = true; g.add(tile);
    const ceil = new THREE.Mesh(new THREE.PlaneGeometry(X1 - X0, Z1 - Z0), matWhite);
    ceil.rotation.x = Math.PI / 2; ceil.position.set(0, H, (Z0 + Z1) / 2);
    g.add(ceil);
  }

  /* ---------------- 墙（带门洞） ----------------
     wallZ: 沿 x 走向、法线朝 z；wallX: 沿 z 走向、法线朝 x。
     门洞就是在墙中间让出一段，不建东西 —— 简单、可靠，也不会漏光。 */
  function wallSeg(x0, x1, z, mat) {
    if (x1 - x0 < 0.02) return;
    box(x1 - x0, H, 0.10, (x0 + x1) / 2, H / 2, z, mat || matWall);
    collider(x0, x1, z - 0.05, z + 0.05, H);
  }
  function wallSegX(z0, z1, x, mat) {
    if (z1 - z0 < 0.02) return;
    box(0.10, H, z1 - z0, x, H / 2, (z0 + z1) / 2, mat || matWall);
    collider(x - 0.05, x + 0.05, z0, z1, H);
  }
  function wallWithDoor(x0, x1, z, doorAt, mat) {
    wallSeg(x0, doorAt - DOOR, z, mat);
    wallSeg(doorAt + DOOR, x1, z, mat);
    /* 门楣：门洞上面那一截，不然天花板到门框之间是空的 */
    box(DOOR * 2, 0.45, 0.10, doorAt, 2.42 + 0.225, z, mat || matWall);
  }
  function wallWithDoorX(z0, z1, x, doorAt, mat) {
    wallSegX(z0, doorAt - DOOR, x, mat);
    wallSegX(doorAt + DOOR, z1, x, mat);
    box(0.10, 0.45, DOOR * 2, x, 2.42 + 0.225, doorAt, mat || matWall);
  }

  /* 走廊两壁：每间房一个门洞 */
  wallWithDoorX(Z0, ZA, -HW, 4.30, matWallWarm);        /* 西 · 卧 A */
  wallWithDoorX(ZA, Z1, -HW, 7.30, matWallWarm);        /* 西 · 卧 B */
  wallWithDoorX(Z0, ZB, HW, 4.30, matWallWarm);         /* 东 · 厨房 */
  wallWithDoorX(ZB, Z1, HW, 7.30, matWallWarm);         /* 东 · 卫浴 */
  /* 外墙 */
  wallSegX(Z0, Z1, X0, matWall);                        /* 西外墙 */
  wallSegX(Z0, Z1, X1, matWall);                        /* 东外墙 */
  wallSeg(X0, X1, Z1, matWall);                         /* 北外墙 */
  wallSeg(X0, -HW, ZA, matWall); wallSeg(HW, X1, ZB, matWall);  /* A/B、厨房/卫浴之间的隔断 */
  /* 走廊尽头那面墙：卫浴和卧 B 之间 */
  wallSegX(ZA, ZB, HW, matWall);

  /* ---------------- 灯 ----------------
     每个房间一盏，不投影（手机上四盏带阴影的灯会直接跪）。
     灯的亮度跟着"入夜"走：白天只补一点，晚上才真的照。 */
  const lamps = [];
  function lamp(x, z) {
    const p = new THREE.PointLight(0xffe6c4, 0.5, 6.2, 2);
    p.position.set(x, H - 0.35, z);
    g.add(p);
    const shade = new THREE.Mesh(new THREE.SphereGeometry(0.115, 12, 8), matLamp);
    shade.position.copy(p.position);
    g.add(shade);
    lamps.push(p);
    return p;
  }
  lamp(-2.55, 4.55); lamp(-2.55, 7.60); lamp(2.55, 4.75); lamp(2.55, 8.00); lamp(0, 6.20);

  /* ---------------- 卧 A（西 · 南） ---------------- */
  {
    furn(1.35, 0.46, 2.05, -3.20, 4.30, matWood);                 /* 床架 */
    furn(1.28, 0.16, 1.95, -3.20, 4.30, matFabric, { y: 0.46 });  /* 床垫 */
    furn(0.52, 0.11, 0.34, -3.20, 3.52, matWhite, { y: 0.62 });   /* 枕头 */
    furn(0.62, 0.12, 0.9, -3.20, 4.95, matFabric2, { y: 0.46 });  /* 被子一角 */
    furn(0.42, 0.52, 0.40, -2.10, 3.55, matWoodDark);             /* 床头柜 */
    furn(1.05, 1.92, 0.55, -3.40, 5.36, matWood);                 /* 衣柜 */
  }
  /* ---------------- 卧 B（西 · 北） ---------------- */
  {
    furn(1.30, 0.42, 1.98, -3.25, 8.10, matWood);
    furn(1.24, 0.16, 1.88, -3.25, 8.10, matFabric, { y: 0.42 });
    furn(0.50, 0.11, 0.32, -3.25, 7.36, matWhite, { y: 0.58 });
    furn(1.55, 0.74, 0.62, -2.35, 6.62, matWood);                 /* 书桌 */
    /* 书房那把凳子换成真椅子（SheenChair，CC0） */
    {
      const stool = furn(0.44, 0.46, 0.44, -2.35, 7.12, matWoodDark);   /* 凳子：模型的退路 */
      loadProp(THREE, 'models/props/SheenChair.glb',
        { x: -2.35, z: 7.12, rot: -Math.PI / 2, width: 0.56 }, (pivot) => {
          g.add(pivot);
          stool.visible = false;
        });
    }
    furn(0.90, 1.70, 0.34, -3.80, 6.70, matWoodDark);             /* 书架（靠外墙） */
  }
  /* ---------------- 厨房（东 · 南） ---------------- */
  {
    furn(1.90, 0.88, 0.62, 3.00, 3.66, matWoodDark);              /* 灶台/料理台 */
    furn(1.94, 0.06, 0.66, 3.00, 3.66, matCounter, { y: 0.88 });    /* 台面：石材/木纹贴图 */
    furn(0.42, 0.42, 0.36, 3.55, 3.62, matMetal, { y: 0.94 });    /* 水槽 */
    furn(0.58, 1.72, 0.62, 3.62, 5.55, matMetal);                 /* 冰箱 */
    furn(1.05, 0.76, 0.95, 2.60, 5.55, matWood);                  /* 小餐桌 */
    furn(0.42, 0.44, 0.42, 2.00, 5.55, matWoodDark);
    furn(0.42, 0.44, 0.42, 3.20, 5.55, matWoodDark);
  }
  /* ---------------- 卫浴（东 · 北） ---------------- */
  {
    furn(0.42, 0.40, 0.62, 3.62, 6.95, matWhite);                 /* 马桶 */
    furn(0.42, 0.74, 0.16, 3.62, 6.61, matWhite, { y: 0.40 });    /* 水箱 */
    furn(0.95, 0.80, 0.50, 2.55, 6.90, matWhite);                 /* 洗手台 */
    furn(0.40, 0.10, 0.34, 2.55, 6.90, matMetal, { y: 0.80 });    /* 盆 */
    box(0.95, 1.0, 0.03, 2.55, 1.55, 6.64, matGlass, { cast: false });  /* 镜子 */
    furn(0.95, 0.08, 0.95, 2.45, 8.55, matTile, { y: 0.02 });      /* 淋浴地盘 */
    box(0.03, 1.95, 0.95, 1.98, 0.98, 8.55, matGlass, { cast: false });
    box(0.95, 1.95, 0.03, 2.45, 0.98, 9.03, matGlass, { cast: false });
  }
  /* ---------------- 走廊端景 ---------------- */
  {
    furn(0.90, 0.32, 0.30, 0.0, 9.40, matWoodDark);               /* 窄几 */
    box(0.26, 0.34, 0.10, 0.0, 0.55, 9.40, matLamp, { cast: false });
  }

  /* 走廊里那盏"随时会亮"的顶灯，由 needLight() 控制（夜里或有人走动时才开） */
  const hallLight = new THREE.PointLight(0xffedcf, 0.0, 5.4, 2);
  hallLight.position.set(0, H - 0.30, 6.20);
  g.add(hallLight);

  return {
    group: g,
    colliders,
    lamps,
    /* 入夜系数 t∈[0,1]：白天灯只亮 15%，入夜全亮 */
    setDaylight(t) {
      const k = 0.15 + 0.85 * Math.max(0, Math.min(1, t));
      for (const p of lamps) p.intensity = 0.55 * k;
      hallLight.intensity = 0.25 * k;
    },
    /* 走廊灯：她进走廊就亮一点，出去就暗回去（不用每帧重建） */
    hall(on, k) { hallLight.intensity = on ? (0.5 + 0.5 * (k || 0)) : 0.0; },
    /* 各房间中心 —— 习惯层要用（睡觉在卧 A、喝水在厨房、排泄在卫浴） */
    rooms: {
      bedA: { x: -2.20, z: 4.30, name: '卧房' },
      bedB: { x: -2.10, z: 8.20, name: '书房' },
      kitchen: { x: 2.10, z: 4.60, name: '厨房' },
      bath: { x: 2.10, z: 7.70, name: '卫生间' },
      hall: { x: 0.0, z: 6.20, name: '走廊' },
    },
  };
}
