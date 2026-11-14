/* ================= 光 与 空 间 =================
   原来只有一盏环境光 + 两盏方向光，地面纯白没有接收面，
   所以画面读起来像"白屏上贴了个模型"。

   这里做的是摄影棚那套：一主两辅一背，一盏有影子的灯负责落地，
   环境贴图负责让皮肤/布料有反射，色调映射负责别把白烧成死白。
   白空间的方向不变——纸白、墨线、没有多余颜色。
*/

export function createStage(THREE, { renderer, scene, quality, room }) {
  const PAPER = 0xf6f5f1;
  const q = quality || { dpr: 1.5, shadow: 1024, decor: 1 };

  /* ---------- 色调映射：白要白得住，暗部要留得住 ---------- */
  if (renderer) {
    renderer.toneMapping = THREE.ACESFilmicToneMapping;
    renderer.toneMappingExposure = 0.95;
    if ('outputColorSpace' in renderer) renderer.outputColorSpace = THREE.SRGBColorSpace;
    renderer.shadowMap.enabled = true;
    renderer.shadowMap.type = (window.__SHT === 2 ? THREE.BasicShadowMap : THREE.PCFShadowMap);
  }

  /* ---------- 环境：一张竖向渐变当 IBL ---------- */
  function envTexture() {
    const c = document.createElement('canvas');
    c.width = 128; c.height = 128;
    const x = c.getContext('2d');
    const g = x.createLinearGradient(0, 0, 0, 128);
    g.addColorStop(0.00, '#ffffff');
    g.addColorStop(0.42, '#f4f2ec');
    g.addColorStop(0.70, '#e7e3da');
    g.addColorStop(1.00, '#cfcac0');
    x.fillStyle = g; x.fillRect(0, 0, 128, 128);
    const r = x.createRadialGradient(92, 24, 3, 92, 24, 52);
    r.addColorStop(0, '#ffffff'); r.addColorStop(1, '#ffffff00');
    x.fillStyle = r; x.fillRect(0, 0, 128, 128);
    const t = new THREE.CanvasTexture(c);
    if ('colorSpace' in t) t.colorSpace = THREE.SRGBColorSpace;
    t.mapping = THREE.EquirectangularReflectionMapping;
    return t;
  }
  const pmrem = new THREE.PMREMGenerator(renderer);
  pmrem.compileEquirectangularShader();
  const envRT = pmrem.fromEquirectangular(envTexture());
  scene.environment = envRT.texture;
  if ('environmentIntensity' in scene) scene.environmentIntensity = 0.28;
  pmrem.dispose();

  /* ---------- 背景 + 雾 ---------- */
  scene.background = new THREE.Color(room ? 0xe9e3d6 : PAPER);
  /* 房间里不需要"无限大"：雾只用来给窗外一点空气感，从 12 米才开始 */
  scene.fog = room ? new THREE.Fog(0xe9e3d6, 12, 26) : new THREE.Fog(PAPER, 10, 30);

  /* 房间里光由 room.js 自己打（窗外那盏才是主光），这里只留一点提亮，
     不再投影 —— 两盏投影灯会互相踩影，而且手机上多一张 shadow map 就是 5 fps。 */
  if (room) {
    /* 房间里有真实光源和窗：曝光要收到 0.8 附近，
       不然白墙会被 ACES 压成一片死白，木头的纹路全糊掉 */
    if (renderer) renderer.toneMappingExposure = 0.86;
    const soft = new THREE.DirectionalLight(0xfff6ea, 0.26);
    soft.position.set(2.6, 6.0, -1.6);
    soft.castShadow = false;
    scene.add(soft);
    scene.add(soft.target);
    if ('environmentIntensity' in scene) scene.environmentIntensity = 0.42;
    return { update() {} };
  }

  /* ---------- 主光：唯一投影的那盏 ---------- */
  const key = new THREE.DirectionalLight(0xfff7ec, 2.05);
  key.position.set(3.6, 7.4, -3.6);
  key.castShadow = true;
  key.shadow.mapSize.set(q.shadow, q.shadow);
  const sc = key.shadow.camera;
  sc.left = -3.4; sc.right = 3.4; sc.top = 3.4; sc.bottom = -3.4; sc.near = 1.0; sc.far = 20;
  key.shadow.bias = -0.0006;
  key.shadow.normalBias = 0.022;
  key.shadow.radius = 3.2;
  sc.updateProjectionMatrix();
  scene.add(key);
  scene.add(key.target);

  /* ---------- 辅光 ---------- */
  const fill = new THREE.DirectionalLight(0xdfe9ff, 0.30);
  fill.position.set(-5.2, 3.4, 4.0);
  scene.add(fill);

  /* ---------- 背光：勾一条边 ---------- */
  const rim = new THREE.DirectionalLight(0xffffff, 0.62);
  rim.position.set(-2.4, 4.6, -5.6);
  scene.add(rim);

  /* ---------- 天光 ---------- */
  const hemi = new THREE.HemisphereLight(0xffffff, 0xded8cc, 0.34);
  scene.add(hemi);

  /* ---------- 窗光：地上那块很淡的亮斑 ---------- */
  function glowTexture() {
    const c = document.createElement('canvas');
    c.width = c.height = 128;
    const x = c.getContext('2d');
    const g = x.createRadialGradient(64, 64, 4, 64, 64, 62);
    g.addColorStop(0, 'rgba(255,255,255,0.55)');
    g.addColorStop(0.5, 'rgba(255,255,255,0.18)');
    g.addColorStop(1, 'rgba(255,255,255,0)');
    x.fillStyle = g; x.fillRect(0, 0, 128, 128);
    const t = new THREE.CanvasTexture(c);
    if ('colorSpace' in t) t.colorSpace = THREE.SRGBColorSpace;
    return t;
  }
  const pool = new THREE.Mesh(
    new THREE.PlaneGeometry(9, 9),
    new THREE.MeshBasicMaterial({ map: glowTexture(), transparent: true, depthWrite: false,
      blending: THREE.AdditiveBlending, opacity: 0.5 }));
  pool.rotation.x = -Math.PI / 2;
  pool.position.set(2.0, 0.004, -0.6);
  scene.add(pool);

  /* ---------- 接触阴影 ---------- */
  function blobTexture() {
    const c = document.createElement('canvas');
    c.width = c.height = 64;
    const x = c.getContext('2d');
    const g = x.createRadialGradient(32, 32, 1, 32, 32, 31);
    g.addColorStop(0, 'rgba(0,0,0,0.42)');
    g.addColorStop(0.45, 'rgba(0,0,0,0.16)');
    g.addColorStop(1, 'rgba(0,0,0,0)');
    x.fillStyle = g; x.fillRect(0, 0, 64, 64);
    return new THREE.CanvasTexture(c);
  }
  const blobTex = blobTexture();
  function contactShadow(size = 0.9) {
    const m = new THREE.Mesh(new THREE.PlaneGeometry(size, size),
      new THREE.MeshBasicMaterial({ map: blobTex, transparent: true, depthWrite: false, opacity: 0.9 }));
    m.rotation.x = -Math.PI / 2;
    m.position.y = 0.005;
    m.renderOrder = 2;
    return m;
  }

  const focus = new THREE.Vector3(0, 1, 0);
  let t = 0, shadowTick = 0;
  /* 影子不用每帧重算：她在走，但影子慢半拍没人看得出来。
     30 Hz 刷新一次，手机上省掉一半的深度渲染。 */
  if (renderer) renderer.shadowMap.autoUpdate = false;

  return {
    key, fill, rim, hemi, pool, contactShadow, focus,
    update(dt, at) {
      t += dt;
      if (renderer && (++shadowTick & 1) === 0) renderer.shadowMap.needsUpdate = true;
      if (at) focus.lerp(at, Math.min(1, dt * 3));
      const fx = focus.x, fz = focus.z;
      key.position.set(fx + 3.6, 7.4, fz - 3.6);
      key.target.position.set(fx, 0, fz);
      key.target.updateMatrixWorld();
      fill.position.set(fx - 5.2, 3.4, fz + 4.0);
      rim.position.set(fx - 2.4, 4.6, fz - 5.6);
      pool.position.set(fx + 2.0 + Math.sin(t * 0.07) * 0.35, 0.004, fz - 0.6 + Math.cos(t * 0.05) * 0.3);
      pool.material.opacity = 0.44 + Math.sin(t * 0.31) * 0.03;
    },
  };
}
