/* ================= VRM 身体挂载 =================
   模型自带骨架，我们不重定向骨长，只重定向"方向"：
   每根骨在世界系里从静止方向转到 SOMA 节点给出的目标方向。
   骨长不匹配只会让末端偏一点，不会炸；关节角永远是物理算出来的那个。
*/
import * as THREE from 'three';
import { GLTFLoader } from 'three/addons/GLTFLoader.js';
import { VRMLoaderPlugin, VRMUtils } from '@pixiv/three-vrm';

const UP = new THREE.Vector3(0, 1, 0);

/* SOMA 节点 → [VRM humanoid 骨名, 用来定方向的子节点] */
const NODES = [
  { n: 0,  bone: 'hips',          child: 1 },
  { n: 1,  bone: 'chest',         child: 2 },
  { n: 2,  bone: 'neck',          child: 3 },
  { n: 3,  bone: 'head',          child: -1 },
  { n: 4,  bone: 'leftUpperArm',  child: 5 },
  { n: 5,  bone: 'leftLowerArm',  child: 6 },
  { n: 6,  bone: 'leftHand',      child: -1, from: 5, palm: 1 },
  { n: 7,  bone: 'rightUpperArm', child: 8 },
  { n: 8,  bone: 'rightLowerArm', child: 9 },
  { n: 9,  bone: 'rightHand',     child: -1, from: 8, palm: -1 },
  { n: 10, bone: 'leftUpperLeg',  child: 11 },
  { n: 11, bone: 'leftLowerLeg',  child: 12 },
  { n: 12, bone: 'leftFoot',      child: 13 },
  { n: 13, bone: 'leftToes',      child: -1 },
  { n: 14, bone: 'rightUpperLeg', child: 15 },
  { n: 15, bone: 'rightLowerLeg', child: 16 },
  { n: 16, bone: 'rightFoot',     child: 17 },
  { n: 17, bone: 'rightToes',     child: -1 },
];

/* 父先于子，别打乱 */
const ORDER = ['hips', 'spine', 'chest', 'upperChest', 'neck', 'head',
  'leftShoulder', 'leftUpperArm', 'leftLowerArm', 'leftHand',
  'rightShoulder', 'rightUpperArm', 'rightLowerArm', 'rightHand',
  'leftUpperLeg', 'leftLowerLeg', 'leftFoot', 'leftToes',
  'rightUpperLeg', 'rightLowerLeg', 'rightFoot', 'rightToes'];

/* 模型身上还有几根我们从来没动过的骨：脊柱中段、上胸、左右锁骨。
   18 个节点只能驱动 18 根骨，中间那截脊柱就是"一根硬棍"，转身时上半身
   像块板子 —— 这就是"只有几大块能控制"的由来。这几根用同一套方向对齐
   补上，不新增物理，只是把已有的姿态摊到更细的骨链上。
   from/to 是拿哪两个 SOMA 节点算方向；frac 是只转过去多少（避免和上一节打架）。 */
const EXTRA = [
  { bone: 'spine',        childBone: 'chest',        from: 0, to: 1, yaw: true },
  { bone: 'upperChest',   childBone: 'neck',         from: 1, to: 2, yaw: true, frac: 0.55 },
  { bone: 'leftShoulder', childBone: 'leftUpperArm', from: 1, to: 4 },
  { bone: 'rightShoulder', childBone: 'rightUpperArm', from: 1, to: 7 },
];

const BY_BONE = {};
const BY_NODE = {};
for (const d of NODES) { BY_BONE[d.bone] = d; BY_NODE[d.n] = d; }

const stage = s => { try { window.__vrmStage = s; } catch (e) { } };

export async function loadVRMAvatar(url, onProgress) {
  stage('下载中');
  const loader = new GLTFLoader();
  loader.register(p => new VRMLoaderPlugin(p));
  /* 33 MB 走 GLTFLoader 自己的 FileLoader：
     它内部用 XHR，进度事件最稳定 —— 之前手工 getReader 反而卡在下载阶段。 */
  const gltf = await loader.loadAsync(url, e => {
    if (onProgress && e.total) onProgress(e.loaded / e.total);
    else if (onProgress && e.loaded) onProgress(Math.min(0.99, e.loaded / 33616628));
  });
  stage('解析完成');
  const vrm = gltf.userData.vrm;
  if (!vrm) throw new Error('不是 VRM 文件');

  VRMUtils.rotateVRM0(vrm);          /* VRM 0.x 是 -Z 朝前，转成 1.0 的 +Z */
  /* 影子：以前这里只关剔除，结果模型不投影，地上永远是干净的 —— 看着像贴图。
     VRM 的网格统统打开 castShadow（不收影，免得自己脸上出现自己的阴影）。 */
  vrm.scene.traverse(o => {
    o.frustumCulled = false;
    if (o.isMesh || o.isSkinnedMesh) { o.castShadow = true; o.receiveShadow = false; }
    /* 画质：MToon 的描边是"动漫感"最便宜的一招 —— 一圈 2~3 像素的墨线，
       比加多少灯光都更像手绘。屏幕坐标描边（screenCoordinates）在手机上也稳，
       不会因为离镜头近就变粗成一条黑杠。 */
    const mats = o.material ? (Array.isArray(o.material) ? o.material : [o.material]) : []
    for (const m of mats) {
      if (!m || !m.isMToonMaterial) continue
      if (m.isOutline) continue
      try {
        m.outlineWidthMode = 'screenCoordinates'
        m.outlineWidth = 0.0022          /* 屏幕坐标下 ≈ 1.5~2 px */
        m.outlineColor = new THREE.Color(0x2a2b30)
        m.needsUpdate = true
      } catch (e) { }
      /* 让皮肤/布料对环境的反射弱一点，纸片感就淡了 */
      if ('envMapIntensity' in m) m.envMapIntensity = 0.55
    }
  });

  const root = new THREE.Group();
  root.add(vrm.scene);
  /* VRM 正脸是 +Z，SOMA 是 +X：整组绕 Y 转 90° */
  root.rotation.y = Math.PI / 2;
  root.updateMatrixWorld(true);

  const hb = vrm.humanoid.humanBones;
  const boneOf = name => (hb[name] ? hb[name].node : null);

  /* 尺寸：按 hips 离地高度对齐 SOMA 的 0.895 */
  const hips = boneOf('hips');
  const v = new THREE.Vector3();
  hips.getWorldPosition(v);
  const k = v.y > 1e-3 ? 0.895 / v.y : 1;
  root.scale.setScalar(k);
  root.updateMatrixWorld(true);

  /* 手指：节点里没有手指，所以骨骼永远停在绑定姿态 —— 五指张着、指节笔直，
     是"塑料模特"感最重的一处。给一个统一的轻度握拳。
     注意要写在 humanoid 的 normalized 骨上：每帧 `vrm.humanoid.update()` 会拿
     normalized 覆盖 raw，写在 raw 上会被冲掉。 */
  try {
    const hmd = vrm.humanoid;
    for (const nm of Object.keys(hb)) {
      if (!/(Index|Middle|Ring|Little|Thumb)(Metacarpal|Proximal|Intermediate|Distal)$/.test(nm)) continue;
      const nb = hmd.getNormalizedBoneNode ? hmd.getNormalizedBoneNode(nm) : null;
      if (!nb) continue;
      const curl = /Thumb/.test(nm) ? 0.16 : (/(Proximal)$/.test(nm) ? 0.30 : 0.22);
      nb.rotation.z += curl;
      if (!/Thumb/.test(nm)) nb.rotation.x += 0.04;   /* 手指略向掌心收 */
    }
  } catch (e) { console.warn('[vrm] 手指没摆上', e && e.message); }

  /* 记静止姿态：世界四元数 + 每根骨朝子骨的世界方向 */
  /* 身体朝向要用的三样（髋线向量对齐，见下面 update 里的说明）。
     必须声明在 rest 髋线捕获（:150）之前 —— 放后面会 TDZ：加载器是同步跑的，
     `Cannot access 'restHip' before initialization` 直接把整个 VRM 加载打挂。 */
  const restHip = new THREE.Vector3(1, 0, 0)
  const nowHip = new THREE.Vector3(1, 0, 0)
  const qBody = new THREE.Quaternion()
const latNow = new THREE.Vector3(1, 0, 0)   /* 当前身体左侧（单位向量），手的掌向用它 */
  const qFlip = new THREE.Quaternion()
  const qId = new THREE.Quaternion()

  const rest = {};
  const tmpA = new THREE.Vector3(), tmpB = new THREE.Vector3();
  for (const d of NODES) {
    const b = boneOf(d.bone);
    if (!b) continue;
    const wq = new THREE.Quaternion();
    b.getWorldQuaternion(wq);
    let dir = null;
    if (d.child >= 0) {
      const cd = BY_NODE[d.child];
      const cb = cd && boneOf(cd.bone);
      if (cb) {
        b.getWorldPosition(tmpA); cb.getWorldPosition(tmpB);
        const dd = tmpB.sub(tmpA);
        if (dd.lengthSq() > 1e-10) dir = dd.normalize().clone();
      }
    } else if (d.from != null) {
      /* 手：静止方向取"前臂→手"这根轴（节点里手没有子节点可用） */
      const fb = boneOf(BY_NODE[d.from].bone);
      if (fb) {
        fb.getWorldPosition(tmpA); b.getWorldPosition(tmpB);
        const dd = tmpB.sub(tmpA);
        if (dd.lengthSq() > 1e-10) dir = dd.normalize().clone();
      }
    }
    rest[d.bone] = { bone: b, wq, dir, def: d };
  }
  for (const d of EXTRA) {
    const b = boneOf(d.bone), cb = boneOf(d.childBone);
    if (!b || !cb) continue;
    const wq = new THREE.Quaternion();
    b.getWorldQuaternion(wq);
    b.getWorldPosition(tmpA); cb.getWorldPosition(tmpB);
    const dd = tmpB.sub(tmpA);
    rest[d.bone] = { bone: b, wq, dir: dd.lengthSq() > 1e-10 ? dd.normalize().clone() : null, extra: d, def: d };
  }

  /* rest 髋线：左右大腿骨的世界位置差（水平向量，代表"身子朝哪"） */
  {
    const bl = boneOf('leftUpperLeg'), br = boneOf('rightUpperLeg')
    if (bl && br) {
      bl.getWorldPosition(tmpA); br.getWorldPosition(tmpB)
      const d = tmpA.sub(tmpB)
      /* ★ 取反：restHip 是"模型左 − 模型右"，而 SOMA 的 nodes[10]/[14] 在 C++ 世界里的
         左右和 VRM 骨架的左右符号相反（同一个 measure() 两套骨架拓扑，早证过）。
         不取反的话 qBody 会整整差 180° —— 表现就是"身子朝前、脚朝后"，
         或者反过来：她照走不误，但看着像倒着走。这一条是用眼睛的位置定的：
         eyeDir（头骨→双眼）和脚（踝→趾）在三维世界里的夹角，取反前 180°、取反后 0°。 */
      if (d.lengthSq() > 1e-8) restHip.copy(d).setY(0).normalize()
    }
  }

  /* rest 的"正面"：肩线 × 上方向 = 前（和髋线独立的一对，用来消 180° 歧义） */
  const restFwd = new THREE.Vector3(0, 0, 1)
  {
    const bl = boneOf('leftShoulder') || boneOf('leftUpperArm')
    const br = boneOf('rightShoulder') || boneOf('rightUpperArm')
    if (bl && br) {
      bl.getWorldPosition(tmpA); br.getWorldPosition(tmpB)
      const lat = tmpA.sub(tmpB).setY(0).normalize()
      restFwd.crossVectors(lat, UP).normalize()      /* lat × up = 前 */
    }
  }

  /* hips 在 vrm.scene 本地系的偏移：root.position = pelvis - R*S*hipsLocal */
  const hipsLocal = new THREE.Vector3();
  vrm.scene.updateMatrixWorld(true);
  hips.getWorldPosition(hipsLocal);
  root.worldToLocal(hipsLocal);
  const hipsOff = hipsLocal.clone().multiplyScalar(k).applyAxisAngle(UP, Math.PI / 2);

  /* ---- 弹簧骨（头发/裙子/尾巴）----
     这些不是 humanoid 骨，是模型自带的 VRMSpringBone：
     每帧拿世界坐标算一次"尾巴该甩到哪"。我们对 humanoid 骨的覆盖不会碰它们，
     但如果喂给它的 dt 太大，或者根节点瞬移，它就会把头发甩成一朵蒲公英。 */
  let springJoints = 0;
  const springSample = [];
  const springGroups = {};   /* 'DS_hair' → {n, stiff, grav, drag} 按名字前缀归类，调参时按组来 */
  try {
    const sb = vrm.springBoneManager;
    if (sb && sb.joints) {
      springJoints = sb.joints.size;
      for (const j of sb.joints) {
        if (springSample.length >= 4) break;
        const st = j.settings || {};
        springSample.push({
          bone: (j.bone && j.bone.name) || '?',
          stiff: +(st.stiffness || 0).toFixed(3),
          drag: +(st.dragForce || 0).toFixed(3),
          grav: +(st.gravityPower || 0).toFixed(3),
          gdir: st.gravityDir ? st.gravityDir.toArray().map(n => +n.toFixed(2)) : null,
        });
      }
      for (const j of sb.joints) {
        const nm = (j.bone && j.bone.name) || '?';
        const key = nm.split('_').slice(0, 2).join('_') || nm;
        const st = j.settings || {};
        const g = springGroups[key] || (springGroups[key] = { n: 0, stiff: 0, grav: 0, drag: 0 });
        g.n++; g.stiff += st.stiffness || 0; g.grav += st.gravityPower || 0; g.drag += st.dragForce || 0;
      }
    }
  } catch (e) { springJoints = -1; }

  const qParent = new THREE.Quaternion();
  const qInv = new THREE.Quaternion();
  const qDelta = new THREE.Quaternion();
const qRestB = new THREE.Quaternion(), qNewB = new THREE.Quaternion();
const mBasis = new THREE.Matrix4();
const axY = new THREE.Vector3(), axZ = new THREE.Vector3(), axX = new THREE.Vector3();
const tmpPalm = new THREE.Vector3();
/* 用"骨轴 + 掌心法线"造旋转：只对齐方向时绕骨轴的转角是任意的，手就会反；
   把掌心方向也钉住，手的滚转就没有自由度了。 */
function basisQuat(yDir, zHint, out) {
  axY.copy(yDir).normalize();
  axZ.copy(zHint);
  axZ.addScaledVector(axY, -axZ.dot(axY));
  if (axZ.lengthSq() < 1e-8) axZ.set(0, 0, 1).addScaledVector(axY, -axY.z);
  axZ.normalize();
  axX.crossVectors(axY, axZ).normalize();
  mBasis.makeBasis(axX, axY, axZ);
  return out.setFromRotationMatrix(mBasis);
}
  const dirT = new THREE.Vector3();
  let breathT = 0, breathMix = 0, lastPx = null, lastPz = null;
  let blinkT = 2.0, blinkPhase = 0;

  /* ---------------- 表情 ----------------
     激素/事件 → 表情目标权重，一阶平滑到；眨眼之外只动 VRM 认得的那些名字。
     "只有高兴/生气/难过/惊讶/放松"五种太粗，所以每种都有自己的衰减时间，
     并且可以用 setMood 从外面推（app.js 每帧把 frame.horm 递进来）。 */
  const EMO = { happy: 0, angry: 0, sad: 0, relaxed: 0, surprised: 0 };
  const EMO_T = { happy: 0, angry: 0, sad: 0, relaxed: 0, surprised: 0 };
  let speaking = 0;            /* 0~1，说话时嘴在动 */
  let mouthT = 0;
  let emoOK = null;            /* 模型到底认不认这些名字，加载后探一次 */
  function setMood(m) {
    if (!m) return;
    const dopa = m.dopa != null ? m.dopa : 0.5;
    const cort = m.cort != null ? m.cort : 0.2;
    const oxy = m.oxy != null ? m.oxy : 0.3;
    const ser = m.ser != null ? m.ser : 0.5;
    const adr = m.adr != null ? m.adr : 0.05;
    const pain = m.pain != null ? m.pain : 0;
    const mel = m.mel != null ? m.mel : 0;
    EMO_T.happy = Math.max(0, Math.min(1, (dopa - 0.45) * 1.5 + (oxy - 0.30) * 0.9));
    EMO_T.angry = Math.max(0, Math.min(1, (cort - 0.62) * 1.6 + Math.max(0, pain - 0.30) * 1.5));
    EMO_T.sad = Math.max(0, Math.min(1, (0.42 - ser) * 1.4 + (0.30 - oxy) * 0.6 + mel * 0.30));
    EMO_T.relaxed = Math.max(0, Math.min(1, (ser - 0.45) * 1.2 + mel * 0.8 - pain * 0.8));
    EMO_T.surprised = Math.max(0, Math.min(1, (adr - 0.45) * 1.8));
  }
  const EMOTUV = { happy: 0.85, angry: 0.7, sad: 0.75, relaxed: 0.55, surprised: 0.8 };
  function poke(name, w) { EMO_T[name] = Math.max(EMO_T[name] || 0, w); }
  /* 二次元的那点"活气"：预分配，每帧不 new 对象 */
  const qAlive = new THREE.Quaternion();
  const eAlive = new THREE.Euler(0, 0, 0, 'YXZ');
  let aliveT = 0;

  /* 身体朝向：竖直的骨头（骨盆/胸/颈/头）自己算不出 yaw —— rest 方向和当前方向
     都是正上方，setFromUnitVectors 给的是"最小旋转"，绕身体轴的转角完全没被约束。
     结果就是脊柱永远保持绑定姿态的朝向，只有四肢跟着节点转：脚在走，人不动。

     做法：拿髋线这一个 3D 向量直接对齐（rest 的髋线 → 当前帧的髋线），
     而不是算角度差。两个水平向量之间的最短旋转就是绕竖轴的旋转，方向、
     符号都由 setFromUnitVectors 自己定，不用人去猜 +θ 还是 -θ（猜过两次，都猜反）。 */

  function update(nodes, dt) {
    /* 这里原来第一句是 vrm.update(dt)，也就是「three-vrm 全套」先跑一遍。
       那个顺序是错的，穿模就是它造成的：
         · 弹簧骨（头发/裙子/尾巴，127 根）在世界坐标里积分；
         · 但它跑的时候根节点还停在上一帧的位置，而 humanoid 又是原始绑定姿态
           （我们改的是 raw 骨，要等它跑完才覆盖）；
         · 于是它拿「上一帧的位置 + 绑定姿态」算出一撮头发，我们紧接着把整个身子
           挪到位、把胸/颈/头拧到真是的角度 —— 头发只能硬跟着甩过去。
       现在拆开按真实因果排：
           根节点位置 → humanoid 同步 → 我们的骨对齐 → 弹簧 → 表情
       弹簧还拆成 ≤1/60 的小步（显式积分，dt 一大就飞）。 */
    const p = nodes[0];

    /* 0) 位置：骨盆对齐
       站着的时候加一点呼吸起伏 —— 幅度只有 4 mm，但"完全不动"和"在呼吸"
       在眼睛看来是两个物种。走路时不加，否则会被步态抖成噪点。 */
    breathT += dt;
    if (lastPx === null) { lastPx = p.x; lastPz = p.z; }
    const spd = Math.hypot(p.x - lastPx, p.z - lastPz) / Math.max(dt, 1e-4);
    lastPx = nodes[0].x; lastPz = nodes[0].z;
    const still = spd < 0.12;
    breathMix += ((still ? 1 : 0) - breathMix) * Math.min(1, dt * 2.0);
    const breath = Math.sin(breathT * 1.35) * 0.0045 * breathMix;
    root.position.set(p.x - hipsOff.x, p.y - hipsOff.y + breath, p.z - hipsOff.z);
    root.updateMatrixWorld(true);

    /* 1) 归一化骨 → raw 骨。这一步会把我们上一帧的覆盖冲掉，所以必须在我们前面。 */
    if (vrm.humanoid) vrm.humanoid.update();

    /* 1.5) 身体朝向：rest 髋线 → 当前髋线 的最短旋转 */
    {
      nowHip.set(nodes[10].x - nodes[14].x, 0, nodes[10].z - nodes[14].z)
      if (nowHip.lengthSq() > 1e-8) {
        nowHip.normalize()
        latNow.copy(nowHip)
        qBody.setFromUnitVectors(restHip, nowHip)
      } else {
        qBody.identity()
      }
      /* ★ 消 180° 歧义（这一步是"脚跟人体反了"的正解）：
         髋线只能定出生死线，定不出正反 —— 模型左右手系和 SOMA 节点左右手系
         只要差一个符号，qBody 就整整差半圈：身子朝前、脚朝后（或反过来）。
         脚是**逐骨**对齐节点的（不经过 qBody），所以它是可信的参照物：
         把 rest 的正面用 qBody 转过去，若和双脚平均朝向相反，就给 qBody 补半圈。
         不用人去猜 ±θ，也不用改型号约定。 */
      {
        const fwdNow = tmpA.copy(restFwd).applyQuaternion(qBody)
        nowHip.set(nodes[13].x - nodes[12].x, 0, nodes[13].z - nodes[12].z)
        const fx = nowHip.x + (nodes[17].x - nodes[16].x)
        const fz = nowHip.z + (nodes[17].z - nodes[16].z)
        if (fx * fx + fz * fz > 1e-8) {
          if (fwdNow.x * fx + fwdNow.z * fz < 0) {
            qFlip.setFromAxisAngle(UP, Math.PI)
            qBody.premultiply(qFlip)
          }
        }
      }
    }

    /* 1.8) 手别插进身体。
       手是按 SOMA 的臂长（0.28+0.26）摆到节点上的，模型的臂长不一样；再加上
       PosePrior 本来就把手往身前带一点，抬手/摆手时手会穿进胸口。
       这里只把"已经进到躯干柱子里"的那只手沿半径推出来，不改物理、不改骨骼视图。 */
    {
      const ax = nodes[1].x - nodes[0].x, az = nodes[1].z - nodes[0].z
      const L2 = ax * ax + az * az
      for (const hi of [6, 9]) {
        const hx = nodes[hi].x - nodes[0].x, hz = nodes[hi].z - nodes[0].z
        const tt = L2 > 1e-6 ? Math.max(0, Math.min(1, (hx * ax + hz * az) / L2)) : 0
        let dx = hx - ax * tt, dz = hz - az * tt
        const d = Math.hypot(dx, dz)
        const R = 0.19
        if (d < R) {
          if (d < 1e-4) { dx = 0; dz = 1 } else { dx /= d; dz /= d }
          nodes[hi].x = nodes[0].x + ax * tt + dx * R
          nodes[hi].z = nodes[0].z + az * tt + dz * R
        }
      }
    }

    /* 2) 旋转：自顶向下，每根骨朝它的子节点 */
    for (const name of ORDER) {
      const r = rest[name];
      if (!r || !r.dir) continue;
      let fromN, toN;
      if (r.extra) { fromN = r.extra.from; toN = r.extra.to; }
      else {
        const def = BY_BONE[name];
        if (!def) continue;
        if (def.child >= 0) { fromN = def.n; toN = def.child; }
        else if (def.from != null) { fromN = def.from; toN = def.n; }   /* 手：跟着前臂走 */
        else continue;
      }
      dirT.set(nodes[toN].x - nodes[fromN].x,
               nodes[toN].y - nodes[fromN].y,
               nodes[toN].z - nodes[fromN].z);
      if (dirT.lengthSq() < 1e-8) continue;
      dirT.normalize();

      r.bone.parent.updateWorldMatrix(true, false);
      r.bone.parent.getWorldQuaternion(qParent);
      const def2 = r.def;
      if (def2 && def2.palm) {
        /* 掌心朝着身体中线：左掌朝右（-lat），右掌朝左（+lat） */
        tmpPalm.set(latNow.x * def2.palm, 0, latNow.z * def2.palm);
        basisQuat(dirT, tmpPalm, qNewB);
        /* rest 的基：直接拿静止世界四元数的三条轴 */
        axX.set(1, 0, 0).applyQuaternion(r.wq);
        axY.set(0, 1, 0).applyQuaternion(r.wq);
        axZ.set(0, 0, 1).applyQuaternion(r.wq);
        mBasis.makeBasis(axX, axY, axZ);
        qRestB.setFromRotationMatrix(mBasis);
        qDelta.copy(qNewB).multiply(qRestB.invert());
      } else {
        qDelta.setFromUnitVectors(r.dir, dirT);
      }
      /* 只转过去一部分（上胸用，免得和下一节撞在一起） */
      if (r.extra && r.extra.frac) {
        qId.identity();
        qDelta.slerpQuaternions(qId, qDelta, r.extra.frac);
      }
      /* 竖直骨缺的那一转补上（作用在世界系里，所以是左乘） */
      if (r.extra ? r.extra.yaw : Math.abs(r.dir.y) > 0.85) {
        qDelta.premultiply(qBody);
      }
      qInv.copy(qParent).invert();
      r.bone.quaternion.copy(qInv.multiply(qDelta).multiply(r.wq));
      /* 手是要滚一下的：只对齐方向时，"绕骨轴转多少"是任意的（最小旋转），
         结果手掌朝后、手背超前 —— 看起来就是"两只手反着长"。
         绕自身骨轴（VRM 是 +Y）再转π，掌心才朝向身体。
         顺便：手指的弯曲方向也跟着这一滚才对着掌心，不再是往手背翻。 */
      r.bone.updateWorldMatrix(false, false);
    }

    /* 2.5) 活气：物理算的是"她往哪走"，这一层是"她是个女孩子"。
       站住的时候头会慢慢转、会歪一点（动漫里那种"嗯？"），身体重量左右倒，
       胸不跟着手臂一起硬挺。幅度都很小（不超过 0.11 rad，6°），但整个人
       从"骨架在动"变成"有人在站着"。走路时按速度压低，免得跟步态打架。 */
    aliveT += dt;
    const alive = 1 - Math.min(1, spd / 0.5);
    if (alive > 0.02) {
      const hd = rest['head'];
      if (hd) {
        const lookY = (0.14 * Math.sin(aliveT * 0.41) + 0.045 * Math.sin(aliveT * 1.9)) * alive;
        const tilt = (0.05 * Math.sin(aliveT * 0.27 + 1.1) + 0.018 * Math.sin(aliveT * 0.9)) * alive;
        eAlive.set(0.015 * Math.sin(aliveT * 0.7) * alive, lookY, tilt);
        qAlive.setFromEuler(eAlive);
        hd.bone.quaternion.multiply(qAlive);
      }
      const ch2 = rest['chest'];
      if (ch2) {
        eAlive.set(0, 0.018 * Math.sin(aliveT * 0.6) * alive, 0.026 * Math.sin(aliveT * 0.33 + 0.6) * alive);
        qAlive.setFromEuler(eAlive);
        ch2.bone.quaternion.multiply(qAlive);
      }
    }

    /* 3) 弹簧骨：姿态已经是真的了，现在算才跟得上身子 */
    if (vrm.springBoneManager) {
      const steps = Math.min(4, Math.max(1, Math.ceil(dt / (1 / 60))));
      const h = dt / steps;
      for (let si = 0; si < steps; si++) vrm.springBoneManager.update(h);
    }

    /* 4) 眨眼 / 表情 / 视线 */
    if (vrm.expressionManager) {
      const em = vrm.expressionManager;
      blinkT -= dt;
      if (blinkT < 0) { blinkT = 2.4 + Math.random() * 3.4; blinkPhase = 0.16; }
      if (blinkPhase > 0) blinkPhase = Math.max(0, blinkPhase - dt);
      const b = blinkPhase > 0 ? Math.sin((1 - blinkPhase / 0.16) * Math.PI) : 0;

      /* 表情：目标 → 实际，每种一个时间常数（惊讶最快、难过最慢） */
      const TAU = { surprised: 0.10, happy: 0.28, angry: 0.22, sad: 0.55, relaxed: 0.60 };
      for (const k in EMO) {
        const tau = TAU[k] || 0.3;
        EMO[k] += (EMO_T[k] - EMO[k]) * Math.min(1, dt / tau);
        if (EMO[k] < 0.004) EMO[k] = 0;
      }
      if (emoOK === null) emoOK = {};
      const has = (n) => {
        if (!(n in emoOK)) { try { emoOK[n] = !!em.getExpression(n); } catch { emoOK[n] = false; } }
        return emoOK[n];
      };
      for (const k in EMO) {
        if (EMO[k] > 0.001 && has(k)) em.setValue(k, EMOTUV[k] ? EMO[k] * EMOTUV[k] : EMO[k]);
        else if (has(k)) em.setValue(k, 0);
      }
      /* 说话时的嘴：'aa' 一张一合，跟说话的节奏走；不说话时归零 */
      const aaMax = 0.62;
      if (speaking > 0.01) {
        mouthT += dt * 11.0;
        const m = (Math.sin(mouthT) * 0.5 + 0.5) * aaMax * speaking;
        if (has('aa')) em.setValue('aa', m);
      } else if (has('aa')) em.setValue('aa', 0);
      em.setValue('blink', b);
      em.update();
    }
    if (vrm.lookAt) vrm.lookAt.update(dt);

  }

  console.log('[vrm] 骨骼挂上 ' + Object.keys(rest).length + '/18 根，'
    + 'hips 高 ' + v.y.toFixed(3) + 'm，缩放 ' + k.toFixed(3));

  const springBase = [];
  function collectBase() {
    springBase.length = 0;
    const sb = vrm.springBoneManager;
    if (!sb || !sb.joints) return;
    for (const j of sb.joints) {
      const st = j.settings || {};
      const nm = (j.bone && j.bone.name) || '?';
      const key = nm.split('_').slice(0, 2).join('_') || nm;
      springBase.push({ j, key, stiff: st.stiffness || 0, grav: st.gravityPower || 0, drag: st.dragForce || 0 });
    }
  }
  collectBase();

  return {
    root, vrm, update,
    /* 根节点瞬移之后要重挂一次，不然弹簧会以为"刚才被人甩了 2 米" */
    resetSprings() {
      try { if (vrm.springBoneManager) vrm.springBoneManager.reset(); } catch (e) { }
    },
    /* 0~2 的倍数：1 = 模型原值。头发太飘调小 stiff、加大 drag。 */
    setSpringTune(scale) {
      /* 全局倍数 + 按组覆盖（头发 76 根、裙子 32 根、尾巴 5 根……各要各的脾气） */
      const clampK = (v, lo, hi, dflt) => (v == null ? dflt : Math.max(lo, Math.min(hi, v)));
      const k1 = clampK(scale.stiff, 0, 3, 1);
      const k2 = clampK(scale.grav, 0, 12, 1);
      const k3 = clampK(scale.drag, 0, 1, 0);
      const G = scale.groups || null;
      for (const b of springBase) {
        const st = b.j.settings || {};
        const g = G ? G[b.key] : null;
        const a1 = g ? clampK(g.stiff, 0, 3, k1) : k1;
        const a2 = g ? clampK(g.grav, 0, 12, k2) : k2;
        const a3 = g ? clampK(g.drag, 0, 1, k3) : k3;
        st.stiffness = b.stiff * a1;
        st.gravityPower = b.grav * a2;
        if (a3 > 0) st.dragForce = Math.min(0.95, b.drag + (0.95 - b.drag) * a3);
      }
    },
    /* 各组现在的实际值（调参时看这个，不看模型原值） */
    springNow() {
      const out = {};
      for (const b of springBase) {
        const st = b.j.settings || {};
        const g = out[b.key] || (out[b.key] = { n: 0, stiff: 0, grav: 0, drag: 0 });
        g.n++; g.stiff += st.stiffness || 0; g.grav += st.gravityPower || 0; g.drag += st.dragForce || 0;
      }
      for (const k in out) {
        const g = out[k];
        out[k] = { n: g.n, stiff: +(g.stiff / g.n).toFixed(2),
          grav: +(g.grav / g.n).toFixed(2), drag: +(g.drag / g.n).toFixed(2) };
      }
      return out;
    },
    diag() {
      const groups = {};
      for (const k in springGroups) {
        const g = springGroups[k];
        groups[k] = { n: g.n, stiff: +(g.stiff / g.n).toFixed(2),
          grav: +(g.grav / g.n).toFixed(2), drag: +(g.drag / g.n).toFixed(2) };
      }
      return {
        springs: springJoints,
        groups,
        sample: springSample,
        bones: Object.keys(rest).length,
        meta: (vrm.meta && vrm.meta.metaVersion) || '?',
        scale: +k.toFixed(3),
      };
    },
    setExpression(name, val) {
      if (vrm.expressionManager) vrm.expressionManager.setValue(name, val);
    },
    setMood,
    poke(name, w) { poke(name, w); },
    setSpeaking(v) { speaking = Math.max(0, Math.min(1, v || 0)); },
    mood() {
      return { emo: Object.assign({}, EMO), target: Object.assign({}, EMO_T),
               names: (vrm.expressionManager && vrm.expressionManager.expressions)
                 ? vrm.expressionManager.expressions.map(e => e.expressionName) : [] };
    },
    dispose() { VRMUtils.deepDispose(vrm.scene); },
  };
}
