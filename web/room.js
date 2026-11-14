/* ================= 房间 · 她的家 =================
   白空间那个无尽走廊是测试台，这里才是要发布的地方。

   尺寸：x ∈ [-4, 4]，z ∈ [-3.25, 3.25]，层高 2.9。
   -z 那面是窗（光是斜着进来的，下午四点），+x 是门。
   家具沿墙排，中间留一条环形空给她散步 —— 她自己会的（走、停、蹲下看东西）。

   所有材质程序化生成，没有外部贴图：打包体积、离线、加载速度都是理由。
   风格还是纸白 + 墨线的底子，但允许暖色和木头进来 —— 白空间没有"家"的感觉。
*/
import { GLTFLoader } from 'three/addons/GLTFLoader.js';

/* 真模型道具：GLB 到了就把盒子替掉，没到就留着盒子（不至于整间房空掉） */
export function loadProp(THREE, url, { x, z, rot = 0, width = 2.0, y = 0 }, onDone) {
  new GLTFLoader().load(url, (gltf) => {
    const root = gltf.scene;
    root.traverse((o) => {
      if (!o.isMesh) return;
      o.castShadow = true; o.receiveShadow = true;
      if (o.material) { o.material.envMapIntensity = 0.7; o.material.needsUpdate = true }
    });
    const bb = new THREE.Box3().setFromObject(root);
    const size = new THREE.Vector3(); bb.getSize(size);
    const s = width / Math.max(size.x, size.z, 1e-3);
    root.scale.setScalar(s);
    const bb2 = new THREE.Box3().setFromObject(root);
    const c = new THREE.Vector3(); bb2.getCenter(c);
    root.position.set(-c.x, -bb2.min.y + y, -c.z);      /* 脚底贴地、中心归零 */
    const pivot = new THREE.Group();
    pivot.add(root);
    pivot.position.set(x, 0, z);
    pivot.rotation.y = rot;
    onDone(pivot);
  }, undefined, (e) => { console.warn('[prop] 失败', url, e && e.message) });
}

export function createRoom(THREE, quality, MATS) {
  const q = quality || { decor: 1 };
  const g = new THREE.Group();
  const INK = 0x2a2723;

  const W = 4.0, D = 3.25, H = 2.90;      /* 半宽 / 半深 / 层高 */

  /* ---------------- 材质库 ---------------- */
  const rnd = (() => { let s = 1234567; return () => (s = (s * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff; })();

  function canvasTex(size, draw) {
    const c = document.createElement('canvas');
    c.width = c.height = size;
    draw(c.getContext('2d'), size);
    const t = new THREE.CanvasTexture(c);
    t.wrapS = t.wrapT = THREE.RepeatWrapping;
    t.anisotropy = 4;
    if ('colorSpace' in t) t.colorSpace = THREE.SRGBColorSpace;
    return t;
  }

  /* 木地板：一条一条的板子，缝是暗的，木纹是极淡的竖线 */
  const woodTex = canvasTex(512, (x, S) => {
    x.fillStyle = '#b08a5c';
    x.fillRect(0, 0, S, S);
    const plank = S / 8;
    for (let r = 0; r < 8; r++) {
      const y0 = r * plank;
      const tone = 0.86 + rnd() * 0.22;
      x.fillStyle = `rgb(${Math.round(180 * tone)},${Math.round(140 * tone)},${Math.round(96 * tone)})`;
      x.fillRect(0, y0, S, plank - 1);
      /* 木纹 */
      for (let i = 0; i < 26; i++) {
        x.strokeStyle = `rgba(120,86,50,${0.03 + rnd() * 0.05})`;
        x.lineWidth = 0.6 + rnd();
        x.beginPath();
        const yy = y0 + rnd() * plank;
        x.moveTo(0, yy);
        x.bezierCurveTo(S * 0.3, yy + (rnd() - 0.5) * 5, S * 0.6, yy + (rnd() - 0.5) * 5, S, yy + (rnd() - 0.5) * 3);
        x.stroke();
      }
      /* 板缝 */
      x.fillStyle = 'rgba(60,40,24,0.28)';
      x.fillRect(0, y0 + plank - 1.5, S, 1.5);
      /* 端头错缝 */
      const cut = ((r * 3) % 5) / 5 * S;
      x.fillRect(cut, y0, 1.5, plank);
    }
  });
  woodTex.repeat.set(3, 2.5);

  const wallTex = canvasTex(256, (x, S) => {
    x.fillStyle = '#efeae0';
    x.fillRect(0, 0, S, S);
    const img = x.getImageData(0, 0, S, S), d = img.data;
    for (let i = 0; i < d.length; i += 4) {
      const n = (Math.random() - 0.5) * 5;
      d[i] += n; d[i + 1] += n; d[i + 2] += n * 0.9;
    }
    x.putImageData(img, 0, 0);
  });
  wallTex.repeat.set(4, 2);

  /* 有贴图就用贴图（Poly Haven CC0），没有就退回原来的纯色 —— 两条路都留，
     免得哪天贴图没下下来整个房间变成白模。 */
  const mk = MATS ? MATS.mk : (opt) => new THREE.MeshStandardMaterial(opt);
  const matFloor = MATS ? mk({ map: 'floor', rep: [5, 4], rot: 0, color: 0xffffff, rough: 0.82, metal: 0.0 })
                        : new THREE.MeshStandardMaterial({ map: woodTex, roughness: 0.88, metalness: 0.0, color: 0xd2c3ac });
  const matWall = MATS ? mk({ map: 'wall', rep: [4.5, 1.6], color: 0xf4efe6, rough: 0.95 })
                       : new THREE.MeshStandardMaterial({ map: wallTex, roughness: 0.95, color: 0xd9d0be });
  const matWallDeep = MATS ? mk({ map: 'wall', rep: [4.5, 1.6], color: 0xdcd3c4, rough: 0.95 })
                           : new THREE.MeshStandardMaterial({ map: wallTex, roughness: 0.95, color: 0xc6bca7 });
  const matCeil = mk({ color: 0xe9e4da, rough: 1.0 });
  const matWood = MATS ? mk({ map: 'wood', rep: [2.5, 1.5], color: 0xffffff, rough: 0.5 })
                       : mk({ color: 0xb08a5e, roughness: 0.62 });
  const matWoodDark = MATS ? mk({ map: 'wood', rep: [2, 1.2], color: 0x9a7a52, rough: 0.55 })
                           : mk({ color: 0x8a6844, roughness: 0.60 });
  const matWhite = mk({ color: 0xf7f4ee, roughness: 0.78 });
  const matLinen = MATS ? mk({ map: 'fabric', rep: [2, 2], color: 0xf2eee5, rough: 0.94 })
                        : mk({ color: 0xf2eee5, roughness: 0.94 });
  const matBlue = mk({ color: 0x4a7ba8, roughness: 0.86 });      /* 那条被子：房间里唯一一块饱和色 */
  const matMetal = mk({ color: 0xc9c3b8, roughness: 0.35, metalness: 0.65 });
  const matInk = mk({ color: 0x332f2a, roughness: 0.55 });
  /* 后面还有一堆就地造材质的写法（窗帘、门板、书本…），保持原签名不变 */
  const M = (opt) => {
    if (!MATS) return new THREE.MeshStandardMaterial(opt);
    const o = { color: opt.color, rough: opt.roughness, metal: opt.metalness, side: opt.side,
                transparent: opt.transparent, opacity: opt.opacity };
    if (opt.emissive !== undefined) { o.emissive = opt.emissive; o.emissiveIntensity = opt.emissiveIntensity; }
    const m = MATS.mk(o);
    if (opt.side !== undefined) m.side = opt.side;
    return m;
  };

  const edgeMat = new THREE.LineBasicMaterial({ color: INK, transparent: true, opacity: 0.30 });

  const colliders = [];   /* {x0,x1,z0,z1,h} —— 物理层用，也是给人看的地板占位 */

  /* 每盏灯都记下"白天多亮/夜里多亮"，切换是一行循环，不用满地找引用 */
  const reg = [];
  const L = (light, day, night) => { reg.push({ light, day, night }); return light; };
  let lampBase = 12.0;

  function box(w, h, d, x, y, z, mat, opt = {}) {
    const geo = new THREE.BoxGeometry(w, h, d);
    const m = new THREE.Mesh(geo, mat);
    m.position.set(x, y, z);
    m.castShadow = opt.cast !== false;
    m.receiveShadow = true;
    if (opt.rot) m.rotation.y = opt.rot;
    if (opt.rotX) m.rotation.x = opt.rotX;
    g.add(m);
    if (opt.edge !== false && opt.edge !== undefined ? opt.edge : false) {
      const e = new THREE.LineSegments(new THREE.EdgesGeometry(geo), edgeMat);
      e.position.copy(m.position); e.rotation.copy(m.rotation);
      g.add(e);
    }
    return m;
  }
  function collider(x0, x1, z0, z1, h) { colliders.push({ x0, x1, z0, z1, h }); }
  /* 真模型顶上来的开关：?props=0 可以退回全程序化的老房间来对比 */
  function wantProps() {
    try { return new URLSearchParams(location.search).get('props') !== '0'; } catch (e) { return true; }
  }
  /* 真模型顶上来以后，把这一段里程序化的盒子藏掉（灯不藏）。
     段落用"进入时记 g.children.length"来界定，不靠包围盒猜 —— 猜错过一次（误伤茶几）。 */
  function hideSpan(mark) {
    if (!wantProps()) return;
    for (let i = mark; i < g.children.length; i++) {
      const o = g.children[i];
      if (o.isLight || o.isLineSegments) continue;
      o.userData.oldProp = true;
      o.visible = false;
    }
  }

  /* ---------------- 地板 / 天花板 ---------------- */
  const floor = new THREE.Mesh(new THREE.PlaneGeometry(W * 2, D * 2), matFloor);
  floor.rotation.x = -Math.PI / 2;
  floor.receiveShadow = true;
  g.add(floor);

  const ceil = new THREE.Mesh(new THREE.PlaneGeometry(W * 2, D * 2), matCeil);
  ceil.rotation.x = Math.PI / 2;
  ceil.position.y = H;
  ceil.receiveShadow = true;
  g.add(ceil);

  /* ---------------- 四面墙 ---------------- */
  const wallH = H;
  function wall(w, x, z, rotY, mat) {
    const m = new THREE.Mesh(new THREE.PlaneGeometry(w, wallH), mat);
    m.position.set(x, wallH / 2, z);
    m.rotation.y = rotY;
    m.receiveShadow = true;
    g.add(m);
    return m;
  }
  /* 窗墙不能是一整块 —— 整块墙不投影，太阳就会把整个地板照亮，
     房间立刻变平。拆成四块围出窗洞，光只能从洞里进来，
     地板上才会出现那道斜着的窗格，这是一间屋子最像屋子的地方。 */
  {
    const WINX0 = -1.10, WINX1 = 2.30, WINY0 = 0.78, WINY1 = 2.46, tz = 0.10;
    const piece = (x0, x1, y0, y1) =>
      box(x1 - x0, y1 - y0, tz, (x0 + x1) / 2, (y0 + y1) / 2, -D + tz / 2, matWall, { cast: true, edge: false });
    piece(-W, WINX0, 0, wallH);
    piece(WINX1, W, 0, wallH);
    piece(WINX0, WINX1, 0, WINY0);
    piece(WINX0, WINX1, WINY1, wallH);
  }
  /* 前墙（+z）中间开一道门洞（x∈[-0.6,0.6]）—— 通往走廊和三间卧室。
     原来是整面一块，现在拆两段；门楣不建墙，让天花板自己接住。 */
  wall(W - 0.6, -(W + 0.6) / 2, D, Math.PI, matWall);
  wall(W - 0.6, (W + 0.6) / 2, D, Math.PI, matWall);
  wall(D * 2, -W, 0, Math.PI / 2, matWallDeep);/* 左墙（-x） */
  const DOOR = { z: 0.9, w: 0.92, h: 2.06 };
  const DOORH = 2.06;
  /* 右墙（+x）中间开一道门 —— 门外是那截小走廊。
     门洞 z∈[0.44, 1.36]（门宽 0.92，中心 0.90），所以拆两段 + 上面留一条门楣墙。 */
  wall(D - 1.36, W, -1.855, -Math.PI / 2, matWallDeep);
  wall(D - 0.44, W, 2.305, -Math.PI / 2, matWallDeep);
  box(0.06, wallH - DOORH, 1.84, W, DOORH + (wallH - DOORH) / 2, 0.9, matWallDeep, { cast: false });

  /* 墙根那道暗：真实房间里，ao 全都堆在墙和地的缝上。
     没有它，墙就是一张纸——光再对也像糊上去的。一条渐变就够，不烧帧。
     （真 AO 要 GTAO/postprocess，手机上跑 21 fps 的底子扛不住。） */
  {
    const aoTex = canvasTex(64, (x, S) => {
      const gr = x.createLinearGradient(0, S, 0, 0);
      gr.addColorStop(0, 'rgba(46,33,20,0.50)');
      gr.addColorStop(0.45, 'rgba(46,33,20,0.16)');
      gr.addColorStop(1, 'rgba(46,33,20,0)');
      x.fillStyle = gr; x.fillRect(0, 0, S, S);
    });
    aoTex.wrapS = aoTex.wrapT = THREE.ClampToEdgeWrapping;
    const aoMat = new THREE.MeshBasicMaterial({
      map: aoTex, transparent: true, depthWrite: false, opacity: 0.9,
    });
    const strip = (w, x, z, rotY) => {
      const m = new THREE.Mesh(new THREE.PlaneGeometry(w, 0.62), aoMat);
      m.position.set(x, 0.31, z);
      m.rotation.y = rotY;
      m.renderOrder = 2;
      g.add(m);
      return m;
    };
    strip(W * 2, 0, D - 0.012, Math.PI);
    strip(W * 2, 0, -D + 0.012, 0);
    strip(D * 2, -W + 0.012, 0, Math.PI / 2);
    strip(D * 2, W - 0.012, 0, -Math.PI / 2);
  }


  /* 踢脚线：一圈 8cm 的白条，房间一下就有"装修过"的感觉 */
  const skirtH = 0.085, skirtT = 0.018;
  [[W * 2, 0, -D + skirtT / 2, 0], [W * 2, 0, D - skirtT / 2, 0],
   [D * 2, -W + skirtT / 2, 0, Math.PI / 2], [D * 2, W - skirtT / 2, 0, Math.PI / 2]]
    .forEach(([len, x, z, rot]) => {
      const m = new THREE.Mesh(new THREE.BoxGeometry(len, skirtH, skirtT), matWhite);
      m.position.set(x, skirtH / 2, z);
      m.rotation.y = rot;
      m.castShadow = false; m.receiveShadow = true;
      g.add(m);
    });

  /* 墙体的碰撞：四面内壁内缩一点（她/你都不能穿墙） */
  collider(-W - 1, -W + 0.06, -D - 1, D + 1, H);
  /* 右墙的碰撞盒也拆两段：原来是一条从 W-0.06 一直到 W+1 的整条，
     把门外那截走廊全堵死了。 */
  collider(W - 0.06, W + 0.06, -D - 1, 0.44, H);
  collider(W - 0.06, W + 0.06, 1.36, D + 1, H);
  collider(-W - 1, W + 1, -D - 1, -D + 0.06, H);
  /* 前墙的碰撞只占墙厚（±0.06），别再往外吃到走廊里去 ——
     原来那两块是 D-0.06..D+1，等于在走廊两侧各压了一堵看不见的墙。 */
  collider(-W, -0.6, D - 0.06, D + 0.06, H);
  collider(0.6, W, D - 0.06, D + 0.06, H);

  /* ---------------- 窗：一整面，光从这儿斜着进来 ---------------- */
  const WIN = { x0: -1.10, x1: 2.30, y0: 0.78, y1: 2.46, z: -D + 0.02 };
  {
    /* 窗洞：先把墙挖不出来的地方补上 —— 直接在墙上贴一块窗，四周用框压住 */
    const ww = WIN.x1 - WIN.x0, wh = WIN.y1 - WIN.y0;
    const cx = (WIN.x0 + WIN.x1) / 2, cy = (WIN.y0 + WIN.y1) / 2;

    /* 外面的天：一块比窗大的板，压在墙后面 */
    const skyTex = canvasTex(256, (x, S) => {
      const grad = x.createLinearGradient(0, 0, 0, S);
      grad.addColorStop(0, '#cfe3f2');
      grad.addColorStop(0.55, '#e8eef1');
      grad.addColorStop(1, '#f2ead9');
      x.fillStyle = grad; x.fillRect(0, 0, S, S);
      /* 远处几棵树影，虚的 */
      x.fillStyle = 'rgba(120,140,120,0.20)';
      for (let i = 0; i < 9; i++) {
        const bx = rnd() * S, by = S * 0.62 + rnd() * S * 0.2, r = 16 + rnd() * 34;
        x.beginPath(); x.arc(bx, by, r, 0, Math.PI * 2); x.fill();
      }
      x.fillStyle = 'rgba(150,165,150,0.18)';
      for (let i = 0; i < 7; i++) {
        const bx = rnd() * S, by = S * 0.72 + rnd() * S * 0.18, r = 10 + rnd() * 22;
        x.beginPath(); x.arc(bx, by, r, 0, Math.PI * 2); x.fill();
      }
    });
    const sky = new THREE.Mesh(new THREE.PlaneGeometry(ww * 1.9, wh * 1.9),
      new THREE.MeshBasicMaterial({ map: skyTex }));
    sky.position.set(cx, cy + 0.2, -D - 0.55);
    g.add(sky);

    /* ---- 斜射进来的光柱 ----
       下午四点太阳角度低，光会以一条斜带铺到地板上。真正的体积光要后处理，
       这里用三片带渐变的加法平面 —— 便宜，但正对着镜头看过去那一下就"亮起来了"。 */
    {
      const cv = document.createElement('canvas');
      cv.width = 64; cv.height = 256;
      const c2 = cv.getContext('2d');
      const gr = c2.createLinearGradient(0, 0, 0, 256);
      gr.addColorStop(0, 'rgba(255,236,200,0.00)');
      gr.addColorStop(0.22, 'rgba(255,238,206,0.42)');
      gr.addColorStop(0.72, 'rgba(255,232,196,0.16)');
      gr.addColorStop(1, 'rgba(255,230,190,0.00)');
      c2.fillStyle = gr; c2.fillRect(0, 0, 64, 256);
      const gr2 = c2.createLinearGradient(0, 0, 64, 0);
      gr2.addColorStop(0, 'rgba(0,0,0,1)');
      gr2.addColorStop(0.20, 'rgba(0,0,0,0)');
      gr2.addColorStop(0.80, 'rgba(0,0,0,0)');
      gr2.addColorStop(1, 'rgba(0,0,0,1)');
      c2.globalCompositeOperation = 'destination-out';
      c2.fillStyle = gr2; c2.fillRect(0, 0, 64, 256);
      const beamTex = new THREE.CanvasTexture(cv);
      beamTex.colorSpace = THREE.SRGBColorSpace;
      const beamMat = new THREE.MeshBasicMaterial({
        map: beamTex, transparent: true, opacity: 0.10,
        blending: THREE.AdditiveBlending, depthWrite: false,
        side: THREE.DoubleSide, toneMapped: false,
      });
      for (let i = 0; i < 2; i++) {
        const bm = new THREE.Mesh(new THREE.PlaneGeometry(2.6, 4.2), beamMat);
        bm.position.set(cx + 0.10 + i * 0.16, 1.86 - i * 0.24, -1.55 + i * 0.52);
        bm.rotation.set(-0.98, 0, 0);
        bm.renderOrder = 3;
        g.add(bm);
      }
      /* 夜里窗外是黑的，屋里那盏灯射不出这种光柱 */
      reg.push({ light: { set intensity(v) { beamMat.opacity = v; }, get intensity() { return beamMat.opacity; } }, day: 0.10, night: 0.0 });
    }

    /* 窗帘：两片半透的布挂在窗两边。
       有了它，窗洞才有"框"，光才有地方被挡一下 —— 光秃秃一个洞像工地。 */
    {
      const cm = M({ color: 0xded4c4, roughness: 1.0, side: THREE.DoubleSide });
      const mkDrape = (x0) => {
        const wD = 0.52, hD = wh + 0.30;
        const geo = new THREE.PlaneGeometry(wD, hD, 10, 1);
        const pos = geo.attributes.position;
        for (let i = 0; i < pos.count; i++) {
          const u = (pos.getX(i) / wD + 0.5);
          /* 竖向的褶子：一串正弦，往下摆得开一点 */
          const fold = Math.sin(u * Math.PI * 5.5) * 0.035;
          const taper = 0.6 + 0.4 * (1 - (pos.getY(i) / hD + 0.5));
          pos.setZ(i, fold * taper);
        }
        geo.computeVertexNormals();
        const m = new THREE.Mesh(geo, cm);
        m.position.set(x0, WIN.y0 + hD / 2 - 0.12, -D + 0.16);
        m.castShadow = true; m.receiveShadow = true;
        g.add(m);
      };
      mkDrape(WIN.x0 - 0.16);
      mkDrape(WIN.x1 + 0.16);
    }

    /* 玻璃：几乎透明的一层，带一点点蓝 */
    const glass = new THREE.Mesh(new THREE.PlaneGeometry(ww, wh),
      new THREE.MeshBasicMaterial({ color: 0xeaf3ff, transparent: true, opacity: 0.10 }));
    glass.position.set(cx, cy, -D + 0.03);
    g.add(glass);

    /* 窗框：四条 + 中间一根竖的分格 */
    const fr = 0.055, fd = 0.09;
    const frameMat = matWhite;
    box(ww + fr * 2, fr, fd, cx, WIN.y1 + fr / 2, -D + 0.04, frameMat, { cast: true });
    box(ww + fr * 2, fr, fd, cx, WIN.y0 - fr / 2, -D + 0.04, frameMat, { cast: true });
    box(fr, wh + fr * 2, fd, WIN.x0 - fr / 2, cy, -D + 0.04, frameMat, { cast: true });
    box(fr, wh + fr * 2, fd, WIN.x1 + fr / 2, cy, -D + 0.04, frameMat, { cast: true });
    /* 窗格：三竖一横。不是为了好看 —— 是为了让阳光在地板上落下来的时候
       有一格一格的影子，房间才像有窗，不像有个洞。 */
    const gx = (WIN.x1 - WIN.x0) / 3;
    for (let i = 1; i < 3; i++) {
      box(0.030, wh, fd * 0.62, WIN.x0 + gx * i, cy, -D + 0.04, frameMat, { cast: true });
    }
    box(ww, 0.030, fd * 0.62, cx, WIN.y0 + wh * 0.52, -D + 0.04, frameMat, { cast: true });

    /* 窗台：往房间里伸出来一块 */
    box(ww + 0.34, 0.05, 0.24, cx, WIN.y0 - 0.05, -D + 0.13, matWhite, { cast: true });

    /* 窗帘：两块垂下来的布，靠两边 */
    const curMat = M({ color: 0xece4d6, roughness: 0.96, side: THREE.DoubleSide });
    [[WIN.x0 - 0.16, -1], [WIN.x1 + 0.16, 1]].forEach(([x, s]) => {
      const cw = 0.34;
      const cur = new THREE.Mesh(new THREE.PlaneGeometry(cw, 2.28, 6, 1), curMat);
      const pos = cur.geometry.attributes.position;
      for (let i = 0; i < pos.count; i++) {
        const u = (pos.getX(i) / cw + 0.5);
        pos.setZ(i, Math.sin(u * Math.PI * 3.2) * 0.045);
      }
      cur.geometry.computeVertexNormals();
      cur.position.set(x + s * 0.05, 1.90, -D + 0.16);
      cur.castShadow = true; cur.receiveShadow = true;
      g.add(cur);
    });
  }

  /* ---------------- 门：在 +x 墙上 ---------------- */
  {
    const x = W - 0.03;
    /* 门套 */
    box(0.10, DOOR.h + 0.10, DOOR.w + 0.16, x, (DOOR.h + 0.10) / 2, DOOR.z, matWhite, { cast: true });
    /* 门板 */
    /* 门板开着：这扇门现在是"通到外面"的门，关着的话走廊就只是个摆设。
       挂在门框内侧、转 105°，看着像刚被人推开。 */
    const leaf = box(0.045, DOOR.h, DOOR.w, x - 0.08, DOOR.h / 2, DOOR.z, matWood, { cast: true });
    /* 门是开着的：门板贴着走廊那侧的墙（等于"刚被推开、还没关"） */
    leaf.position.set(W + 0.55, DOOR.h / 2, DOOR.z + DOOR.w / 2 + 0.06);
    leaf.rotation.y = Math.PI / 2;
    leaf.material = M({ color: 0xe8dfcd, roughness: 0.66 });
    /* 门把手 */
    const knob = new THREE.Mesh(new THREE.SphereGeometry(0.032, 14, 12), matMetal);
    knob.position.set(x - 0.10, 1.02, DOOR.z + DOOR.w / 2 - 0.10);
    knob.castShadow = true;
    g.add(knob);
    /* 门板凹格（两小块，视觉上的"木门"） */
    box(0.012, 0.62, DOOR.w * 0.62, x - 0.084, 1.46, DOOR.z, M({ color: 0xf0e8d8, roughness: 0.7 }), { cast: false });
    box(0.012, 0.52, DOOR.w * 0.62, x - 0.084, 0.62, DOOR.z, M({ color: 0xf0e8d8, roughness: 0.7 }), { cast: false });
  }

  /* ---------------- 床：左后角 ---------------- */
  {
    const bx0 = -W + 0.10, bx1 = -1.92, bz0 = -D + 0.12, bz1 = -0.86;
    const _mb = g.children.length;
    const bw = bx1 - bx0, bd = bz1 - bz0, cx = (bx0 + bx1) / 2, cz = (bz0 + bz1) / 2;
    /* 床架 */
    box(bw, 0.30, bd, cx, 0.20, cz, matWoodDark, { cast: true });
    /* 床垫 */
    box(bw - 0.05, 0.22, bd - 0.05, cx, 0.46, cz, matLinen, { cast: true });
    /* 被子：蓝色，盖住中后段 */
    box(bw - 0.02, 0.10, bd * 0.62, cx, 0.585, cz + bd * 0.17, matBlue, { cast: true });
    /* 被子翻边 */
    box(bw - 0.02, 0.035, 0.10, cx, 0.635, cz + bd * 0.17 - bd * 0.31, M({ color: 0x5f8fb8, roughness: 0.9 }), { cast: true });
    /* 枕头两个 */
    for (const s of [-1, 1]) {
      const p = box(bw * 0.44, 0.13, 0.30, cx + s * bw * 0.24, 0.635, bz0 + 0.22, matWhite, { cast: true });
      p.rotation.z = s * 0.02;
    }
    /* 床头板：贴墙 */
    box(0.07, 0.72, bd + 0.06, bx0 + 0.035, 0.62, cz, matWoodDark, { cast: true });
    /* 床头柜 + 小灯 */
    box(0.42, 0.44, 0.42, bx1 - 0.26, 0.22, bz0 + 0.00, matWood, { cast: true });
    const lampShade = new THREE.Mesh(new THREE.CylinderGeometry(0.09, 0.11, 0.14, 16, 1, true),
      M({ color: 0xfbf3e2, roughness: 0.8, side: THREE.DoubleSide, emissive: 0xffe9c4, emissiveIntensity: 0.35 }));
    lampShade.position.set(bx1 - 0.26, 0.55, bz0 + 0.00);
    lampShade.castShadow = true;
    g.add(lampShade);
    const bedLamp = L(new THREE.PointLight(0xffd9a0, 3.4, 4.2, 2), 3.4, 5.2);
    bedLamp.position.set(bx1 - 0.26, 0.52, bz0 + 0.00);
    g.add(bedLamp);
    hideSpan(_mb);
    collider(bx0, bx1, bz0, bz1, 0.60);
  }

  /* ---------------- 书桌：窗下 ---------------- */
  {
    const dx0 = 0.72, dx1 = 2.36, dz0 = -D + 0.06, dz1 = -D + 0.72;
    const _md = g.children.length;
    const cx = (dx0 + dx1) / 2, cz = (dz0 + dz1) / 2, dw = dx1 - dx0, dd = dz1 - dz0;
    /* 桌面 */
    box(dw, 0.045, dd, cx, 0.74, cz, matWood, { cast: true });
    /* 抽屉柜 */
    box(dw * 0.42, 0.62, dd - 0.08, cx + dw * 0.26, 0.40, cz, M({ color: 0xefe9dd, roughness: 0.72 }), { cast: true });
    /* 抽屉面板 */
    for (let i = 0; i < 2; i++) {
      box(dw * 0.36, 0.20, 0.02, cx + dw * 0.26, 0.24 + i * 0.25, cz + dd / 2 - 0.05, M({ color: 0xf6f1e6, roughness: 0.7 }), { cast: false });
      box(0.10, 0.012, 0.02, cx + dw * 0.26, 0.24 + i * 0.25, cz + dd / 2 - 0.04, matMetal, { cast: false });
    }
    /* 桌腿 */
    for (const s of [-1, 1]) box(0.05, 0.72, 0.05, cx - s * dw * 0.45, 0.36, cz + dd * 0.36, matWoodDark, { cast: true });
    /* 笔记本电脑 */
    box(0.30, 0.012, 0.21, cx - 0.18, 0.765, cz + 0.02, matMetal, { cast: true });
    const lid = box(0.30, 0.19, 0.008, cx - 0.18, 0.86, cz - 0.10, matMetal, { cast: true });
    lid.rotation.x = -0.22;
    const scr = new THREE.Mesh(new THREE.PlaneGeometry(0.27, 0.165),
      new THREE.MeshBasicMaterial({ color: 0xd8ecff }));
    scr.position.set(cx - 0.18, 0.862, cz - 0.096);
    scr.rotation.x = -0.22;
    g.add(scr);
    /* 屏幕的光：一点点冷色，夜景里是房间里唯一的冷光源 */
    const glow = L(new THREE.PointLight(0xbcd8f5, 1.1, 2.0, 2), 1.1, 1.5);
    glow.visible = false;   /* 屏幕光太弱，肉眼看不出，白搭一次逐像素光照 */
    glow.position.set(cx - 0.18, 0.90, cz + 0.06);
    g.add(glow);
    /* 桌上几本书、一个杯子 */
    box(0.16, 0.03, 0.22, cx + 0.30, 0.775, cz + 0.05, M({ color: 0xc96f5a, roughness: 0.9 }), { cast: true });
    box(0.15, 0.025, 0.21, cx + 0.30, 0.803, cz + 0.07, M({ color: 0x7f9b74, roughness: 0.9 }), { cast: true });
    const cup = new THREE.Mesh(new THREE.CylinderGeometry(0.032, 0.028, 0.085, 16, 1, true),
      M({ color: 0xf8f5ee, roughness: 0.5, side: THREE.DoubleSide }));
    cup.position.set(cx - 0.52, 0.80, cz + 0.12);
    cup.castShadow = true;
    g.add(cup);
    hideSpan(_md);
    collider(dx0, dx1, dz0, dz1, 0.76);
  }

  /* ---------------- 椅子 ---------------- */
  {
    const _mc = g.children.length;
    const cx = 1.62, cz = -2.06;
    box(0.42, 0.04, 0.42, cx, 0.44, cz, matWood, { cast: true });
    box(0.40, 0.42, 0.035, cx, 0.68, cz - 0.19, matWood, { cast: true });
    for (const sx of [-1, 1]) for (const sz of [-1, 1]) {
      box(0.032, 0.44, 0.032, cx + sx * 0.17, 0.22, cz + sz * 0.17, matWoodDark, { cast: true });
    }
    hideSpan(_mc);
    collider(cx - 0.24, cx + 0.24, cz - 0.24, cz + 0.24, 0.90);
  }

  /* ---------------- 书架：右墙 ---------------- */
  {
    const _ms = g.children.length;
    const sx = W - 0.16, sz0 = -1.62, sz1 = -0.10, sy = 0.98;
    const sd = sz1 - sz0;
    /* 背板 + 两侧 + 四层隔板 */
    box(0.02, 1.96, sd, sx + 0.14, sy, (sz0 + sz1) / 2, matWoodDark, { cast: false });
    for (const s of [-1, 1]) box(0.10, 1.96, 0.03, sx, sy, (sz0 + sz1) / 2 + s * sd / 2, matWoodDark, { cast: true });
    const shelfY = [0.28, 0.70, 1.12, 1.54, 1.93];
    shelfY.forEach(y => box(0.11, 0.028, sd, sx, y, (sz0 + sz1) / 2, matWood, { cast: true }));
    /* 书：一排一排的小方块，颜色从桌上那几本来 */
    const cols = [0x8f5c4a, 0x6b7f8c, 0xc2a06a, 0x7d6b8f, 0xa8554f, 0x5f7a63, 0xb98d5e, 0x556575];
    for (let sh = 0; sh < 4; sh++) {
      let zz = sz0 + 0.05;
      const y = shelfY[sh] + 0.014;
      while (zz < sz1 - 0.08) {
        const w = 0.025 + rnd() * 0.03, h = 0.16 + rnd() * 0.09;
        const lean = rnd() < 0.08;
        box(0.14, h, w, sx - 0.02, y + h / 2, zz + w / 2,
          M({ color: cols[(Math.random() * cols.length) | 0], roughness: 0.9 }),
          { cast: true, rot: lean ? (rnd() - 0.5) * 0.16 : 0 });
        zz += w + 0.006 + (rnd() < 0.1 ? 0.04 : 0);
      }
    }
    hideSpan(_ms);
    collider(sx - 0.16, W, sz0 - 0.02, sz1 + 0.02, 1.96);
  }

  /* ---------------- 沙发 + 茶几：前墙 ---------------- */
  {
    const _mso = g.children.length;
    const sx0 = -1.30, sx1 = 1.24, sz0 = 1.92, sz1 = D - 0.12;
    /* 沙发原来正堵在 +z 墙中间 —— 那面墙现在是通往卧室的门。
       往东挪 2.25 m，门洞（x∈[-0.6,0.6]）才真的有路可走。 */
    const cx = (sx0 + sx1) / 2 + 2.25, cz = (sz0 + sz1) / 2;
    const sw = sx1 - sx0, sd = sz1 - sz0;
    const sofaParts = [];        /* 真模型来了就把这些藏掉 */
    /* 底座 */
    sofaParts.push(box(sw, 0.26, sd, cx, 0.20, cz, M({ color: 0xb9a98f, roughness: 0.92 }), { cast: true }));
    /* 坐垫：三个 */
    for (let i = -1; i <= 1; i++) {
      sofaParts.push(box(sw / 3 - 0.03, 0.16, sd - 0.14, cx + i * sw / 3, 0.41, cz + 0.05,
        M({ color: 0xcbbca2, roughness: 0.95 }), { cast: true }));
    }
    /* 靠背 */
    sofaParts.push(box(sw, 0.52, 0.16, cx, 0.55, sz1 - 0.10, M({ color: 0xcbbca2, roughness: 0.95 }), { cast: true }));
    /* 扶手 */
    for (const s of [-1, 1]) sofaParts.push(box(0.14, 0.30, sd, cx + s * (sw / 2 - 0.07), 0.48, cz, M({ color: 0xb9a98f, roughness: 0.92 }), { cast: true }));
    /* 抱枕 */
    sofaParts.push(box(0.26, 0.26, 0.10, cx - 0.42, 0.60, sz1 - 0.24, M({ color: 0x4a7ba8, roughness: 0.95 }), { cast: true, rot: 0.3 }));
    sofaParts.push(box(0.24, 0.24, 0.10, cx + 0.30, 0.60, sz1 - 0.24, M({ color: 0xd8cbb0, roughness: 0.95 }), { cast: true, rot: -0.2 }));
    collider(cx - sw / 2, cx + sw / 2, sz0, sz1, 0.84);

    /* 茶几 */
    const tc = new THREE.Mesh(new THREE.CylinderGeometry(0.46, 0.46, 0.035, 32), matWood);
    tc.position.set(cx, 0.40, 1.05);
    tc.castShadow = true; tc.receiveShadow = true;
    g.add(tc);
    for (let i = 0; i < 3; i++) {
      const a = i / 3 * Math.PI * 2;
      box(0.028, 0.40, 0.028, cx + Math.cos(a) * 0.30, 0.20, 1.05 + Math.sin(a) * 0.30, matWoodDark, { cast: true });
    }
    /* 茶几上的东西：一本书 + 一个杯子 */
    box(0.19, 0.028, 0.26, cx - 0.06, 0.432, 1.02, M({ color: 0xc96f5a, roughness: 0.9 }), { cast: true, rot: 0.35 });
    const cup2 = new THREE.Mesh(new THREE.CylinderGeometry(0.030, 0.026, 0.075, 14, 1, true),
      M({ color: 0xe8eff2, roughness: 0.4, side: THREE.DoubleSide }));
    cup2.position.set(cx + 0.22, 0.455, 1.14);
    cup2.castShadow = true;
    g.add(cup2);
    collider(cx - 0.5, cx + 0.5, 0.60, 1.50, 0.42);
    hideSpan(_mso);      /* 沙发/茶几的盒子全藏掉，真模型在 props.js 里摆 */
    const rugTex = canvasTex(256, (x, S) => {
      x.fillStyle = '#cfc4ac';
      x.fillRect(0, 0, S, S);
      x.strokeStyle = 'rgba(150,132,108,0.35)';
      x.lineWidth = 3;
      x.strokeRect(S * 0.06, S * 0.06, S * 0.88, S * 0.88);
      x.strokeStyle = 'rgba(150,132,108,0.22)';
      x.lineWidth = 1.5;
      for (let r = 0; r < 4; r++) {
        const i = S * (0.14 + r * 0.06);
        x.strokeRect(i, i, S - i * 2, S - i * 2);
      }
      /* 细小的菱形花纹 */
      x.strokeStyle = 'rgba(120,104,84,0.13)';
      x.lineWidth = 1;
      const step = S / 12;
      for (let i = 0; i < 12; i++) for (let j = 0; j < 12; j++) {
        const cxp = i * step + step / 2, cyp = j * step + step / 2;
        x.beginPath();
        x.moveTo(cxp, cyp - 4); x.lineTo(cxp + 4, cyp); x.lineTo(cxp, cyp + 4); x.lineTo(cxp - 4, cyp);
        x.closePath(); x.stroke();
      }
    });
    rugTex.repeat.set(2, 2);
    const rug = new THREE.Mesh(new THREE.PlaneGeometry(3.4, 2.9),
      M({ map: rugTex, roughness: 0.99, color: 0xbfb5a0 }));
    rug.rotation.x = -Math.PI / 2;
    rug.position.set(-0.05, 0.008, 1.12);
    rug.receiveShadow = true;
    g.add(rug);
  }

  /* ---------------- 落地灯：左前角 ---------------- */
  {
    const lx = -W + 0.42, lz = 1.55;
    const pole = new THREE.Mesh(new THREE.CylinderGeometry(0.016, 0.016, 1.58, 12), matMetal);
    pole.position.set(lx, 0.79, lz);
    pole.castShadow = true;
    g.add(pole);
    const base = new THREE.Mesh(new THREE.CylinderGeometry(0.15, 0.17, 0.03, 20), matMetal);
    base.position.set(lx, 0.015, lz);
    base.castShadow = true; base.receiveShadow = true;
    g.add(base);
    const shade = new THREE.Mesh(new THREE.CylinderGeometry(0.17, 0.24, 0.30, 20, 1, true),
      M({ color: 0xfdf6e6, roughness: 0.85, side: THREE.DoubleSide, emissive: 0xffdca8, emissiveIntensity: 0.55 }));
    shade.position.set(lx, 1.66, lz);
    shade.castShadow = true;
    g.add(shade);
    const bulb = new THREE.Mesh(new THREE.SphereGeometry(0.045, 12, 10),
      new THREE.MeshBasicMaterial({ color: 0xfff0d0 }));
    bulb.position.set(lx, 1.60, lz);
    g.add(bulb);
    const light = L(new THREE.PointLight(0xffd39a, 9.0, 7.5, 2), 9.0, 13.0);
    light.position.set(lx, 1.56, lz);
    light.castShadow = false;
    g.add(light);
    collider(lx - 0.2, lx + 0.2, lz - 0.2, lz + 0.2, 1.8);
  }

  /* ---------------- 吊灯：房间中央偏上 ---------------- */
  {
    const cx = 0.0, cz = 0.1, y = 2.28;
    const cord = new THREE.Mesh(new THREE.CylinderGeometry(0.005, 0.005, H - y, 6),
      M({ color: 0x6a625a, roughness: 0.9 }));
    cord.position.set(cx, (H + y) / 2 + 0.1, cz);
    g.add(cord);
    const shade = new THREE.Mesh(new THREE.ConeGeometry(0.34, 0.24, 28, 1, true),
      M({ color: 0xfdf8ec, roughness: 0.7, side: THREE.DoubleSide, emissive: 0xffeccb, emissiveIntensity: 0.28 }));
    shade.position.set(cx, y, cz);
    shade.castShadow = true;
    g.add(shade);
    const bulb = new THREE.Mesh(new THREE.SphereGeometry(0.05, 12, 10),
      new THREE.MeshBasicMaterial({ color: 0xfff6e0 }));
    bulb.position.set(cx, y - 0.10, cz);
    g.add(bulb);
    const lamp = L(new THREE.PointLight(0xffe6c0, 12.0, 9, 2), 12.0, 17.0);
    lamp.position.set(cx, y - 0.14, cz);
    /* 点光源投影是 6 面 cubemap —— 等于把整个房间重画六遍。手机上这一项最贵，
       而房间有窗光的方向阴影已经够了，吊灯只负责把角落提亮。 */
    lamp.castShadow = false;
    lamp.shadow.bias = -0.004;
    g.add(lamp);
    g.userData.lamp = { lamp, bulb, shade, y, cx, cz };
  }

  /* ---------------- 绿植：右前角 ---------------- */
  {
    const px = W - 0.52, pz = D - 0.58;
    const pot = new THREE.Mesh(new THREE.CylinderGeometry(0.15, 0.12, 0.26, 18),
      M({ color: 0xc8b49a, roughness: 0.92 }));
    pot.position.set(px, 0.13, pz);
    pot.castShadow = true; pot.receiveShadow = true;
    g.add(pot);
    /* 土 */
    const soil = new THREE.Mesh(new THREE.CylinderGeometry(0.135, 0.135, 0.02, 18),
      M({ color: 0x4b3b2c, roughness: 1.0 }));
    soil.position.set(px, 0.255, pz);
    g.add(soil);
    /* 茎 */
    const stem = new THREE.Mesh(new THREE.CylinderGeometry(0.012, 0.018, 0.55, 8),
      M({ color: 0x4e6b3f, roughness: 0.9 }));
    stem.position.set(px, 0.53, pz);
    stem.castShadow = true;
    g.add(stem);
    /* 叶子：几片扁椭球，向外撇 */
    const leafMat = M({ color: 0x4f7a4a, roughness: 0.85, side: THREE.DoubleSide });
    for (let i = 0; i < 9; i++) {
      const a = i / 9 * Math.PI * 2 + rnd() * 0.5;
      const rr = 0.16 + rnd() * 0.13;
      const yy = 0.62 + rnd() * 0.42;
      const leaf = new THREE.Mesh(new THREE.SphereGeometry(0.13, 10, 8), leafMat);
      leaf.scale.set(1.0, 0.16, 0.52);
      leaf.position.set(px + Math.cos(a) * rr, yy, pz + Math.sin(a) * rr * 0.8);
      leaf.rotation.set((rnd() - 0.5) * 0.9, a, (rnd() - 0.5) * 0.9);
      leaf.castShadow = true;
      g.add(leaf);
    }
    collider(px - 0.2, px + 0.2, pz - 0.2, pz + 0.2, 1.1);
  }

  /* ---------------- 墙上的画：两幅 ---------------- */
  {
    const artA = canvasTex(128, (x, S) => {
      x.fillStyle = '#f4efe4'; x.fillRect(0, 0, S, S);
      x.fillStyle = 'rgba(74,123,168,0.55)';
      x.beginPath(); x.arc(S * 0.5, S * 0.62, S * 0.26, 0, Math.PI * 2); x.fill();
      x.fillStyle = 'rgba(201,111,90,0.5)';
      x.beginPath(); x.arc(S * 0.68, S * 0.38, S * 0.14, 0, Math.PI * 2); x.fill();
      x.strokeStyle = 'rgba(40,36,30,0.35)'; x.lineWidth = 2;
      x.beginPath(); x.moveTo(S * 0.2, S * 0.78); x.lineTo(S * 0.8, S * 0.78); x.stroke();
    });
    const artB = canvasTex(128, (x, S) => {
      x.fillStyle = '#f4efe4'; x.fillRect(0, 0, S, S);
      x.strokeStyle = 'rgba(40,36,30,0.4)'; x.lineWidth = 2.5;
      for (let i = 0; i < 5; i++) {
        x.beginPath();
        x.moveTo(S * 0.2, S * (0.28 + i * 0.11));
        x.bezierCurveTo(S * 0.45, S * (0.2 + i * 0.11), S * 0.6, S * (0.38 + i * 0.11), S * 0.82, S * (0.28 + i * 0.11));
        x.stroke();
      }
    });
    /* 一幅在床上面（-x 墙），一幅在沙发上面（+z 墙） */
    const mkArt = (tex, w, h, x, y, z, rotY) => {
      const frame = new THREE.Mesh(new THREE.BoxGeometry(w + 0.06, h + 0.06, 0.03), matWhite);
      frame.position.set(x, y, z); frame.rotation.y = rotY;
      frame.castShadow = true;
      g.add(frame);
      const pic = new THREE.Mesh(new THREE.PlaneGeometry(w, h), new THREE.MeshBasicMaterial({ map: tex }));
      pic.position.set(x + Math.sin(rotY) * 0.02, y, z + Math.cos(rotY) * 0.02);
      pic.rotation.y = rotY;
      g.add(pic);
    };
    mkArt(artA, 0.62, 0.44, -W + 0.05, 1.82, -2.05, Math.PI / 2);
    mkArt(artB, 0.78, 0.34, 0.0, 1.72, D - 0.04, Math.PI);
  }

  /* ---------------- 生活痕迹 ----------------
     样板间和"她真的住在这儿"的区别全在这些边角：被子掀了一角、杯子里还剩半杯水、
     椅背上搭着外套、拖鞋踢在床边、书没上架先摞在地上。
     都不是装饰品，是"刚才有人用过"的证据。 */
  {
    const matCloth = M({ color: 0x7d8493, roughness: 0.93 });   /* 搭着的那件外套：灰蓝 */
    const matPaper = M({ color: 0xf6f2e8, roughness: 0.90 });
    const matCup   = M({ color: 0xf2efe6, roughness: 0.42 });

    /* 书桌：摊开的笔记本（两页微微翘起）+ 半杯水 + 一支笔 */
    const nx = 1.34, nz = -2.88, ny = 0.772;
    box(0.17, 0.008, 0.235, nx - 0.086, ny, nz, matPaper, { cast: false }).rotation.z = 0.055;
    box(0.17, 0.008, 0.235, nx + 0.086, ny, nz, matPaper, { cast: false }).rotation.z = -0.055;
    box(0.012, 0.010, 0.235, nx, ny + 0.004, nz, matWoodDark, { cast: false });   /* 书脊 */
    const cupBody = new THREE.Mesh(new THREE.CylinderGeometry(0.038, 0.032, 0.092, 16), matCup);
    cupBody.position.set(2.02, 0.806, -2.72); cupBody.castShadow = true; g.add(cupBody);
    const water = new THREE.Mesh(new THREE.CylinderGeometry(0.034, 0.030, 0.040, 16),
      new THREE.MeshStandardMaterial({ color: 0xcfdcea, roughness: 0.15, metalness: 0.0, transparent: true, opacity: 0.72 }));
    water.position.set(2.02, 0.788, -2.72); g.add(water);
    box(0.008, 0.008, 0.145, 1.72, 0.775, -2.66, matWoodDark).rotation.y = 0.42;   /* 笔 */

    /* 椅子背：外套搭在上面，袖子垂下来 */
    box(0.40, 0.44, 0.045, 1.62, 0.78, -1.90, matCloth);
    box(0.075, 0.30, 0.055, 1.43, 0.62, -1.89, matCloth).rotation.z = 0.07;
    box(0.075, 0.26, 0.055, 1.81, 0.58, -1.89, matCloth).rotation.z = -0.09;

    /* 床边：拖鞋踢得很随意，左右不等距 */
    const slip = (x, z, rot) => {
      box(0.095, 0.030, 0.235, x, 0.016, z, matCloth).rotation.y = rot;
      const s = box(0.095, 0.020, 0.10, x, 0.042, z + 0.13, matCloth); s.rotation.y = rot;
    };
    slip(-2.62, -0.62, 0.34);
    slip(-2.36, -0.53, -0.18);

    /* 墙角：一摞还没来得及上架的书 */
    let by = 0.0;
    const spineCols = [0xc0705a, 0x6f8a6a, 0x8a6a9c, 0xb8914e, 0x5d7d99];
    for (let i = 0; i < 5; ++i) {
      const w = 0.20 + (i % 3) * 0.02, d = 0.145 + (i % 2) * 0.02, h = 0.032 + (i % 2) * 0.010;
      const m = M({ color: spineCols[i], roughness: 0.86 });
      box(w, h, d, 3.30, by + h / 2, -2.34 + (i % 2) * 0.012, m).rotation.y = (i - 2) * 0.055;
      by += h;
    }

    /* 沙发：抱枕歪着，一个角翘起来 */
    box(0.42, 0.42, 0.13, 0.52, 0.72, 2.02, matLinen).rotation.z = 0.22;
    box(0.38, 0.34, 0.12, -0.72, 0.66, 2.05, matBlue).rotation.set(0.06, 0.18, -0.14);

    /* 窗台：两个小东西，证明窗是常开的 */
    box(0.055, 0.085, 0.055, -0.86, 0.82, -3.06, matCup);
    box(0.035, 0.030, 0.10, -0.70, 0.80, -3.04, matWoodDark).rotation.y = 0.9;
  }

  /* ---------------- 光：一盏天光 + 窗光 ---------------- */
  const hemi = L(new THREE.HemisphereLight(0xf2f6ff, 0xd9c9ae, 0.10), 0.10, 0.06);
  g.add(hemi);

  /* 窗光：斜着从窗外进来，是唯一投影的平行光 */
  const sun = L(new THREE.DirectionalLight(0xffe3bd, 2.10), 2.10, 0.06);
  sun.position.set(1.6, 4.2, -7.5);
  sun.target.position.set(0.2, 0.8, 1.2);
  sun.castShadow = true;
  sun.shadow.camera.near = 0.5;
  sun.shadow.camera.far = 22;
  sun.shadow.camera.left = -7; sun.shadow.camera.right = 7;
  sun.shadow.camera.top = 7; sun.shadow.camera.bottom = -6;
  /* 改完视锥必须自己更新投影矩阵 —— 不调用的话 three 还用默认的 ±5，
     7 米宽的房间有一半落在影子外面，地板就是均匀的亮，看不出窗格 */
  sun.shadow.camera.updateProjectionMatrix();
  sun.shadow.bias = -0.0012;
  sun.shadow.normalBias = 0.02;
  sun.shadow.mapSize.set(q.decor > 1 ? 2048 : 1536, q.decor > 1 ? 2048 : 1536);
  g.add(sun);
  g.add(sun.target);

  /* 窗边的冷色补光，让窗那一侧不发黄 */
  const skyFill = L(new THREE.PointLight(0xcfe2ff, 1.3, 6.5, 2), 1.3, 0.40);
  skyFill.visible = false;   /* 窗外已经有平行光 + 环境光，这盏是重复照明 */
  skyFill.position.set(0.6, 1.8, -2.9);
  g.add(skyFill);

  /* ---------------- 她的动线 ----------------
     绕茶几一圈的椭圆，跟所有家具至少留 25 cm —— 之前那条会从床角切过去，
     人会半个身子陷在床垫里走。闭合 Catmull-Rom，一圈十几米，
     以 0.85 m/s 走完 ≈ 16 秒，正好是"在屋里转一圈"的节奏。 */
  const pathPts = [
    new THREE.Vector3(-1.38, 0, -1.90),
    new THREE.Vector3(-0.35, 0, -2.25),
    new THREE.Vector3(1.02, 0, -2.02),
    new THREE.Vector3(1.42, 0, -1.05),
    new THREE.Vector3(1.30, 0, 0.15),
    new THREE.Vector3(0.98, 0, 1.45),
    new THREE.Vector3(-0.95, 0, 1.58),
    new THREE.Vector3(-1.24, 0, 0.95),
    new THREE.Vector3(-1.26, 0, -0.55),
  ];
  const walkPath = new THREE.CatmullRomCurve3(pathPts, true, 'catmullrom', 0.5);

  /* ---------------- 每帧：灯在呼吸 ---------------- */
  let t = 0;
  return {
    group: g,
    colliders,
    path: walkPath,
    pathLength: walkPath.getLength(),
    spawn: { x: 0.0, z: 0.0, yaw: 0 },
    bounds: { x0: -W + 0.1, x1: W - 0.1, z0: -D + 0.1, z1: D - 0.1 },
    lights: { sun, hemi, skyFill },
    /* 白天 → 夜里：窗光几乎灭掉，室内的灯接管。切换只要一次循环，不重建场景。 */
    night: false,
    setNight(on) { this.setDaylight(on ? 1 : 0); },
    /* t: 0 = 下午四点，1 = 入夜。连续插值，不是开关 —— 天色是慢慢变暗的。 */
    setDaylight(t) {
      t = Math.max(0, Math.min(1, t));
      for (const r of reg) r.light.intensity = r.day + (r.night - r.day) * t;
      lampBase = 12.0 + 5.0 * t;
      this.night = t > 0.5;
    },
    update(dt) {
      t += dt;
      const L = g.userData.lamp;
      if (L) {
        /* 吊灯非常轻微地晃：0.3 mm 级别的位移，只在静止画面上看得出来 */
        const sway = Math.sin(t * 0.7) * 0.004;
        L.bulb.position.x = L.cx + sway;
        L.shade.position.x = L.cx + sway;
        L.lamp.position.x = L.cx + sway;
        L.lamp.intensity = lampBase + Math.sin(t * 5.3) * 0.12;
      }
    },
  };
}
