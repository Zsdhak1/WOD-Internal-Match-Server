// Real Chromium rendering via Node 22+ native WebSocket/CDP; no npm installation.
// Pass --python PATH --browser PATH. Own fixture/profile are isolated and cleaned.
import {spawn} from 'node:child_process';
import {mkdtemp, readFile, writeFile, rm, access} from 'node:fs/promises';
import path from 'node:path';
import net from 'node:net';
import {fileURLToPath} from 'node:url';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const out = path.join(root, 'verification');
const option = name => process.argv[process.argv.indexOf(name) + 1];
const python = option('--python');
const browser = option('--browser');
if (!process.argv.includes('--python') || !process.argv.includes('--browser')) throw Error('Pass --python and --browser executable paths');
const runtime = await mkdtemp(path.join(out, '.qa-runtime-'));
const children = [];
const report = {started_at:new Date().toISOString(), engine:'Real isolated Chromium headless via CDP', fixtures:37, resolutions:[], checks:{}, errors:[], browser_visual_tested:false, passed:false};
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
async function until(fn, label, limit=10000) {
  const end = Date.now() + limit;
  while (Date.now() < end) { try { const value = await fn(); if (value) return value; } catch {} await wait(60); }
  throw Error(`Timed out: ${label}`);
}
function check(value, label) { if (!value) throw Error(label); }
function launch(executable, args, stdio='inherit') { const child = spawn(executable, args, {stdio}); children.push(child); return child; }
let ws;
let serial=0;
const pending = new Map();
async function cdp(method, params={}) {
  const id=++serial;
  return new Promise((resolve,reject) => {
    const timeout=setTimeout(() => { pending.delete(id); reject(Error(`CDP timeout ${method}`)); },15000);
    pending.set(id,{resolve:v=>{clearTimeout(timeout);resolve(v);},reject:e=>{clearTimeout(timeout);reject(e);}});
    ws.send(JSON.stringify({id,method,params}));
  });
}
async function evaluate(expression) {
  const result=await cdp('Runtime.evaluate',{expression,returnByValue:true,awaitPromise:true});
  if (result.exceptionDetails) throw Error(result.exceptionDetails.exception?.description || result.exceptionDetails.text);
  return result.result.value;
}
async function navigate(url) {
  await cdp('Page.navigate',{url});
  await until(async()=>await evaluate('document.readyState === "complete" && location.href === '+JSON.stringify(url)),url);
  await wait(200);
}
async function screenshot(name) {
  const result=await cdp('Page.captureScreenshot',{format:'png',captureBeyondViewport:false});
  await writeFile(path.join(out,name),Buffer.from(result.data,'base64'));
}
const metrics = `(() => {
 const box = document.getElementById('table-space').getBoundingClientRect();
 const table = document.querySelector('.standings-table').getBoundingClientRect();
 const rows = [...document.querySelectorAll('#standings tr')];
 return {width:innerWidth,height:innerHeight,per_page:perPage,pages:pageCount(),page:page+1,rows:rows.length,
   names:rows.map(r=>r.querySelector('.team-name').textContent),
   horizontal_overflow:document.documentElement.scrollWidth > innerWidth || table.right > innerWidth+1,
   vertical_overflow:document.documentElement.scrollHeight > innerHeight || rows.some(r=>r.getBoundingClientRect().bottom > box.bottom+1),
   table_height:Math.round(box.height),row_height:rows.length?Math.round(rows[0].getBoundingClientRect().height):0,
   name_font:rows.length?parseFloat(getComputedStyle(rows[0].querySelector('.team-name')).fontSize):0,
   score_font:rows.length?parseFloat(getComputedStyle(rows[0].querySelector('.points')).fontSize):0,
   footer_visible:document.querySelector('.broadcast-footer').getBoundingClientRect().bottom <= innerHeight,
   long_names_preserved:rows.every(r=>r.querySelector('.team-name').title === r.querySelector('.team-name').textContent)};
})()`;
try {
  const fixture=launch(python,[path.join(root,'tests','qa_server.py'),runtime]);
  const port=await until(async()=>Number(await readFile(path.join(runtime,'server-port.txt'),'utf8')),'fixture port');
  const base=`http://127.0.0.1:${port}`;
  await until(async()=>(await fetch(base+'/api/public')).ok,'fixture HTTP');
  const debugPort=await new Promise((resolve,reject)=>{const probe=net.createServer();probe.on('error',reject);probe.listen(0,'127.0.0.1',()=>{const port=probe.address().port;probe.close(()=>resolve(port));});});
  const chromium=launch(browser,['--headless=new','--disable-gpu','--no-first-run','--no-default-browser-check',`--remote-debugging-port=${debugPort}`,`--user-data-dir=${path.join(runtime,'profile')}`,'about:blank'],'ignore');
  await until(async()=>{if(chromium.exitCode!==null) throw Error(`Chromium exited ${chromium.exitCode}`);return (await fetch(`http://127.0.0.1:${debugPort}/json/version`)).ok;},'DevTools HTTP',15000);
  const target=await until(async()=>(await (await fetch(`http://127.0.0.1:${debugPort}/json/list`)).json()).find(t=>t.type==='page'),'CDP target');
  ws=new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((resolve,reject)=>{ws.addEventListener('open',resolve,{once:true});ws.addEventListener('error',reject,{once:true});});
  ws.addEventListener('message',event=>{
    const message=JSON.parse(event.data);
    if (message.id && pending.has(message.id)) { const callback=pending.get(message.id);pending.delete(message.id);message.error?callback.reject(Error(message.error.message)):callback.resolve(message.result); }
    if (message.method==='Runtime.exceptionThrown') report.errors.push(message.params.exceptionDetails.exception?.description || message.params.exceptionDetails.text);
  });
  await cdp('Page.enable');await cdp('Runtime.enable');
  await cdp('Emulation.setDeviceMetricsOverride',{width:1440,height:1080,deviceScaleFactor:1,mobile:false});
  await navigate(base+'/');
  await until(async()=>await evaluate('snapshot !== null'),'empty snapshot');
  check(await evaluate('!document.getElementById("empty-state").hidden && document.getElementById("next").disabled'),'empty state');
  await screenshot('display-empty-1440x1080.png');report.checks.empty_state=true;
  const login=await fetch(base+'/api/login',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({username:'admin',password:'Headless-only-password-7'})});
  const cookie=login.headers.get('set-cookie').split(';')[0];const csrf=(await login.json()).csrf;
  async function api(route,body) {const response=await fetch(base+route,{method:'POST',headers:{'Content-Type':'application/json',Cookie:cookie,'X-CSRF-Token':csrf},body:JSON.stringify(body)});const result=await response.json();check(response.ok,JSON.stringify(result));return result;}
  const names=Array.from({length:37},(_,i)=>`${String(i+1).padStart(2,'0')} ${i%2?'International Robotics Extraordinary Long Team Name '.repeat(2):'超长中文机器人竞技队伍名称'.repeat(7)}`.slice(0,120).trim());
  const preview=await api('/api/import/preview',{format:'json',content:JSON.stringify(names.map((name,i)=>({name,short_name:`T${i+1}`,organization:i%2?'International College of Engineering and Technology':'机器人与人工智能联合学院'.repeat(5)})))});
  await api('/api/import/commit',{token:preview.token});
  await api('/api/matches',{team_a:1,team_b:2,date:'2026-10-10',round:'SD互杀回归',score_a:2,score_b:2,overtime:true,overtime_score_a:3,overtime_score_b:4,status:'finished'});
  await evaluate('poll()');
  await until(async()=>await evaluate('snapshot?.teams.length === 37'),'37 teams');
  for (const [width,height] of [[1440,1080],[1920,1440],[1920,1080],[2560,1440]]) {
    await cdp('Emulation.setDeviceMetricsOverride',{width,height,deviceScaleFactor:1,mobile:false});
    await evaluate('page=0;paused=true;lastPageKey="";render()');await wait(450);
    const metric=await evaluate(metrics);
    check(!metric.horizontal_overflow && !metric.vertical_overflow && metric.footer_visible,`overflow ${width}x${height}`);
    check(await evaluate(`(() => {
      const update = document.getElementById('update-time');
      const box = update.getBoundingClientRect();
      return document.querySelectorAll('.metrics > div').length === 2
        && !document.querySelector('.team-org, .rule-copy, .broadcast-header .eyebrow')
        && !document.body.textContent.includes('STANDINGS')
        && !document.body.textContent.includes('同分规则')
        && parseFloat(getComputedStyle(update).fontSize) >= 26
        && box.bottom <= innerHeight && box.right <= innerWidth
        && /^\\d{2}:\\d{2}:\\d{2}$/.test(update.textContent);
    })()`), `clean display and prominent update time ${width}x${height}`);
    report.checks.clean_display_and_prominent_update = true;
    check(await evaluate(`(async () => {
      const urls=['/assets/arena-background.jpg','/assets/arena-corners.jpg'];
      for (const url of urls) { const image=new Image();image.src=url;await image.decode();if(!image.naturalWidth) return false; }
      const left=getComputedStyle(document.body,'::before');
      const right=getComputedStyle(document.body,'::after');
      const bounds=document.querySelector('.broadcast').getBoundingClientRect();
      return left.backgroundImage.includes('arena-background.jpg') && right.backgroundImage.includes('arena-background.jpg')
        && left.backgroundSize==='auto 100%' && right.backgroundSize==='auto 100%'
        && document.querySelectorAll('.arena-corner').length===4
        && bounds.right<=innerWidth && bounds.bottom<=innerHeight;
    })()`), `artwork loaded, unstretched and proportion adapted ${width}x${height}`);
    report.checks.artwork_and_aspect_adaptation = true;
    const seen=new Set();
    for (let p=0;p<metric.pages;p++) {
      const current=await evaluate(metrics);
      check(!current.vertical_overflow && current.long_names_preserved,`row bounds page ${p}`);
      current.names.forEach(n=>seen.add(n));
      await evaluate('changePage(1)');await wait(300);
    }
    check(seen.size===37 && names.every(n=>seen.has(n)),`all teams covered ${width}x${height}`);
    metric.unique_teams_visible=seen.size;metric.complete_cycle_seconds=metric.pages*12;
    delete metric.names;report.resolutions.push(metric);
    await evaluate('page=0;lastPageKey="";render()');await wait(350);await screenshot(`display-${width}x${height}.png`);
  }
  report.checks.resize_four_dimensions=true;
  // Actual default timer tick while pointer is over table. One unaccelerated 8-second advance.
  await evaluate('page=0;paused=false;document.activeElement.blur();document.getElementById("interval").value="8";pageStarted=performance.now();render()');
  await cdp('Input.dispatchMouseEvent',{type:'mouseMoved',x:500,y:450});
  await wait(8500);check(await evaluate('page === 1'),'unattended real 8-second rotation with mouse over table');report.checks.real_timer_hover_rotation=true;
  // A scheduled poll may still be in flight; do not race its success with the failure fixture.
  await until(async()=>await evaluate('!pollBusy'),'poll idle before disconnect');
  await evaluate('paused=true;window.qaOriginalFetch=window.fetch;window.fetch=async()=>{throw new Error("QA simulated disconnect")};poll()');
  await until(async()=>await evaluate('!document.getElementById("display-error").hidden'),'simulated disconnect applied');
  check(await evaluate('!document.getElementById("display-error").hidden && snapshot.teams.length === 37'),'stale-data error state');
  check(!(await evaluate(metrics)).vertical_overflow,'disconnect row fit');await screenshot('display-disconnected-2560x1440.png');report.checks.disconnect_retains_data=true;
  await evaluate('window.fetch=window.qaOriginalFetch;poll()');
  await cdp('Emulation.setDeviceMetricsOverride',{width:1440,height:1080,deviceScaleFactor:1,mobile:false});
  await navigate(base+'/admin');
  await until(async()=>await evaluate('typeof api === "function"'),'admin JS');
  await evaluate(`document.querySelector('#login-form [name=password]').value='Headless-only-password-7';document.getElementById('login-form').requestSubmit()`);
  await until(async()=>await evaluate('!document.getElementById("workspace").hidden && data.teams.length === 37'),'actual admin login');
  await evaluate('document.querySelector("[data-tab=import]").click()');
  await evaluate(`window.qaFetch=window.fetch;window.fetch=async(...args)=>{const result=await window.qaFetch(...args);if(args[0]==='/api/import/preview') await new Promise(resolve=>{window.qaReleasePreview=resolve});return result};document.getElementById('import-content').value='name\\nOld pending name';document.getElementById('import-content').dispatchEvent(new Event('input'));document.getElementById('preview-import').click()`);
  await until(async()=>await evaluate('typeof window.qaReleasePreview === "function"'),'delayed preview response');
  await evaluate(`document.getElementById('import-content').value='name\\nCurrent visible name';document.getElementById('import-content').dispatchEvent(new Event('input'));window.qaReleasePreview()`);
  await until(async()=>await evaluate('!document.getElementById("preview-import").disabled'),'preview returned');
  check(await evaluate('!importToken && document.getElementById("commit-import").disabled && !document.querySelector("#import-preview tr")'),'stale preview discarded');report.checks.async_preview_revision_guard=true;
  await evaluate('window.fetch=window.qaFetch;document.getElementById("preview-import").click()');
  await until(async()=>await evaluate('!document.getElementById("commit-import").disabled'),'fresh preview');
  await screenshot('admin-import-preview-1440x1080.png');
  await evaluate('window.confirm=()=>true;document.getElementById("commit-import").click()');
  await until(async()=>await evaluate('data.teams.length === 38'),'commit completed');
  check(await evaluate('!importToken && document.getElementById("commit-import").disabled'),'commit stays disabled');
  check(await evaluate('data.teams.some(t=>t.name === "Current visible name") && !data.teams.some(t=>t.name === "Old pending name")'),'only current preview imported');report.checks.commit_button_and_current_content=true;
  await evaluate('document.querySelector("[data-tab=matches]").click();editMatch(data.matches[0]);matchForm.elements.overtime_score_b.value=5;updateVerdict()');
  check(await evaluate('document.getElementById("verdict").classList.contains("invalid") && document.getElementById("verdict").textContent.includes("恰好")'),'frontend SD gap validation');report.checks.frontend_sd=true;
  await screenshot('admin-sd-validation-1440x1080.png');
  await evaluate('matchForm.elements.overtime_score_b.value=4;updateVerdict();matchForm.requestSubmit()');
  await until(async()=>await evaluate('data.matches[0].version === 2'),'actual SD form save');
  check(await evaluate('data.standings.find(t=>t.id === 2).points === 6'),'SD save points no bonus');
  await evaluate('document.querySelector("#match-list").scrollIntoView({block:"center",behavior:"instant"})');
  await screenshot('admin-records-1440x1080.png');report.checks.actual_sd_form_save=true;
  await cdp('Runtime.evaluate',{expression:'window.scrollTo(0,0)'});
  await evaluate('document.getElementById("logout").click()');
  await until(async()=>await evaluate('document.getElementById("workspace").hidden'),'logout state');report.checks.logout_state=true;
  check(report.errors.length===0,'runtime JS exceptions');
  report.browser_visual_tested=true;report.passed=true;report.finished_at=new Date().toISOString();
} catch (error) {report.errors.push(error.stack);process.exitCode=1;}
finally {
  if(ws) ws.close();
  for(const child of children.reverse()) { if(child.exitCode===null) {child.kill();await Promise.race([new Promise(resolve=>child.once('exit',resolve)),wait(5000)]);} }
  // Explicitly verify exact target before removing only this script's generated runtime.
  check(path.dirname(runtime)===out && path.basename(runtime).startsWith('.qa-runtime-'),'refuse unsafe cleanup');
  await rm(runtime,{recursive:true,force:true,maxRetries:5,retryDelay:250});
  report.owned_processes_cleaned=true;
  await writeFile(path.join(out,'browser-report.json'),JSON.stringify(report,null,2));
  console.log(JSON.stringify(report,null,2));
}
