/* 材质：把 CC0（Poly Haven）贴图接进场景，替掉一水儿的纯色盒子。
   贴图是 1k JPG，diff / rough / nor 三张；加载是异步的，material 先带颜色，
   图到了再挂上去（needsUpdate），所以首屏不会等贴图。 */
export function createMats(THREE, renderer) {
  const loader = new THREE.TextureLoader()
  const anis = renderer && renderer.capabilities ? renderer.capabilities.getMaxAnisotropy() : 4
  const FILE = {
    floor: { diff: 'tex/floor_wood_diff.jpg', rough: 'tex/floor_wood_rough.jpg', nor: 'tex/floor_wood_nor.jpg' },
    wall: { diff: 'tex/wall_diff.jpg', rough: 'tex/wall_rough.jpg', nor: 'tex/wall_nor.jpg' },
    wood: { diff: 'tex/wood_diff.jpg', rough: 'tex/wood_rough.jpg', nor: 'tex/wood_nor.jpg' },
    fabric: { diff: 'tex/fabric_rough.jpg', rough: 'tex/fabric_rough.jpg', nor: 'tex/fabric_nor.jpg' },
    kitchen: { diff: 'tex/kitchen_diff.jpg', rough: 'tex/kitchen_rough.jpg', nor: 'tex/kitchen_nor.jpg' },
    tile: { diff: 'tex/tile_diff.jpg', rough: 'tex/tile_rough.jpg', nor: 'tex/tile_nor.jpg' },
  }
  const pool = new Map()          /* 'floor:4x4' -> {diff,rough,nor} 已设好 repeat 的贴图 */
  const jobs = []

  function get(set, ru, rv, rot) {
    const key = set + ':' + ru.toFixed(2) + 'x' + rv.toFixed(2) + ':' + rot.toFixed(2)
    let e = pool.get(key)
    if (e) return e
    e = { ready: false }
    pool.set(key, e)
    const f = FILE[set]
    const load = (file, srgb) => new Promise(res => {
      loader.load(file, t => {
        t.wrapS = t.wrapT = THREE.RepeatWrapping
        t.repeat.set(ru, rv)
        if (rot) { t.center.set(0.5, 0.5); t.rotation = rot }
        t.anisotropy = anis
        if (srgb) t.colorSpace = THREE.SRGBColorSpace
        res(t)
      }, undefined, () => res(null))
    })
    jobs.push((async () => {
      const [d, r, n] = await Promise.all([load(f.diff, true), load(f.rough, false), load(f.nor, false)])
      e.diff = d; e.rough = r; e.nor = n; e.ready = true
      for (const m of e.users || []) { apply(m) }
    })())
    e.users = e.users || []
    return e
  }

  function apply(m) {
    const e = m.userData.somaTex
    if (!e || !e.ready) return
    if (e.diff) m.map = e.diff
    if (e.rough) m.roughnessMap = e.rough
    if (e.nor) { m.normalMap = e.nor; m.normalScale = new THREE.Vector2(0.65, 0.65) }
    m.needsUpdate = true
  }

  /* opt: { color,rough,metal,map:'floor'|'wall'|..., rep:[u,v], rot, emissive, emissiveIntensity, transparent, opacity } */
  function mk(opt = {}) {
    const m = new THREE.MeshStandardMaterial({
      color: opt.color !== undefined ? opt.color : 0xffffff,
      roughness: opt.rough !== undefined ? opt.rough : 0.9,
      metalness: opt.metal !== undefined ? opt.metal : 0.0,
    })
    if (opt.emissive !== undefined) { m.emissive = new THREE.Color(opt.emissive); m.emissiveIntensity = opt.emissiveIntensity || 1 }
    if (opt.transparent) { m.transparent = true; m.opacity = opt.opacity !== undefined ? opt.opacity : 1 }
    if (opt.map) {
      const rep = opt.rep || [2, 2]
      const e = get(opt.map, rep[0], rep[1], opt.rot || 0)
      m.userData.somaTex = e
      e.users.push(m)
      apply(m)
    }
    return m
  }

  return { mk, ready: () => Promise.all(jobs) }
}
