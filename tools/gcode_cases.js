// Runs every G-code generator of data/index.html with a set of parameter cases, in a headless browser, and writes the
// programs to tools/gcode_cases.json for tools/check_gcode.py. Needs Node and Playwright with Chromium:
//   node tools/gcode_cases.js && python3 tools/check_gcode.py
// Run both after any change to the generators; the checker must end with FAILURES: 0.
const {chromium}=require('playwright');const fs=require('fs');
(async()=>{const b=await chromium.launch();const p=await b.newPage();const errs=[];p.on('pageerror',e=>errs.push(String(e)));p.on('console',m=>{if(m.type()==='error')errs.push(m.text())});
await p.goto('file://'+require('path').resolve(__dirname,'..','data','index.html')+'#programs');await p.waitForTimeout(800);
const cases={
 part:[{},{mode:'peck'},{ref:'l'},{thru:true},{S:0},{mode:'peck',S:.2},{D:2,L:.5,clr:0},{D:.375,web:0,d:.005}],
 groove:[{},{fin:false},{ref:'l'},{Wg:.094},{Wg:.3,Df:.6},{Zw:0}],
 face:[{},{to:.5},{z0:.1,z1:0,fz:0},{dz:.015,z1:-.05}],
 turn:[{},{c:0},{tail:true},{D0:1,Df:.2,c:.05,L:.5},{doc:.03,fa:0}],
 taper:[{},{D0:.8,Db:.8},{Ds:.05,Db:.6,D0:.75,L:1},{fa:0,doc:.05}],
 drill:[{},{mode:'brk'},{to:'tip',depth:1,q:.15}]};
const out=await p.evaluate(cases=>{const r={};for(const id in cases){r[id]=cases[id].map(c=>{const v=Object.assign(gcVals(id),c);const res=GC_GEN[id].gen(v);
  return res.err?{v,err:res.err}:{v,code:res.p.out.join('\n'),warn:res.warn,info:res.info,name:res.name}})}return r},cases);
fs.writeFileSync(require('path').join(__dirname,'gcode_cases.json'),JSON.stringify(out));console.log('page errors:',errs.length?errs:'none');await b.close()})();
