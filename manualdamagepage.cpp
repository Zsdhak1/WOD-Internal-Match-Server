// Mobile page served by ManualDamageServer. Kept dependency-free (no CDN):
// phones on an isolated match Wi-Fi usually have no Internet access.
#include "manualdamageserver.h"

QByteArray ManualDamageServer::pageHtml()
{
    // Split into pieces: MSVC limits a single string literal to ~16 KB.
    static const QByteArray html = QByteArray(R"HTML(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<meta name="theme-color" content="#0b1720"><title>手动扣血</title>
<style>
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html,body{margin:0;height:100%;background:#0b1720;color:#f4f7fa;font-family:system-ui,-apple-system,"PingFang SC","Microsoft YaHei",sans-serif}
main{min-height:100%;display:flex;flex-direction:column;padding:20px 18px calc(24px + env(safe-area-inset-bottom));gap:16px}
h1{margin:0;font-size:20px;font-weight:600;color:#9fb3c2}
.pick{flex:1;display:flex;flex-direction:column;gap:16px;justify-content:center}
.pick a{display:flex;align-items:center;justify-content:center;height:30vh;border-radius:18px;font-size:34px;font-weight:700;color:#fff;text-decoration:none}
.red{background:#c8323c}.blue{background:#2a6fd6}
.card{border-radius:16px;padding:16px 18px;background:#13242f;border:1px solid #24394a}
.side{font-size:28px;font-weight:700}.side.r{color:#ff6a6f}.side.b{color:#6aa8ff}
.row{display:flex;justify-content:space-between;align-items:baseline;margin-top:10px;color:#9fb3c2;font-size:15px}
.hp{font-size:44px;font-weight:700;color:#fff;font-variant-numeric:tabular-nums}
#hit{flex:1;min-height:42vh;border:0;border-radius:24px;font-size:40px;font-weight:800;color:#fff;letter-spacing:2px;touch-action:manipulation;user-select:none;-webkit-user-select:none;transition:transform .08s,filter .2s}
#hit.r{background:radial-gradient(circle at 50% 40%,#ff5a63,#a3222b)}#hit.b{background:radial-gradient(circle at 50% 40%,#5aa0ff,#1d4fa8)}
#hit:active{transform:scale(.97)}#hit:disabled{filter:grayscale(.85) brightness(.6)}
#hit small{display:block;font-size:17px;font-weight:500;letter-spacing:0;margin-top:8px;opacity:.9}
.switch{text-align:center;color:#6f8798;font-size:14px}.switch a{color:#9fb3c2}
#snack{position:fixed;left:16px;right:16px;bottom:calc(20px + env(safe-area-inset-bottom));padding:14px 16px;border-radius:12px;font-size:16px;background:#26323b;color:#fff;box-shadow:0 6px 24px rgba(0,0,0,.45);transform:translateY(calc(100% + 60px));visibility:hidden;transition:transform .22s,visibility 0s .22s;z-index:9}
#snack.show{transform:none;visibility:visible;transition:transform .22s}#snack.ok{background:#1f7a4a}#snack.err{background:#a3332b}
</style></head><body><main id="app"></main><div id="snack" role="status" aria-live="polite"></div>
)HTML") + QByteArray(R"HTML(<script>
(function(){
"use strict";
var app=document.getElementById("app"),snack=document.getElementById("snack"),snackTimer=0;
var path=location.pathname.replace(/\/+$/,"");
var team=path==="/red"?"red":path==="/blue"?"blue":"";
var canVibrate=typeof navigator.vibrate==="function";
function esc(s){return String(s).replace(/[&<>"']/g,function(c){return {"&":"&amp;","<":"&lt;",">":"&gt;","\"":"&quot;","'":"&#39;"}[c];});}
function toast(text,kind){snack.textContent=text;snack.className="show "+(kind||"");clearTimeout(snackTimer);snackTimer=setTimeout(function(){snack.className="";},kind==="err"?3200:1800);}
if(!team){
  app.innerHTML='<h1>手动扣血 · 选择队伍</h1><div class="pick"><a class="red" href="/red">红方</a><a class="blue" href="/blue">蓝方</a></div>';
  return;
}
var r=team==="red",label=r?"红方":"蓝方",cls=r?"r":"b",other=r?"blue":"red";
app.innerHTML='<div class="card"><div class="side '+cls+'">'+label+' <span id="name"></span></div>'
 +'<div class="row"><span>当前 HP</span><span class="hp" id="hp">—</span></div>'
 +'<div class="row"><span id="phase">连接中…</span><span id="amount"></span></div></div>'
 +'<button id="hit" class="'+cls+'" disabled>扣血<small id="hint">等待服务端</small></button>'
 +'<div class="switch"><a href="/'+other+'">切换到'+(r?"蓝方":"红方")+'</a> · <a href="/">返回</a></div>';
var hit=document.getElementById("hit"),hint=document.getElementById("hint");
var busy=false,state=null;
function render(){
  var s=state;
  document.getElementById("name").textContent=s&&s.teamName?"· "+s.teamName:"";
  document.getElementById("hp").textContent=s&&s.hp>=0?s.hp:"—";
  document.getElementById("phase").textContent=s?(s.message||""):"无法连接服务端";
  document.getElementById("amount").textContent=s&&s.amount?"每次 "+s.amount+" HP":"";
  hit.firstChild.nodeValue=s&&s.amount?"扣 "+s.amount+" 血":"扣血";
  hit.disabled=busy||!s||!s.available;
  hint.textContent=busy?"等待车辆确认…":(s&&s.available?"点按扣血":(s?(s.reason||"当前不可用"):"离线"));
}
function poll(){
  fetch("/api/state?team="+team,{cache:"no-store"}).then(function(x){return x.json();})
   .then(function(j){state=j;render();},function(){state=null;render();});
}
function newId(){
  if(window.crypto&&crypto.getRandomValues){var a=new Uint8Array(12);crypto.getRandomValues(a);return Array.prototype.map.call(a,function(b){return("0"+b.toString(16)).slice(-2);}).join("");}
  return Date.now().toString(36)+Math.random().toString(36).slice(2,12);
}
function post(id,attempt){
  var ctl=window.AbortController?new AbortController():null,t=setTimeout(function(){if(ctl)ctl.abort();},3500);
  return fetch("/api/damage",{method:"POST",headers:{"Content-Type":"application/json"},cache:"no-store",
    body:JSON.stringify({team:team,requestId:id}),signal:ctl?ctl.signal:undefined})
   .then(function(x){clearTimeout(t);return x.json();})
   .catch(function(e){clearTimeout(t);if(attempt<2)return post(id,attempt+1);throw e;});
}
hit.addEventListener("click",function(){
  if(busy||hit.disabled)return;
  busy=true;render();
  post(newId(),0).then(function(j){
    if(j&&j.ok){
      // Android: vibrate only after the car ACKed. iOS Safari has no
      // Vibration API (or it may be blocked): fall back to a snackbar.
      var buzzed=false;try{buzzed=canVibrate&&navigator.vibrate([60,40,60]);}catch(e){}
      if(!buzzed)toast(j.message||"已扣血","ok");
    }else{toast((j&&j.message)||"扣血失败","err");}
  },function(){toast("网络异常，未确认扣血结果，请看控制台","err");})
  .then(function(){busy=false;poll();});
});
render();poll();setInterval(poll,1000);
document.addEventListener("visibilitychange",function(){if(!document.hidden)poll();});
})();
</script></body></html>
)HTML");
    return html;
}
