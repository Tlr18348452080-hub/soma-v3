/* 真家具（Poly Haven CC0，glTF 1k）。
   原来这些全是盒子拼的 —— 形状对了、材质也贴了，但一眼还是"渲染的几何"。
   这里按"家具 id → 模型 + 位置 + 朝向 + 宽度"摆真模型；摆不上去就退回盒子（盒子还在，只是被藏了，
   加载失败时会把 hidden 的那些重新显出来）。

   物理完全不受影响：碰撞盒仍然来自 room.js / wing.js 里那些 collider(...)，
   这里只动"看得见的部分"。 */
import { GLTFLoader } from 'three/addons/GLTFLoader.js';

const PH = 'models/ph';

/* 客厅：跟 room.js 里的坐标一字不差（那边只藏盒子，不再画） */
const LIVING = [
  { slug: 'old_bed_frame',         file: 'old_bed_frame_1k.gltf',         x: -2.91, z: -2.00, rot: Math.PI / 2, width: 1.95, y: 0 },
  { slug: 'metal_office_desk',     file: 'metal_office_desk_1k.gltf',     x: 1.54,  z: -2.86, rot: 0,           width: 1.62, y: 0 },
  { slug: 'WoodenChair_01',        file: 'WoodenChair_01_1k.gltf',        x: 1.62,  z: -2.02, rot: Math.PI,     width: 0.46, y: 0 },
  { slug: 'wooden_bookshelf_worn', file: 'wooden_bookshelf_worn_1k.gltf', x: 3.72,  z: -0.86, rot: -Math.PI / 2, width: 1.48, y: 0 },
  { slug: 'sofa_02',               file: 'sofa_02_1k.gltf',               x: 2.22,  z: 2.52,  rot: Math.PI,     width: 2.30, y: 0 },
  { slug: 'CoffeeTable_01',        file: 'CoffeeTable_01_1k.gltf',        x: 2.22,  z: 1.05,  rot: 0,           width: 0.94, y: 0 },
  { slug: 'desk_lamp_arm_01',      file: 'desk_lamp_arm_01_1k.gltf',      x: 2.10,  z: -2.95, rot: 0,           width: 0.42, y: 0.76 },
  { slug: 'potted_plant_01',       file: 'potted_plant_01_1k.gltf',       x: -3.55, z: 2.60,  rot: 0,           width: 0.55, y: 0 },
];

/* 北厢：坐标取自 wing.js 的 rooms；这些是"整屋摆位"，用近点隐藏法把旧盒子藏掉 */
const WING = [
  { slug: 'old_bed_frame',          file: 'old_bed_frame_1k.gltf',          x: -2.55, z: 4.55, rot: 0,            width: 1.55, y: 0, hide: 0.85 },
  { slug: 'vintage_wooden_drawer_01', file: 'vintage_wooden_drawer_01_1k.gltf', x: -1.15, z: 4.35, rot: -Math.PI / 2, width: 0.62, y: 0, hide: 0.55 },
  { slug: 'old_bed_frame',          file: 'old_bed_frame_1k.gltf',          x: -2.45, z: 8.10, rot: 0,            width: 1.30, y: 0, hide: 0.85 },
  { slug: 'wooden_bookshelf_worn',  file: 'wooden_bookshelf_worn_1k.gltf',  x: -3.30, z: 7.20, rot: Math.PI / 2,  width: 1.60, y: 0, hide: 0.80 },
  { slug: 'SchoolChair_01',         file: 'SchoolChair_01_1k.gltf',         x: -1.55, z: 7.10, rot: Math.PI / 2,  width: 0.46, y: 0, hide: 0.45 },
  { slug: 'drawer_cabinet',         file: 'drawer_cabinet_1k.gltf',         x: 3.35,  z: 4.40, rot: -Math.PI / 2, width: 0.95, y: 0, hide: 0.70 },
  { slug: 'electric_stove',         file: 'electric_stove_1k.gltf',         x: 3.35,  z: 5.35, rot: -Math.PI / 2, width: 0.60, y: 0, hide: 0.50 },
  { slug: 'vintage_microwave',      file: 'vintage_microwave_1k.gltf',      x: 3.30,  z: 6.00, rot: -Math.PI / 2, width: 0.52, y: 0.90, hide: 0.40 },
  { slug: 'painted_wooden_cabinet', file: 'painted_wooden_cabinet_1k.gltf', x: 3.40,  z: 8.30, rot: -Math.PI / 2, width: 0.90, y: 0, hide: 0.70 },
  { slug: 'ornate_mirror_01',       file: 'ornate_mirror_01_1k.gltf',       x: 3.60,  z: 8.95, rot: -Math.PI / 2, width: 0.65, y: 1.05, hide: 0.35 },
];

const loader = new GLTFLoader();
const cache = new Map();

function loadModel(url) {
  if (cache.has(url)) return cache.get(url);
  const p = new Promise((res, rej) => loader.load(url, (g) => res(g.scene), undefined, rej));
  cache.set(url, p);
  return p;
}

/* 藏掉"这个点附近、这个高度以下"的旧盒子；灯线和线段不动 */
function hideNear(parent, x, z, r, maxH) {
  const hidden = [];
  parent.traverse((o) => {
    if (!o.isMesh || o.isInstancedMesh === false && !o.geometry) return;
    if (o.isLight) return;
    const p = o.getWorldPosition(new o.position.constructor());
    const dx = p.x - x, dz = p.z - z;
    if (dx * dx + dz * dz > r * r) return;
    if (p.y > maxH) return;
    if (!o.visible) return;
    hidden.push(o);
    o.visible = false;
  });
  return hidden;
}

/**
 * 摆一组真模型。
 * @param {object} THREE
 * @param {Array<THREE.Object3D>} parents 场景里可以被隐藏的父节点（客厅组、北厢组）
 * @param {Array} list 摆位表
 * @param {Function} [onEach] 每个模型落地后的回调（调试用）
 */
export async function placeProps(THREE, parents, list, onEach) {
  const jobs = list.map(async (it) => {
    const base = `./${PH}/${it.slug}/${it.file}`;
    try {
      const proto = await loadModel(base);
      const obj = proto.clone(true);
      obj.traverse((m) => {
        if (!m.isMesh) return;
        m.castShadow = true;
        m.receiveShadow = true;
        if (m.material) {
          m.material.envMapIntensity = 0.75;
          if (m.material.map) m.material.map.anisotropy = 4;
        }
      });
      const box = new THREE.Box3().setFromObject(obj);
      const size = box.getSize(new THREE.Vector3());
      const s = it.width / Math.max(size.x, size.z, 0.001);
      obj.scale.setScalar(s);
      const box2 = new THREE.Box3().setFromObject(obj);
      const c = box2.getCenter(new THREE.Vector3());
      obj.position.x -= c.x;
      obj.position.z -= c.z;
      obj.position.y -= box2.min.y;            /* 脚底贴地 */
      const pivot = new THREE.Group();
      pivot.position.set(it.x, it.y || 0, it.z);
      pivot.rotation.y = it.rot || 0;
      pivot.add(obj);
      parents[0].add(pivot);
      /* 先把旧盒子藏掉，模型成功落地才藏 —— 顺序反过来的话加载失败就只剩空气 */
      if (it.hide) {
        for (const p of parents) hideNear(p, it.x, it.z, it.hide, 1.2);
      }
      onEach && onEach(it, pivot);
      return true;
    } catch (e) {
      console.warn('[props] 失败', it.slug, e && e.message);
      return false;
    }
  });
  const r = await Promise.all(jobs);
  return r.filter(Boolean).length;
}

export const PROP_LIST = { living: LIVING, wing: WING };
