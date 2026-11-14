#!/usr/bin/env node
/* vscreen 直连小工具：node vsd.mjs list|frame [i]|tap i x y|swipe i x1 y1 x2 y2|key i NAME|app pkg [i]|sh "cmd" */
import http from 'node:http'; import fs from 'node:fs'; import path from 'node:path'
const TOK = fs.readFileSync('/run/dsh-vsd/token','utf8').trim()
const req = (port,p,{method='GET',body=null,timeoutMs=20000}={}) => new Promise((res,rej)=>{
  const r=http.request({host:'127.0.0.1',port,path:p,method,timeout:timeoutMs,
    headers:{'X-Token':TOK,...(body!==null?{'Content-Type':'text/plain; charset=utf-8','Content-Length':Buffer.byteLength(body)}:{})}},
    (rs)=>{const c=[];rs.on('data',d=>c.push(d));rs.on('end',()=>res({code:rs.statusCode,body:Buffer.concat(c)}))})
  r.on('error',e=>rej(e)); r.on('timeout',()=>r.destroy(new Error('timeout')))
  if(body!==null) r.write(body); r.end() })
const shd = async (cmd)=> (await req(18242,'/exec',{method:'POST',body:cmd,timeoutMs:60000})).body.toString()
const [cmd,...a]=process.argv.slice(2)
const list = async () => JSON.parse((await req(18240,'/list')).body.toString())
const did = async (i)=>{const l=await list();const s=l.find(x=>Number(x.i)===Number(i))||l[0];return s}
try{
  if(cmd==='list'){ console.log(JSON.stringify(await list(),null,1)) }
  else if(cmd==='health'){ console.log((await req(18240,'/health')).body.toString()) }
  else if(cmd==='frame'){ const i=Number(a[0]||0); const w=a[1]?`?w=${a[1]}`:''
    const r=await req(18240,`/${i}/frame${w}`,{timeoutMs:20000});
    const dir='/root/.dsh-vscreen'; fs.mkdirSync(dir,{recursive:true})
    const f=path.join(dir,`shot-${i}-${Date.now()}.jpg`); fs.writeFileSync(f,r.body); console.log(f) }
  else if(cmd==='tap'){ console.log(await shd(`input -d ${(await did(a[0])).displayId} tap ${a[1]} ${a[2]}`)) }
  else if(cmd==='swipe'){ console.log(await shd(`input -d ${(await did(a[0])).displayId} swipe ${a[1]} ${a[2]} ${a[3]} ${a[4]} ${a[5]||300}`)) }
  else if(cmd==='text'){ console.log(await shd(`input -d ${(await did(a[0])).displayId} text '${a[1]}'`)) }
  else if(cmd==='key'){ console.log(await shd(`input -d ${(await did(a[0])).displayId} keyevent ${a[1]}`)) }
  else if(cmd==='app'){ const i=Number(a[1]!==undefined?a[1]:1); const d=(await did(i)).displayId
    console.log(await shd(`am start --display ${d} -n ${a[0]}`)) }
  else if(cmd==='sh'){ console.log(await shd(a.join(' '))) }
  else console.log('cmds: list frame tap swipe text key app sh')
}catch(e){ console.error('ERR',e.message); process.exit(1) }
