/* ================= 鲸鱼娘 · 低模 =================
   做法：不蒙皮、不做骨骼重定向 —— 把几何体直接钉在 18 个节点上。
   物理怎么动，她就怎么动；多出来的只有一条尾巴，单独用弹簧链滞后于骨盆。
   好处：C++ 那边加什么动作、摔成什么样，模型都自动跟，不用再对一次骨架。
   朝向：+x 是脸（因为 walker 里脚尖朝 +x）。
*/

export function createWhaleGirl(THREE, pal = {}) {
  const V = (x, y, z) => new THREE.Vector3(x, y, z);
  const UP = V(0, 1, 0);
  const PI = Math.PI;

  const std = (c, rough = 0.6, metal = 0) =>
    new THREE.MeshStandardMaterial({ color: c, roughness: rough, metalness: metal });
  const flat = (c, op = 1) =>
    new THREE.MeshBasicMaterial({ color: c, transparent: op < 1, opacity: op });

  /* 换配色不用改几何：给她一份 palette，玩家那份就是另一个她 */
  const C = Object.assign({
    skin: 0xffe0cc, hair: 0x18305c, hairLt: 0x2b6ba8,
    dress: 0x121b30, dress2: 0x1d2c4c, white: 0xf4f8ff, sock: 0xf7faff,
    shoe: 0x0c1526, eye: 0x0b1a2e, blush: 0x8fbcec,
    tail: 0x2663a4, fin: 0x57b6e0, metal: 0xd8b45a,
    emissive: 0x2b3550,
  }, pal);

  const M = {
    skin:   std(C.skin, 0.72),
    hair:   std(C.hair, 0.58, 0.06),
    hairLt: std(C.hairLt, 0.52, 0.08),
    dress:  std(C.dress, 0.78),
    dress2: std(C.dress2, 0.7),
    white:  new THREE.MeshStandardMaterial({ color: C.white, roughness: 0.82, emissive: C.emissive, emissiveIntensity: 0.45 }),
    sock:   new THREE.MeshStandardMaterial({ color: C.sock, roughness: 0.9, emissive: C.emissive, emissiveIntensity: 0.5 }),
    shoe:   std(C.shoe, 0.42, 0.28),
    eye:    flat(C.eye),
    glint:  flat(0xffffff),
    blush:  flat(C.blush, 0.42),
    tail:   std(C.tail, 0.5, 0.05),
    fin:    std(C.fin, 0.42, 0.08),
    metal:  std(C.metal, 0.3, 0.75),
  };

  const g = new THREE.Group();
  const add = (m, parent = g) => { parent.add(m); return m; };

  /* 一段圆柱：从 a 拉到 b，半径 r（首尾一样粗） */
  function seg(mesh, a, b, r) {
    const d = new THREE.Vector3().subVectors(b, a);
    const L = d.length() || 1e-4;
    mesh.position.copy(a).addScaledVector(d, 0.5);
    mesh.quaternion.setFromUnitVectors(UP, d.divideScalar(L));
    mesh.scale.set(r, L, r);
    return mesh;
  }
  /* 一段锥台：首尾半径不同，用 CylinderGeometry(1,1,1) 缩放做不出来，单独建 */
  function cone(a, b, rA, rB, mat, segs = 14) {
    const d = new THREE.Vector3().subVectors(b, a);
    const L = d.length() || 1e-4;
    const m = new THREE.Mesh(new THREE.CylinderGeometry(rB, rA, 1, segs, 1, true), mat);
    m.position.copy(a).addScaledVector(d, 0.5);
    m.quaternion.setFromUnitVectors(UP, d.clone().divideScalar(L));
    m.scale.y = L;
    return add(m);
  }

  const CYL = new THREE.CylinderGeometry(1, 1, 1, 12, 1);
  const SPH = new THREE.SphereGeometry(1, 16, 12);

  /* ---------------- 头 ---------------- */
  const headG = new THREE.Group(); add(headG);
  const skull = add(new THREE.Mesh(SPH, M.skin), headG);
  // 刘海：锅盖，包住上半的头
  const bang = add(new THREE.Mesh(
    new THREE.SphereGeometry(1, 20, 12, 0, PI * 2, 0, PI * 0.42), M.hair), headG);
  // 后发：一大坨垂到背
  const backHair = add(new THREE.Mesh(SPH, M.hairLt), headG);
  // 呆毛：一根细毛，会晃
  const ahoge = add(new THREE.Mesh(new THREE.CylinderGeometry(0.0065, 0.0022, 0.135, 6), M.hair), headG);
  // 耳鳍 ×2
  const earFin = [0, 1].map(() => {
    const m = add(new THREE.Mesh(new THREE.ConeGeometry(0.03, 0.075, 8), M.fin), headG);
    m.scale.z = 0.34;
    return m;
  });
  // 侧发 ×2
  const sideHair = [0, 1].map(() => add(new THREE.Mesh(SPH, M.hair), headG));
  // 眼睛 + 高光 + 腮红
  const eyes = [0, 1].map(() => add(new THREE.Mesh(SPH, M.eye), headG));
  const glints = [0, 1].map(() => add(new THREE.Mesh(SPH, M.glint), headG));
  const blushes = [0, 1].map(() => add(new THREE.Mesh(SPH, M.blush), headG));
  const mouth = add(new THREE.Mesh(new THREE.CircleGeometry(1, 12, PI, PI), M.eye), headG);

  /* ---------------- 躯干 ---------------- */
  const torsoMesh = add(new THREE.Mesh(new THREE.CylinderGeometry(1, 1, 1, 16, 1), M.dress));
  const apron = add(new THREE.Mesh(
    new THREE.CylinderGeometry(1, 1, 1, 16, 1, true, PI * 0.5 - 0.85, 1.70), M.white));
  const collar = add(new THREE.Mesh(new THREE.TorusGeometry(1, 0.32, 6, 16), M.white));
  const bow = add(new THREE.Mesh(new THREE.BoxGeometry(1, 1, 1), M.metal));

  /* ---------------- 裙 ---------------- */
  const skirt = add(new THREE.Mesh(new THREE.CylinderGeometry(0.132, 0.208, 1, 18, 1, true), M.dress2));
  const skirtIn = add(new THREE.Mesh(new THREE.CylinderGeometry(0.124, 0.196, 1, 18, 1, true), M.white));

  /* ---------------- 四肢 ---------------- */
  const upperArm = [0, 1].map(() => add(new THREE.Mesh(CYL, M.skin)));
  const foreArm = [0, 1].map(() => add(new THREE.Mesh(CYL, M.skin)));
  const puff = [0, 1].map(() => add(new THREE.Mesh(SPH, M.white)));
  const hand = [0, 1].map(() => add(new THREE.Mesh(SPH, M.skin)));
  const thigh = [0, 1].map(() => add(new THREE.Mesh(CYL, M.skin)));
  const shank = [0, 1].map(() => add(new THREE.Mesh(CYL, M.sock)));
  const sockTop = [0, 1].map(() => add(new THREE.Mesh(new THREE.TorusGeometry(1, 0.22, 6, 14), M.white)));
  const shoe = [0, 1].map(() => add(new THREE.Mesh(SPH, M.shoe)));

  /* ---------------- 尾巴 ---------------- */
  const TAILN = 11;
  const tailBall = [];
  for (let i = 0; i < TAILN; i++) tailBall.push(add(new THREE.Mesh(SPH, i > 7 ? M.fin : M.tail)));
  const fluke = [0, 1].map(() => add(new THREE.Mesh(SPH, M.fin)));
  const tailP = []; for (let i = 0; i < TAILN; i++) tailP.push(V(-0.05 * i, -0.02 * i, 0));
  let tailReady = false;

  let blinkT = 1.5, blink = 0;
  let lastHead = null;

  function update(nodes, radii, dt, tt) {
    const head = nodes[3];
    const hr = Math.max(0.085, radii[3]);
    const chest = nodes[1], pelvis = nodes[0], neck = nodes[2];

    /* --- 头 --- */
    headG.position.copy(head);
    headG.scale.setScalar(1.24);
    skull.scale.setScalar(hr);
    bang.scale.setScalar(hr * 1.05);
    bang.position.set(0.002, hr * 0.06, 0);
    bang.rotation.set(0, 0, 0);

    backHair.position.set(-hr * 0.42, -hr * 0.72, 0);
    backHair.scale.set(hr * 0.92, hr * 1.55, hr * 0.92);

    const wob = Math.sin(tt * 3.1) * 0.16 + Math.sin(tt * 7.3) * 0.05;
    ahoge.position.set(hr * 0.16, hr * 1.02, 0);
    ahoge.rotation.set(0, 0, -0.5 + wob);
    ahoge.scale.set(1, 1, 1);

    for (let i = 0; i < 2; i++) {
      const s = i === 0 ? 1 : -1;
      earFin[i].position.set(-hr * 0.20, hr * 0.30, s * hr * 0.90);
      earFin[i].rotation.set(s * 0.5, 0, 0.55 + wob * 0.4 * s);
      earFin[i].scale.set(1, 1 + wob * 0.1, 0.34);

      sideHair[i].position.set(hr * 0.30, -hr * 0.55, s * hr * 0.78);
      sideHair[i].scale.set(hr * 0.30, hr * 0.85, hr * 0.26);

      eyes[i].position.set(hr * 0.835, hr * 0.02, s * hr * 0.360);
      eyes[i].scale.set(hr * 0.135, hr * 0.205 * (1 - blink * 0.93), hr * 0.150);

      glints[i].position.set(hr * 0.90, hr * 0.10, s * hr * 0.315);
      glints[i].scale.setScalar(hr * 0.055 * (1 - blink));

      blushes[i].position.set(hr * 0.795, -hr * 0.22, s * hr * 0.545);
      blushes[i].scale.set(hr * 0.10, hr * 0.055, hr * 0.075);
    }
    mouth.position.set(hr * 0.93, -hr * 0.26, 0);
    mouth.scale.setScalar(hr * 0.055);
    mouth.rotation.set(0, 0, 0);

    /* 眨眼 */
    blinkT -= dt;
    if (blinkT <= 0) { blink = 1; blinkT = 2.2 + Math.random() * 3.5; }
    blink = Math.max(0, blink - dt * 9);

    /* --- 躯干 --- */
    const up = new THREE.Vector3().subVectors(chest, pelvis);
    const tL = up.length() || 0.3;
    torsoMesh.position.copy(pelvis).addScaledVector(up, 0.52);
    torsoMesh.quaternion.setFromUnitVectors(UP, up.clone().normalize());
    torsoMesh.scale.set(0.106, tL * 1.02, 0.097);

    apron.position.copy(torsoMesh.position);
    apron.quaternion.copy(torsoMesh.quaternion);
    apron.scale.set(0.109, tL * 0.96, 0.104);

    collar.position.copy(neck).lerp(chest, 0.15);
    collar.quaternion.setFromUnitVectors(UP, up.clone().normalize());
    collar.scale.set(0.062, 0.062, 0.062);

    bow.position.copy(pelvis).addScaledVector(up, 0.80);
    bow.position.x += 0.075;
    bow.scale.set(0.045, 0.03, 0.055);

    /* --- 裙：从骨盆往下张 --- */
    const skTop = pelvis.clone().addScaledVector(up.clone().normalize(), -0.02);
    const skBot = pelvis.clone().add(V(0, -0.255, 0));
    skirt.position.copy(skTop).add(skBot).multiplyScalar(0.5);
    skirt.quaternion.setFromUnitVectors(UP, V(0, 1, 0));
    skirt.scale.set(1, 0.26, 1);
    skirtIn.position.copy(skirt.position);
    skirtIn.position.y -= 0.014;
    skirtIn.scale.set(1, 0.335, 1);

    /* --- 手臂 --- */
    for (let i = 0; i < 2; i++) {
      const base = 4 + i * 3;
      const sh = nodes[base], el = nodes[base + 1], hd = nodes[base + 2];
      seg(upperArm[i], sh, el, 0.030);
      seg(foreArm[i], el, hd, 0.025);
      puff[i].position.copy(sh);
      puff[i].scale.setScalar(0.046);
      hand[i].position.copy(hd);
      hand[i].scale.setScalar(0.031);
    }

    /* --- 腿 --- */
    for (let i = 0; i < 2; i++) {
      const base = 10 + i * 4;
      const hip = nodes[base], kn = nodes[base + 1], an = nodes[base + 2], toe = nodes[base + 3];
      seg(thigh[i], hip, kn, 0.050);
      seg(shank[i], kn, an, 0.043);
      sockTop[i].position.copy(kn).lerp(an, 0.12);
      sockTop[i].quaternion.copy(shank[i].quaternion);
      sockTop[i].scale.setScalar(0.048);
      shoe[i].position.copy(an).lerp(toe, 0.55).add(V(0, -0.02, 0));
      shoe[i].scale.set(0.115, 0.048, 0.058);
    }

    /* --- 尾巴：弹簧链 --- */
    const back = V(-1, -0.35, 0).normalize();
    const rootT = pelvis.clone().addScaledVector(back, 0.09).add(V(0, -0.045, 0));
    if (!tailReady) { for (let i = 0; i < TAILN; i++) tailP[i].copy(rootT).addScaledVector(V(-1, -0.15, 0).normalize(), 0.062 * i); tailReady = true; }
    tailP[0].lerp(rootT, 1 - Math.exp(-dt * 26));
    const segLen = 0.052;
    for (let i = 1; i < TAILN; i++) {
      const d = tailP[i].clone().sub(tailP[i - 1]);
      d.y -= 0.30 * segLen;
      if (d.lengthSq() < 1e-8) d.set(-1, 0, 0);
      const want = tailP[i - 1].clone().addScaledVector(d.normalize(), segLen);
      tailP[i].lerp(want, 1 - Math.exp(-dt * (20 - i * 0.7)));
    }
    for (let i = 1; i < TAILN; i++) {
      const d = tailP[i].clone().sub(tailP[i - 1]);
      if (d.lengthSq() > 1e-8) tailP[i].copy(tailP[i - 1]).addScaledVector(d.normalize(), segLen);
    }
    for (let i = 0; i < TAILN; i++) {
      tailBall[i].position.copy(tailP[i]);
      const r = 0.088 * (1 - i / (TAILN + 3.0));
      tailBall[i].scale.set(r, r * 0.94, r);
    }
    // 尾鳍：末端两片，跟着最后一段的方向张
    const td = tailP[TAILN - 1].clone().sub(tailP[TAILN - 2]).normalize();
    for (let i = 0; i < 2; i++) {
      const s = i === 0 ? 1 : -1;
      fluke[i].position.copy(tailP[TAILN - 1]).addScaledVector(td, 0.03);
      fluke[i].scale.set(0.085, 0.012, 0.045);
      fluke[i].rotation.set(s * 0.5, 0, 0);
      fluke[i].position.z += s * 0.028;
    }

    lastHead = head;
  }

  g.traverse(o => { if (o.isMesh) { o.castShadow = true; o.receiveShadow = false; } });
  return { group: g, update };
}
