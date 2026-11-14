import crypto from 'node:crypto'
import http from 'node:http'
import fs from 'node:fs'
const b64 = (b) => Buffer.from(b).toString('base64').replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '')
const cred = fs.readFileSync('/root/.dsh/.credentials.yaml', 'utf8')
const secretB64 = cred.match(/client-connection\/browser-session:[\s\S]*?secret:\s*(\S+)/)[1]
const secretBuf = Buffer.from(secretB64.replace(/-/g, '+').replace(/_/g, '/') + '=='.slice(0, (4 - secretB64.length % 4) % 4), 'base64')
const authority = '127.0.0.1:3080'
const now = Date.now()
const body = b64(Buffer.from(JSON.stringify({ version: 1, authority, issuedAt: now, expiresAt: now + 3600e3 })))
const sig = b64(crypto.createHmac('sha256', secretBuf).update(body).digest())
const cookieName = 'dsh-auth-' + b64(crypto.createHash('sha256').update(authority).digest())
export const COOKIE = `${cookieName}=v1.${body}.${sig}`
export function req(path, { method = 'GET', json } = {}) {
  return new Promise((done) => {
    const data = json ? JSON.stringify(json) : null
    const r = http.request({ host: '127.0.0.1', port: 3080, path, method, headers: {
      cookie: COOKIE, origin: 'http://127.0.0.1:3080', referer: 'http://127.0.0.1:3080/',
      ...(data ? { 'content-type': 'application/json', 'content-length': Buffer.byteLength(data) } : {}),
    } }, (res) => { let b = ''; res.on('data', c => b += c); res.on('end', () => done({ code: res.statusCode, body: b })) })
    r.on('error', e => done({ err: e.message }))
    r.end(data)
  })
}
