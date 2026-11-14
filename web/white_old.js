/* ================= 白空间 =================
   一片什么都没有的白地，几件黑线画的东西。
   不为好看，为的是：她能走、能摔、能撞，你一眼就能看见她到底动了没有。
*/
export function createWhiteSpace(THREE) {
  const g = new THREE.Group();
  const PAPER = 0xf6f5f1;
  const INK = 0x1c1c1c;

  const face = new THREE.MeshBasicMaterial({ color: PAPER });
  const faceBoth = new THREE.MeshBasicMaterial({ color: PAPER, side: THREE.DoubleSide });
  const inkMat = new THREE.LineBasicMaterial({ color: INK });
  const inkThin = new THREE.LineBasicMaterial({ color: 0x6a6a6a });

  /* 白面 + 黑边，所有东西都这么画 */
  function solid(geo, x, y, z, rotY = 0, thin = false) {
    const m = new THREE.Mesh(geo, face);
    m.position.set(x, y, z); m.rotation.y = rotY;
    g.add(m);
    const e = new THREE.LineSegments(new THREE.EdgesGeometry(geo, 20), thin ? inkThin : inkMat);
    e.position.copy(m.position); e.rotation.copy(m.rotation);
    g.add(e);
    m.userData.edge = e;
    return m;
  }
  const box = (w, h, d, x, y, z, rotY, thin) =>
    solid(new THREE.BoxGeometry(w, h, d), x, y, z, rotY, thin);

  /* ---- 地 ---- */
  const ground = new THREE.Mesh(new THREE.PlaneGeometry(64, 64), face);
  ground.rotation.x = -Math.PI / 2;
  g.add(ground);

  /* 很淡的点阵：白空间里没有它，你会分不清是她在走还是你在转 */
  const dotN = 22, dotGeo = [], dotMat = new THREE.PointsMaterial({
    color: 0xd8d6d1, size: 0.045, sizeAttenuation: true,
  });
  for (let i = -dotN; i <= dotN; i++)
    for (let j = -dotN; j <= dotN; j++) dotGeo.push(i * 1.2, 0.002, j * 1.2);
  const pts = new THREE.BufferGeometry();
  pts.setAttribute('position', new THREE.Float32BufferAttribute(dotGeo, 3));
  g.add(new THREE.Points(pts, dotMat));

  /* ---- 一扇门：她能走进去（走不进去也行，先立着） ---- */
  box(1.3, 2.3, 0.12, -5.2, 1.15, -4.4);
  box(0.86, 2.05, 0.06, -5.2, 1.02, -4.33);      // 门板
  const knob = new THREE.Mesh(new THREE.SphereGeometry(0.045, 10, 8), face);
  knob.position.set(-4.9, 1.0, -4.28); g.add(knob);
  const ring = new THREE.LineSegments(
    new THREE.EdgesGeometry(new THREE.SphereGeometry(0.045, 10, 8), 30), inkMat);
  ring.position.copy(knob.position); g.add(ring);

  /* ---- 毯子：一块方格布 ---- */
  box(1.5, 0.012, 1.5, 2.1, 0.006, 1.8);
  for (let i = -2; i <= 2; i++) {
    const v = new THREE.BufferGeometry().setFromPoints([
      new THREE.Vector3(2.1 + i * 0.3, 0.014, 1.05),
      new THREE.Vector3(2.1 + i * 0.3, 0.014, 2.55),
    ]);
    g.add(new THREE.Line(v, inkThin));
    const h = new THREE.BufferGeometry().setFromPoints([
      new THREE.Vector3(1.35, 0.014, 1.8 + i * 0.3),
      new THREE.Vector3(2.85, 0.014, 1.8 + i * 0.3),
    ]);
    g.add(new THREE.Line(h, inkThin));
  }

  /* ---- 一台笔记本：开着，屏幕朝着毯子 ---- */
  box(0.34, 0.02, 0.24, 2.1, 0.03, 1.35);
  const lid = box(0.34, 0.24, 0.014, 2.1, 0.15, 1.24, 0);
  lid.rotation.x = -0.32;
  lid.userData.edge.rotation.x = -0.32;

  /* ---- 灯泡：唯一的光 ---- */
  const cord = new THREE.BufferGeometry().setFromPoints([
    new THREE.Vector3(0, 3.15, 0), new THREE.Vector3(0, 2.62, 0),
  ]);
  g.add(new THREE.Line(cord, inkMat));
  const bulb = new THREE.Mesh(new THREE.SphereGeometry(0.085, 14, 10), face);
  bulb.position.set(0, 2.56, 0); g.add(bulb);
  const bulbRing = new THREE.LineSegments(
    new THREE.EdgesGeometry(new THREE.SphereGeometry(0.085, 14, 10), 30), inkMat);
  bulbRing.position.copy(bulb.position); g.add(bulbRing);

  const lamp = new THREE.PointLight(0xfff4e2, 40, 16, 2);
  lamp.position.set(0, 2.5, 0);
  g.add(lamp);

  /* ---- 她走不到头的地方：一圈淡淡的墙，暗示这不是无限大 ---- */
  const wallH = 3.2;
  [[0, 0, -11, 0], [0, 0, 11, 0], [-11, 0, 0, 1], [11, 0, 0, 1]].forEach(([x, , z, rot]) => {
    const geo = new THREE.PlaneGeometry(22, wallH);
    const m = new THREE.Mesh(geo, faceBoth);
    m.position.set(x, wallH / 2, z);
    m.rotation.y = rot * Math.PI / 2 + Math.PI;
    g.add(m);
    const e = new THREE.LineSegments(new THREE.EdgesGeometry(geo), inkThin);
    e.position.copy(m.position); e.rotation.copy(m.rotation);
    g.add(e);
  });

  let t = 0;
  return {
    group: g,
    /* 灯泡晃一点点，不晃也不像死的 */
    update(dt) {
      t += dt;
      const s = Math.sin(t * 1.1) * 0.012, c = Math.cos(t * 0.83) * 0.010;
      bulb.position.set(s, 2.56, c);
      bulbRing.position.copy(bulb.position);
      lamp.position.set(s, 2.5, c);
    },
    /* 她能撞的东西：门框和墙 */
    props: [
      { x0: -5.87, x1: -4.53, z0: -4.46, z1: -4.34, h: 2.3 },
    ],
  };
}
