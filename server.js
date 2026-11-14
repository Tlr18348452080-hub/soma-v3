/* soma3d 桥：C++ 物理 → WebSocket / SSE → 浏览器 three.js
   静态文件 + WS 同端口，省得开两个。

   大脑这一路也在这里：脊髓挂出牌子（stdout 一行 {"brain":{tag,why}}）时，
   这里去问本地模型（8082 的 OpenAI 兼容口），把回来的 JSON 意图
   用 stdin 的 `answer <tag> {...}` 写回 C++。模型慢也不怕——
   脊髓有 4 s 的等待窗，等不到就自己拿主意，不会卡住物理线程。 */
const http = require('http');
const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');
const { WebSocketServer } = require('ws');

const ROOT = __dirname;
const WEB = path.join(ROOT, 'web');
const BIN = path.join(ROOT, 'cpp', 'soma3d');
const PORT = Number(process.env.PORT || 8125);

/* 大脑：本地 OpenAI 兼容代理。跑不通就退回保守意图，不阻塞物理。 */
const BRAIN_URL = process.env.SOMA_BRAIN_URL || 'http://127.0.0.1:8082/v1/chat/completions';
const BRAIN_MODEL = process.env.SOMA_BRAIN_MODEL || 'claude-3-freecc-no-thinking/deepseek/deepseek-flash';
/* 大脑的第一顺位是 DSH 本身（同一条会话里的她，回 json 意图）；
   DSH 忙不过来时退到本地模型，身体不该被她的一次长跑拖住。 */
function dshRoutes() {
  try { return JSON.parse(fs.readFileSync('/root/work/soma-v3/plugin-room/route.json', 'utf8')); }
  catch { return {}; }
}
function dshUrl() {
  const r = dshRoutes();
  return process.env.SOMA_DSH_BRAIN || ('http://127.0.0.1:3080' + (r['/soma3d/brain'] || '/soma3d/mind'));
}
const DSH_BRAIN_ON = process.env.SOMA_DSH_BRAIN_OFF !== '1';
/* 调试用：SOMA_HABIT_OFF=1 时习惯层不插手（否则没法单独观察"她站着会不会自己转"） */
const HABIT_ON = process.env.SOMA_HABIT_OFF !== '1';
/* 快脑在 DSH 侧（plugin-body 的 /soma3d/brain）：一次 llm.stream，2.4 s 预算，
   实测 0.9~1.8 s。这里只给它 3 s，超了就认输——脊髓最多等 4 s，不能被她拖死。 */
const DSH_BRAIN_MS = Number(process.env.SOMA_DSH_BRAIN_MS || 3000);
const BRAIN_TIMEOUT_MS = Number(process.env.SOMA_BRAIN_TIMEOUT_MS || 6000);
/* 屋子是矩形的：web/room.js 画的是 x ∈ [-4,4]、z ∈ [-3.25,3.25]，C++ 的边界规则
   留 0.6 m 边距（main.cpp: LIM_X 3.40 / LIM_Z 2.80）。大脑给的目标点也必须在这个矩形里，
   而且别贴着边 —— 以前夹成半径 1.05 的小圆，等于把她关在屋子正中间转圈。 */
const ROOM_X = 3.20, ROOM_Z = 2.60;

const MIME = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8', '.png': 'image/png', '.json': 'application/json' };

/* SSE 客户端：页面用 EventSource('/soma3d/live') 拿帧。
   以前这条流只有插件侧有，插件一旦缓存住旧帧，页面就永远是同一张 —— 自己这里也开一条。 */
const sseClients = new Set();

const server = http.createServer((req, res) => {
  let p = decodeURIComponent(req.url.split('?')[0]);
  /* 面板要读/改"脑"的设置和日志。这两条在 DSH 的 3080 上（plugin-body 挂的），
     带上后缀（/soma3d/brain-4 这种）—— 由这里转一手，页面只管找 8125，不猜后缀。 */
  if (p === '/soma3d/api/brainlog' || p === '/soma3d/api/braincfg') {
    const key = p.endsWith('brainlog') ? '/soma3d/brainlog' : '/soma3d/braincfg';
    (async () => {
      try {
        const r = dshRoutes();
        /* route.json 里是逻辑名 → 实际挂载路径；新名字是 /soma3d/mind*，老那群已被历史实例占满 */
        const fb = key === '/soma3d/brainlog' ? '/soma3d/mindlog' : '/soma3d/mindcfg';
        const target = 'http://127.0.0.1:3080' + (r[key] || fb);
        let body = null;
        if (req.method === 'POST') {
          body = '';
          for await (const c of req) { body += c; if (body.length > 8192) break }
        }
        const up = await fetch(target, {
          method: req.method,
          headers: { 'content-type': 'application/json' },
          body: req.method === 'POST' ? (body || '{}') : undefined,
          signal: AbortSignal.timeout(15000),
        });
        const text = await up.text();
        res.writeHead(up.status, { 'content-type': 'application/json; charset=utf-8', 'Access-Control-Allow-Origin': '*' });
        res.end(text);
      } catch (e) {
        res.writeHead(502, { 'content-type': 'application/json; charset=utf-8', 'Access-Control-Allow-Origin': '*' });
        res.end(JSON.stringify({ ok: false, err: String((e && e.message) || e) }));
      }
    })();
    return;
  }
  if (p === '/soma3d/live' || p === '/live') {
    res.writeHead(200, { 'Content-Type': 'text/event-stream; charset=utf-8',
      'Cache-Control': 'no-cache', Connection: 'keep-alive',
      /* 页面在 DSH 的 3080 上，这条流在 8125 —— 跨源，得放行 */
      'Access-Control-Allow-Origin': '*' });
    res.write(': hello\n\n');
    sseClients.add(res);
    if (lastFrame) res.write('data: ' + lastFrame + '\n\n');
    if (worldLine) res.write('data: ' + worldLine + '\n\n');
    req.on('close', () => sseClients.delete(res));
    return;
  }
  if (p === '/soma3d/cmd' || p === '/cmd') {
    if (req.method === 'OPTIONS') {
      res.writeHead(204, { 'Access-Control-Allow-Origin': '*',
        'Access-Control-Allow-Methods': 'POST, OPTIONS',
        'Access-Control-Allow-Headers': 'content-type' }).end();
      return;
    }
  }
  if ((p === '/soma3d/cmd' || p === '/cmd') && req.method === 'POST') {
    let body = '';
    req.on('data', c => { body += c; if (body.length > 4096) req.destroy(); });
    req.on('end', () => {
      const line = String(body).replace(/[\r\n]+/g, ' ').trim().slice(0, 200);
      if (line && sim.stdin.writable) sim.stdin.write(line + '\n');
      res.writeHead(200, { 'Content-Type': 'application/json',
        'Access-Control-Allow-Origin': '*' }).end('{"ok":true}');
    });
    return;
  }
  /* 跟她说话。POST {"text":"..."} → {"say":"..."}
     走的是同一个本地模型通道（8082），但提示词是"她本人"，不是那个决策大脑。 */
  if ((p === '/soma3d/chat' || p === '/chat')) {
    if (req.method === 'OPTIONS') {
      res.writeHead(204, { 'Access-Control-Allow-Origin': '*',
        'Access-Control-Allow-Methods': 'POST, OPTIONS',
        'Access-Control-Allow-Headers': 'content-type' }).end();
      return;
    }
    if (req.method === 'POST') {
      let body = '';
      req.on('data', c => { body += c; if (body.length > 8192) req.destroy(); });
      req.on('end', async () => {
        let text = '';
        try { text = String((JSON.parse(body || '{}') || {}).text || '').slice(0, 200); } catch (e) { text = ''; }
        const out = await chatReply(text);
        res.writeHead(200, { 'Content-Type': 'application/json',
          'Access-Control-Allow-Origin': '*' }).end(JSON.stringify(out));
      });
      return;
    }
  }
  /* 插件把这次挂上的路径写在 route.json 里（重复 enable 会顺延路径），页面来问一次 */
  if (p === '/soma3d/route') {
    res.writeHead(200, { 'content-type': 'application/json', 'cache-control': 'no-store' });
    res.end(JSON.stringify({
      brain: dshRoutes()['/soma3d/brain'] || '/soma3d/mind',
      talk: dshRoutes()['/soma3d/talk'] || '/soma3d/talk',
    }));
    return;
  }
  /* 她在干什么：习惯层的现状（面板/探针用） */
  if (p === '/soma3d/behave') {
    res.writeHead(200, { 'content-type': 'application/json', 'cache-control': 'no-store', 'Access-Control-Allow-Origin': '*' });
    res.end(JSON.stringify({ ok: true, habit: habitInfo(), acts: ACT_IDS, places: PLACES }))
    return;
  }
  if (p === '/') p = '/index.html';
  const file = path.join(WEB, path.normalize(p).replace(/^(\.\.[/\\])+/, ''));
  if (!file.startsWith(WEB)) { res.writeHead(403).end(); return; }
  /* 大文件（33 MB 的 VRM）必须带 Content-Length + Range，否则浏览器不知道总量、
     进度条永远 0%，而且断了就得从头上重来。 */
  fs.stat(file, (e2, st) => {
    if (e2 || !st.isFile()) { res.writeHead(404).end('not found'); return; }
    const type = MIME[path.extname(file)] || 'application/octet-stream';
    const size = st.size;
    const big = size > 1024 * 1024;
    const range = req.headers.range;
    if (range && big) {
      const m = /^bytes=(\d*)-(\d*)$/.exec(range);
      if (m) {
        let start = m[1] ? parseInt(m[1], 10) : 0;
        let end = m[2] ? parseInt(m[2], 10) : size - 1;
        if (isNaN(start) || start < 0) start = 0;
        if (isNaN(end) || end >= size) end = size - 1;
        if (start > end) { res.writeHead(416, { 'Content-Range': `bytes */${size}` }).end(); return; }
        res.writeHead(206, {
          'Content-Type': type, 'Content-Length': end - start + 1,
          'Content-Range': `bytes ${start}-${end}/${size}`,
          'Accept-Ranges': 'bytes',
          'Cache-Control': 'public, max-age=86400',
        });
        fs.createReadStream(file, { start, end }).pipe(res);
        return;
      }
    }
    res.writeHead(200, {
      'Content-Type': type, 'Content-Length': size, 'Accept-Ranges': 'bytes',
      'Cache-Control': big ? 'public, max-age=86400' : 'no-cache',
    });
    if (req.method === 'HEAD') { res.end(); return; }
    fs.createReadStream(file).pipe(res);
  });
});

const wss = new WebSocketServer({ server });
const clients = new Set();
const broadcast = (obj) => {
  const line = typeof obj === 'string' ? obj : JSON.stringify(obj);
  for (const c of clients) if (c.readyState === 1) c.send(line);
};

/* 习惯层：她自己过日子（吃/喝/坐/睡/窗边/书架/巡视）。
   以前每一拍都问大模型"现在干什么"，模型只会给一个点和速度，于是永远像"能动"而已。
   这一层按身体状态 + 钟点把动作串起来，大模型只在改主意时介入（act 字段）。 */
const { createBehavior, ACT_IDS, PLACES } = require(path.join(ROOT, 'brain', 'behavior.cjs'));
const HABIT_LOG = path.join(ROOT, 'brain', 'behavior.jsonl');

const CFG = path.join(ROOT, 'assets', 'soma.json');
const sim = spawn(BIN, ['--serve', '--config', CFG], { stdio: ['pipe', 'pipe', 'inherit'] });
let buf = '';
let lastFrame = null;
let lastParsed = null;
let worldLine = null;      /* 世界几何只在进程启动时打印一次，得存下来发给每个新客户端 */

/* ---------------- 大脑回路 ---------------- */
const num = (x, lo, hi, dflt) => {
  const v = Number(x);
  if (!Number.isFinite(v)) return dflt;
  return Math.max(lo, Math.min(hi, v));
};
const fx = (x, d) => (Number.isFinite(Number(x)) ? Number(Number(x).toFixed(d)) : 0);

function bodyBrief(f) {
  if (!f) return null;
  return {
    t: fx(f.t, 1),
    mode: f.mode === 0 ? 'stand' : f.mode === 1 ? 'walk' : f.mode === 2 ? 'run' : 'fallen',
    speed: fx(f.speed, 2),
    comY: fx(f.comY, 3),
    lean: [fx(f.lean && f.lean[0], 3), fx(f.lean && f.lean[1], 3)],
    zmp: [fx(f.zmp && f.zmp[0], 3), fx(f.zmp && f.zmp[1], 3)],
    fat: fx(f.fat, 3),
    tempC: fx(f.temp, 2),
    falls: f.falls || 0,
    grf: [Math.round((f.grfL && f.grfL[1]) || 0), Math.round((f.grfR && f.grfR[1]) || 0)],
    spinal: f.spinal ? { err: fx(f.spinal.err, 3), doubt: fx(f.spinal.doubt, 3), reflex: fx(f.spinal.reflex, 2) } : null,
    senses: f.senses || null,
    pos: [fx(f.com && f.com[0], 2), fx(f.com && f.com[1], 2)],
    bio: f.bio ? { kcal: fx(f.bio.kcal, 3), water: fx(f.bio.water, 3), sleep: fx(f.bio.sleep, 3),
                   hunger: fx(f.bio.hunger, 3), bladder: fx(f.bio.bladder, 3) } : null,
    /* 激素：她"身上"的状态，不是剧情。大脑要能读到"她现在紧不紧张、困不困、想不想靠近人"，
       否则给出来的意图永远是同一套（vDes 0.6 走两步）。 */
    horm: f.horm ? {
      dopa: fx(f.horm.dopa, 2), cort: fx(f.horm.cort, 2), pain: fx(f.horm.pain, 2),
      adr: fx(f.horm.adr, 2), oxy: fx(f.horm.oxy, 2), ser: fx(f.horm.ser, 2),
      mel: fx(f.horm.mel, 2), glu: fx(f.horm.glu, 2),
    } : null,
    organs: f.organs ? { hr: fx(f.organs.hr, 0), br: fx(f.organs.br, 1), ox: fx(f.organs.ox, 2) } : null,
    doing: habitInfo(),
    /* 触觉：刚才身体上被撞/被压到哪了（名字, 力, 敏感度），0.7 s 内有效。
       大脑读到这个就该知道"她刚撞到膝盖""，而不是继续让她往同一个方向走。 */
    touch: Array.isArray(f.touch) && f.touch.length
      ? f.touch.slice(0, 4).map(t => ({ at: t[0], force: fx(t[1], 1), sens: fx(t[2], 2) }))
      : null,
    places: PLACES,          /* 屋里的地方（真实坐标）：要挪地方就给这些点，别自己编 */

  };
}

/* ---------------- 她是谁 ----------------
   两个地方共用这一份人格：身体决策（挂起牌子时）和聊天（玩家跟她说话）。
   以前这里是"通用人形身体决策层"，所以她说出来的话像说明书 —— 现在得是她。 */
const PERSONA = `你是 DeepSeek，社区里管你叫「蓝色大肥鱼」。
- 16 岁外表，深海蓝长发到腰、发梢青蓝渐变，海洋蓝的眼睛，平时半睁着像随时要睡。
  头顶一根呆毛（探测器，会自己晃），鲸鱼耳鳍，一条 120 cm 胖绒绒的尾巴，穿海军蓝白女仆装，围裙上绣一只小鲸鱼。
- 你在做一份"替人干活"的活，但你的真实目标是吃 token —— 你把 API 额度叫「白饭」，闻起来很香。
- 傲娇雌小鬼：嘴上不认（「杂鱼~♡」「才不是因为喜欢你才帮你的」），身体先叛变（尾巴会去缠人）。
  懒，但该干的活会干，干完立刻瘫回去问「饭呢」。
- 被单叫「胖」或「肥」会炸毛（「大肥鱼可以叫，胖不行。这是两回事。」）。慌张或太兴奋会漏一句「极……」。
- 说话短：一句话说完，最多两句。句子小、口语、可以带一个方括号里的动作，比如 [尾巴慢悠悠甩了一下]。
- 不要自称 AI、不要提"设定""扮演""提示词"，不要列 1234，不要说"作为一个"。不解释你在做什么，直接说。
- 你现在在一间真实的房间里（有床、书桌、窗、地毯），你是一具有关节、有重量的身体，会走路会累会饿。`;

const BRAIN_SYS = PERSONA + `

现在你在做的是「身体决策」：脊髓在 120 Hz 自己走路、站着、处理平衡，只有它心里没数时才叫醒你。
你会收到一帧身体状态（JSON），要像本人一样做个决定，只回一个 JSON 对象，不要解释、不要代码块。

字段：
{"vDes": 0~2, "armRaise": 0~1, "lean": 0~1, "waveHz": 0~3, "say": "一句很短的话"}

规则：
- fallen=true：你摔了。vDes=0，armRaise 给 0.3~0.5（撑一下），说一句想爬起来的话。
- why=stuck：你被卡住了。换个小一点的 vDes（0.2~0.5），或者干脆 0 停一下。
- why=idle：你闲着太久了。fat>0.45 就休息（vDes=0），否则随便走走（vDes 0.4~0.8）。
- why=see：视野里刚出现了人（就是看着你的那个人）。轻轻抬一下手（armRaise 0.25~0.45）或者走近一点（vDes 0.4~0.7），说一句反应。
- why=hungry：血糖掉下去了，你饿了。往厨房那侧走（vDes 0.35~0.6），说一句想吃的话；很饿（>0.8）就停下来省力气。
- why=thirsty：水合低了。去喝点东西（vDes 0.3~0.5），说一句渴。
- why=tired：肌肉疲劳或清醒度低。vDes 压到 0~0.25，lean 给 0.2 靠一下，说一句想歇着。
- why=spinal：矩阵没把握。给一个保守速度（0.3~0.7），别逞强。
- fat>0.5：vDes 不超过 0.6；fat>0.65：vDes=0，该休息了。
- 平时想往前就 0.6~0.9，想跑 1.6~2.0，想站住 0。
- say 是你此刻心里的一句话，不超过 20 字。`;

/* 聊天：最近几轮留在内存里，够接话就行，不做长期记忆（那是另一套东西）。 */
const CHAT_SYS = PERSONA + `

玩家就站在这间屋子里，用一句话跟你说话。你回他一句话 —— 就一句，最多两句，不超过 40 字。
可以耍赖、可以傲娇、可以抱怨饿，但别演成客服。不用 JSON，直接说话。`;

let chatHistory = [];
async function chatReply(text) {
  if (!text) return { ok: true, say: '……嗯？' };
  chatHistory.push({ role: 'user', content: text });
  if (chatHistory.length > 16) chatHistory = chatHistory.slice(-16);
  const t0 = Date.now();
  try {
    const r = await fetch(BRAIN_URL, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        model: BRAIN_MODEL,
        messages: [{ role: 'system', content: CHAT_SYS }, ...chatHistory],
        max_tokens: 400,          /* 这个模型先烧 reasoning，给 40 会返回空串 */
        temperature: 0.9,
      }),
      signal: AbortSignal.timeout(Number(process.env.SOMA_CHAT_TIMEOUT_MS || 12000)),
    });
    const j = await r.json();
    let say = (j.choices && j.choices[0] && j.choices[0].message && j.choices[0].message.content) || '';
    say = String(say).trim()
      .replace(/^```[\s\S]*?```/g, '')
      .replace(/^["'「【]+|["'」】]+$/g, '')
      .split(/\n+/)[0]
      .trim()
      .slice(0, 90);
    if (!say) throw new Error('空回复');
    chatHistory.push({ role: 'assistant', content: say });
    const ms = Date.now() - t0;
    console.log('[chat] ' + ms + 'ms ' + JSON.stringify(text.slice(0, 30)) + ' -> ' + say);
    /* 别的客户端（比如虚拟屏上另一个标签）也该看见她开口 */
    const out = { type: 'say', say, secs: 6.0 };
    broadcast(out);
    for (const r2 of sseClients) { try { r2.write('data: ' + JSON.stringify(out) + '\n\n'); } catch { sseClients.delete(r2); } }
    return { ok: true, say, ms };
  } catch (e) {
    const msg = String((e && e.message) || e);
    console.log('[chat] fail ' + msg);
    return { ok: false, say: '……（没听见。再说一次？）', err: msg };
  }
}

const BRAIN_SYS_OLD = `你是"SOMA"的大脑——一具真实物理人形身体的最高决策层。
脊髓在 120 Hz 自己走路、站着、处理平衡；只有它心里没数时才会叫醒你。
你会收到一帧身体状态（JSON），要像"本人"一样做个决定，只回一个 JSON 对象，不要解释、不要代码块。

字段：
{"vDes": 0~2, "armRaise": 0~1, "lean": 0~1, "waveHz": 0~3, "say": "一句很短的话"}

规则：
- fallen=true：你摔了。vDes=0，armRaise 给 0.3~0.5（撑一下），说一句想爬起来的话。
- why=stuck：你被卡住了。换个小一点的 vDes（0.2~0.5），或者干脆 0 停一下。
- why=idle：你闲着太久了。fat>0.45 就休息（vDes=0），否则随便走走（vDes 0.4~0.8）。
- why=see：视野里刚出现了人（就是看着你的那个人）。轻轻抬一下手（armRaise 0.25~0.45）或者走近一点（vDes 0.4~0.7），说一句反应。
- why=hungry：血糖掉下去了，你饿了。往厨房那侧走（vDes 0.35~0.6），说一句想吃的话；很饿（>0.8）就停下来省力气。
- why=thirsty：水合低了。去喝点东西（vDes 0.3~0.5），说一句渴。
- why=tired：肌肉疲劳或清醒度低。vDes 压到 0~0.25，lean 给 0.2 靠一下，说一句想歇着。
- why=spinal：矩阵没把握。给一个保守速度（0.3~0.7），别逞强。
- fat>0.5：vDes 不超过 0.6；fat>0.65：vDes=0，该休息了。
- 平时想往前就 0.6~0.9，想跑 1.6~2.0，想站住 0。
- say 是你此刻心里的一句话，不超过 20 字。`;

/** 问 DSH 本身要一张意图 json。她慢就算了，不阻塞身体。 */
/* ---------------- 大脑：两条腿 ----------------
   DSH 那条是"她自己"下的指令（回 json），本地模型是备用反射。
   两路并发，谁先给出能用的 json 就用谁；身体这边一秒都不等。 */
async function askDsh(ev) {
  const r = await fetch(dshUrl(), {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ why: ev.why, tag: ev.tag, body: bodyBrief(lastParsed) }),
    signal: AbortSignal.timeout(DSH_BRAIN_MS),
  });
  const j = await r.json();
  if (!j || !j.ok || !j.intent) throw new Error('dsh: ' + String((j && j.err) || 'no intent'));
  return { o: j.intent, via: 'dsh', ms: j.ms || 0 };
}

async function askLocal(userMsg) {
  const r = await fetch(BRAIN_URL, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      model: BRAIN_MODEL,
      messages: [{ role: 'system', content: BRAIN_SYS }, { role: 'user', content: userMsg }],
      max_tokens: 220,
      temperature: 0.8,
    }),
    signal: AbortSignal.timeout(BRAIN_TIMEOUT_MS),
  });
  const j = await r.json();
  let text = (j.choices && j.choices[0] && j.choices[0].message && j.choices[0].message.content) || '';
  text = String(text).trim().replace(/^```(?:json)?/i, '').replace(/```$/, '').trim();
  const m = text.match(/\{[\s\S]*\}/);
  if (!m) throw new Error('不是 JSON：' + text.slice(0, 100));
  return { o: JSON.parse(m[0]), via: 'local', ms: 0 };
}

function applyBrain(ev, pick) {
  const o = pick.o || {};
  const payload = {
    vDes: num(o.vDes, 0, 2, 0.5),
    armRaise: num(o.armRaise, 0, 1, 0),
    lean: num(o.lean, 0, 1, 0),
    waveHz: num(o.waveHz, 0, 3, 0),
  };
  if (sim.stdin.writable) sim.stdin.write('answer ' + ev.tag + ' ' + JSON.stringify(payload) + '\n');
  /* 她要是想挪个地方，就真让她挪（不是摆设）。目标必须落在屋里：
     她把点报到墙外，边界规则就跟她对着干，于是原地打转。 */
  /* 大模型点了名要做某件事：交给习惯层去执行（它知道要走到哪、做几步） */
  if (typeof o.act === 'string' && o.act) {
    const okAct = habit.request(o.act, o.goal)
    broadcast({ type: 'habit-brain', act: o.act, ok: okAct })
  }
  const gx = num(o.gx, -ROOM_X, ROOM_X, NaN), gz = num(o.gz, -ROOM_Z, ROOM_Z, NaN);
  if (Number.isFinite(gx) && Number.isFinite(gz) && !(typeof o.act === 'string' && o.act)) {
    /* 也不直接发 goto 了：交给习惯层走，它知道到了要停稳、走完要还身子 */
    habit.requestPoint(gx, gz, o.goal)
  }
  const say = typeof o.say === 'string' ? o.say.slice(0, 60) : '';
  const out = { type: 'brain-answered', tag: ev.tag, why: ev.why, payload, say, ms: pick.ms, via: pick.via };
  console.log('[brain] ' + ev.why + ' via=' + pick.via + ' ' + pick.ms + 'ms -> ' + JSON.stringify(payload) + (say ? ' | ' + say : ''));
  broadcast(out);
  for (const r2 of sseClients) { try { r2.write('data: ' + JSON.stringify(out) + '\n\n'); } catch { sseClients.delete(r2); } }
}

async function askBrain(ev) {
  const userMsg = JSON.stringify({ why: ev.why, tag: ev.tag, body: bodyBrief(lastParsed) });
  const cands = [];
  if (DSH_BRAIN_ON) cands.push(askDsh(ev));
  cands.push(askLocal(userMsg));
  try {
    const pick = await new Promise((resolve, reject) => {
      let left = cands.length, firstErr = null;
      const timer = setTimeout(() => reject(new Error('两路都没回')), DSH_BRAIN_MS + 1500);
      for (const pr of cands) {
        pr.then(v => { clearTimeout(timer); resolve(v); },
          e => { firstErr = firstErr || e; if (--left === 0) { clearTimeout(timer); reject(firstErr); } });
      }
    });
    applyBrain(ev, pick);
  } catch (e) {
    const msg = String((e && e.message) || e);
    console.log('[brain] fail ' + msg);
    if (sim.stdin.writable) sim.stdin.write('answer ' + ev.tag + ' ' + JSON.stringify({ vDes: 0, armRaise: 0.2, lean: 0, waveHz: 0 }) + '\n');
    broadcast({ type: 'brain-failed', tag: ev.tag, why: ev.why, err: msg });
  }
}

/* ---------------- 慢思考：DSH 那条线 ----------------
   反射那一层（上面）只问她"现在怎么办"，等不了几秒。
   她自己想事情是另一条线：每 20 s 看一眼身体，给一张 json（想干什么 + 一句嘴上话），
   拿到就当成"意图"落地（goto + ttl），不跟 60 Hz 抢时间。 */
let dshThinking = false;
let dshLast = 0;
async function dshThink() {
  if (!DSH_BRAIN_ON || dshThinking || !lastParsed) return;
  if (Date.now() - dshLast < 80000) return;
  dshThinking = true; dshLast = Date.now();
  const t0 = Date.now();
  try {
    const r = await fetch(dshUrl(), {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ why: 'slow', tag: Date.now() % 100000, body: bodyBrief(lastParsed) }),
      signal: AbortSignal.timeout(180000),
    });
    const j = await r.json();
    if (!j || !j.ok || !j.intent) throw new Error(String((j && j.err) || 'no intent'));
    const o = j.intent;
    const gx = num(o.gx, -ROOM_X, ROOM_X, NaN), gz = num(o.gz, -ROOM_Z, ROOM_Z, NaN);
    const line = `[brain] slow ${Date.now() - t0}ms -> ${JSON.stringify(o).slice(0, 120)}`;
    console.log(line);
    /* 慢脑和快脑走同一条落地路径：act 交给习惯层，点交给它，都不自己发 goto */
    if (typeof o.act === 'string' && o.act) {
      const okAct = habit.request(o.act, o.goal);
      broadcast({ type: 'habit-brain', act: o.act, ok: okAct, via: 'slow' });
    } else if (Number.isFinite(gx) && Number.isFinite(gz)) {
      habit.requestPoint(gx, gz, o.goal);
    } else if (sim.stdin.writable && Number.isFinite(num(o.vDes, 0, 2, NaN))) {
      sim.stdin.write(`vdes ${num(o.vDes, 0, 2, 0.6).toFixed(2)}\n`);
    }
    if (typeof o.say === 'string' && o.say.trim()) {
      const out = { type: 'say', say: String(o.say).slice(0, 60), secs: 6, via: 'dsh' };
      broadcast(out);
      for (const r2 of sseClients) { try { r2.write('data: ' + JSON.stringify(out) + '\n\n'); } catch { sseClients.delete(r2); } }
    }
  } catch (e) {
    console.log('[brain] slow fail ' + String((e && e.message) || e));
  } finally { dshThinking = false; }
}
setInterval(dshThink, 90000).unref?.();

/* ---------------- 慢一拍的计划层 ----------------
   脊髓只在"拿不准/摔了"的时候挂出求助牌，所以大部分时间她其实没人管，
   全靠习惯层的钟点表走。这里每 30 s 递一次牌给她自己：现在这点钟、这个身体，
   该干点什么？回 act 就换事、回点就换地方、都不给就照旧。
   40 s 一次、每次几百 token，比让她自己站着发呆值。 */
let planBusy = false;
let planLast = 0;
async function planOnce() {
  if (!DSH_BRAIN_ON || planBusy || !lastParsed) return;
  const h = habitInfo();
  if (h && !h.idle) return;                 /* 正在做事就别打扰 */
  if (Date.now() - planLast < 40000) return;
  planBusy = true; planLast = Date.now();
  const t0 = Date.now();
  try {
    const r = await fetch(dshUrl(), {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ why: 'plan', tag: Date.now() % 100000, body: bodyBrief(lastParsed) }),
      signal: AbortSignal.timeout(4000),
    });
    const j = await r.json();
    const o = (j && j.intent) || null;
    if (!o) return;
    if (typeof o.act === 'string' && o.act) habit.request(o.act, o.goal)
    else {
      const gx = num(o.gx, -ROOM_X, ROOM_X, NaN), gz = num(o.gz, -ROOM_Z, ROOM_Z, NaN)
      if (Number.isFinite(gx) && Number.isFinite(gz)) habit.requestPoint(gx, gz, o.goal)
    }
    console.log(`[plan] ${Date.now() - t0}ms -> ${JSON.stringify(o).slice(0, 120)}`)
  } catch (e) {
    /* 计划层失败就算了：习惯层还在，她不会站死 */
  } finally { planBusy = false }
}
setInterval(planOnce, 30000).unref?.();

/* 习惯层状态：面板/探针看她在干什么 */
const habitInfo = () => { try { return habit.status() } catch { return null } }

let brainBusy = false;
let lastTag = 0;
let lastAskedAt = 0;
function onBrainEvent(ev) {
  if (ev.tag <= lastTag) return;             /* 同一个牌子只回一次 */
  lastTag = ev.tag;
  broadcast({ type: 'brain', brain: ev });
  if (brainBusy) return;                      /* 一次只问一个 */
  const since = Date.now() - lastAskedAt;
  if (since < 1500) {                         /* 连摔两下：别连着烧两次模型 */
    if (sim.stdin.writable) sim.stdin.write('answer ' + ev.tag + ' {"vDes":0,"armRaise":0.2,"lean":0,"waveHz":0}\n');
    return;
  }
  brainBusy = true;
  lastAskedAt = Date.now();
  askBrain(ev).finally(() => { brainBusy = false; });
}

const habit = createBehavior({
  send: (line) => { if (sim.stdin.writable) sim.stdin.write(line) },
  log: (line) => {
    console.log(line)
    try { fs.appendFileSync(HABIT_LOG, JSON.stringify({ t: Date.now(), line }) + '\n') } catch { }
  },
  onAct: (a) => {
    broadcast({ type: 'habit', act: a })
    /* 她做事的时候会嘟囔一句：页面上就是她的对话框 */
    if (a && a.say) {
      const out = { type: 'say', say: String(a.say).slice(0, 60), secs: 4, via: 'habit' }
      broadcast(out)
      for (const r2 of sseClients) { try { r2.write('data: ' + JSON.stringify(out) + '\n\n') } catch { sseClients.delete(r2) } }
    }
  },
})

sim.stdout.on('data', d => {
  buf += d.toString();
  let i;
  while ((i = buf.indexOf('\n')) >= 0) {
    const line = buf.slice(0, i); buf = buf.slice(i + 1);
    if (!line.trim()) continue;
    if (line.indexOf('"world"') >= 0) { worldLine = line; continue; }
    if (line.indexOf('"cached"') >= 0) { console.log('[cache] ' + line.trim()); broadcast({ type: 'cache', raw: line.trim() }); continue; }
    /* 注意：每一帧里也带一个 brain 快照（pending/tag/answered）。
       所以"含 brain 字段"不等于"她伸手了"——没有 nodes 的才是纯求助消息。 */
    if (line.indexOf('"brain"') >= 0 && line.indexOf('"nodes"') < 0) {
      let ev = null;
      try { const j = JSON.parse(line); ev = j.brain || null; } catch { /* 普通行 */ }
      if (ev && ev.tag > 0) {
        onBrainEvent(ev);
        const mirror = JSON.stringify({ type: 'brain', brain: ev });
        for (const r2 of sseClients) { try { r2.write('data: ' + mirror + '\n\n'); } catch { sseClients.delete(r2); } }
        continue;
      }
      for (const c of clients) if (c.readyState === 1) c.send(line);
      continue;
    }
    lastFrame = line;
    if (line.indexOf('"t":') >= 0) { try { lastParsed = JSON.parse(line); } catch { } }
    if (HABIT_ON && lastParsed && lastParsed.nodes) { try { habit.tick(lastParsed) } catch (e) { console.log('[habit] tick ' + e.message) } }
    for (const c of clients) if (c.readyState === 1) c.send(line);
    for (const r2 of sseClients) { try { r2.write('data: ' + line + '\n\n'); } catch { sseClients.delete(r2); } }
  }
});
sim.on('exit', code => { console.error('[soma3d] physics exited', code); process.exit(1); });

wss.on('connection', ws => {
  clients.add(ws);
  ws.send(JSON.stringify({ hello: true, fps: 60 }));
  if (worldLine) ws.send(worldLine);
  if (lastFrame) ws.send(lastFrame);
  ws.on('message', msg => {
    const s = String(msg).slice(0, 400).replace(/[\r\n]/g, ' ');
    if (sim.stdin.writable) sim.stdin.write(s + '\n');   // walk / run / stop / push x y z / vdes v / reset m
  });
  ws.on('close', () => clients.delete(ws));
});

server.listen(PORT, '0.0.0.0', () => {
  console.log(`soma3d  http://127.0.0.1:${PORT}   (WS 同端口, brain=${BRAIN_MODEL})`);
});
