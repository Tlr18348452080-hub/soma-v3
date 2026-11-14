import https from 'node:https'
import fs from 'node:fs'

const UA = { 'User-Agent': 'soma-mocap', Accept: 'application/vnd.github+json' }
const get = (u, accept) => new Promise((res, rej) => {
  const r = https.get(u, { headers: { ...UA, ...(accept ? { Accept: accept } : {}) } }, x => {
    const chunks = []
    x.on('data', c => chunks.push(c))
    x.on('end', () => res([x.statusCode, Buffer.concat(chunks)]))
  })
  r.on('error', rej)
  r.setTimeout(60000, () => { r.destroy(); rej(new Error('timeout')) })
})

const [s, b] = await get('https://api.github.com/repos/una-dinosauria/cmu-mocap/git/trees/master?recursive=1')
if (s !== 200) { console.log('tree fail', s); process.exit(1) }
const tree = JSON.parse(b.toString()).tree.filter(e => e.path.endsWith('.bvh'))
fs.writeFileSync('/tmp/mocap/manifest.json', JSON.stringify(tree))
console.log('files available', tree.length)

// 挑：001 走路、002 走路、009 跑/走、035 跳跃、016 走 —— 覆盖不同节奏
const want = ['data/001/01_01.bvh', 'data/002/02_01.bvh', 'data/009/09_01.bvh', 'data/013/13_01.bvh',
  'data/035/35_01.bvh', 'data/035/35_03.bvh', 'data/016/16_15.bvh', 'data/007/07_01.bvh',
  'data/008/08_01.bvh', 'data/091/91_01.bvh', 'data/111/111_01.bvh']
let total = 0
for (const p of want) {
  const e = tree.find(t => t.path === p)
  if (!e) { console.log('skip', p); continue }
  const name = '/tmp/mocap/' + p.split('/').slice(1).join('_')
  if (fs.existsSync(name)) { total += fs.statSync(name).size; continue }
  const [st, body] = await get('https://api.github.com/repos/una-dinosauria/cmu-mocap/git/blobs/' + e.sha, 'application/vnd.github.raw')
  if (st === 200) { fs.writeFileSync(name, body); total += body.length; console.log('got', name, (body.length / 1024).toFixed(0) + 'KB') }
  else console.log('fail', p, st)
}
console.log('total MB', (total / 1048576).toFixed(2))
