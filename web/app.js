import * as THREE from 'three';
import { createWhaleGirl } from './whale.js';
import { loadVRMAvatar } from './vrm.js';
import { createVoice } from './voice.js';

let voice = null;              /* AudioContext 要等用户手势，见 ensureVoice() */
let VOICE_LEVEL = 'soft';      /* off | soft | loud */
import { createRoom } from './room.js';
import { createMats } from './mats.js';
import { createWing } from './wing.js';
import { placeProps, PROP_LIST } from './props.js';
import { createHall } from './hall.js';
import { createItems, pickItem } from './items.js';
import { moveCircle, clampCamera } from './physics.js';
import { createStage } from './stage.js';
import { createPlayer } from './player.js';

/* ---------------- 声带：自填 TTS ----------------
   以前只有 voice.js 里手搭的共振峰（机器味很重）。这里给一个口子：
   地址/密钥/模型/音色都存在 localStorage['soma.tts']，有就走去远端，
   没有或失败还落回本地合成。地址填到 /audio/speech 那一层（OpenAI 兼容）。
   ★ 必须放在文件最前面：菜单在 import 阶段就会调 applyTts()，
     放到下面（1200 行）会撞 TDZ —— 报 `Cannot access 'ttsCfg' before initialization`，
     整个模块 import 失败，页面直接白掉（踩过一次，靠无头浏览器截图才看见）。 */
const TTS_KEY = 'soma.tts';
let ttsCfg = { url: '', key: '', model: '', voice: '' };
try { ttsCfg = Object.assign(ttsCfg, JSON.parse(localStorage.getItem(TTS_KEY) || '{}')); } catch (e) { }
function ttsStatusText() {
  /* 本地共振峰已按主人要求停用（听着像机器念字）。没接就真的不出声。 */
  if (!ttsCfg.url || !ttsCfg.key) return '没接（本地声带已停用，她暂时不出声）';
  return '已接：' + (ttsCfg.model || '默认模型') + (ttsCfg.voice ? ' / ' + ttsCfg.voice : '') + ' · 空气传播';
}
function applyTts() {
  if (voice && voice.setApi) voice.setApi(ttsCfg);
  /* 直接用 getElementById：`$id` 是下面 700 多行才定义的 const，
     import 阶段提前调它同样是 TDZ（typeof 也救不了，照样抛）。 */
  const st = document.getElementById('ttsStat');
  if (st) st.textContent = ttsStatusText();
}

/* sim 自己的 HTTP 端口（8125）：帧流和指令都走它，不经过插件转发 */
const SIM = 'http://127.0.0.1:8125';

/* ---------------- 场景 ---------------- */
const comYEl = document.getElementById('vComY');
const metabEl = document.getElementById('vMetab');
const fatEl = document.getElementById('vFat');
const tempEl = document.getElementById('vTemp');
const brainEl = document.getElementById('vBrain');
const kcalEl = document.getElementById('vKcal');
const waterEl = document.getElementById('vWater');
const sleepEl = document.getElementById('vSleep');
const spinalEl = document.getElementById('vSpinal');
const MOBILE = /Android|iPhone|iPad|iPod|Mobile/i.test(navigator.userAgent);
const CORES = navigator.hardwareConcurrency || 4;
/* 画质档：手机上少一半像素、影子小一号；桌面上放开。
   不是"降级"，是让 60 Hz 的物理别被 GPU 拖住 —— 帧率比像素重要。 */
/* 画质档：流畅 / 均衡 / 精细。菜单里能改，存 localStorage，下次进来还是这档。 */
let QTIER = 'mid';
try { QTIER = localStorage.getItem('soma.q') || (MOBILE ? 'mid' : 'high'); } catch {}
const QTUNE = {
  low:  { dpr: 1.0, shadow: 512,  decor: 0, beam: 0 },
  mid:  { dpr: Math.min(devicePixelRatio || 1, 1.5), shadow: 768,  decor: 1, beam: 1 },
  high: { dpr: Math.min(devicePixelRatio || 1, 2),   shadow: 2048, decor: 2, beam: 1 },
};
const _qt = QTUNE[QTIER] || QTUNE.mid;
const Q = { name: MOBILE ? '手机' : '桌面', dpr: _qt.dpr, shadow: _qt.shadow, decor: _qt.decor, beam: _qt.beam };
/* 显卡不给面子的时候，别整个页面跟着死——把话说明白，数据通道照连 */
let renderer = null;
try {
  try {
    renderer = new THREE.WebGLRenderer({ antialias: true, alpha: false, powerPreference: 'high-performance' });
  } catch (e1) {
    renderer = new THREE.WebGLRenderer({ antialias: false, alpha: false, powerPreference: 'default' });   /* 弱显卡降级 */
  }
  renderer.setPixelRatio(Q.dpr);
  if (renderer) renderer.setSize(innerWidth, innerHeight);
  renderer.shadowMap.enabled = true;
  renderer.shadowMap.type = THREE.PCFSoftShadowMap;
  document.body.appendChild(renderer.domElement);
} catch (e) {
  console.error('[soma3d] WebGL 起不来：', e);
  window.__glErr = String((e && (e.message || e)));          /* 排错用：写到 title 里手机上也能看 */
  if (!window.__err) document.title = 'GL ' + window.__glErr.slice(0, 120);
  document.body.classList.add('nogl');
  const t = document.getElementById('vrmLoad');
  if (t) { t.textContent = '这台浏览器没有 WebGL，画不出来'; t.style.display = 'block'; }
}

const scene = new THREE.Scene();

const camera = new THREE.PerspectiveCamera(40, innerWidth / innerHeight, 0.05, 90);

/* 光、环境、色调映射全在 stage.js：主光带影子，辅光提暗部，背光勾边 */
window.__somaScene = scene;
window.__somaItems = () => ITEMS.items;
window.__somaTap = (x, y) => tapWorld(x, y);   /* 验收：直接喂屏幕坐标 */
window.__somaTHREE = THREE;
/* 验收：跳过射线，直接"用某个东西"（走的是同一条 goto → cmd 流程） */
window.__somaUseItem = (id) => {
  const it = ITEMS.items.find(o => o.id === id);
  if (!it) return null;
  send('goto ' + it.x.toFixed(2) + ' ' + it.z.toFixed(2));
  pendingUse = { item: it, t0: performance.now(), cmdSent: false };
  return it;
};
window.__somaPick = (x, y) => { const r = renderer.domElement.getBoundingClientRect(); const v = new THREE.Vector2(((x-r.left)/r.width)*2-1, -((y-r.top)/r.height)*2+1); const rr = new THREE.Raycaster(); rr.setFromCamera(v, camera); const p = pickItem(THREE, rr, ITEMS.hotspots); return p ? p.item.id : null; };   /* 验收用：无头环境没有 GL 也能数场景里的东西 */
const stage = renderer ? createStage(THREE, { renderer, scene, quality: Q, room: true })
                       : { update() { } };   /* 没有 GL 就是空舞台：灯不用点，界面照样活着 */
/* 贴图：Poly Haven CC0 的 PBR 贴图，异步加载，材质先带颜色、图到了再挂上 */
const MATS = renderer ? createMats(THREE, renderer) : { mk: (o) => new THREE.MeshStandardMaterial(o || {}), ready: () => Promise.resolve() };
window.__somaMats = MATS;
const ROOM = createRoom(THREE, Q, MATS);
scene.add(ROOM.group);
/* 北厢：走廊 + 三间卧室 + 卫浴 + 厨房。碰撞盒并进同一个数组 ——
   物理、习惯层、镜头避让读的都是这一份，不另写一套。 */
const WING = createWing(THREE, MATS);
const HALL = createHall(THREE, MATS);
scene.add(HALL.group);
ROOM.colliders.push(...HALL.colliders);
scene.add(WING.group);

/* 真家具：Poly Haven CC0 模型按坐标摆进来，摆上去的那几件会把程序化盒子藏掉。
   异步的，摆不完也不影响走路（物理用的是 colliders，不是这些网格）。 */
{
  const on = (() => { try { return new URLSearchParams(location.search).get('props') !== '0' } catch (e) { return true } })();
  if (on) {
    placeProps(THREE, [ROOM.group, WING.group], PROP_LIST.living)
      .then(n => console.log('[props] 客厅', n + '/' + PROP_LIST.living.length))
      .catch(e => console.warn('[props] 客厅失败', e && e.message));
    placeProps(THREE, [WING.group], PROP_LIST.wing)
      .then(n => console.log('[props] 北厢', n + '/' + PROP_LIST.wing.length))
      .catch(e => console.warn('[props] 北厢失败', e && e.message));
  }
}
ROOM.colliders.push(...WING.colliders);
ROOM.bounds = { x0: -3.90, x1: 6.10, z0: -3.15, z1: 9.45 };   /* +x 撑到走廊尽头 */
ROOM.wing = WING;
ROOM.hall = HALL;
/* 可点的东西：隐形热点 + 射线 */
const ITEMS = createItems(THREE);
scene.add(ITEMS.group);

/* ---------------- 她在房间里的位置 ----------------
   v3 起朝向进了动力学：C++ 给的位置/朝向就是真的，不再拿房间动线的弧长去"猜"。
   房间坐标 = C++ 世界坐标（路线航点就是用房间坐标发过去的），不需要换算。 */
const her = { x: 0, z: 0, yaw: 0, speed: 0 };
function placeHer() {
  /* 转弯时轨迹会比航点偏出去一点点，她可能蹭到家具 —— 每帧把她当成 0.30 的圆
     （差不多是裙摆）推出来。推出量通常只有几毫米，看不出来，但不会半个身子进床里。 */
  const fixd = moveCircle(her.x, her.z, 0, 0, 0.34, ROOM.colliders, ROOM.bounds);
  her.x = fixd.x; her.z = fixd.z;
}
placeHer();

/* ---------------- 角色 ---------------- */
const N = 18;
const jointMat = new THREE.MeshStandardMaterial({
  color: 0x63b4ff, emissive: 0x1b4d86, emissiveIntensity: 0.9, roughness: 0.35, metalness: 0.4 });
const jointGeo = new THREE.SphereGeometry(1, 12, 10);
const joints = [];
for (let i = 0; i < N; i++) {
  const m = new THREE.Mesh(jointGeo, jointMat);
  m.castShadow = true; scene.add(m); joints.push(m);
}

const boneGeo = new THREE.CylinderGeometry(1, 1, 1, 10, 1, true);
const boneMat = new THREE.MeshStandardMaterial({
  color: 0x9ad7ff, emissive: 0x0d3663, emissiveIntensity: 0.55, roughness: 0.45, metalness: 0.3,
  transparent: true, opacity: 0.94, side: THREE.DoubleSide });
const bones = [];
for (let i = 0; i < 17; i++) {
  const m = new THREE.Mesh(boneGeo, boneMat);
  m.castShadow = true; scene.add(m); bones.push(m);
}

/* 脚：扁盒子，落地时亮一下 */
const footHot = new THREE.MeshStandardMaterial({ color: 0x9fe6ff, emissive: 0x2f7fc0, emissiveIntensity: 1.1, roughness: 0.5, metalness: 0.2 });
const footCold = new THREE.MeshStandardMaterial({ color: 0x3d76a8, emissive: 0x0a2038, emissiveIntensity: 0.6, roughness: 0.7, metalness: 0.2 });
const footL = new THREE.Mesh(new THREE.BoxGeometry(0.25, 0.035, 0.095), footHot); footL.castShadow = true;
const footR = footL.clone();
scene.add(footL, footR);

/* 脸：两只眼睛 + 一点高光，方便分辨朝向 */
const eyeMat = new THREE.MeshBasicMaterial({ color: 0xeaf6ff });
const pupilMat = new THREE.MeshBasicMaterial({ color: 0x0a1626 });
const eyeGeo = new THREE.SphereGeometry(1, 12, 10);
const eyes = [], pupils = [];
for (let i = 0; i < 2; i++) {
  const e = new THREE.Mesh(eyeGeo, eyeMat); e.scale.setScalar(0.022); scene.add(e); eyes.push(e);
  const p = new THREE.Mesh(eyeGeo, pupilMat); p.scale.setScalar(0.012); scene.add(p); pupils.push(p);
}
const blushMat = new THREE.MeshBasicMaterial({ color: 0x2b6bb0, transparent: true, opacity: 0.8 });

/* ---------------- 鲸鱼娘本体 ---------------- */
/* 调试用的关节球/骨柱留着：按 V 键可以在"骨骼视图"和"她"之间切。
   两套都跟同一批节点，所以不存在对不上的问题。 */
const whale = createWhaleGirl(THREE);
scene.add(whale.group);
let showBones = false;
const boneView = [...joints, ...bones, ...eyes, ...pupils, footL, footR];

/* 三态：0 = 真模型（VRM）· 1 = 程序化低模 · 2 = 骨骼调试 */
let vrmAvatar = null, viewMode = 0;
/* 弹簧骨（头发/裙子/尾巴）调参。
   模型原值是给「站着摆拍」调的：头发 stiff 2.3 / 裙子 6，一走路就整片乱甩；
   尾巴 drag 只有 0.34，甩起来像鞭子。下面这套是在房间里边走边看调出来的：
     DS_hair   76 根 —— 软一点、重力加倍，让它垂着
     DS_skirt  32 根 —— 刚度砍半，裙摆跟着腿走而不是硬邦邦一片
     DS_whale   5 根 —— 尾巴最重：阻力拉高、重力 ×6，甩完能收住
     DS_ribbon 10 根 / DS_ahoge 4 根 —— 飘带和呆毛，轻但别飞
   数值都是「相对模型原值的倍数」，不是绝对值。 */
const SPRING_PROFILE = {
  DS_hair: { stiff: 0.85, grav: 2.0, drag: 0.25 },
  DS_ahoge: { stiff: 1.0, grav: 6.0, drag: 0.20 },
  DS_skirt: { stiff: 0.50, grav: 0.40, drag: 0.45 },
  DS_whale: { stiff: 0.70, grav: 6.0, drag: 0.65 },
  DS_ribbon: { stiff: 0.90, grav: 1.60, drag: 0.30 },
};
let SPRING_TUNE = { groups: SPRING_PROFILE };
let SHOW_ROUTE = false;
function applyView() {
  const hasVRM = !!vrmAvatar;
  if (vrmAvatar) vrmAvatar.root.visible = (viewMode === 0);
  whale.group.visible = (viewMode === 1) || (viewMode === 0 && !hasVRM);
  boneView.forEach(o => o.visible = (viewMode === 2));
  const b = document.getElementById('vView');
  if (b) b.textContent = viewMode === 2 ? '骨骼'
    : viewMode === 1 ? '低模' : (hasVRM ? '本体' : '加载中');
}
function setBoneView(v) { viewMode = v ? 2 : 0; applyView(); }
applyView();
addEventListener('keydown', e => {
  if (e.key === 'v' || e.key === 'V') { viewMode = viewMode === 2 ? 0 : 2; applyView(); }
  if (e.key === 'd' || e.key === 'D') setDiag(!diagOn);
});

/* 33 MB 的 VRM 走 HTTP 慢慢来；加载期间先看低模，不白屏 */
const vrmBar = document.getElementById('vrmLoad');
let vrmOk = false, vrmErr = '', vrmPct = 0;
function vrmMsg(t) { if (vrmBar) vrmBar.textContent = t; }

/* ---- 诊断面板（菜单里可开；D 键也能开）----
   穿模这种事不看数就只剩猜：弹簧骨几根、dt 多少、她这帧在哪。 */
const diagEl = document.getElementById('diag');
let diagOn = false, diagTick = 0;
function setDiag(v) {
  diagOn = !!v;
  if (diagEl) diagEl.classList.toggle('on', diagOn);
  try { if (routeLine) routeLine.visible = diagOn; } catch (e) { }
}
const diagState = { dt: 0, fps: 0, frames: 0, t0: 0, fpsN: 0 };
function drawDiag() {
  if (!diagOn || !diagEl) return;
  if (++diagTick % 20) return;
  const d = vrmAvatar && vrmAvatar.diag ? vrmAvatar.diag() : null;
  const lines = [];
  lines.push('fps ' + (fpsShown || 0) + '  dt ' + (diagState.dt * 1000).toFixed(0) + 'ms');
  lines.push('pos ' + her.x.toFixed(2) + ',' + her.z.toFixed(2)
    + '  yaw ' + (her.yaw * 180 / Math.PI).toFixed(0) + '°');
  if (diagState.win) lines.push('win ' + diagState.win);
  if (window.__chat) lines.push('chat ' + window.__chat);
  lines.push('cam d' + cam.dist.toFixed(2) + ' tg ' + cam.target.x.toFixed(2) + ',' + cam.target.y.toFixed(2)
    + ',' + cam.target.z.toFixed(2) + ' pos ' + camera.position.x.toFixed(2) + ',' + camera.position.y.toFixed(2)
    + ',' + camera.position.z.toFixed(2) + ' wl ' + whale.group.position.x.toFixed(2) + ',' + whale.group.position.z.toFixed(2)
    + ' v' + (whale.group.visible ? 1 : 0) + ' m' + viewMode);
  if (sayEl) {
    lines.push('say ' + (sayEl.classList.contains('on') ? 'on' : 'off')
      + ' len' + (sayEl.textContent || '').length + ' ' + JSON.stringify(sayLog.slice(-2)));
  }
  if (d) {
    lines.push('springs ' + d.springs + '  bones ' + d.bones + '  ' + d.meta);
    const now = (vrmAvatar.springNow && vrmAvatar.springNow()) || d.groups;
    for (const k in now) {
      const g = now[k];
      lines.push(' ' + k + ' x' + g.n + '  s' + g.stiff + ' d' + g.drag + ' g' + g.grav);
    }
  } else lines.push('vrm —');
  diagEl.textContent = lines.join('\n');
}

/* 这条状态一直留着：她不动的时候，先分清是「帧不来」还是「骨没挂上」 */
let statN = 0;
let vrmReadyAt = 0;
function updateStat(f) {
  if (!vrmBar || (statN++ % 15)) return;
  /* 加载完成后这条就不该一直挂着了：它是诊断，不是装饰 */
  if (vrmOk && vrmReadyAt && (performance.now() - vrmReadyAt) > 4000) {
    vrmBar.classList.add('done'); return;
  }
  if (state === 'down') { vrmBar.textContent = '桥接断了'; return; }
  if (vrmErr) { vrmBar.textContent = 'VRM 失败：' + vrmErr.slice(0, 34); return; }
  if (!vrmOk) {
    vrmBar.textContent = 'VRM ' + (Math.round(vrmPct * 100) + '% ' + (window.__vrmStage || ''))
      + ' · 帧 ' + frameN;
    return;
  }
  vrmBar.textContent = (vrmOk ? 'VRM' : '低模') + ' · 帧 ' + frameN
    + (f && typeof f.t === 'number' ? ' · t=' + f.t.toFixed(1) : '');
}
/* 建模：高档用 33 MB 的原版（贴图 2K、头发分层更细），其它档用 12.9 MB 的轻量版。
   两个文件是同一套骨架与同一份材质名 —— 换档只影响清晰度，不影响动作。 */
const MODEL_URL = (QTIER === 'high' ? './models/DeepSeek-chan.vrm?v=1' : './models/DeepSeek-chan-small.vrm?v=1');
loadVRMAvatar(MODEL_URL, p => {
  vrmPct = p;
  vrmMsg('VRM ' + Math.round(p * 100) + '%');
}).then(a => {
  vrmOk = true;
  vrmReadyAt = performance.now();
  vrmMsg('VRM 就绪');
  vrmAvatar = a;
  if (SPRING_TUNE) a.setSpringTune(SPRING_TUNE);
  a.root.visible = (viewMode === 0);
  scene.add(a.root);
  applyView();
  console.log('[vrm] ok');
}).catch(e => {
  vrmErr = (e && e.message) || 'unknown';
  console.warn('[vrm] 加载失败，继续用低模：', e && e.message);
  vrmMsg('VRM 失败，用低模');
  applyView();
});

/* 影子 + CoM/CaP/ZMP 标记 */
/* 接触阴影：真影子在 30Hz 刷、边缘偏软，脚底这一小块负责"她确实踩在地上" */
const blob = new THREE.Mesh(new THREE.CircleGeometry(0.30, 32),
  new THREE.MeshBasicMaterial({ color: 0x1a1712, transparent: true, opacity: 0.20, depthWrite: false }));
blob.rotation.x = -Math.PI / 2; blob.position.y = 0.003; scene.add(blob);

function disc(color, r) {
  const m = new THREE.Mesh(new THREE.CircleGeometry(r, 20),
    new THREE.MeshBasicMaterial({ color, transparent: true, opacity: 0.85 }));
  m.rotation.x = -Math.PI / 2; m.position.y = 0.006; scene.add(m); return m;
}
/* 重心/捕获点/压心这些是给调试看的，默认藏起来 —— 干净的白空间里
   地上三个彩色小点会显得像没扫干净。要看就 #debug=1。 */
const DEBUG_MARKS = /(^|[#;])debug=1/.test(location.hash) || /[?&]debug=1/.test(location.search);
const comMark = disc(0x38e08a, 0.055);   // 重心 绿
const capMark = disc(0xffc857, 0.045);   // 捕获点 黄
const zmpMark = disc(0xff5d73, 0.04);    // 压心 红
comMark.visible = capMark.visible = zmpMark.visible = DEBUG_MARKS;

/* 轨迹线：走两步留一条尾巴 */
const trailGeo = new THREE.BufferGeometry();
const TRAIL = 240;
trailGeo.setAttribute('position', new THREE.BufferAttribute(new Float32Array(TRAIL * 3), 3));
const trail = new THREE.Line(trailGeo, new THREE.LineBasicMaterial({ color: 0x7fa9cf, transparent: true, opacity: 0.10 }));
scene.add(trail);
let trailN = 0;

/* ---------------- 地面反作用力箭头 ---------------- */
const grfArrows = [0, 1].map(() => {
  const a = new THREE.ArrowHelper(new THREE.Vector3(0, 1, 0), new THREE.Vector3(), 0.1, 0xffd166, 0.06, 0.04);
  a.line.material.linewidth = 2;
  scene.add(a);
  return a;
});
const GRF_SCALE = 1.0 / 1400;   /* 700 N ≈ 0.5 m 的箭头 */

/* ---------------- 世界几何（台阶/平台/墙） ---------------- */
const boxGroup = new THREE.Group();
scene.add(boxGroup);
const boxMat = new THREE.MeshStandardMaterial({ color: 0x18334f, roughness: 0.72, metalness: 0.25 });
const boxEdge = new THREE.LineBasicMaterial({ color: 0x3f7fbf, transparent: true, opacity: 0.55 });
function buildWorld(list) {
  while (boxGroup.children.length) boxGroup.remove(boxGroup.children[0]);
  const M = (c, r = 0.7) => new THREE.MeshStandardMaterial({ color: c, roughness: r, metalness: 0.02 });
  for (const b of list) {
    const kind = b.kind || 'platform';
    if (kind === 'food') {
      const r = Math.max(0.042, (b.w || 0.16) * 0.45);
      const m = new THREE.Mesh(new THREE.SphereGeometry(r, 16, 12), M(0xcf3a2c, 0.5));
      m.position.set(b.x, b.h, b.z);
      m.castShadow = true; m.receiveShadow = true;
      boxGroup.add(m);
      const leaf = new THREE.Mesh(new THREE.ConeGeometry(r * 0.30, r * 0.42, 6), M(0x3f8f4a, 0.8));
      leaf.position.set(b.x, b.h + r * 1.05, b.z);
      boxGroup.add(leaf);
      continue;
    }
    if (kind === 'drink') {
      const r = Math.max(0.032, (b.w || 0.16) * 0.36);
      const m = new THREE.Mesh(new THREE.CylinderGeometry(r, r * 0.86, b.h, 14), M(0xdff2f7, 0.35));
      m.position.set(b.x, b.h / 2, b.z);
      m.castShadow = true;
      boxGroup.add(m);
      const tea = new THREE.Mesh(new THREE.CylinderGeometry(r * 0.88, r * 0.88, 0.004, 14), M(0x9fd8a0, 0.3));
      tea.position.set(b.x, b.h * 0.92, b.z);
      boxGroup.add(tea);
      continue;
    }
    if (kind === 'seat') {
      const w = b.w || 0.42, d = b.d || 0.42, h = b.h || 0.45;
      const top = new THREE.Mesh(new THREE.BoxGeometry(w, 0.05, d), M(0x8a5f3a, 0.65));
      top.position.set(b.x, h - 0.025, b.z);
      top.castShadow = true; top.receiveShadow = true;
      boxGroup.add(top);
      for (const sx of [-1, 1]) for (const sz of [-1, 1]) {
        const leg = new THREE.Mesh(new THREE.BoxGeometry(0.04, h - 0.05, 0.04), M(0x6d4a2c, 0.7));
        leg.position.set(b.x + sx * (w / 2 - 0.035), (h - 0.05) / 2, b.z + sz * (d / 2 - 0.035));
        boxGroup.add(leg);
      }
      continue;
    }
    /* 平台 / 墙 / 默认方块 */
    const wall = (b.h || 0) > 0.24;
    const x0 = b.x0, x1 = b.x1, z0 = b.z0, z1 = b.z1;
    const w = Math.max(0.01, x1 - x0), d = Math.max(0.01, z1 - z0), h = Math.max(0.01, b.h);
    const geo = new THREE.BoxGeometry(w, h, d);
    const m = new THREE.Mesh(geo, wall ? boxMat : M(0x3a4a63, 0.85));
    m.position.set((x0 + x1) / 2, h / 2, (z0 + z1) / 2);
    m.castShadow = true; m.receiveShadow = true;
    boxGroup.add(m);
    const e = new THREE.LineSegments(new THREE.EdgesGeometry(geo), boxEdge);
    e.position.copy(m.position);
    boxGroup.add(e);
  }
}

/* ---------------- 相机：导演镜头 ----------------
   默认不是第一人称 —— 第一人称看自己，你永远看不到她。
   导演镜头站在她前侧 3/4 的位置，跟着她的重心走；拖一下可以绕着看。
   按 T 才是第一人称（你的眼睛，那就是"你自己在走"）。
*/
const cam = {
  yaw: -1.571, pitch: 0.04, dist: 2.55, mode: 'fp', follow: false, /* 站在她路线旁边、正对着路：她横着走过你面前 */   /* 主视角＝玩家自己的眼睛 */
  target: new THREE.Vector3(0, 1.0, 0),
};
const _camWant = { x: 0, y: 0, z: 0 };
function applyCamera() {
  const cy = Math.cos(cam.pitch), sy = Math.sin(cam.pitch);
  _camWant.x = cam.target.x + Math.cos(cam.yaw) * cy * cam.dist;
  _camWant.y = cam.target.y + sy * cam.dist;
  _camWant.z = cam.target.z + Math.sin(cam.yaw) * cy * cam.dist;
  /* 椅子腿挡不住镜头，书架挡得住 —— 撞上就把镜头拉到家具前面来 */
  const cp = clampCamera(cam.target, _camWant, ROOM.colliders, ROOM.bounds, 0.26);
  /* 家具/墙把镜头顶到她脸前时（只剩 0.3 m），画面里就是一块布。
     留一条硬下限：离焦点至少 1.15 m；不够就往外退、并且往上抬一点，
     变成一个俯看的角度 —— 总比贴在她后颈上强。 */
  {
    const dx = cp.x - cam.target.x, dy = cp.y - cam.target.y, dz = cp.z - cam.target.z;
    const d3 = Math.sqrt(dx * dx + dy * dy + dz * dz) || 1e-4;
    /* 竖屏是竖长条、FOV 又只截到 50°，同样的距离会把她截成半身。
       竖屏的硬下限就抬到 2.1 m；矮一点的那些走"往头顶上抬"那条路。 */
    const porNow = innerHeight > innerWidth * 1.05;
    const MIND = porNow ? 2.1 : 1.35, GOOD = porNow ? 3.0 : 2.1;
    if (d3 < MIND) {
      const s = MIND / d3;
      cp.x = cam.target.x + dx * s * 0.72;
      cp.z = cam.target.z + dz * s * 0.72;
      cp.y = cam.target.y + Math.max(0.42, dy * s * 0.8 + (MIND - d3) * 0.85);
    } else if (d3 < GOOD) {
      /* 只退到 1.2~2 m 还是会把她的头发塞满画面（裙摆半径 0.46 m）。
         这个区间不硬顶，改成往上抬 —— 从头顶斜俯看，人是完整的。 */
      const k = (GOOD - d3) / (GOOD - MIND);
      cp.x = cam.target.x + dx * (0.42 + 0.58 * (1 - k));
      cp.z = cam.target.z + dz * (0.42 + 0.58 * (1 - k));
      cp.y = cam.target.y + dy * (1 - k) + k * (porNow ? 2.6 : 2.15);
    }
  }
  camera.position.set(cp.x, cp.y, cp.z);
  camera.lookAt(cam.target);
}
/* 调试用：?yaw=-0.9&pitch=0.1&d=2.2 直接定住机位，截图对比省事 */
{
  const u = new URLSearchParams((location.search + '&' + location.hash.replace(/^#/, '').replace(/;/g, '&')).replace(/^&/, ''));
  if (u.has('yaw')) cam.yaw = parseFloat(u.get('yaw'));
  if (u.has('pitch')) cam.pitch = parseFloat(u.get('pitch'));
  if (u.has('d')) { cam.dist = parseFloat(u.get('d')); cam.lockD = true; }
  if (u.get('fp') === '1') cam.mode = 'fp';
  if (u.get('tp') === '1') cam.mode = 'director';   /* 想看导演机位就加 #tp=1 */
  if (u.get('watch') === 'her') cam.watch = 'her';    /* 调试：镜头临时跟着她 */
  /* 竖屏默认"看她"：手机上一人称只剩一条窄缝，房间里什么都看不到。
     横屏/桌面还是第一人称自己走。参数里显式给了视角就以参数为准。 */
  if (!u.has('watch') && !u.has('fp') && !u.has('tp') && innerHeight > innerWidth * 1.05) {
    cam.mode = 'director';
    cam.watch = 'her';
  }
  if (u.get('diag') === '1') setDiag(true);          /* 调试：直接开诊断面板（手机上没有键盘） */
  if (u.has('model')) {
    viewMode = u.get('model') === 'skel' ? 2 : u.get('model') === 'low' ? 1 : 0;
    applyView();                 /* 参数是在 applyView() 之后才读的，得手动刷一次 */
  }
  if (u.get('desk') === '1') document.documentElement.classList.add('desk-force');   /* 调试：强制桌面布局 */
  if (u.get('nosplash') === '1') {                    /* 调试：跳过开场，直接进房间（截图用） */
    const sp = document.getElementById('splash');
    if (sp) sp.classList.add('gone');
  }
  /* 调试：?sayline=文字&hold=60 —— 直接把一句她的话贴在屏幕上，验样式用 */
  if (u.has('sayline')) {
    const t0 = decodeURIComponent(u.get('sayline'));
    const hold = parseFloat(u.get('hold') || '60');
    setTimeout(() => {
      const sp = document.getElementById('splash');
      if (sp) sp.classList.add('gone');
      showSayLine(t0, hold);
    }, 1200);
  }
  if (u.get('route') === '1') SHOW_ROUTE = true;      /* 调试：把动线画在地上 */
  /* 调试：?sp=stiff,grav,drag 直接压弹簧骨参数（头发/裙子太飘时用） */
  if (u.has('sp')) {
    const a = (u.get('sp') || '').split(',').map(Number);
    SPRING_TUNE = { stiff: a[0], grav: a[1], drag: a[2] == null ? 0 : a[2] };
  }
  /* 调试：?spt=组:stiff,grav,drag;组:... 按组压（DS_hair / DS_whale / DS_skirt …） */
  if (u.has('spt')) {
    SPRING_TUNE = SPRING_TUNE || {};
    SPRING_TUNE.groups = {};
    for (const part of (u.get('spt') || '').split(';')) {
      const m = /^([A-Za-z0-9_]+):(.+)$/.exec(part.trim());
      if (!m) continue;
      const a = m[2].split(',').map(Number);
      SPRING_TUNE.groups[m[1]] = { stiff: a[0], grav: a[1], drag: a[2] };
    }
  }
  if (u.get('view')) {
    const v = u.get('view');
    viewMode = v === 'skel' ? 2 : v === 'low' ? 1 : 0;
    if (typeof applyView === 'function') applyView();
  }
}
/* 天色跟着钟走：白天是下午四点，傍晚慢慢变暗，夜里只剩她那几盏灯。
   没有按钮 —— 房间本来就该自己知道现在是几点。 */
let TIME_PIN = null;      /* null = 跟着真实钟点；0/0.62/1 = 白天/黄昏/夜里 */
function autoLight() {
  const d = new Date();
  let h = d.getHours() + d.getMinutes() / 60;
  if (TIME_PIN != null) {
    /* 钉住的是"天色的程度"，不是钟点：0 白天、1 夜里，中间插值 */
    const t0 = TIME_PIN;
    ROOM.setDaylight(t0);
    if (WING) WING.setDaylight(t0);
    if (HALL) HALL.setDaylight(t0);
    const bg0 = new THREE.Color(0xe9e3d6).lerp(new THREE.Color(0x171a20), t0);
    scene.background = bg0;
    if (scene.fog) { scene.fog.color = bg0; scene.fog.near = 12 - 5 * t0; scene.fog.far = 26 - 7 * t0; }
    if ('environmentIntensity' in scene) scene.environmentIntensity = 0.42 - 0.28 * t0;
    return;
  }
  /* #h=16 可以把天色钉在某个钟点 —— 调光的时候不用等真的到四点 */
  const m = /[#&]h=([0-9.]+)/.exec(location.hash);
  if (m) h = parseFloat(m[1]);
  /* 6 点开始亮，17 点到 20 点是最慢的一段，20 点之后彻底入夜 */
  const t = h < 6 ? 1 : h < 17 ? 0 : h < 20 ? (h - 17) / 3 : 1;
  ROOM.setDaylight(t);
  if (WING) WING.setDaylight(t);
    if (HALL) HALL.setDaylight(t);
  const bg = new THREE.Color(0xe9e3d6).lerp(new THREE.Color(0x171a20), t);
  scene.background = bg;
  if (scene.fog) { scene.fog.color = bg; scene.fog.near = 12 - 5 * t; scene.fog.far = 26 - 7 * t; }
  if ('environmentIntensity' in scene) scene.environmentIntensity = 0.42 - 0.28 * t;
}
autoLight();
setInterval(autoLight, 120000);

applyCamera();
/* 临时诊断：?shadow=1 放个方块看它投不投影 */
if (location.hash.indexOf('diag=1') >= 0) {
  const lights = scene.children.filter(o => o.isLight);
  const keyL = lights.find(l => l.isDirectionalLight && l.castShadow);
  setTimeout(() => {
    const el = document.getElementById('splash');
    el.classList.remove('gone');
    el.style.display = 'grid';
    const big = el.querySelector('.big');
    const sub = el.querySelector('.sub');
    big.style.fontSize = '13px';
    big.textContent = 'sm=' + (renderer ? renderer.shadowMap.enabled : '-') + ' gl2=' + renderer.capabilities.isWebGL2 +
      ' L=' + lights.length + ' key=' + (!!keyL) +
      ' map=' + (keyL && keyL.shadow.map ? keyL.shadow.map.width : 'null');
    sub.textContent = 'dpr=' + (renderer ? renderer.getPixelRatio().toFixed(2) : '-') +
      ' meshes=' + scene.children.filter(o => o.isMesh).length +
      ' autoUpdate=' + (renderer ? renderer.shadowMap.autoUpdate : '-');
  }, 3000);
}
if (location.hash.indexOf('shadow=1') >= 0 || location.search.indexOf('shadow=1') >= 0) {
  const cube = new THREE.Mesh(new THREE.BoxGeometry(0.5, 0.9, 0.5),
    new THREE.MeshStandardMaterial({ color: 0xcc4444, roughness: 0.6 }));
  cube.position.set(1.2, 0.45, 0.8);
  cube.castShadow = true;
  scene.add(cube);
  document.getElementById('fps').textContent =
    'shadowMap=' + (renderer ? renderer.shadowMap.enabled : '-') + ' type=' + (renderer ? renderer.shadowMap.type : '-') +
    ' cast=' + cube.castShadow;
}

/* 鼠标 / 触摸拖拽 */
let dragging = false, lx = 0, ly = 0, moved = 0;
if (renderer) renderer.domElement.addEventListener('pointerdown', e => { dragging = true; lx = e.clientX; ly = e.clientY; moved = 0; });
addEventListener('pointerup', e => {
  const wasDrag = moved > 12;
  dragging = false;
  if (!wasDrag && renderer && e.target === renderer.domElement) tapWorld(e.clientX, e.clientY);
});
addEventListener('pointermove', e => {
  if (!dragging) return;
  const dx = e.clientX - lx, dy = e.clientY - ly; lx = e.clientX; ly = e.clientY;
  moved += Math.abs(dx) + Math.abs(dy);
  cam.yaw += dx * 0.005;
  cam.pitch = Math.max(-0.12, Math.min(0.80, cam.pitch + dy * 0.004));
  cam.follow = false;   /* 你自己转过镜头了，就由你做主，别再自己绕回去 */
  applyCamera();
});
if (renderer) renderer.domElement.addEventListener('wheel', e => {
  cam.dist = Math.max(1.6, Math.min(11, cam.dist * (1 + Math.sign(e.deltaY) * 0.1)));
  applyCamera(); e.preventDefault();
}, { passive: false });

/* ---------------- 玩家替身 ----------------
   她那条命是 C++ 算的，你这条是"想走就走"。共用同一个模型和姿态接口。
*/
const player = createPlayer(THREE);
/* 玩家不是"另一个角色"，是你在白空间里的影子：
   半透明、偏蓝、不投影。这样镜头里只有她一个是实的。 */
player.group.traverse(o => {
  if (!o.isMesh) return;
  o.castShadow = false;
  o.receiveShadow = false;
  const m = o.material;
  if (!m) return;
  o.material = m;
  o.castShadow = true;
});
scene.add(player.group);
/* 玩家站在她这条路线的侧后方，看向 +x：她是从面前走过去，不是从背后走过去 */
const P = { x: 2.55, z: 2.35, yaw: -1.2, speed: 0, step: 0 };
player.group.position.set(P.x, 0, P.z);

const keysDown = new Set();
addEventListener('keydown', e => keysDown.add(e.key.toLowerCase()));
/* 桌面上没摇杆：回车/空格把焦点给输入框，Esc 关菜单 —— 一只键盘就能玩 */
addEventListener('keydown', e => {
  const tag = (e.target && e.target.tagName || '').toLowerCase();
  const typing = tag === 'input' || tag === 'textarea';
  if (e.key === 'Escape') {
    const m = document.getElementById('menu');
    if (m && m.classList.contains('open')) { m.classList.remove('open'); }
    if (typing && e.target.blur) e.target.blur();
  }
  if (typing) return;
  if (e.key === 'Enter' || e.key === '/') {
    const i = document.getElementById('chatInput');
    if (i) { e.preventDefault(); i.focus(); }
  }
});
addEventListener('keyup', e => keysDown.delete(e.key.toLowerCase()));
addEventListener('blur', () => keysDown.clear());

const SPEED = 1.55;
const _fwd = new THREE.Vector3(), _rgt = new THREE.Vector3(), _want = new THREE.Vector3();
/* 低模用的局部坐标缓冲（免得每帧 new 一堆 Vector3） */
const whLocal = Array.from({ length: 18 }, () => new THREE.Vector3());

function updatePlayer(dt) {
  let ix = 0, iz = 0;
  if (keysDown.has('w') || keysDown.has('arrowup')) iz += 1;
  if (keysDown.has('s') || keysDown.has('arrowdown')) iz -= 1;
  if (keysDown.has('a') || keysDown.has('arrowleft')) ix -= 1;
  if (keysDown.has('d') || keysDown.has('arrowright')) ix += 1;
  /* 导演镜头下"你"站着不动 —— 那条摇杆这时候管的是镜头，不是人。
     不然推半天只看见一个看不见的自己在地上走，画面一动不动。 */
  const walking = cam.mode === 'fp';
  /* 摇杆横向就是左右平移。以前这条被拿去转镜头了，于是"方向键只能前后" ——
     第一人称下转头靠拖动（pointer 已经在做），摇杆不该抢这件事。 */
  if (walking) { iz += stickV; ix += stickH; }

  const mag = Math.hypot(ix, iz);
  if (mag > 1) { ix /= mag; iz /= mag; }

  /* 跟着镜头走：屏幕上按上，就是朝画面深处走 */
  _fwd.set(Math.cos(cam.yaw), 0, Math.sin(cam.yaw));
  /* 屏幕右 = (-sin, 0, cos)：视线 f=(cos,0,sin)，f×up 就是它。
     以前写成 (sin,0,-cos)，是它的反向 —— 所以摇杆左右是反的。 */
  _rgt.set(-Math.sin(cam.yaw), 0, Math.cos(cam.yaw));
  _want.set(0, 0, 0).addScaledVector(_fwd, iz).addScaledVector(_rgt, ix);

  const wl = _want.length();
  P.speed = Math.min(1, wl) * SPEED;

  if (wl > 0.02) {
    _want.divideScalar(wl);
    const goal = Math.atan2(-_want.z, _want.x);
    let d = goal - P.yaw;
    while (d > Math.PI) d -= Math.PI * 2;
    while (d < -Math.PI) d += Math.PI * 2;
    P.yaw += d * Math.min(1, dt * 11);
    P.step += dt * (5.0 + P.speed * 2.0);
  }

  /* 真正的一步：圆盘 vs 一屋子盒子，撞上就贴着滑。
     半径 0.24 是按肩宽给的 —— 再大就会在桌子前面莫名其妙停住。 */
  if (P.speed > 1e-4) {
    const step = P.speed * dt;
    const mv = moveCircle(P.x, P.z, _want.x * step, _want.z * step, 0.24, ROOM.colliders, ROOM.bounds);
    P.x = mv.x; P.z = mv.z;
    if (mv.hit === 'x') _want.x *= 0.2;   // 侧向被挡：只影响这一帧的位移，不积攒成"卡住"
    if (mv.hit === 'z') _want.z *= 0.2;
  }

  player.group.position.set(P.x, 0, P.z);
  player.group.rotation.y = P.yaw;
  player.update(dt, P.speed);
}

/* 第一人称：眼睛就长在你身上。
   `T` 切回第三人称——调试她的时候还得看全身。 */
const EYE = 1.56;
const _look = new THREE.Vector3();

function followCamera(dt) {
  if (cam.mode === 'fp') {
    player.group.visible = false;
    /* 走起来有一点起伏，不然像台滑行的摄像机 */
    const bob = Math.sin(P.step * 2) * 0.014 * Math.min(1, P.speed / 1.4);
    camera.position.set(P.x, EYE + bob, P.z);
    const cp = Math.cos(cam.pitch), sp = Math.sin(cam.pitch);
    _look.set(P.x + Math.cos(cam.yaw) * cp, EYE + bob - sp, P.z + Math.sin(cam.yaw) * cp);
    camera.lookAt(_look);
    return;
  }
  /* 导演镜头：跟**玩家**，不跟地面 —— 跟地面会拍出一半的天花板。
     她是自主的，镜头不该牵着她走。 */
  player.group.visible = true;
  const k = Math.min(1, dt * 14.0);
  const fy = Math.max(0.62, Math.min(1.24, lastComY * 0.62 + 0.46));
  const tx = (cam.watch === 'her') ? her.x : P.x;
  const tz = (cam.watch === 'her') ? her.z : P.z;
  cam.target.x += (tx - cam.target.x) * k;
  cam.target.z += (tz - cam.target.z) * k;
  cam.target.y += (fy - cam.target.y) * k;
  /* 走得快就退远一点，不然步子糊在镜头上 */
  if (!cam.lockD) {
    const wantD = (cam.baseD || 2.85) + Math.min(0.9, Math.abs(lastSpeed) * 0.9) - stickV * 1.1;
    cam.dist += (wantD - cam.dist) * Math.min(1, dt * 1.1);
  }
  /* 摇杆横轴：镜头绕着她转，看正面 / 看侧面一句话的事 */
  if (cam.mode !== 'fp' && Math.abs(stickH) > 0.04) { cam.yaw += stickH * dt * 1.7; cam.follow = false; }
  /* 镜头被墙挡住的时候会"贴"上来，人就顶到脸前 —— 那一刻宁可变角度。
     她走到屋子南边，镜头就绕去北侧；房间窄，构图靠转，不靠拉。 */
  {
    const px = her.x + Math.cos(cam.yaw) * cam.dist;
    const pz = her.z + Math.sin(cam.yaw) * cam.dist;
    const B = ROOM.bounds;
    const out = px < B.x0 + 0.30 || px > B.x1 - 0.30 || pz < B.z0 + 0.30 || pz > B.z1 - 0.30;
    if (out && Math.hypot(her.x, her.z) > 0.35) {
      let d = Math.atan2(-her.z, -her.x) - cam.yaw;
      while (d > Math.PI) d -= Math.PI * 2;
      while (d < -Math.PI) d += Math.PI * 2;
      cam.yaw += d * Math.min(1, dt * 2.2);
    }
  }
  /* 拖拽是有惯性的：松手后 yaw 停在原地，不自己转 */
  applyCamera();
  /* 被家具顶住的时候不要贴脸 —— 抬高机位从她头顶绕过去，看着才不像撞墙。
     没被顶住就慢慢回到平视。 */
  const got = camera.position.distanceTo(cam.target);
  if (got < cam.dist * 0.78) {
    cam.pitch = Math.min(0.42, cam.pitch + dt * 0.9);   /* 别从头顶直落下来看 */
    applyCamera();
  } else if (cam.pitch > 0.11) {
    cam.pitch += (0.10 - cam.pitch) * Math.min(1, dt * 0.5);
    applyCamera();
  }
}

/* ---------------- 菜单 ----------------
   视角 / 镜头 / 画质 / 天色 / 光影 / 头发物理 / 模型 / 动作 / 调试，全在这里。
   每一项都是立刻生效 —— 房间里的东西不该等一个"确定"按钮。 */
const $id = (id) => document.getElementById(id);

/* 头发裙摆三档。数值是"相对模型原值的倍数"（和 vrm.js 的 setSpringTune 对齐）：
   stiff 越大越硬，grav 越大越往下垂，drag 越大越不容易甩。 */
const SPRING_LEVELS = {
  calm: {
    DS_hair: { stiff: 0.60, grav: 2.6, drag: 0.55 },
    DS_ahoge: { stiff: 1.0, grav: 6.0, drag: 0.40 },
    DS_skirt: { stiff: 0.36, grav: 0.50, drag: 0.70 },
    DS_whale: { stiff: 0.50, grav: 7.0, drag: 0.80 },
    DS_ribbon: { stiff: 0.65, grav: 2.0, drag: 0.55 },
  },
  std: SPRING_PROFILE,
  lively: {
    DS_hair: { stiff: 1.20, grav: 0.90, drag: 0.05 },
    DS_ahoge: { stiff: 1.30, grav: 2.0, drag: 0.02 },
    DS_skirt: { stiff: 0.80, grav: 0.30, drag: 0.10 },
    DS_whale: { stiff: 1.10, grav: 2.5, drag: 0.25 },
    DS_ribbon: { stiff: 1.20, grav: 0.80, drag: 0.10 },
  },
};
let SPRING_LEVEL = 'std';
try { SPRING_LEVEL = localStorage.getItem('soma.spring') || 'std'; } catch { }
if (!SPRING_LEVELS[SPRING_LEVEL]) SPRING_LEVEL = 'std';

let shadowOn = true;
try { shadowOn = localStorage.getItem('soma.shadow') !== '0'; } catch { }
let fpsOn = false;

function applySpringLevel(v) {
  SPRING_LEVEL = SPRING_LEVELS[v] ? v : 'std';
  SPRING_TUNE = { groups: SPRING_LEVELS[SPRING_LEVEL] };
  if (vrmAvatar && vrmAvatar.setSpringTune) vrmAvatar.setSpringTune(SPRING_TUNE);
  try { localStorage.setItem('soma.spring', SPRING_LEVEL); } catch { }
}
function applyShadow(on) {
  shadowOn = !!on;
  if (renderer) {
    renderer.shadowMap.enabled = shadowOn;
    /* 关影子不让材质重算的话，下一帧还是旧结果 —— 这里宁可多扫一遍场景 */
    scene.traverse(o => { if (o.material) { const m = o.material; (Array.isArray(m) ? m : [m]).forEach(x => { x.needsUpdate = true; }); } });
  }
  try { localStorage.setItem('soma.shadow', shadowOn ? '1' : '0'); } catch { }
}
applyShadow(shadowOn);

/* 摇杆显示：auto 交给 CSS 断点，on/off 是主人自己定的 */
let STICK_MODE = 'auto';
try { STICK_MODE = localStorage.getItem('soma.stick') || 'auto'; } catch { }
function applyStick(m) {
  STICK_MODE = (m === 'on' || m === 'off') ? m : 'auto';
  const de = document.documentElement;
  de.classList.toggle('stick-force', STICK_MODE === 'on');
  de.classList.toggle('stick-hide', STICK_MODE === 'off');
  try { localStorage.setItem('soma.stick', STICK_MODE); } catch { }
}
applyStick(STICK_MODE);

/* 走多快：只动路线巡航速度，步态参数不碰（快了自然会迈大） */
const WALK_VS = { slow: 0.55, norm: 0.82, fast: 1.15 };
let WALK_V = 0.82;
try { WALK_V = parseFloat(localStorage.getItem('soma.walkv') || '0.82'); } catch { }
if (!isFinite(WALK_V) || WALK_V < 0.3 || WALK_V > 1.6) WALK_V = 0.82;
function applyWalkSpeed(v) {
  WALK_V = isFinite(v) ? v : 0.82;
  try { localStorage.setItem('soma.walkv', String(WALK_V)); } catch { }
  sendSeq(['route speed ' + WALK_V.toFixed(2)]).catch(() => { });   /* 正在走的话立刻生效 */
}

{
  const btn = $id('menuBtn'), box = $id('menu'), closeBtn = $id('menuClose');
  const segView = $id('segView'), segQ = $id('segQ'), segTime = $id('segTime');
  const segShadow = $id('segShadow'), segSpring = $id('segSpring'), segModel = $id('segModel');
  const segShow = $id('segShow'), rngDist = $id('rngDist'), lblDist = $id('lblDist');
  const segStick = $id('segStick'), segSpeed = $id('segSpeed'), segVoice = $id('segVoice');

  const pick = (el, v) => { if (el) for (const b of el.children) b.classList.toggle('on', b.dataset.v === v); };
  const chips = (el, on) => { if (el) for (const b of el.children) b.classList.toggle('on', !!on[b.dataset.v]); };
  const onSeg = (el, fn) => {
    if (!el) return;
    el.addEventListener('click', e => {
      const v = e.target && e.target.dataset && e.target.dataset.v;
      if (!v) return;
      fn(v);
      sync();
    });
  };

  function sync() {
    pick(segView, cam.mode === 'fp' ? 'fp' : (cam.watch === 'her' ? 'her' : 'tp'));
    pick(segQ, QTIER || 'mid');
    pick(segTime, TIME_PIN == null ? 'now' : TIME_PIN < 0.2 ? 'day' : TIME_PIN < 0.85 ? 'dusk' : 'night');
    pick(segShadow, shadowOn ? 'on' : 'off');
    pick(segSpring, SPRING_LEVEL);
    pick(segModel, viewMode === 2 ? 'skel' : viewMode === 1 ? 'low' : 'vrm');
    pick(segStick, STICK_MODE);
    pick(segSpeed, WALK_V < 0.7 ? 'slow' : WALK_V > 1.0 ? 'fast' : 'norm');
    pick(segVoice, VOICE_LEVEL);
    chips(segShow, { diag: diagOn, route: SHOW_ROUTE, fps: fpsOn });
    if (rngDist) rngDist.value = cam.dist.toFixed(1);
    if (lblDist) lblDist.textContent = cam.dist.toFixed(1);
  }
  window.__menuSync = sync;

  if (btn) btn.addEventListener('click', () => { box.classList.add('on'); sync(); });
  if (closeBtn) closeBtn.addEventListener('click', () => box.classList.remove('on'));
  if (box) box.addEventListener('click', e => { if (e.target === box) box.classList.remove('on'); });

  /* 视角：第一人称 / 第三人称（跟玩家）/ 跟着她（看她自己在屋里转） */
  onSeg(segView, v => {
    if (v === 'fp') { cam.mode = 'fp'; cam.watch = null; }
    else { cam.mode = 'director'; cam.watch = (v === 'her') ? 'her' : null; }
    cam.follow = false;
  });
  /* 镜头远近：拖动时锁住自动距离，不然走两步又被它拉回去 */
  if (rngDist) rngDist.addEventListener('input', () => {
    cam.dist = parseFloat(rngDist.value) || 3.2;
    cam.lockD = true;
    if (lblDist) lblDist.textContent = cam.dist.toFixed(1);
  });
  onSeg(segQ, v => {
    QTIER = v;
    try { localStorage.setItem('soma.q', v); } catch { }
    location.reload();   /* 画质牵涉 dpr/shadow，重建最省事 */
  });
  onSeg(segTime, v => {
    TIME_PIN = v === 'day' ? 0.0 : v === 'dusk' ? 0.62 : v === 'night' ? 1.0 : null;
    autoLight();
  });
  onSeg(segShadow, v => applyShadow(v === 'on'));
  onSeg(segSpring, v => applySpringLevel(v));
  onSeg(segModel, v => {
    viewMode = v === 'skel' ? 2 : 0;
    applyView();
  });

  /* 她此刻在干什么：停一下 / 继续转圈 / 说话。
     停 = 关掉路线并且把目标速度压到 0，她会自己收步站住（物理那边有停稳逻辑）。 */
  const bPause = $id('btnPause'), bResume = $id('btnResume'), bTalk = $id('btnTalk');
  if (bPause) bPause.addEventListener('click', () => {
    behaveStop();
    send('route off');
    send('vdes 0');
    showSayLine('……干嘛。', 2.6);
    box.classList.remove('on');
  });
  if (bResume) bResume.addEventListener('click', () => { behaveStart(); box.classList.remove('on'); });
  if (bTalk) bTalk.addEventListener('click', () => {
    box.classList.remove('on');
    const i = $id('chatInput');
    if (i) setTimeout(() => { i.focus(); }, 120);
  });

  /* 调试三连：诊断面板 / 动线 / 帧率 */
  onSeg(segStick, v => applyStick(v));
  onSeg(segVoice, v => { applyVoice(v); pick(segVoice, v); if (v !== 'off') herSpeak('嗯，我在这儿。'); });
  {
    const t = $id('btnVoiceTest');
    if (t) t.addEventListener('click', () => { applyVoice(VOICE_LEVEL === 'off' ? 'soft' : VOICE_LEVEL); herSpeak('喂，你在看什么？饭呢？'); });
  }
  {
    /* 声带设置：四个框 + 存/试/清 */
    const iu = $id('ttsUrl'), ik = $id('ttsKey'), im = $id('ttsModel'), iv = $id('ttsVoice');
    const st = $id('ttsStat');
    const fill = () => {
      if (iu) iu.value = ttsCfg.url || '';
      if (ik) ik.value = ttsCfg.key || '';
      if (im) im.value = ttsCfg.model || '';
      if (iv) iv.value = ttsCfg.voice || '';
      if (st) st.textContent = ttsStatusText();
    };
    const read = () => {
      ttsCfg = {
        url: (iu && iu.value || '').trim(),
        key: (ik && ik.value || '').trim(),
        model: (im && im.value || '').trim(),
        voice: (iv && iv.value || '').trim(),
      };
      try { localStorage.setItem(TTS_KEY, JSON.stringify(ttsCfg)); } catch (e) { }
      applyTts();
    };
    fill();
    applyTts();
    const sv = $id('btnTtsSave'); if (sv) sv.addEventListener('click', () => { read(); if (st) st.textContent = '已保存 · ' + ttsStatusText(); });
    const ts = $id('btnTtsTest');
    if (ts) ts.addEventListener('click', () => {
      read();
      if (st) st.textContent = '正在请求……';
      applyVoice(VOICE_LEVEL === 'off' ? 'soft' : VOICE_LEVEL);
      herSpeak('喂，你在看什么？饭呢？');
      setTimeout(() => { if (st) st.textContent = ttsStatusText(); }, 2600);
    });
    const cl = $id('btnTtsClear');
    if (cl) cl.addEventListener('click', () => {
      ttsCfg = { url: '', key: '', model: '', voice: '' };
      try { localStorage.removeItem(TTS_KEY); } catch (e) { }
      fill(); applyTts();
    });
  }
  onSeg(segSpeed, v => { applyWalkSpeed(WALK_VS[v]); pick(segSpeed, v); });
  if (segShow) segShow.addEventListener('click', e => {
    const v = e.target && e.target.dataset && e.target.dataset.v;
    if (!v) return;
    if (v === 'diag') setDiag(!diagOn);
    else if (v === 'route') { SHOW_ROUTE = !SHOW_ROUTE; if (routeLine) routeLine.visible = SHOW_ROUTE || diagOn; }
    else if (v === 'fps') { fpsOn = !fpsOn; const fe = $id('fps'); if (fe) fe.style.display = fpsOn ? 'block' : 'none'; }
    sync();
  });

  sync();
}

/* ---------------- 说话：她真的会回 ----------------
   输入框 → 同源的 /soma3d/room（DSH 插件开的路由）→ 这条 DSH 会话里的她 →
   一句回话贴上来。走的是真会话，不是另开一个模型。
   她没通也不装死：直接说一句"没信号"。 */
{
  const input = $id('chatInput'), sendBtn = $id('chatSend');
  let busy = false;
  /* 跟她说话：先停下来朝你转过身（站在原地转身，不是走过来） */
  function facePlayer() {
    if (!BEHAVE_ON) return;
    const yaw = Math.atan2(-her.z, -her.x);
    sendSeq(['route off', 'goto ' + her.x.toFixed(2) + ' ' + her.z.toFixed(2) + ' ' + yaw.toFixed(3)]).catch(() => { });
    clearTimeout(behaveTimer);
    behaveTimer = setTimeout(goSpot, 9000);      /* 听完再接着自己的事 */
  }
  /* 插件的路由会被重复 enable 顶掉（路径顺延成 talk-2…），问桥要一次当前路径 */
  let talkPath = null;
  async function talkUrl() {
    if (!talkPath) {
      try {
        const r = await fetch(SIM + '/soma3d/route', { cache: 'no-store' });
        const j = await r.json();
        if (j && j.talk) talkPath = 'http://127.0.0.1:3080' + j.talk;
      } catch (e) { }
      if (!talkPath) talkPath = '/soma3d/talk';
    }
    return talkPath;
  }
  async function submit() {
    if (busy) return;
    const text = (input && input.value || '').trim();
    if (!text) return;
    busy = true;
    if (sendBtn) sendBtn.disabled = true;
    input.value = '';
    showSayLine('（她在想……）', 240);
    send('vdes 0');
    try {
      const r = await fetch(await talkUrl(), {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ text }),
      });
      const raw = await r.text();
      window.__chat = 'st' + r.status + ' len' + raw.length + ' ' + raw.slice(0, 48);
      const j = JSON.parse(raw);
      showSayLine((j && j.say) || '……', 9.0);
    } catch (e) {
      showSayLine('……（没信号。等会儿再说。）', 4.0);
    } finally {
      busy = false;
      if (sendBtn) sendBtn.disabled = false;
    }
  }
  /* 语音输入：用浏览器自带的识别（Chrome/Android 都有），不给就不显示按钮 */
  {
    const mic = $id('chatMic');
    const SR = window.SpeechRecognition || window.webkitSpeechRecognition;
    if (mic && !SR) mic.hidden = true;
    else if (mic) {
      let rec = null, listening = false, finalText = '';
      mic.addEventListener('click', () => {
        if (listening) { try { rec.stop(); } catch (e) { } return; }
        rec = new SR();
        rec.lang = 'zh-CN';
        rec.interimResults = true;
        rec.continuous = false;
        finalText = '';
        rec.onresult = ev => {
          let txt = '';
          for (let i = 0; i < ev.results.length; i++) txt += ev.results[i][0].transcript;
          if (input) input.value = txt.slice(0, 120);
          for (let i = 0; i < ev.results.length; i++) if (ev.results[i].isFinal) finalText = txt;
        };
        rec.onerror = () => { mic.classList.remove('rec'); listening = false; showSayLine('……（没听清，再说一次？）', 3.2); };
        rec.onend = () => {
          mic.classList.remove('rec'); listening = false;
          if (finalText.trim()) submit();
        };
        try {
          ensureVoice();               /* 顺带把 AudioContext 解锁 */
          rec.start(); listening = true; mic.classList.add('rec');
          showSayLine('（听着呢……）', 30);
        } catch (e) { listening = false; mic.classList.remove('rec'); }
      });
    }
  }

  if (sendBtn) sendBtn.addEventListener('click', submit);
  if (input) input.addEventListener('keydown', e => {
    if (e.key === 'Enter') { e.preventDefault(); submit(); }
    e.stopPropagation();      /* 别让 WASD 之类的地面按键吃掉输入 */
  });
  /* 调试口（手机上没键盘、也没法开 devtools）：?ask=你今天干嘛
     —— 走的是和真打字完全同一条路，用来验 /soma3d/chat 和那句浮字。 */
  {
    const u = new URLSearchParams(location.search.replace(/^\?/, ''));
    const q = u.get('ask');
    if (q) setTimeout(() => {
      const sp = document.getElementById('splash');
      if (sp) sp.classList.add('gone');       /* 调试口不需要"点一下进去" */
      if (input) input.value = q;
      submit();
    }, 900);
  }
}

/* ---------------- WebSocket ---------------- */
/* 原来这里连的是 ws://<host>:8123，赌手机把 8123 也转发了。
   赌输了就是「她不动」——页面能开、帧不来。改走同源 SSE，页面能开就一定通。 */
let es = null, state = 'idle', frameN = 0, lastFalls = 0;
function connect() {
  /* 直连 sim 自己的 8125，不经过插件转发 ——
     插件会把最后一条帧缓存住，sim 重启后它连不回来，页面就永远停在同一张。 */
  es = new EventSource(SIM + '/live');
  es.onopen = () => { state = 'live'; send('walk'); };
  es.onerror = () => {
    state = 'down';
    try { es.close(); } catch (e) { }
    setTimeout(connect, 1200);
  };
  es.onmessage = ev => {
    try {
      const m = JSON.parse(ev.data);
      window.__lastFrame = m;          /* 验收钩子用（见 window.__soma.frame） */
      /* 每帧都带一个 brain 状态快照（pending/tag/answered…），
         只有 tag>0 才是真的在向你伸手。别把快照当成求助。 */
      if (m.world) buildWorld(m.world);
      else if (m.type === 'brain-answered') { onBrainSaid(m); }
      else if (m.type === 'say') { showSayLine(m.say, m.secs || 5.0); }
      else if (m.brain && m.brain.tag > 0) onBrain(m.brain);
      else if (m.nodes) {
        frameN++; render(m);
        /* 表情：激素 → 脸（10 Hz 就够，VRM 那边自己平滑） */
        if (vrmAvatar && vrmAvatar.setMood && frameN % 6 === 0) {
          vrmAvatar.setMood(m.horm || {});
        }
        /* 摔了/被撞：惊讶一下；被摸（touch 里出现手/头）：高兴一下 */
        if (vrmAvatar && vrmAvatar.poke) {
          if (m.falls > (lastFalls || 0)) { vrmAvatar.poke('surprised', 0.9); vrmAvatar.poke('angry', 0.35); }
          if (Array.isArray(m.touch) && m.touch.length && frameN % 10 === 0) {
            const t0 = m.touch[0];
            if (t0 && (t0[0] === 'head' || t0[0] === 'palm.R' || t0[0] === 'palm.L')) vrmAvatar.poke('happy', 0.55);
          }
        }
        if (m.falls != null) lastFalls = m.falls;
      }
    } catch (e) {
      if (frameN < 3) console.error('[soma3d] 帧处理失败', e, ev.data.slice(0, 120));
    }
  };
}
connect();
function send(s) { fetch(SIM + '/cmd', { method: 'POST', body: s }).catch(() => { }); }

/* ---------------- 点一下世界 ----------------
   点到她 → 摸摸头（一条 pet 指令 + 一个表情 + 一句）；
   点到东西 → 她走过去，到了再用它（走的是 server 的 /cmd，不新增 C++ 函数）。
   射线是纯数学，没有 GL 也能算，所以这条在无头环境里也能验。 */
const ray = new THREE.Raycaster();
const rayNDC = new THREE.Vector2();
let pendingUse = null;          /* { item, t0, cmdSent } */

function tapWorld(cx, cy) {
  const rect = renderer.domElement.getBoundingClientRect();
  rayNDC.set(((cx - rect.left) / rect.width) * 2 - 1, -((cy - rect.top) / rect.height) * 2 + 1);
  ray.setFromCamera(rayNDC, camera);

  /* 1) 点她：VRM 整棵子树（含裙子/头发），只要打到就算 */
  if (vrmAvatar && vrmAvatar.root && vrmAvatar.root.visible) {
    const hits = ray.intersectObject(vrmAvatar.root, true);
    if (hits.length) {
      send('pet head 0.62');
      if (vrmAvatar.poke) vrmAvatar.poke('happy', 0.75);
      showSayLine('（你摸了摸她的头）', 2.4);
      return;
    }
  }

  /* 2) 点东西 */
  const p = pickItem(THREE, ray, ITEMS.hotspots);
  if (!p) return;
  const it = p.item;
  if (habitBusy()) { showSayLine('（她手上还有事）', 2.0); return; }
  send('goto ' + it.x.toFixed(2) + ' ' + it.z.toFixed(2));
  pendingUse = { item: it, t0: performance.now(), cmdSent: false };
  showSayLine('（去' + it.name + '）', 2.0);
}

/* 她此刻在做什么（面板/点击都要用）——5 s 一拍，断了就静默 */
setInterval(async () => {
  try {
    const r = await fetch(SIM + '/soma3d/behave');
    const j = await r.json();
    window.__somaHabit = j && j.habit ? j.habit : null;
  } catch (e) { }
}, 5000);

function habitBusy() {
  const st = window.__somaHabit;
  return !!(st && st.act);
}

/* 到了没有：拿最后一帧看她离热点多远，近了就执行 */
function tickUse() {
  if (!pendingUse) return;
  const f = window.__lastFrame;
  const it = pendingUse.item;
  const t = (performance.now() - pendingUse.t0) / 1000;
  if (f && f.nodes) {
    const dx = f.nodes[0][0] - it.x, dz = f.nodes[0][2] - it.z;
    const d = Math.hypot(dx, dz);
    if (d < 0.55) {
      send(it.cmd);
      if (it.say) setTimeout(() => showSayLine(it.say, 3.2), 500);
      pendingUse = null;
      return;
    }
  }
  if (t > 22) { pendingUse = null; showSayLine('（走不过去……）', 2.4); }
}
async function sendSeq(lines) { for (const L of lines) { try { await fetch(SIM + '/cmd', { method: 'POST', body: L }); } catch (e) { } } }

/* ---------------- 路线：把房间动线交给物理 ----------------
   以前是前端按弧长"假装"她在走；现在她会自己照着这串航点走（边走边拐）。
   顺序敏感，所以串行发：clear → add × N → on（on 的时候会把她放到第一个航点）。 */
/* 动线是美术给的椭圆，但她的裙摆半径有 45 cm —— 椭圆擦着沙发/床角时
   人就半个身子进家具里。这里把每个采样点推出所有碰撞盒（按 SKIRT_R 外扩），
   再交给物理去跟 —— 路线本身干净了，就不用每帧硬推她（那个会滑步）。 */
const SKIRT_R = 0.46;
function freeOf(x, z, rr) {
  for (const b of ROOM.colliders) {
    if (b.h < 0.25) continue;                        /* 地毯之类的不算 */
    if (x > b.x0 - rr && x < b.x1 + rr && z > b.z0 - rr && z < b.z1 + rr) return false;
  }
  return x > ROOM.bounds.x0 + rr && x < ROOM.bounds.x1 - rr &&
         z > ROOM.bounds.z0 + rr && z < ROOM.bounds.z1 - rr;
}
/* 动线该给身子留多少 —— 不是裙摆半径 0.46：茶几到床之间只有 42 cm，
   2×0.46 = 92 cm 根本过不去，硬塞的结果是路线从床尾穿过去（实测穿 4 cm）。
   路线按"身子"算 0.30，裙摆最坏情况下擦一下家具边；要一寸不擦得挪茶几。 */
const PATH_R = 0.30;
/* 动线形状：房间南半边的一个椭圆（围着茶几走），不穿过床/书桌/椅子所在的带子。
   原来的 ROOM.path 是个大椭圆，北弧直接压进床里 —— 那一段几何上就没法走。 */
const LOOP_C = [-0.05, -0.95], LOOP_R = [1.00, 0.90];
function fitPath(K) {
  const cx = (ROOM.bounds.x0 + ROOM.bounds.x1) / 2;
  const cz = (ROOM.bounds.z0 + ROOM.bounds.z1) / 2;
  const pts = [];
  for (let i = 0; i < K; i++) {
    const a = i / K * Math.PI * 2;
    let x = LOOP_C[0] + Math.cos(a) * LOOP_R[0];
    let z = LOOP_C[1] + Math.sin(a) * LOOP_R[1];
    if (!freeOf(x, z, PATH_R)) {
      let t = 0;                                     /* 往椭圆中心缩，缩到干净为止 */
      while (!freeOf(x, z, PATH_R) && t < 0.9) {
        t += 0.03;
        const L = Math.hypot(x - LOOP_C[0], z - LOOP_C[1]) || 1;
        x += (LOOP_C[0] - x) / L * 0.03;
        z += (LOOP_C[1] - z) / L * 0.03;
      }
      if (!freeOf(x, z, PATH_R)) continue;
    }
    pts.push([x, z]);
  }
  /* 弦也要干净：椭圆是凸的，正常不会擦角，但投影过的点可能挤出一条穿过家具的弦 */
  const clearSeg = (a, b) => {
    for (let t = 0.1; t < 0.99; t += 0.1) {
      if (!freeOf(a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, PATH_R)) return false;
    }
    return true;
  };
  const fin = [];
  for (let i = 0; i < pts.length; i++) {
    const a = pts[i], b = pts[(i + 1) % pts.length];
    fin.push(a);
    if (!clearSeg(a, b)) {                            /* 弦脏：往圆心插一个绕行点 */
      const mx = (a[0] + b[0]) / 2, mz = (a[1] + b[1]) / 2;
      const L = Math.hypot(mx - LOOP_C[0], mz - LOOP_C[1]) || 1;
      const x = mx + (LOOP_C[0] - mx) / L * 0.12, z = mz + (LOOP_C[1] - mz) / L * 0.12;
      if (freeOf(x, z, PATH_R) && clearSeg(a, [x, z]) && clearSeg([x, z], b)) fin.push([x, z]);
    }
  }
  return { pts: fin, open: false };
}
const LOOP_ROUTE = new URLSearchParams(location.search).get('loop') === '1';   /* 想看绕圈就用 ?loop=1 */
const FIT = fitPath(30);
let ROUTE_PTS = FIT.pts;
/* 调试口：让 CDP / 控制台能直接量动线与家具的净距（裙摆半径 0.46） */
/* 无头浏览器验收用的钩子（只读，不影响运行）：
   把 VRM 骨骼、她、最后一帧都暴露出来，别人（或我）可以在页面里直接量数值。 */
window.__soma = {
  route: () => ROUTE_PTS, colliders: ROOM.colliders, skirt: SKIRT_R,
  get her() { return her },
  get avatar() { return vrmAvatar },
  get frame() { return window.__lastFrame || null },
};

async function sendRoute() {
  const lines = ['route clear'];
  for (const [x, z] of ROUTE_PTS) lines.push('route add ' + x.toFixed(3) + ' ' + z.toFixed(3));
  /* keep = 别瞬移回第一个航点：刷新页面时她从原地走过去 */
  lines.push('route speed ' + WALK_V.toFixed(2), 'route on keep');
  await sendSeq(lines);
}

/* ---------------- 她自己的日程 ----------------
   以前进房间就 `route on` 绕那圈椭圆 —— 于是她永远在转圈，看着像个陀螺。
   现在改成"想去哪就去哪"：每次挑一个站得住的地方（离家具 ≥0.3、能转身的），
   走过去（`goto`），站 4~9 秒，顺手朝向屋子中间，然后再挑下一个。
   点是她自己选的，不是一条画死的线。 */
const SPOTS = [
  [0.95, 1.25], [-1.28, 1.20], [1.12, -1.02], [-1.05, -1.90], [0.05, -0.55], [1.25, 0.10],
];
let BEHAVE_ON = true, spotI = -1, behaveTimer = 0;
function pickSpot() {
  let i = spotI;
  for (let k = 0; k < 12 && i === spotI; k++) i = (Math.random() * SPOTS.length) | 0;
  spotI = i;
  return SPOTS[i];
}
async function goSpot() {
  if (!BEHAVE_ON) return;
  /* 没 GL 也照样让她在物理里走（her 的坐标来自 SSE 帧，与渲染无关） */
  const [x, z] = pickSpot();
  const hx = isFinite(her.x) ? her.x : 0, hz = isFinite(her.z) ? her.z : 0;
  const d = Math.hypot(x - hx, z - hz);
  const face = Math.atan2(-z, -x);          /* 站定了朝屋子中间 */
  await sendSeq(['route off', 'goto ' + x.toFixed(2) + ' ' + z.toFixed(2) + ' ' + face.toFixed(3)]);
  const walkMs = Math.min(14000, 1200 + d / Math.max(0.35, WALK_V) * 1000);
  behaveTimer = setTimeout(goSpot, walkMs + 3500 + Math.random() * 5500);
}
function behaveStart() { BEHAVE_ON = true; clearTimeout(behaveTimer); setTimeout(goSpot, 300); }
function behaveStop() { BEHAVE_ON = false; clearTimeout(behaveTimer); }
/* 默认什么都不发：她想不想动，是 C++ 那边"骨髓中枢"按身体参数自己算的，
   这里不再排班、不再画路线（想看那两种调试行为就加 ?loop=1 / ?behave=1）。 */
if (LOOP_ROUTE) setTimeout(sendRoute, 400);
else if (new URLSearchParams(location.search).get('behave') === '1') setTimeout(behaveStart, 500);

/* 把动线画在地上：只在 ?route=1 或开了诊断时出现，平时不打扰 */
function drawRouteLine() {
  if (routeLine) scene.remove(routeLine);
  const pts = ROUTE_PTS.map(([x, z]) => new THREE.Vector3(x, 0.012, z));
  pts.push(pts[0].clone());
  const g = new THREE.BufferGeometry().setFromPoints(pts);
  routeLine = new THREE.Line(g, new THREE.LineBasicMaterial({ color: 0xd08a3a, transparent: true, opacity: 0.55 }));
  routeLine.visible = SHOW_ROUTE || diagOn;
  scene.add(routeLine);
}
let routeLine = null;
drawRouteLine();

/* 主角的位置每 200 ms 推给物理进程一次 —— 她要知道你在哪，
   才谈得上"看见你/听见你/撞到你"。不推的话这些感知全是瞎的。 */
let lastPush = 0;
function pushPlayer(t) {
  if (t - lastPush < 0.2) return;
  lastPush = t;
  send('player ' + P.x.toFixed(3) + ' ' + P.z.toFixed(3) + ' ' + P.speed.toFixed(3) + ' ' + cam.yaw.toFixed(3));
}

/* ---------------- 脊髓挂牌子 ----------------
   她"心里没数"的时候会挂一块牌子等大脑（就是那个 0.6 秒里她站着不动）。
   这里只让这件事显形，不替她做决定——按钮已经拆了，接下来是看她自己怎么办。 */
const thinkEl = document.getElementById('think');
let thinkUntil = 0;                   /* 绝对时刻：别用"每帧减 dt"攒时间，
                                         帧率一变（或者同一帧被画两次）就走样 */
function onBrain(b) {
  if (thinkEl) thinkEl.classList.add('on');
  thinkUntil = Date.now() + 8000;   /* 挂到答案回来为止：模型真要想 2~4 秒 */
  console.log('[brain] tag=' + b.tag + ' why=' + b.why);
}

/* 她心里那句话。不是字幕，是她真说了才浮上来一句，说完自己退场。 */
const sayEl = document.getElementById('say');
/* 那句话浮上来的时长交给 CSS 动画 —— 计时用浏览器自己的钟。
   之前用"每帧减 dt"攒时间，帧率一变（或同一帧被画两次）就走样，
   结果是回话明明来了、字幕却已经消失。 */
/* ---------------- 声音：她的声带 + 屋里的传播 ----------------
   声带是源-滤波器现搭的（见 voice.js），传播按她离镜头多远做衰减/空气吸收/左右定位。
   AudioContext 必须等一次用户手势——开场那一下点正好。 */
try { VOICE_LEVEL = localStorage.getItem('soma.voice') || 'soft'; } catch (e) { }
const VOICE_GAIN = { off: 0, soft: 0.55, loud: 1.0 };
function ensureVoice() {
  if (voice) return voice;
  const AC = window.AudioContext || window.webkitAudioContext;
  if (!AC) return null;
  try { voice = createVoice(new AC()); } catch (e) { voice = null; }
  try { window.__voice = voice; } catch (e) { }
  return voice;
}
function applyVoice(v) {
  VOICE_LEVEL = VOICE_GAIN[v] === undefined ? 'soft' : v;
  try { localStorage.setItem('soma.voice', VOICE_LEVEL); } catch (e) { }
  if (VOICE_LEVEL !== 'off') ensureVoice();
  if (VOICE_LEVEL === 'off' && voice) voice.hush();
}
function herSpeak(text) {
  if (VOICE_LEVEL === 'off') return;
  const v = ensureVoice();
  if (!v) return;
  if (v.resume) v.resume();
  try {
    if (v.setApi) v.setApi(ttsCfg);
    /* 空气传播：位置按她的嘴巴算，镜头是听者 —— 她在远角说话就该又小又闷、偏一边 */
    v.place({
      px: her.x, pz: her.z,
      cx: camera.position.x, cz: camera.position.z,
      facingYaw: her.yaw || 0,
    });
    v.speak(text);
    /* 嘴跟着说话动：TTS 是异步的，这里给一个"她正在说"的窗口（按字数十来算） */
    if (vrmAvatar && vrmAvatar.setSpeaking) {
      const dur = Math.max(0.9, Math.min(6.0, Array.from(String(text)).length * 0.16));
      vrmAvatar.setSpeaking(1);
      clearTimeout(speakOff);
      speakOff = setTimeout(() => vrmAvatar.setSpeaking(0), dur * 1000);
    }
    /* 本地共振峰已停用：没接 TTS 就不出声，明说，别装哑 */
    if (v.hasApi === false && !window.__ttsHinted) {
      window.__ttsHinted = true;
      showSayLine('（声带没接：菜单 → 声带，填地址和密钥）', 4.5);
    }
  } catch (e) { }
}
function showSayLine(text, secs) {
  const t = String(text || '').trim();
  if (!t || !sayEl) return;
  sayEl.textContent = t;
  sayEl.classList.remove('on');
  void sayEl.offsetWidth;                    /* 强制回流，动画才能重放 */
  const hold = Math.max(1.2, (secs || 4.6));
  sayEl.style.animation = '';
  sayEl.style.animationDelay = '0s, ' + hold.toFixed(2) + 's';
  sayEl.classList.add('on');
  if (!/^[…（(]/.test(t) && t !== '……') herSpeak(t);
  sayLog.push([(Date.now() % 100000), t.slice(0, 8), hold]);
  if (sayLog.length > 6) sayLog.shift();
}
const sayLog = [];
let speakOff = 0;
function onBrainSaid(m) {
  if (thinkEl) thinkEl.classList.remove('on');
  thinkUntil = 0;
  showSayLine(m && m.say, 4.6);
}

/* ---------------- 渲染一帧 ---------------- */
const upV = new THREE.Vector3(0, 1, 0);
const tgt = new THREE.Vector3();
function placeBone(mesh, a, b, ra, rb) {
  tgt.subVectors(b, a);
  const L = tgt.length() || 1e-5;
  mesh.position.copy(a).addScaledVector(tgt, 0.5);
  mesh.quaternion.setFromUnitVectors(upV, tgt.divideScalar(L));
  const r = Math.max(0.012, (ra + rb) * 0.5);
  mesh.scale.set(r, L, r);
}

let frameT = 0, lastZ = 0, hits = 0;
const lastCom = [0, 0];
let lastComY = 0.95;
let lastSpeed = 0;
let lastFrameT = 0, whaleDt = 1 / 60;
function render(f) {
  frameT = f.t;
  whaleDt = lastFrameT ? Math.min(0.1, Math.max(0.002, f.t - lastFrameT)) : 1 / 60;
  lastFrameT = f.t;
  const nodes = f.nodes.map(n => new THREE.Vector3(n[0], n[1], n[2]));
  const radii = f.nodes.map(n => n[3]);

  /* C++ 给的是"沿 +x 无限走"的全局坐标。这里三步搬到房间里：
     ① 减质心，搬回以她为原点；② 按她在动线上的朝向转；③ 加上她在屋里的位置。
     不改 y —— 跳和蹲的高度是物理给的，不该被我动。 */
  her.speed = f.speed;
  her.x = f.com[0]; her.z = f.com[1];
  /* C++ 的 yaw 是绕 +y 逆时针；这套坐标里朝向写成 atan2(-z, x)，差一个符号 */
  her.yaw = -(f.yaw || 0);
  placeHer();
  {
    /* 关节坐标从 C++ 出来时已经是世界朝向（物理那边按 yaw 转好了），
       这里只做平移：她若被家具推了一下，整个人跟着偏同样的量。
       再按 her.yaw 转一次会变成 2θ —— 腿会指向身子背面。 */
    const ox = f.com[0], oz = f.com[1];
    const dx = her.x - ox, dz = her.z - oz;
    for (let i = 0; i < nodes.length; i++) {
      nodes[i].x += dx; nodes[i].z += dz;
    }
  }

  for (let i = 0; i < N; i++) {
    joints[i].position.copy(nodes[i]);
    joints[i].scale.setScalar(i === 3 ? radii[i] * 1.0 : Math.max(0.018, radii[i] * 0.92));
    const c = f.nodes[i][4];
    joints[i].material = joints[i].material; // 共用材质
  }
  for (let i = 0; i < 17; i++) {
    const [a, b] = f.bones[i];
    placeBone(bones[i], nodes[a], nodes[b], radii[a], radii[b]);
  }

  if (thinkEl && thinkUntil && Date.now() > thinkUntil) { thinkUntil = 0; thinkEl.classList.remove('on'); }
  /* 模型本身得跟着动线转 —— 节点位置只驱动骨骼，
     整组朝向要单独给，不然她会侧着身子滑过房间。 */
  /* VRM 局部朝向是 -z（vrm.js 的 rest 按 +π/2 建）。
     实测 +π/2 会让她背对着前进方向走（180° 反），所以是 -π/2。 */
  if (vrmAvatar) vrmAvatar.root.rotation.y = her.yaw - Math.PI / 2;
  else whale.group.rotation.y = her.yaw;
  if (vrmAvatar && viewMode === 0) vrmAvatar.update(nodes, whaleDt);
  else {
    /* 低模/骨骼是照着"脸朝 +x"建的（walker 里脚尖也朝 +x），
       而 nodes 是世界坐标。整组要转，节点就得先转回局部，
       否则双重旋转 —— 人会从画面里消失到一个按 2θ 摆开的位置上。 */
    const ca = Math.cos(her.yaw), sa = Math.sin(her.yaw);
    for (let i = 0; i < nodes.length; i++) {
      const dx = nodes[i].x - her.x, dz = nodes[i].z - her.z;
      whLocal[i].x = dx * ca - dz * sa;
      whLocal[i].y = nodes[i].y;
      whLocal[i].z = dx * sa + dz * ca;
    }
    whale.group.position.set(her.x, 0, her.z);
    whale.update(whLocal, radii, whaleDt, f.t);
  }

  /* 眼睛贴在头上，朝前（+x） */
  const head = nodes[3], hr = radii[3];
  for (let i = 0; i < 2; i++) {
    const side = i === 0 ? 1 : -1;
    const ex = head.x + hr * 0.80, ey = head.y + hr * 0.20, ez = head.z + side * hr * 0.30;
    eyes[i].position.set(ex, ey, ez);
    pupils[i].position.set(ex + hr * 0.30, ey, ez + side * hr * 0.04);
  }

  /* 脚掌：用脚踝位置 + 大概朝前 */
  const fa = nodes[13], fb = nodes[17];
  footL.position.set(fa.x + 0.04, 0.022, fa.z);
  footR.position.set(fb.x + 0.04, 0.022, fb.z);
  const cL = f.nodes[13][4], cR = f.nodes[17][4];
  footL.material = cL ? footHot : footCold;
  footR.material = cR ? footHot : footCold;

  /* 标记 */
  comMark.position.set(f.com[0], 0.006, f.com[1]);
  capMark.position.set(f.cap[0], 0.006, f.cap[1]);
  zmpMark.position.set(f.zmp[0], 0.006, f.zmp[1]);
  blob.position.set(f.com[0], 0.003, f.com[1]);

  /* 地面反作用力：从踝关节画出去，长度 ∝ 合力 */
  if (f.grfL && f.grfR) {
    [[f.grfL, 12], [f.grfR, 16]].forEach(([g, ni], i) => {
      const n = f.nodes[ni];
      const v = new THREE.Vector3(g[0], g[1], g[2]);
      const mag = v.length();
      grfArrows[i].position.set(n[0], 0.01, n[2]);
      if (mag > 20) {
        grfArrows[i].visible = true;
        grfArrows[i].setDirection(v.normalize());
        grfArrows[i].setLength(mag * GRF_SCALE, 0.06, 0.04);
      } else grfArrows[i].visible = false;
    });
  }

  /* 她站在哪，记下来给镜头用（render 是 WS 驱动的，镜头不能等它） */
  lastCom[0] = f.com[0]; lastCom[1] = f.com[1];
  lastComY = (f.comY != null) ? f.comY : 0.95;
  lastSpeed = f.speed || 0;
  updateStat(f);

  /* 轨迹 */
  const arr = trailGeo.attributes.position.array;
  if (trailN < TRAIL) {
    arr[trailN * 3] = f.com[0]; arr[trailN * 3 + 1] = 0.01; arr[trailN * 3 + 2] = f.com[1];
    trailN++;
    trailGeo.setDrawRange(0, trailN);
    trailGeo.attributes.position.needsUpdate = true;
  } else {
    arr.copyWithin(0, 3);
    arr[(TRAIL - 1) * 3] = f.com[0]; arr[(TRAIL - 1) * 3 + 1] = 0.01; arr[(TRAIL - 1) * 3 + 2] = f.com[1];
    trailGeo.attributes.position.needsUpdate = true;
  }

  /* HUD */
  vEl.textContent = Math.abs(f.speed).toFixed(2);
  { const d = document.getElementById('dot'); if (d) d.classList.toggle('moving', Math.abs(f.speed) > 0.06); }
  modeEl.textContent = f.mode === 3 ? '摔倒' : f.mode === 2 ? '跑' : f.mode === 1 ? '走' : '站立';
  if (comYEl) comYEl.textContent = (f.comY != null ? f.comY : 0).toFixed(3);
  metabEl.textContent = (f.metab != null ? f.metab : 0).toFixed(0);
  fatEl.textContent = (f.fat != null ? f.fat : 0).toFixed(2);
  fatEl.style.color = (f.fat > 0.5) ? '#ff7b7b' : (f.fat > 0.3 ? '#ffd166' : '#9ad0ff');
  tempEl.textContent = (f.temp != null ? f.temp : 37).toFixed(2);
  tempEl.style.color = (f.temp > 38.2) ? '#ff7b7b' : (f.temp > 37.6 ? '#ffd166' : '#9ad0ff');
  if (brainEl) {
    const b = f.brain || {};
    brainEl.textContent = b.pending ? ('挂起 #' + b.tag) : '空闲';
    brainEl.style.color = b.pending ? '#ffd166' : '#9ad0ff';
  }
  /* 慢变量 + 脊髓：血糖/水/清醒是"今天过得怎么样"，脊髓是"她自己有没有数" */
  if (f.bio) {
    if (kcalEl) {
      kcalEl.textContent = f.bio.kcal.toFixed(2);
      kcalEl.style.color = f.bio.kcal < 0.25 ? '#ff7b7b' : f.bio.kcal < 0.55 ? '#ffd166' : '#9ad0ff';
    }
    if (waterEl) {
      waterEl.textContent = f.bio.water.toFixed(2);
      waterEl.style.color = f.bio.water < 0.25 ? '#ff7b7b' : f.bio.water < 0.55 ? '#ffd166' : '#9ad0ff';
    }
    if (sleepEl) {
      sleepEl.textContent = f.bio.sleep.toFixed(2);
      sleepEl.style.color = f.bio.sleep < 0.25 ? '#ff7b7b' : f.bio.sleep < 0.55 ? '#ffd166' : '#9ad0ff';
    }
  }
  /* 状态后面挂一句"她现在怎么样"：饿/渴/困/累都是真的慢变量，不是装饰 */
  if (modeEl) {
    const base = f.mode === 2 ? '跑' : f.mode === 1 ? '走' : (f.fallen ? '摔倒' : '站立');
    const tags = [];
    if (f.bio) {
      if (f.bio.kcal < 0.30) tags.push('饿');
      if (f.bio.water < 0.30) tags.push('渴');
      if (f.bio.sleep < 0.30) tags.push('困');
    }
    if (f.fat > 0.45) tags.push('累');
    if (f.temp > 38.2) tags.push('热');
    modeEl.textContent = tags.length ? base + ' · ' + tags.join('/') : base;
  }
  if (f.spinal && spinalEl) {
    /* 脊髓这一格现在不只报"有把握"，还报它往前推 0.4 s 错了多少度 —— 这才是预测能力 */
    const deg = f.spinal.rollDeg;
    spinalEl.textContent = (deg != null && deg >= 0)
      ? (Math.round(100 - f.spinal.doubt * 100) + '% · ' + deg.toFixed(1) + '°')
      : (Math.round(100 - f.spinal.doubt * 100) + '%');
    spinalEl.style.color = (deg != null && deg > 8) ? '#c0392b' : '';
  }
  if (false && f.spinal) {
    const d = f.spinal.doubt || 0;
    spinalEl.textContent = (100 - d * 100).toFixed(0) + '%';
    spinalEl.style.color = d > 0.6 ? '#ff7b7b' : d > 0.3 ? '#ffd166' : '#9ad0ff';
  }
  /* 世界每 8 米一个格子，她过格子时整套状态会平移一格（C++ 里 shiftX）。
     画面本身是无缝的，但**镜头不能平滑地追过去** —— 那会看起来像房间在滑。
     所以位置一跳，镜头目标立刻跟上，不做插值。 */
  if (lastComX !== null && Math.abs(f.com[0] - lastComX) > 2.0) {
    cam.target.x = f.com[0];
    /* 根节点被挪走了：弹簧骨还记着"刚才的运动"，不重挂会把头发甩成蒲公英 */
    if (vrmAvatar) vrmAvatar.resetSprings();
    trailN = 0; trailGeo.setDrawRange(0, 0);   /* 旧脚印留在旧地方，别拖一条线过去 */
  }
  /* 真的被扔到很远的地方才闪白（兜底用的，正常跑不到这条） */
  if (lastComX !== null && Math.abs(f.com[0] - lastComX) > 20.0) {
    const flash = document.getElementById('flash');
    if (flash) { flash.style.transition = 'none'; flash.style.opacity = '0.85';
      requestAnimationFrame(() => { flash.style.transition = 'opacity .45s'; flash.style.opacity = '0'; }); }
  }
  lastComX = f.com[0];
  zEl.textContent = f.com[1].toFixed(3);
  /* falls / err 不再显示在 HUD 上（面板只留'感觉'），留着给调试用 */
  if (fallsEl) fallsEl.textContent = f.falls;
  if (errEl) errEl.textContent = f.err.toFixed(4);
  if (comYEl) comYEl.textContent = (f.comY || 0).toFixed(3);

  /* 落地闪一下 */
  if (Math.abs(f.com[1] - lastZ) > 0.02) hits++;
  lastZ = f.com[1];
}

let lastComX = null;
const vEl = document.getElementById('v'), modeEl = document.getElementById('mode');
const zEl = document.getElementById('z');
const fallsEl = document.getElementById('falls'), errEl = document.getElementById('err');

/* ---------------- 控制：一个摇杆，别的没有 ---------------- */
const intentVal = document.getElementById('intentVal');
const stick = document.getElementById('stick');
const knob = document.getElementById('knob');
/* 这个摇杆推的是**你自己**。她那具身体归她自己，你推不动。 */
let stickV = 0, stickH = 0;
function setIntent(v) { stickV = Math.max(-1, Math.min(1, v)); }
function setTurn(v) { stickH = Math.max(-1, Math.min(1, v)); }

/* 摇杆两条轴：上推 = 往前，左右推 = 转。
   第一人称下推的是"你"；导演镜头下它就是绕着她转、拉近拉远。
   以前只读纵向，手指滑出圆就断了，来电或系统手势一来还卡在"一直走"——
   这次用 pointer capture 一并治掉。 */
(function () {
  if (!stick) return;
  let active = false, cx = 0, cy = 0, R = 44;
  const rect = () => stick.getBoundingClientRect();
  const move = e => {
    const p = (e.touches && e.touches[0]) || (e.changedTouches && e.changedTouches[0]) || e;
    if (!p || p.clientX == null) return;
    let dx = p.clientX - cx, dy = p.clientY - cy;
    const d = Math.hypot(dx, dy);
    if (d > R) { dx = dx / d * R; dy = dy / d * R; }
    knob.style.transform = `translate(${dx}px,${dy}px)`;
    setIntent(-dy / R);
    setTurn(dx / R);
  };
  const down = e => {
    active = true;
    const r = rect(); cx = r.left + r.width / 2; cy = r.top + r.height / 2;
    R = r.width * 0.36;
    stick.classList.add('act');
    move(e);
    if (e.cancelable) e.preventDefault();
  };
  const up = () => {
    if (!active) return;
    active = false; knob.style.transform = 'translate(0,0)';
    stick.classList.remove('act');
    setIntent(0); setTurn(0);      /* 松手 = 站住 */
  };
  if (window.PointerEvent) {
    stick.addEventListener('pointerdown', e => {
      try { stick.setPointerCapture(e.pointerId); } catch (err) { }
      down(e);
    });
    stick.addEventListener('pointermove', e => { if (active) move(e); });
    stick.addEventListener('pointerup', up);
    stick.addEventListener('pointercancel', up);
  } else {
    stick.addEventListener('touchstart', down, { passive: false });
    addEventListener('touchmove', e => { if (active) move(e); }, { passive: false });
    addEventListener('touchend', up);
    addEventListener('touchcancel', up);
    stick.addEventListener('mousedown', down);
    addEventListener('mousemove', e => { if (active) move(e); });
    addEventListener('mouseup', up);
  }
})();

/* ---------------- 右上三个小按钮 ----------------
   镜头：第一人称 ↔ 导演机位。默认导演机位——发布给人看的是"她在屋里"，
   不是"你自己的眼睛"。第一人称留给想自己走两步的时候。
   夜：窗外从下午四点慢慢沉到夜里，室内灯接管（room.js 的 setDaylight 插值）。
   数据：那十格生理数据，默认收起——它是给主人看骨架的，不是给她看的。 */
(function () {
  const bc = document.getElementById('bCam');
  if (bc) bc.onclick = () => {
    cam.mode = cam.mode === 'fp' ? 'director' : 'fp';
    applyCamera();
    bc.textContent = cam.mode === 'fp' ? '第一人称' : '镜头';
    bc.classList.toggle('on', cam.mode === 'fp');
  };
  const bn = document.getElementById('bNight');
  if (bn && ROOM.setNight) {
    let night = false;
    bn.onclick = () => {
      night = !night;
      const t0 = performance.now();
      /* 不是开关，是十分钟的天色压缩成 2.6 秒 */
      const step = () => {
        const k = Math.min(1, (performance.now() - t0) / 2600);
        ROOM.setDaylight(night ? k : 1 - k);
        if (WING) WING.setDaylight(night ? k : 1 - k);
        if (HALL) HALL.setDaylight(night ? k : 1 - k);
        if (k < 1) requestAnimationFrame(step);
      };
      step();
      bn.textContent = night ? '白天' : '夜';
      bn.classList.toggle('on', night);
    };
  }
  const bd = document.getElementById('bData');
  const statsEl = document.getElementById('stats');
  if (bd && statsEl) bd.onclick = () => {
    statsEl.classList.toggle('open');
    bd.classList.toggle('on', statsEl.classList.contains('open'));
  };
})();

const toolBtn = (id, cmd) => { const el = document.getElementById(id); if (el) el.onclick = () => send(cmd); };
toolBtn('bPushZ', 'push 0 0 0.9');
toolBtn('bPushX', 'push 0.8 0 0');
toolBtn('bFall', 'fall');
toolBtn('bGetup', 'getup');
toolBtn('bReach', 'motion reach');
toolBtn('bEat', 'motion eat');
toolBtn('bDrink', 'motion drink');
toolBtn('bSit', 'motion sit');
toolBtn('bStand', 'motion standup');

addEventListener('keydown', e => {
  const k = e.key.toLowerCase();
  if (k === 't') { cam.mode = cam.mode === 'fp' ? 'director' : 'fp'; applyCamera(); return; }
  if (k === 'w' || k === 'a' || k === 's' || k === 'd') return;   /* 走路交给 updatePlayer */
  if (k === 'z') send('push 0 0 0.9');
  else if (k === 'x') send('push 0.8 0 0');
  else if (k === 'f') send('fall');
  else if (k === 'g') send('getup');
  else if (k === '1') send('motion reach');
  else if (k === '2') send('motion eat');
  else if (k === '3') send('motion drink');
  else if (k === '4') send('motion sit');
  else if (k === '5') send('motion standup');
  else if (k === '0') send('mstop');
  /* 身体的事：吃、喝、睡。这三个不是动画，是真的在改血糖/水/清醒度。 */
  else if (k === 'e') send('eat 95');
  else if (k === 'r') send('drink 250');
  else if (k === 'p') send('sleep 1800');
});

/* 开场遮罩 */
{
  const sp = document.getElementById('splash');
  /* 点进来只做一件事：把浏览器界面收掉（全屏）。
     以前这里还锁了横屏 —— 那等于把"竖屏也能玩"这件事从设计里删了，
     而且锁横屏会让窗口在竖屏设备上被压成一条（innerHeight 掉到 300 多）。 */
  const go = () => {
    if (sp) sp.classList.add('gone');
    try {
      const el = document.documentElement;
      const fs = el.requestFullscreen ? el.requestFullscreen() : Promise.resolve();
      Promise.resolve(fs).catch(() => { });
    } catch (e) { }
  };
  /* 别在加载到一半就放人进去 —— 那几十秒的低模和掉帧，比多等一会儿难看得多。
     门先关着，VRM 落地 + 帧流起来才点亮"点一下进去"。 */
  const sub = sp && sp.querySelector('.sub');
  const bar = sp && sp.querySelector('.bar > i');
  let opened = false;
  const openDoor = () => {
    if (opened) return;
    opened = true;
    if (sub) { sub.textContent = '点 一 下 进 去'; sub.classList.remove('wait'); sub.classList.add('ready'); }
    if (bar) bar.style.width = '100%';
  };
  const t0 = Date.now();
  const tick = setInterval(() => {
    const pct = Math.round(Math.max(vrmPct || 0, 0) * 100);
    if (!opened) {
      if (sub) sub.textContent = vrmErr ? '房间准备好了' : (vrmOk ? '她在等你' : '正在铺地板 · ' + pct + '%');
      if (bar) bar.style.width = Math.max(6, pct) + '%';
      /* 门只看帧流：VRM 没落地也让她先用低模站住，总比一扇打不开的门好 */
      if (frameN > 2) openDoor();
      else if (Date.now() - t0 > 90000) openDoor();   /* 兜底：卡住了也不能永远关着门 */
      /* 开场那行小字不再写死「下午四点」—— 读本机钟 + 她此刻在干什么。夜里进来就写夜里。 */
      {
        const ln = sp && sp.querySelector('.ln');
        const f = window.__lastFrame;
        if (ln && f) {
          const H = new Date().getHours();
          const when = H < 5 ? '凌晨' : H < 8 ? '清晨' : H < 11 ? '上午' : H < 13 ? '正午'
            : H < 17 ? '下午' : H < 19 ? '傍晚' : H < 23 ? '夜里' : '深夜';
          const doing = f.fallen ? '她躺在地上' : f.mode === 'run' ? '她在小跑' : f.mode === 'walk' ? '她正走着' : '她站着没动';
          const who = (f.brain && f.brain.pending) ? '她好像在想什么' : doing;
          ln.innerHTML = when + '，窗开着<br>' + who;
        }
      }
    } else clearInterval(tick);
  }, 250);
  /* 整块屏幕都是门闩：click 在部分 WebView 里会被长按/拖拽吞掉，
     所以 pointerdown / touchstart 也接一遍（go() 里有 opened 守卫，重复触发无副作用）。 */
  if (sp) {
    const knock = () => { if (opened) go(); };
    sp.onclick = knock;
    sp.addEventListener('pointerdown', knock, { passive: true });
    sp.addEventListener('touchstart', knock, { passive: true });
    addEventListener('keydown', e => { if (e.key === 'Enter' || e.key === ' ') knock(); });
  }
  /* 这里以前有一句 setIntent(0)：它会把 C++ 的 WANDER_OFF 永久置真，
     于是她一直站着不动、或者被一次大脑回答推着一直走，再也停不下来 ——
     "问过一次就聋了"有一半是它干的。她的速度该由她自己决定。 */
}

/* 桌面：滚轮推拉镜头。手机上没这个东西，所以只绑 wheel。 */
addEventListener('wheel', e => {
  if (cam.mode === 'fp') return;
  cam.lockD = true;   /* 你亲手推过镜头，就别再自动拉回去了 */
  cam.dist = Math.max(1.5, Math.min(9, cam.dist + e.deltaY * 0.0022));
  e.preventDefault();
}, { passive: false });

/** 想看她到底把 GPU 压成什么样：地址后面加 #perf=1 */
if (/[#&]perf=1/.test(location.hash)) {
  const fe = document.getElementById('fps');
  if (fe) fe.style.display = 'block';
}

/* 竖屏：不是"看不了"，是构图换一套 —— FOV 拉宽、镜头退远、抬高一点，
   她的脚和呆毛同时进画面。横屏照旧。 */
const FORCE_POR = /(^|[#;])por=1/.test(location.hash);
const PORTRAIT = () => FORCE_POR || innerHeight > innerWidth * 1.05;
function fitLens() {
  const por = PORTRAIT();
  camera.aspect = innerWidth / innerHeight;
  camera.fov = por ? 50 : 40;
  camera.updateProjectionMatrix();
  cam.baseD = por ? 4.95 : 3.15;   /* 竖屏退远一点：她整个人要进画面 */   /* 竖屏是竖长条：她整个人要进画面，就得退远一点 */
  if (!cam.lockD) cam.dist = cam.baseD;
  const hint = document.getElementById('rotate');
  if (hint) { hint.style.opacity = por ? '1' : '0'; }
}
fitLens();
setTimeout(() => { const h = document.getElementById('rotate'); if (h) h.style.opacity = '0'; }, 6000);

addEventListener('resize', () => {
  fitLens();
  if (renderer) renderer.setSize(innerWidth, innerHeight);
});

let lastMs = 0, fpsN = 0, fpsT = 0, fpsShown = 0;
function loop(ms) {
  const dt = lastMs ? Math.min(0.05, (ms - lastMs) / 1000) : 1 / 60;
  lastMs = ms;
  updatePlayer(dt);
  pushPlayer(ms / 1000);
  followCamera(dt);
  ROOM.update(dt);
  stage.update(dt, cam.target);
  if (renderer) renderer.render(scene, camera);
  diagState.dt = whaleDt;
  diagState.win = innerWidth + 'x' + innerHeight
    + (window.visualViewport ? (' vv' + Math.round(visualViewport.width) + 'x' + Math.round(visualViewport.height)) : '')
    + ' kb' + (getComputedStyle(document.documentElement).getPropertyValue('--kb') || '0');
  drawDiag();

  /* 帧率：调画质的时候没这个就是在盲改 */
  fpsN++; fpsT += dt;
  if (fpsT > 0.5) {
    fpsShown = Math.round(fpsN / fpsT); fpsN = 0; fpsT = 0;
    const fe = document.getElementById('fps');
    if (fe) {
      const inf = renderer ? renderer.info.render : { calls: 0, triangles: 0 };
      fe.textContent = fpsShown + ' fps · ' + Q.name
        + ' · ' + frameN + 'f · dc ' + inf.calls + ' · tri ' + (inf.triangles / 1000).toFixed(0) + 'k';
    }
  }
  requestAnimationFrame(loop);
}
requestAnimationFrame(loop);
