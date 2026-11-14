/* ================= 白空间 · 摄影棚版 =================
   还是纸白 + 墨线。区别是这版所有面都吃光、都收影子：
   地面有纸纤维、墙有接缝、灯真的有光锥、门框会在地上投一条斜影。

   道具（台阶/椅子/苹果/杯子）不在这里画 —— 那些来自 assets/soma.json 的 world.props，
   由 app.js 的 buildWorld() 建。这里只放"场景本身"：地、墙、门、毯子、笔记本、灯。
*/

export function createWhiteSpace(THREE, quality) {
  const q = quality || { decor: 1 };
  const g = new THREE.Group();
  const INK = 0x22221f;

  /* ---------- 纸面：一遍程序化噪点，靠得近才看得见 ---------- */
  function paperTexture() {
    const S = 256;
    const c = document.createElement('canvas');
    c.width = c.height = S;
    const x = c.getContext('2d');
    x.fillStyle = '#f6f5f1';
    x.fillRect(0, 0, S, S);
    const img = x.getImageData(0, 0, S, S);
    const d = img.data;
    for (let i = 0; i < d.length; i += 4) {
      const n = (Math.random() - 0.5) * 7;
      d[i] += n; d[i + 1] += n; d[i + 2] += n * 0.85;
    }
    x.putImageData(img, 0, 0);
    x.strokeStyle = 'rgba(120,116,108,0.05)';
    x.lineWidth = 1;
    for (let i = 0; i < 26; i++) {
      x.beginPath();
      const y = Math.random() * S, len = 20 + Math.random() * 90;
      const x0 = Math.random() * S;
      x.moveTo(x0, y);
      x.bezierCurveTo(x0 + len * 0.3, y + 2, x0 + len * 0.6, y - 2, x0 + len, y + 1);
      x.stroke();
    }
    const t = new THREE.CanvasTexture(c);
    t.wrapS = t.wrapT = THREE.RepeatWrapping;
    t.repeat.set(14, 14);
    t.anisotropy = 4;
    if ('colorSpace' in t) t.colorSpace = THREE.SRGBColorSpace;
    return t;
  }

  const floorMat = new THREE.MeshStandardMaterial({
    color: 0xdfdbd3, map: paperTexture(), roughness: 0.96, metalness: 0.0,
  });
  const ground = new THREE.Mesh(new THREE.PlaneGeometry(90, 90), floorMat);
  ground.rotation.x = -Math.PI / 2;
  ground.receiveShadow = true;
  g.add(ground);

  /* ---------- 墙 ---------- */
  const wallMat = new THREE.MeshStandardMaterial({ color: 0xe4e1d9, roughness: 0.98, metalness: 0 });
  const WALL = 46.0, WALLH = 5.4;
  const wallGeo = new THREE.PlaneGeometry(WALL * 2, WALLH);
  const wallEdge = new THREE.LineBasicMaterial({ color: 0xd9d6cf, transparent: true, opacity: 0.9 });
  [[0, -WALL, 0], [0, WALL, Math.PI], [-WALL, 0, Math.PI / 2], [WALL, 0, -Math.PI / 2]]
    .forEach(([x, z, rot]) => {
      const m = new THREE.Mesh(wallGeo, wallMat);
      m.position.set(x, WALLH / 2 - 0.1, z);
      m.rotation.y = rot;
      m.receiveShadow = true;
      g.add(m);
      const e = new THREE.LineSegments(new THREE.EdgesGeometry(wallGeo), wallEdge);
      e.position.copy(m.position); e.rotation.copy(m.rotation);
      g.add(e);
    });

  const lampParts = [];   /* 每个模块的灯都要单独晃，所以统一收着 */

  /* ---------- 一个模块 ----------
     两种：A 是"门 + 毯子 + 笔记本 + 吊灯"，B 是"窗 + 长凳 + 一摞书 + 吊灯"。
     A B 交替摆，整条走廊的周期就是 16 米；她走过 16 米时世界平移回一格，
     因为两边一模一样，这一步在画面上看不见（以前是"闪现回起点"）。 */
  function moduleAt(ox, kind) {
  const isA = kind !== 'B';
  /* ---------- 门：立在墙边，门框会在地上拉一条影 ---------- */
  const woodMat = new THREE.MeshStandardMaterial({ color: 0xe9e5dc, roughness: 0.72, metalness: 0.02 });
  const inkLine = new THREE.LineBasicMaterial({ color: INK, transparent: true, opacity: 0.85 });

  function panel(w, h, d, x, y, z, mat, edge = true) {
    const geo = new THREE.BoxGeometry(w, h, d);
    const m = new THREE.Mesh(geo, mat);
    m.position.set(x, y, z);
    m.castShadow = true; m.receiveShadow = true;
    g.add(m);
    if (edge) {
      const e = new THREE.LineSegments(new THREE.EdgesGeometry(geo), inkLine);
      e.position.copy(m.position);
      e.userData.follow = m;
      g.add(e);
    }
    return m;
  }

  const DOOR = { x: 0.40, z: -4.35 };
  if (isA) {
    /* 门 */
    panel(1.5, 0.10, 0.16, (DOOR.x + ox), 2.34, DOOR.z, woodMat);
    panel(0.14, 2.34, 0.16, (DOOR.x + ox) - 0.68, 1.17, DOOR.z, woodMat);
    panel(0.14, 2.34, 0.16, (DOOR.x + ox) + 0.68, 1.17, DOOR.z, woodMat);
    const doorLeaf = panel(1.16, 2.20, 0.05, (DOOR.x + ox), 1.10, DOOR.z + 0.02,
      new THREE.MeshStandardMaterial({ color: 0xfaf8f4, roughness: 0.6 }));
    doorLeaf.rotation.y = -0.16;
    const knob = new THREE.Mesh(new THREE.SphereGeometry(0.038, 16, 12),
      new THREE.MeshStandardMaterial({ color: 0xc9a95f, roughness: 0.28, metalness: 0.9 }));
    knob.position.set((DOOR.x + ox) + 0.44, 1.02, DOOR.z + 0.14);
    knob.castShadow = true;
    g.add(knob);
  } else {
    /* 窗：不透明的一块亮面，外面什么都没有，但光从那儿进来 */
    const wx = DOOR.x + ox, wy = 1.62, ww = 1.42, wh = 1.18;
    const frameMat = new THREE.MeshStandardMaterial({ color: 0xf4f1ea, roughness: 0.7 });
    panel(ww + 0.14, 0.09, 0.12, wx, wy + wh / 2 + 0.04, DOOR.z + 0.02, frameMat);
    panel(ww + 0.14, 0.09, 0.12, wx, wy - wh / 2 - 0.04, DOOR.z + 0.02, frameMat);
    panel(0.09, wh + 0.18, 0.12, wx - ww / 2 - 0.02, wy, DOOR.z + 0.02, frameMat);
    panel(0.09, wh + 0.18, 0.12, wx + ww / 2 + 0.02, wy, DOOR.z + 0.02, frameMat);
    panel(0.05, wh, 0.06, wx, wy, DOOR.z + 0.03, frameMat, false);
    const glass = new THREE.Mesh(new THREE.PlaneGeometry(ww, wh),
      new THREE.MeshBasicMaterial({ color: 0xf7f9fc }));
    glass.position.set(wx, wy, DOOR.z - 0.02);
    g.add(glass);
    const spill = new THREE.PointLight(0xdfe9f6, 2.6, 7.5, 2);
    spill.position.set(wx, wy - 0.1, DOOR.z + 0.85);
    g.add(spill);
  }

  /* ---------- 门所在的隔墙：门得长在墙上，不然它就是飘在半空的框 ---------- */
  const partition = new THREE.Mesh(new THREE.PlaneGeometry(4.6, 3.6),
    new THREE.MeshStandardMaterial({ color: 0xf0ede6, roughness: 0.97, metalness: 0 }));
  partition.position.set((DOOR.x + ox), 1.75, DOOR.z - 0.14);
  partition.receiveShadow = true;
  partition.castShadow = true;
  g.add(partition);
  const partEdge = new THREE.LineSegments(new THREE.EdgesGeometry(new THREE.PlaneGeometry(4.6, 3.6)),
    new THREE.LineBasicMaterial({ color: 0xd5d1c8, transparent: true, opacity: 0.9 }));
  partEdge.position.copy(partition.position);
  g.add(partEdge);

  if (isA) {
  /* ---------- 毯子 ---------- */
  const rugMat = new THREE.MeshStandardMaterial({ color: 0xe4e1d8, roughness: 0.95 });
  const rug = new THREE.Mesh(new THREE.PlaneGeometry(2.6, 2.6), rugMat);
  rug.rotation.x = -Math.PI / 2;
  rug.position.set((3.40 + ox), 0.006, -2.60);
  rug.receiveShadow = true;
  g.add(rug);
  const rugEdge = new THREE.LineSegments(new THREE.EdgesGeometry(new THREE.PlaneGeometry(2.6, 2.6)),
    new THREE.LineBasicMaterial({ color: 0xc8c4ba, transparent: true, opacity: 0.9 }));
  rugEdge.rotation.x = -Math.PI / 2; rugEdge.position.set((3.40 + ox), 0.008, -2.60);
  g.add(rugEdge);
  const weave = new THREE.LineBasicMaterial({ color: 0xcfcbc1, transparent: true, opacity: 0.5 });
  for (let i = -2; i <= 2; i++) {
    g.add(new THREE.Line(new THREE.BufferGeometry().setFromPoints([
      new THREE.Vector3((3.40 + ox) + i * 0.42, 0.009, -3.9), new THREE.Vector3((3.40 + ox) + i * 0.42, 0.009, -1.3)]), weave));
    g.add(new THREE.Line(new THREE.BufferGeometry().setFromPoints([
      new THREE.Vector3(2.1 + ox, 0.009, -2.60 + i * 0.42), new THREE.Vector3(4.7 + ox, 0.009, -2.60 + i * 0.42)]), weave));
  }

  /* ---------- 笔记本 ---------- */
  const lapMat = new THREE.MeshStandardMaterial({ color: 0x2b2f36, roughness: 0.45, metalness: 0.3 });
  panel(0.34, 0.016, 0.24, (3.40 + ox), 0.014, -3.30, lapMat);
  const lid = panel(0.34, 0.22, 0.012, (3.40 + ox), 0.11, -3.40, lapMat);
  lid.rotation.x = -0.30;
  const screen = new THREE.Mesh(new THREE.PlaneGeometry(0.30, 0.18),
    new THREE.MeshBasicMaterial({ color: 0xdfe9f4 }));
  screen.position.set(3.40 + ox, 0.112, -3.407);
  screen.rotation.x = -0.30;
  g.add(screen);

  /* 屏幕上一点柔光，像开着的终端 */
  const screenGlow = new THREE.PointLight(0xcfe3ff, 0.45, 2.2, 2);
  screenGlow.position.set((3.40 + ox), 0.16, -3.34);
  g.add(screenGlow);
  } else {
    /* B 的家具：一条长凳 + 一摞书 */
    const benchMat = new THREE.MeshStandardMaterial({ color: 0xcdc7ba, roughness: 0.82 });  /* 白墙前面放白家具＝看不见，压深两档 */
    /* 长凳：一整块实心的（薄板 + 两条细腿会读成"飘着的搁板"） */
    panel(1.56, 0.07, 0.44, 2.60 + ox, 0.415, -3.86, benchMat);
    panel(1.44, 0.38, 0.34, 2.60 + ox, 0.19, -3.88, benchMat, true);
    const bookCols = [0xd9d2c4, 0xc9c0ae, 0xe2dcd0, 0xbfb6a4];
    for (let i = 0; i < 4; i++) {
      panel(0.26 - i * 0.01, 0.035, 0.19, 1.98 + ox, 0.468 + i * 0.037, -3.88,
        new THREE.MeshStandardMaterial({ color: bookCols[i], roughness: 0.85 }));
    }
    /* 毯子换小一块，不然两种模块看着一模一样 */
    const mat = new THREE.Mesh(new THREE.PlaneGeometry(1.5, 1.5),
      new THREE.MeshStandardMaterial({ color: 0xdfdbd0, roughness: 0.96 }));
    mat.rotation.x = -Math.PI / 2;
    mat.position.set(2.60 + ox, 0.006, -2.30);
    mat.receiveShadow = true;
    g.add(mat);
  }

  /* ---------- 灯 ---------- */
  const cordGeo = new THREE.BufferGeometry().setFromPoints([
    new THREE.Vector3((5.05 + ox), 5.3, -1.65), new THREE.Vector3((5.05 + ox), 2.72, -1.65)]);
  g.add(new THREE.Line(cordGeo, new THREE.LineBasicMaterial({ color: 0x8b877f })));
  const shadeMat = new THREE.MeshStandardMaterial({
    color: 0xfffdf6, roughness: 0.55, metalness: 0.0,
    emissive: 0xfff1d8, emissiveIntensity: 0.28, side: THREE.DoubleSide });
  const shade = new THREE.Mesh(new THREE.ConeGeometry(0.30, 0.26, 24, 1, true), shadeMat);
  shade.position.set((5.05 + ox), 2.62, -1.65);
  shade.castShadow = true;
  g.add(shade);
  const bulb = new THREE.Mesh(new THREE.SphereGeometry(0.055, 14, 10),
    new THREE.MeshBasicMaterial({ color: 0xfff8e8 }));
  bulb.position.set((5.05 + ox), 2.50, -1.65);
  g.add(bulb);
  const lamp = new THREE.PointLight(0xffeccb, 4.2, 9, 2);
  lamp.position.set((5.05 + ox), 2.44, -1.65);
  lamp.castShadow = false;
  g.add(lamp);
  lampParts.push({ shade, bulb, lamp, ox });


  }
  const moduleProps = [];
  [[-8, 'B'], [0, 'A'], [8, 'B'], [16, 'A']].forEach(([ox, kind]) => {
    moduleAt(ox, kind);
    if (kind === 'A') moduleProps.push({ x0: 0.40 + ox - 0.75, x1: 0.40 + ox + 0.75,
                                         z0: -4.35 - 0.10, z1: -4.35 + 0.10, h: 2.4 });
  });

  /* ---------- 细柱：也必须在格子里 ----------
     以前是散在三处的固定坐标，世界一平移它们就会整根跳 8 米 —— 那是穿帮。 */
  if (q.decor > 0) {
    const colMat = new THREE.MeshStandardMaterial({ color: 0xf3f1eb, roughness: 0.9 });
    [-8, 0, 8, 16].forEach(ox => {
      [[-2.60 + ox, -3.20, 0.10, 3.6], [4.60 + ox, -3.40, 0.08, 3.0]]
        .forEach(([x, z, r, h]) => {
          const c = new THREE.Mesh(new THREE.CylinderGeometry(r, r * 1.08, h, 14), colMat);
          c.position.set(x, h / 2, z);
          c.castShadow = true; c.receiveShadow = true;
          g.add(c);
        });
    });
  }

  /* ---------- 空气里的纸灰：让"白"有厚度 ---------- */
  let dust = null;
  if (q.decor > 0) {
    const N = 140, pos = new Float32Array(N * 3), vel = new Float32Array(N * 3);
    let seed = 7;
    const rnd = () => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; };
    for (let i = 0; i < N; i++) {
      pos[i * 3] = (rnd() - 0.5) * 18;
      pos[i * 3 + 1] = 0.2 + rnd() * 4.2;
      pos[i * 3 + 2] = (rnd() - 0.5) * 18;
      vel[i * 3] = (rnd() - 0.5) * 0.06;
      vel[i * 3 + 1] = -0.008 - rnd() * 0.016;
      vel[i * 3 + 2] = (rnd() - 0.5) * 0.06;
    }
    const dg = new THREE.BufferGeometry();
    dg.setAttribute('position', new THREE.BufferAttribute(pos, 3));
    dust = new THREE.Points(dg, new THREE.PointsMaterial({
      color: 0xb9b3a6, size: 0.022, sizeAttenuation: true,
      transparent: true, opacity: 0.55, depthWrite: false }));
    g.add(dust);
    dust.userData.vel = vel;
  }

  let t = 0;
  return {
    group: g,
    update(dt) {
      t += dt;
      const sway = Math.sin(t * 0.9) * 0.020 + Math.sin(t * 2.3) * 0.004;
      const sway2 = Math.cos(t * 0.72) * 0.017;
      for (let i = 0; i < lampParts.length; i++) {
        const lp = lampParts[i];
        const bx = 5.05 + lp.ox;
        lp.bulb.position.set(bx + sway, 2.50, -1.65 + sway2);
        lp.shade.position.set(bx + sway, 2.62, -1.65 + sway2);
        lp.lamp.position.set(bx + sway, 2.44, -1.65 + sway2);
        lp.lamp.intensity = 4.1 + Math.sin(t * 7.1 + i) * 0.09;
      }
      if (dust) {
        const p = dust.geometry.attributes.position.array, v = dust.userData.vel;
        for (let i = 0; i < p.length; i += 3) {
          p[i] += v[i] * dt; p[i + 1] += v[i + 1] * dt; p[i + 2] += v[i + 2] * dt;
          if (p[i + 1] < 0.1) p[i + 1] = 4.4;
        }
        dust.geometry.attributes.position.needsUpdate = true;
      }
    },
    props: moduleProps,
  };
}
