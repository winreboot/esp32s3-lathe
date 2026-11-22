// Runs every G-code generator of data/index.html with a set of parameter cases, in a headless browser, and writes the
// programs to tools/gcode_cases.json for tools/check_gcode.py. For each case it also runs the page's own cut
// simulation (the one behind the preview and Play) and records the metal it leaves and any rapid that cuts metal,
// with the picked tool's outline, so the checker can compare the page's simulation with its own model.
// Needs Node and Playwright with Chromium:
//   node tools/gcode_cases.js && python3 tools/check_gcode.py
// Run both after any change to the generators, the tools or the simulation; the checker must end with FAILURES: 0.
const {chromium}=require('playwright');const fs=require('fs');
(async()=>{const b=await chromium.launch();const p=await b.newPage();const errs=[];p.on('pageerror',e=>errs.push(String(e)));p.on('console',m=>{if(m.type()==='error'&&!/ERR_FILE_NOT_FOUND|WebSocket/.test(m.text()))errs.push(m.text())});
await p.goto('file://'+require('path').resolve(__dirname,'..','data','index.html')+'#programs');await p.waitForTimeout(800);
const cases={
 part:[{},{mode:'peck'},{ref:'l'},{thru:true},{S:0},{mode:'peck',S:.2},{D:2,L:.5,clr:0},{D:.375,web:0,d:.005},{tool:'g3',W:.1181},{tool:'b062',W:.0625,mode:'peck',ref:'l'}],
 groove:[{},{fin:false},{ref:'l'},{Wg:.094},{Wg:.3,Df:.6},{Zw:0},{tool:'g2',W:.0787,Wg:.2}],
 face:[{},{to:.5},{z0:.1,z1:0,fz:0},{dz:.015,z1:-.05},{tool:'dcmt'},{tool:'rcmt'},{tool:'vcmtn'}],
 turn:[{},{c:0},{tail:true},{D0:1,Df:.2,c:.05,L:.5},{doc:.03,fa:0},{tool:'vcmt'},{tool:'rcmt',c:0},{tool:'hss'},{tool:'dcmt',c:.04}],
 taper:[{},{D0:.8,Db:.8},{Ds:.05,Db:.6,D0:.75,L:1},{fa:0,doc:.05},{tool:'dcmt'},{tool:'vcmt',Ds:.4}],
 wave:[{},{shape:'bead'},{shape:'cove',tool:'rcmt'},{shape:'v',tool:'vcmtn'},{Dc:.9},{flat:40,a:.06,p:.4},{tool:'ccmt'},{tool:'dcmt',a:.03,p:.3},
       {zs:0,n:2,p:.5,a:.08,tool:'rcmt'},{tool:'hss'},{Dc:.8,shape:'bead',tool:'vcmtn',n:6,p:.2,a:.03},{D0:1.5,Dc:1.4,n:3,p:.6,a:.12,shape:'sine',tool:'rcmt',doc:.02}],
 drill:[{},{mode:'brk'},{to:'tip',depth:1,q:.15},{tool:'d135'},{tool:'d135',to:'tip',Dd:.375}]};
const out=await p.evaluate(cases=>{const r={};for(const id in cases){r[id]=cases[id].map(c=>{const v=Object.assign(gcVals(id),c);const res=GC_GEN[id].gen(v);
  if(res.err)return {v,err:res.err};
  const t0=performance.now(),sm=gcSimAll(res),ms=performance.now()-t0,st=Math.max(1,Math.round(.0025/sm.dz));
  const sim={ms,hits:sm.hits.map(h=>({ln:res.p.mv[h.mi].ln+1,z:h.z,r:h.r,cut:h.cut})),pts:[]};
  for(let k=0;k<sm.n;k+=st)sim.pts.push([sm.zl+(k+.5)*sm.dz,sm.o[k],sm.i[k]]);
  const T=res.tool,env=T.t!=='drill'?gcEnv(gcToolPoly(T),.0005):null;
  return {v,code:res.p.out.join('\n'),warn:res.warn,info:res.info,name:res.name,tool:T,left:res.left,sim,
   env:env&&{u0:env.u0,du:env.du,h:Array.from(env.h)}}})}return r},cases);
fs.writeFileSync(require('path').join(__dirname,'gcode_cases.json'),JSON.stringify(out));console.log('page errors:',errs.length?errs:'none');await b.close()})();
