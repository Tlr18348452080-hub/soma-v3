/* ================= 屋子里能"用"的东西 =================
   没有给每件家具写一个类：只是在它旁边放一个看不见的、可被射线打中的热点（0.28 m 的盒子），
   点中之后她就走过去、到跟前再执行一条 C++ 指令（喝/吃/睡/上厕/看书…）。

   为什么不用"给现有 mesh 打 tag"：家具是程序化生成的盒子，改名/换模型时 tag 就会丢；
   热点在数据里，跟几何解耦，改布局只改这里的坐标。

   每个热点：{ id, name, x, z, cmd, say, face }
     cmd  —— 到了之后发给 C++ 的那一行（沿用已有的指令集，不新增函数）
     say  —— 她做完顺便嘟囔的一句（走 server 的 say 通道，不占 TTS 之外的东西）
     face —— 站在点上时朝向（弧度），没写就朝着热点自己
   ========================================================================= */
export const ITEMS = [
  { id: 'cup',     name: '水杯',   x: 2.05, z: 1.02, cmd: 'drink 250',  say: '……凉的。' },
  { id: 'fridge',  name: '冰箱',   x: 3.30, z: 5.55, cmd: 'drink 300',  say: '喝口凉的。' },
  { id: 'table',   name: '餐桌',   x: 2.60, z: 5.55, cmd: 'eat 95',     say: '……有点咸。' },
  { id: 'bedA',    name: '床',     x: -1.90, z: 4.30, cmd: 'sleep 1200', say: '躺一会儿……' },
  { id: 'bedB',    name: '书房床', x: -1.95, z: 8.10, cmd: 'sleep 900',  say: '就眯一下。' },
  { id: 'desk',    name: '书桌',   x: 1.30, z: -2.35, cmd: 'motion deskwork', say: '嗯……' },
  { id: 'shelf',   name: '书架',   x: 3.00, z: -1.85, cmd: 'motion deskwork', say: '这本看过。' },
  { id: 'toilet',  name: '马桶',   x: 3.10, z: 7.20, cmd: 'pee',        say: '……等一下。' },
  { id: 'sink',    name: '洗手台', x: 2.10, z: 7.30, cmd: 'drink 200',  say: '洗个手。' },
  { id: 'window',  name: '窗',     x: 0.10, z: -2.55, cmd: 'vdes 0',    say: '外面挺亮的。' },
  { id: 'sofa',    name: '沙发',   x: 2.20, z: 2.40, cmd: 'motion sit',  say: '……歇一下。' },
  { id: 'door',    name: '门',     x: 5.30, z: 0.90, cmd: 'vdes 0',    say: '外面好安静。' },
];

/* 热点是"看不见但打得中"的小盒子：material 透明到 0，visible 保持 true
   （three 的 Raycaster 会跳过 visible=false 的对象）。 */
export function createItems(THREE) {
  const group = new THREE.Group();
  const mat = new THREE.MeshBasicMaterial({ transparent: true, opacity: 0, depthWrite: false });
  const hotspots = [];
  for (const it of ITEMS) {
    const m = new THREE.Mesh(new THREE.BoxGeometry(0.30, 0.34, 0.30), mat);
    m.position.set(it.x, 0.55, it.z);
    m.userData.item = it;
    group.add(m);
    hotspots.push(m);
  }
  return { group, hotspots, items: ITEMS };
}

/* 射线挑最近的物品热点；返回 { item, distance } 或 null */
export function pickItem(THREE, raycaster, hotspots) {
  const hit = raycaster.intersectObjects(hotspots, false);
  if (!hit.length) return null;
  return { item: hit[0].object.userData.item, distance: hit[0].distance, point: hit[0].point };
}
