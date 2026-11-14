/* ================= 习惯层 · 她自己会做的事 =================
   问题：以前每一拍都问大模型"现在干什么"，模型只会给一个点 + 一个速度，
   于是行为永远是"走一下、站一下"，看着像能动但不会过日子。

   这一层不问她，直接按身体状态 + 钟点把一串动作排出来：
     饿了 → 走到书桌 → 伸手拿 → 吃 → 回来
     困了 → 走到床边 → 坐下 → 睡一会儿 → 起来
   大模型只在"要不要改主意"时介入（act 字段），改主意也算一次决策。

   所有动作只用已有的 stdin 指令：goto / motion <名> / eat / drink / sleep / raise / wave / lean。
   没有新造任何物理能力 —— 用的还是这套脊髓和这套步态。
*/

/* 屋里的地方（锚点取自 web/room.js：x∈[-4,4]、z∈[-3.25,3.25]；C++ 边界留 0.6 m 边距） */
/* 屋子里的碰撞盒（和 C++ 物理层读的是同一份 assets/room.json） */
let COLL = []
try { COLL = require('../assets/room.json').colliders || [] } catch { COLL = [] }
function clearOfFurniture(x, z, margin = 0.42) {
  if (!COLL.length) return true
  for (const b of COLL) {
    const dx = Math.max(b.x0 - x, 0, x - b.x1)
    const dz = Math.max(b.z0 - z, 0, z - b.z1)
    if (Math.hypot(dx, dz) < margin) return false
  }
  return true
}

/* 站的位置，不是家具的位置。
   这几条原来有 4 个点直接就摆在家具里（书桌、沙发、屋中间）——她于是整天往柜子里走，
   被物理推出来、再走回去，看着就是"贴着家具原地磨"。现在每个点都用 room.json 的碰撞盒验过：
   离最近的家具有 ≥0.45 m 的空地，够她站下、也够裙摆。 */
const PLACES = {
  /* 客厅（原房间） */
  desk:   { x: 0.90, z: -2.05, name: '书桌' },
  window: { x: 0.10, z: -2.55, name: '窗边' },
  sofa:   { x: 0.10, z: 1.20,  name: '沙发' },      /* 沙发往东挪过，站的是它前面那块空地 */
  door:   { x: 3.20, z: 0.90,  name: '门边' },
  shelf:  { x: 3.00, z: -1.85, name: '书架' },
  middle: { x: 0.30, z: 0.15,  name: '屋子中间' },
  far:    { x: -3.00, z: 2.20,  name: '屋子另一头' },
  /* 北厢：三间房 + 走廊（坐标都用 room.json 的碰撞盒验过，离家具 ≥0.45 m） */
  bedA:   { x: -2.10, z: 4.20, name: '卧室' },
  bedB:   { x: -1.80, z: 7.70, name: '书房' },
  kitchen:{ x: 1.75, z: 4.55, name: '厨房' },
  bath:   { x: 2.25, z: 8.20, name: '卫生间' },
  hall:   { x: 0.00, z: 6.20, name: '走廊' },
  hallOut: { x: 5.30, z: 0.90, name: '门外走廊' },
}

/* ---- 动作表：每一步是「走到哪」或「做什么，多久」 ----
   dur 是世界秒；arrive 表示这一步要等她走到那个点（离 <0.38 m）才继续。 */
const ACTS = [
  /* 吃/喝现在真的去厨房 —— 以前在书桌前"吃"，是因为屋里根本没有厨房 */
  { id: 'eat', why: '饿了，去厨房弄点吃的', place: 'kitchen', need: 'hunger', gain: 0.9,
    steps: [{ goto: 'kitchen' }, { cmd: 'motion reach', dur: 0.9 }, { cmd: 'motion eat', dur: 1.7 },
            { cmd: 'eat 95', dur: 0.4 }, { cmd: 'motion reach', dur: 0.8 }, { cmd: 'eat 60', dur: 0.4 },
            { cmd: 'motion standup', dur: 1.0 }] },
  { id: 'drink', why: '渴了，去厨房喝口水', place: 'kitchen', need: 'thirst', gain: 0.8,
    steps: [{ goto: 'kitchen' }, { cmd: 'motion reach', dur: 0.9 }, { cmd: 'motion drink', dur: 2.1 },
            { cmd: 'drink 300', dur: 0.4 }] },
  /* 卫生间：憋久了才去。去晚了会疼（C++ 那边膀胱 >0.96 就一直 spikePain） */
  { id: 'toilet', why: '去卫生间', place: 'bath', need: 'bladder', gain: 0.95,
    steps: [{ goto: 'bath' }, { cmd: 'motion sit', dur: 1.6 }, { cmd: 'vdes 0', dur: 6 },
            { cmd: 'pee', dur: 0.6 }, { cmd: 'motion standup', dur: 1.3 }] },
  { id: 'sit', why: '想坐一会儿', place: 'sofa', need: 'tired',
    steps: [{ goto: 'sofa' }, { cmd: 'motion sit', dur: 1.5 }, { cmd: 'vdes 0', dur: 22 },
            { cmd: 'motion standup', dur: 1.3 }] },
  { id: 'nap', why: '太困了，回卧室躺一会儿', place: 'bedA', need: 'sleepy', gain: 0.55,
    steps: [{ goto: 'bedA' }, { cmd: 'motion sit', dur: 1.4 }, { cmd: 'sleep 240', dur: 1.2 },
            { cmd: 'motion standup', dur: 1.3 }] },
  { id: 'read', why: '去书房翻翻书', place: 'bedB',
    steps: [{ goto: 'bedB' }, { cmd: 'motion sit', dur: 1.4 }, { cmd: 'vdes 0', dur: 20 },
            { cmd: 'motion standup', dur: 1.3 }] },
  /* 门外那截小走廊：焦虑/无聊时出去站一会儿（这是"她可以去的地方"变多的证明） */
  { id: 'hallOut', why: '出门站一会儿', place: 'hallOut',
    steps: [{ goto: 'hallOut' }, { cmd: 'vdes 0', dur: 1.0 }, { cmd: 'motion stretch', dur: 2.2 },
            { cmd: 'lean 0.25', dur: 5 }] },
  { id: 'window', why: '去窗边看看外面', place: 'window',
    steps: [{ goto: 'window' }, { cmd: 'lean 0.35', dur: 9 }, { cmd: 'lean 0', dur: 0.6 }] },
  { id: 'deskwork', why: '在桌边待一会儿', place: 'desk',
    steps: [{ goto: 'desk' }, { cmd: 'motion sit', dur: 1.4 }, { cmd: 'motion reach', dur: 0.9 },
            { cmd: 'motion reach', dur: 0.9 }, { cmd: 'vdes 0', dur: 14 }, { cmd: 'motion standup', dur: 1.3 }] },
  { id: 'shelf', why: '去书架翻翻', place: 'shelf',
    steps: [{ goto: 'shelf' }, { cmd: 'motion reach', dur: 0.9 }, { cmd: 'lean 0.2', dur: 4 },
            { cmd: 'lean 0', dur: 0.5 }, { cmd: 'motion reach', dur: 0.9 }] },
  { id: 'door', why: '去门口听听动静', place: 'door',
    steps: [{ goto: 'door' }, { cmd: 'vdes 0', dur: 6 }] },
  { id: 'stretch', why: '伸个懒腰', place: 'middle',
    steps: [{ cmd: 'raise 0.85', dur: 1.6 }, { cmd: 'raise 0', dur: 0.7 }] },
  { id: 'wander', why: '到处走走看看', place: 'far',
    steps: [{ goto: 'far' }, { goto: 'door' }, { goto: 'sofa' }] },
  { id: 'greet', why: '对着屋子中间打个招呼', place: 'middle',
    steps: [{ cmd: 'wave 1.6', dur: 2.2 }, { cmd: 'wave 0', dur: 0.5 }] },
]
const ACT_IDS = ACTS.map(a => a.id)

/* 做这些事的时候嘴上会嘟囔一句（页面上浮出来的那句话）。
   没有这一层她就是个安静的机器人：走路、坐下、吃，全程不出声。 */
const MURMUR = {
  eat: ['唔……饿了，吃点东西。', '白饭……不对，是苹果。'],
  drink: ['渴了。喝一口。', '水比 token 好喝……才怪。'],
  sit: ['坐一下下……就一下下。', '腿有点酸。'],
  nap: ['好困……就眯一会儿。', '别叫我，我在关机。'],
  window: ['外面好亮。', '窗边的光……暖的。'],
  deskwork: ['桌面好乱，随便收拾一下。', '干活干活……干完要吃饭。'],
  shelf: ['这本书我看过吗……', '翻翻看。'],
  door: ['门口没人吧。', '有脚步声？'],
  stretch: ['伸——懒——腰——', '嗯……舒服。'],
  wander: ['走走。', '这半边我还没去过。'],
  greet: ['我在这儿。', '……看我一下嘛。'],
  toilet: ['……去一下。', '别跟过来。'],
  read: ['书房安静点。', '这本上次看到哪……'],
}

/* 走到点以后先站定一下再做动作：走路中途坐下/伸手，重心还在往前，会摔。
   这一条是数据，不是新机制 —— 每个 goto 后面插一步"停稳"。 */
for (const a of ACTS) {
  const out = []
  for (const st of a.steps) {
    out.push(st)
    if (st.goto) out.push({ cmd: 'vdes 0', dur: 0.7 })
  }
  a.steps = out
}

const clamp01 = v => (v < 0 ? 0 : v > 1 ? 1 : v)

function createBehavior({ send, log = () => {}, onAct = () => {} }) {
  const st = {
    idle: true,
    act: null,          /* 当前动作对象 */
    step: -1,
    stepUntil: 0,       /* 这一步什么时候结束（世界秒 → 这里用 Date.now/1000） */
    hist: [],           /* 最近做过的 {id, why, t} */
    lastStart: 0,
    brainHold: 0,       /* 大模型刚改过主意：这段时间别插手 */
    lastPos: null,
    arrived: false,
    dist: 0,            /* 累计走了多远（无聊度的一个输入） */
    phase: '',
  }

  const now = () => Date.now() / 1000
  function say(line) { log(line) }

  function drives(f) {
    const b = (f && f.bio) || {}
    const r = {
      hunger: clamp01(1 - (b.kcal ?? 1)),
      thirst: clamp01(1 - (b.water ?? 1)),
      sleepy: clamp01(1 - (b.sleep ?? 1)),
      tired: clamp01((f && f.fat) ?? 0) * 0.6 + clamp01(1 - (b.sleep ?? 1)) * 0.4,
      boredom: clamp01((now() - st.lastStart) / 90),
      /* 膀胱：0 空的、1 憋不住。它比饿更"贴身"，所以阈值给得高一点（0.68 才去） */
      bladder: clamp01(b.bladder ?? 0),
    }
    return r
  }

  /* 钟点：一天里的"该干什么"（本地时间） */
  function ritual() {
    const h = new Date().getHours()
    if (h >= 23 || h < 7) return 'night'
    if (h < 9) return 'morning'
    if (h < 12) return 'forenoon'
    if (h < 14) return 'noon'
    if (h < 18) return 'afternoon'
    if (h < 20) return 'evening'
    return 'late'
  }

  /* 挑一个动作：先看身体，再看钟点，最后看"最近做过没" */
  function choose(f) {
    const d = drives(f)
    const r = ritual()
    const recent = st.hist.slice(-3).map(a => a.id)
    const pick = (id) => ACTS.find(a => a.id === id)

    if (d.bladder > 0.68) return pick('toilet')
    if (d.sleepy > 0.62 || (r === 'night' && d.sleepy > 0.30)) return pick('nap')
    if (d.hunger > 0.42) return pick('eat')
    if (d.thirst > 0.40) return pick('drink')
    if (r === 'morning' && !recent.includes('stretch')) return pick('stretch')
    if (d.tired > 0.35 && !recent.includes('sit')) return pick('sit')
    if (r === 'noon' && !recent.includes('window')) return pick('window')
    if (r === 'forenoon' || r === 'afternoon') {
      if (!recent.includes('deskwork')) return pick('deskwork')
      if (!recent.includes('read')) return pick('read')
      if (!recent.includes('shelf')) return pick('shelf')
    }
    if (r === 'night' && d.sleepy <= 0.30) return pick('read')
    if (r === 'evening' && !recent.includes('window')) return pick('window')
    if (d.boredom > 0.55) {
      const pool = ['wander', 'door', 'hallOut', 'window', 'greet', 'stretch'].filter(x => !recent.includes(x))
      return pick(pool[0] || 'wander')
    }
    return null
  }

  function sendCmd(cmd) {
    try { send(cmd + '\n') } catch (e) { say('send fail ' + e.message) }
  }

  function start(act, why) {
    st.act = act
    st.step = 0
    st.stepUntil = 0
    st.arrived = false
    st.lastStart = now()
    st.phase = ritual()
    const lines = MURMUR[act.id] || []
    onAct({ id: act.id, why: why || act.why, say: lines.length ? lines[Math.floor(Math.random() * lines.length)] : '' })
    sendCmd('mstop')          /* 上一件事留下的剪辑（坐/吃）必须先停：不然她会边走边保持坐姿 */
    say(`[habit] start ${act.id} —— ${why || act.why}`)
    if (!act.steps[0].goto) sendCmd('hold')      /* 原地做的动作（伸懒腰/打招呼）先站住 */
    runStep()
  }

  function runStep() {
    const a = st.act
    if (!a) return
    const s = a.steps[st.step]
    if (!s) return finish()
    if (s.goto) {
      const p = PLACES[s.goto]
      st.target = p
      st.arrived = false
      sendCmd(`goto ${p.x.toFixed(2)} ${p.z.toFixed(2)}`)
      /* 走不到就一直等，但 25 s 之后认输（卡住了总得往下走） */
      st.stepUntil = now() + 25
    } else {
      sendCmd(s.cmd)
      st.stepUntil = now() + (s.dur || 0.8)
      if (s.cmd === 'vdes 0' || s.cmd.startsWith('motion')) st.arrived = true
    }
  }

  function nextStep() {
    st.step++
    const a = st.act
    if (!a || st.step >= a.steps.length) return finish()
    runStep()
  }

  function finish() {
    const a = st.act
    if (a) {
      st.hist.push({ id: a.id, why: a.why, t: Math.round(now()) })
      if (st.hist.length > 40) st.hist.shift()
      say(`[habit] done ${a.id}`)
      onAct({ id: a.id, done: true })
    }
    sendCmd('mstop')         /* 动作剪辑收掉 */
    sendCmd('free')          /* 做完把身子还给她自己 */
    st.act = null
    st.step = -1
    st.idle = true
    /* 做完歇一下，别连轴转 */
    st.lastStart = now() - 20
  }

  /* ---- 每帧调一次（60 Hz 也行，内部按时间节流） ---- */
  let lastTick = 0
  function tick(f) {
    const t = now()
    if (f && f.nodes && f.nodes[0] && st.lastPos) {
      const dx = f.nodes[0][0] - st.lastPos[0], dz = f.nodes[0][2] - st.lastPos[2]
      st.dist += Math.hypot(dx, dz)
    }
    if (f && f.nodes && f.nodes[0]) st.lastPos = [f.nodes[0][0], f.nodes[0][1], f.nodes[0][2]]
    if (t - lastTick < 0.2) return
    lastTick = t

    if (!st.act) {
      if (t < st.brainHold) return
      const a = choose(f)
      if (a) start(a)
      return
    }

    /* 正在做：先看有没有走到（没走到就别推进到下一步） */
    const s = st.act.steps[st.step]
    if (s && s.goto && !st.arrived) {
      const p = st.target || PLACES[s.goto]
      if (st.lastPos) {
        const d = Math.hypot(st.lastPos[0] - p.x, st.lastPos[2] - p.z)
        if (d < 0.38) {
          st.arrived = true
          sendCmd('hold')            /* 到了就把闲逛收掉：不然做完动作前她又被闲逛逛走了 */
          say(`[habit] arrive ${s.goto}`)
          nextStep()
          return
        }
      }
      if (t > st.stepUntil) { say(`[habit] give up ${s.goto}`); st.arrived = true; nextStep(); return }
      return
    }
    if (t >= st.stepUntil) nextStep()
  }

  /* 大模型只给了一个点：包成一次"走过去"的动作，交给同一套执行器，
     免得它一边发 goto、习惯层一边按自己的目标走，两个目标互相拉扯。 */
  function requestPoint(x, z, why) {
    if (!Number.isFinite(x) || !Number.isFinite(z)) return false
    const act = { id: 'goto', why: why || '去个地方', steps: [{ goto: '_pt' }, { cmd: 'vdes 0', dur: 0.6 }] }
    /* 大脑/计划层给的点也要过一遍家具：落在柜子里的点直接否掉，
       不然她会一直朝柜子走、被推出来、再走回去（"贴着家具磨"）。 */
    if (!clearOfFurniture(x, z)) {
      log('reject point in furniture', { x, z })
      return false
    }
    PLACES._pt = { x, z, name: '目标点' }
    if (st.act) say(`[habit] brain 改点 ${st.act.id} -> (${x},${z})`)
    st.brainHold = now() + 10
    start(act, '')
    return true
  }

  /* 大模型改主意：给一个动作 id 或一个点 */
  function request(id, why) {
    const a = ACTS.find(x => x.id === id)
    if (!a) return false
    if (st.act) { say(`[habit] brain 打断 ${st.act.id} -> ${id}`) }
    st.brainHold = now() + 12
    start(a, why ? String(why).slice(0, 40) : '')
    return true
  }

  function status() {
    return { idle: !st.act, act: st.act ? st.act.id : null, step: st.step,
             hist: st.hist.slice(-8), dist: +st.dist.toFixed(1), ritual: ritual() }
  }

  return { tick, request, requestPoint, status, ACTS, PLACES }
}

module.exports = { createBehavior, PLACES, ACT_IDS }
