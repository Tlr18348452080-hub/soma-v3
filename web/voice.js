/* 她的嗓子。
 *
 * 两层：
 *  声带  —— 源-滤波器（Klatt 那套的简版）。声门是一串锯齿脉冲＋抖动，
 *          后面三个带通当共振峰（F1/F2/F3），音高按声调走。没有采样、没有 TTS。
 *  传播  —— 她站在屋里说话：直达声 + 早期反射（按房间尺寸算延迟）+ 指数尾巴
 *          卷成一条脉冲响应，再按"离镜头多远"做距离衰减、空气吸收、左右定位。
 *
 * 听不清是正常的——它说的是鲸鱼腔。要听懂看气泡。
 */

const clamp = (x, a, b) => (x < a ? a : x > b ? b : x);

/* 元音共振峰（Hz）与相对增益 */
const VOWELS = [
  { f: [730, 1090, 2440], g: [1.0, 0.5, 0.22], name: 'a' },
  { f: [530, 1840, 2480], g: [1.0, 0.45, 0.2], name: 'e' },
  { f: [270, 2290, 3010], g: [1.0, 0.4, 0.18], name: 'i' },
  { f: [570, 840, 2410], g: [1.0, 0.55, 0.2], name: 'o' },
  { f: [300, 870, 2240], g: [1.0, 0.42, 0.16], name: 'u' },
  { f: [300, 1700, 2200], g: [1.0, 0.5, 0.18], name: 'ü' },
];

/** 把一个字摊成一口"音节"：声母是不是爆破、元音是哪个、声调往哪走。 */
function planSyllable(ch) {
  const c = ch.codePointAt(0) || 0
  const onset = c % 5 === 0 ? 'none' : c % 5 < 3 ? 'burst' : 'fric'
  return {
    onset,
    vowel: VOWELS[c % VOWELS.length],
    tone: [1.06, 0.92, 1.0, 1.14][c % 4],      // 一声平、二声扬、三声压、四声落
    dur: 0.16 + ((c >> 3) % 5) * 0.018,
  }
}

export function createVoice(ac, opts = {}) {
  const ctx = ac
  const out = ctx.createGain()
  out.gain.value = 0.9

  /* 干湿：直达 + 房间 */
  const dry = ctx.createGain(); dry.gain.value = 0.75
  const wet = ctx.createGain(); wet.gain.value = 0.55
  const conv = ctx.createConvolver()
  const air = ctx.createBiquadFilter()          // 空气吸收：越远越闷
  air.type = 'lowpass'; air.frequency.value = 7000; air.Q.value = 0.6
  const pan = ctx.createStereoPanner()

  out.connect(dry).connect(air)
  out.connect(conv).connect(wet).connect(air)
  air.connect(pan).connect(ctx.destination)

  /* ---- 房间脉冲响应：直达 + 六次早期反射 + 指数尾巴 ---- */
  let irSize = 0
  function buildRoom(size = [5.2, 2.5, 4.0], rt60 = 0.55) {
    const sr = ctx.sampleRate
    const len = Math.floor(sr * Math.min(1.6, rt60 * 1.6))
    const buf = ctx.createBuffer(2, len, sr)
    const [W, H, D] = size
    const taps = [
      [0.004, 0.55], [0.009, 0.4], [0.013, 0.32],
      [0.019, 0.24], [0.026, 0.18], [0.034, 0.13],
    ].map(([t, a]) => [t * (1 + (W + D) / 20), a])
    for (let ch = 0; ch < 2; ch++) {
      const d = buf.getChannelData(ch)
      for (const [t, a] of taps) {
        const i = Math.floor(t * sr * (ch ? 1.06 : 0.97))
        if (i < len) d[i] += a * (ch ? 0.9 : 1)
      }
      let seed = 1234 + ch * 77
      for (let i = 0; i < len; i++) {
        const t = i / sr
        seed = (seed * 1103515245 + 12345) & 0x7fffffff
        const noise = (seed / 0x3fffffff - 1)
        d[i] += noise * 0.28 * Math.exp(-t / (rt60 / 3)) *
          (t > 0.006 ? 1 : 0) * (1 - Math.exp(-t * 900))
      }
    }
    conv.buffer = buf
    irSize = len
  }
  buildRoom()

  /** 她在哪、镜头在哪 → 音量/亮度/左右。 */
  function place({ px = 0, pz = 0, cx = 0, cz = 0, facingYaw = 0 }) {
    const dx = px - cx, dz = pz - cz
    const dist = Math.hypot(dx, dz)
    out.gain.value = clamp(1.05 / (1 + dist * 0.35), 0.05, 1)
    air.frequency.value = clamp(7600 / (1 + dist * 0.16), 900, 7600)
    wet.gain.value = clamp(0.35 + dist * 0.12, 0.35, 0.85)
    pan.pan.value = clamp(dx > 0 ? Math.min(0.8, dist / 6) : -Math.min(0.8, dist / 6), -0.85, 0.85)
    return { dist, ...{ yaw: facingYaw } }
  }

  let stopAt = 0
  function hush() {
    stopAt = ctx.currentTime
  }

  /* ---- 自定义声带 ----
     她原来用的是源-滤波器手搭的共振峰（voice.js 这一套），能听出是"机器在说话"。
     真要好听只有一条路：接一个真的 TTS。这里留的是自填的口子 ——
     地址、密钥、模型、音色都放设置里，走 OpenAI 那套 /audio/speech（大多数家都兼容）。
     没填就还用手搭的这套，填了但请求失败也退回手搭的，不至于哑掉。 */
  let apiCfg = { url: '', key: '', model: '', voice: '' }
  let audioEl = null, audioSrc = null
  function setApi(cfg) {
    apiCfg = Object.assign({ url: '', key: '', model: '', voice: '' }, cfg || {})
  }
  function hasApi() { return !!(apiCfg.url && apiCfg.key) }
  async function speakRemote(text) {
    const body = { input: String(text).slice(0, 300), response_format: 'mp3' }
    if (apiCfg.model) body.model = apiCfg.model
    if (apiCfg.voice) body.voice = apiCfg.voice
    const r = await fetch(apiCfg.url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', Authorization: 'Bearer ' + apiCfg.key },
      body: JSON.stringify(body),
    })
    if (!r.ok) throw new Error('tts ' + r.status)
    const ct = r.headers.get('content-type') || ''
    let blob
    if (ct.includes('application/json')) {
      const j = await r.json()
      const b64 = j.audio || j.data || (j.audio_content) || null
      if (!b64) throw new Error('tts no audio')
      const bin = atob(String(b64).replace(/^data:[^,]*,/, ''))
      const u8 = new Uint8Array(bin.length)
      for (let i = 0; i < bin.length; i++) u8[i] = bin.charCodeAt(i)
      blob = new Blob([u8], { type: 'audio/mpeg' })
    } else {
      blob = await r.blob()
    }
    if (!audioEl) {
      audioEl = new Audio(); audioEl.preload = 'auto'; audioEl.crossOrigin = 'anonymous'
      /* ★ 关键：TTS 的音频不能直接"贴在耳朵上"响。
         过 MediaElementSource 接进 out，就跟本地合成走完全同一条链：
         干湿（房间反射）→ 空气低通（越远越闷）→ 立体声声像 → 输出。
         这一步做不做的差别，就是"手机里放录音"和"屋里有人在说话"的差别。 */
      try {
        const src = ctx.createMediaElementSource(audioEl)
        const g = ctx.createGain(); g.gain.value = 0.95
        src.connect(g).connect(out)
      } catch (e) { console.warn('[voice] TTS 没接进空气链路', e.message) }
    }
    if (audioSrc) URL.revokeObjectURL(audioSrc)
    audioSrc = URL.createObjectURL(blob)
    audioEl.src = audioSrc
    audioEl.volume = 1
    await audioEl.play()
    return audioEl.duration || 2
  }

  /** 说一句。返回预计说完的时刻。
      ★ 本地共振峰（speakLocal）已按主人要求停用：听着就是"机器在念字"。
      没有接 TTS 就不出声（hasApi() 为假时 speak 返回但立刻 muted），
      别偷偷退回手搭的那套 —— 那会把"没接好"藏起来。 */
  let lastError = ''
  function speak(text) {
    const chars = Array.from(String(text || '')).slice(0, 120)
    if (!chars.length) return ctx.currentTime
    if (!hasApi()) { lastError = '没接 TTS'; return ctx.currentTime }
    speakRemote(text)
      .then(() => { stopAt = ctx.currentTime + ((audioEl && audioEl.duration) || 2) })
      .catch(e => { lastError = e.message; console.warn('[voice] TTS 失败（不会再退回本地合成）', e.message) })
    return ctx.currentTime + 1.2
  }

  function speakLocal(text, pitch = 232, rate = 1, gain = 1) {
    const chars = Array.from(String(text || '')).slice(0, 120)
    if (!chars.length) return ctx.currentTime
    let t = ctx.currentTime + 0.02
    const start = t
    const master = ctx.createGain()
    master.gain.value = gain
    master.connect(out)

    for (const ch of chars) {
      if (/[，。！？、…,.!?~～\s]/.test(ch)) { t += 0.13; continue }
      if (t > stopAt + 0.4 && stopAt > start) break   /* 被打断 */
      const s = planSyllable(ch)
      const dur = s.dur / clamp(rate, 0.5, 2)

      /* 声门：锯齿 + 抖动 + 声调滑动 */
      const osc = ctx.createOscillator()
      osc.type = 'sawtooth'
      const f0 = pitch * s.tone
      osc.frequency.setValueAtTime(f0 * 1.04, t)
      osc.frequency.linearRampToValueAtTime(f0 * (s.tone > 1 ? 1.1 : 0.9), t + dur)
      const jitter = ctx.createOscillator()
      jitter.frequency.value = 5.2
      const jg = ctx.createGain(); jg.gain.value = f0 * 0.012
      jitter.connect(jg).connect(osc.frequency)

      /* 声门包络 */
      const env = ctx.createGain()
      env.gain.setValueAtTime(0.0001, t)
      env.gain.exponentialRampToValueAtTime(0.5, t + 0.02)
      env.gain.setValueAtTime(0.5, t + dur * 0.7)
      env.gain.exponentialRampToValueAtTime(0.0006, t + dur)
      osc.connect(env)

      /* 三个共振峰 */
      const sum = ctx.createGain(); sum.gain.value = 0.9
      s.vowel.f.forEach((f, i) => {
        const bp = ctx.createBiquadFilter()
        bp.type = 'bandpass'
        bp.frequency.setValueAtTime(f, t)
        bp.Q.value = 7 - i * 1.4
        const g = ctx.createGain(); g.gain.value = s.vowel.g[i] * 2.4
        env.connect(bp).connect(g).connect(sum)
      })

      /* 声母：爆破一记噪声 / 摩擦一小段 */
      if (s.onset !== 'none') {
        const nlen = s.onset === 'burst' ? 0.014 : dur * 0.45
        const nb = ctx.createBuffer(1, Math.max(64, Math.floor(nlen * ctx.sampleRate)), ctx.sampleRate)
        const nd = nb.getChannelData(0)
        for (let i = 0; i < nd.length; i++) nd[i] = Math.random() * 2 - 1
        const src = ctx.createBufferSource(); src.buffer = nb
        const bp = ctx.createBiquadFilter()
        bp.type = 'bandpass'
        bp.frequency.value = s.onset === 'burst' ? 1500 : 4200
        bp.Q.value = 1.2
        const ng = ctx.createGain()
        ng.gain.setValueAtTime(s.onset === 'burst' ? 0.5 : 0.22, t)
        ng.gain.exponentialRampToValueAtTime(0.0005, t + nlen)
        src.connect(bp).connect(ng).connect(master)
        src.start(t); src.stop(t + nlen + 0.01)
      }

      sum.connect(master)
      osc.connect(jg) /* 抖动器接不接都行，留着记号 */
      osc.start(t); jitter.start(t)
      osc.stop(t + dur + 0.02); jitter.stop(t + dur + 0.02)
      t += dur + 0.035
    }
    return t
  }

  return {
    speak, speakLocal, hush, place, setApi, get hasApi() { return hasApi() },
    get lastError() { return lastError },
    get node() { return out },
    rebuildRoom: buildRoom,
    get irLength() { return irSize },
    get speaking() { return ctx.currentTime < stopAt },
    resume: () => ctx.resume(),
  }
}
