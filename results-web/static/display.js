'use strict';
const $ = id => document.getElementById(id);
let snapshot = null;
let page = 0;
let perPage = 1;
let paused = false;
let focusPaused = false;
let pageStarted = performance.now();
let lastSignature = '';
let lastPageKey = '';
let pollBusy = false;
const stateLabels = {scheduled:'未开始',live:'进行中',overtime:'加时中',finished:'已完成',cancelled:'已取消'};
function element(tag, className, value) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (value !== undefined) node.textContent = String(value);
  return node;
}
function pageCount() { return Math.max(1, Math.ceil((snapshot?.standings.length || 0) / perPage)); }
function measureRows() {
  const table = document.querySelector('.standings-table');
  const headerHeight = table.tHead.getBoundingClientRect().height;
  const row = table.tBodies[0].rows[0];
  const rowHeight = row ? row.getBoundingClientRect().height : Math.min(94, Math.max(70, innerHeight * .065));
  const available = $('table-space').getBoundingClientRect().height - headerHeight;
  const next = Math.max(1, Math.floor((available - 2) / rowHeight));
  if (next !== perPage) {
    const first = page * perPage;
    perPage = next;
    page = Math.floor(first / perPage);
    lastPageKey = '';
  }
}
function render() {
  if (!snapshot) return;
  measureRows();
  const total = snapshot.standings.length;
  page = Math.min(page, pageCount() - 1);
  const start = page * perPage;
  const key = `${lastSignature}:${page}:${perPage}`;
  if (key !== lastPageKey) {
    const fragment = document.createDocumentFragment();
    for (const team of snapshot.standings.slice(start, start + perPage)) {
      const tr = element('tr');
      const rank = element('td');
      rank.append(element('span', `rank-number${team.rank <= 3 ? ' top' : ''}`, String(team.rank).padStart(2, '0')));
      const name = element('td');
      const fullName = element('span', 'team-name', team.name);
      fullName.title = team.name;
      name.title = [team.name,team.short_name,team.organization].filter(Boolean).join(' · ');
      name.append(fullName);
      tr.append(rank, name, element('td','',team.played), element('td','',team.wins), element('td','',team.losses),
        element('td','points',team.points), element('td','',team.against),
        element('td','delta',team.kills - team.against > 0 ? `+${team.kills - team.against}` : team.kills - team.against));
      fragment.append(tr);
    }
    const body = $('standings');
    body.classList.remove('page-transition');
    body.replaceChildren(fragment);
    // Re-read height after replacing content, so animations never influence row measurement.
    void body.offsetHeight;
    body.classList.add('page-transition');
    lastPageKey = key;
    requestAnimationFrame(() => { const old = perPage; measureRows(); if (old !== perPage) render(); });
  }
  $('empty-state').hidden = total !== 0;
  $('page-label').textContent = `第 ${page + 1} / ${pageCount()} 页`;
  $('range-label').textContent = total ? `队伍 ${start + 1}–${Math.min(total, start + perPage)} / ${total}` : '所有队伍';
  $('previous').disabled = pageCount() === 1;
  $('next').disabled = pageCount() === 1;
}
function updateSummary() {
  $('team-count').textContent = snapshot.teams.length;
  $('finished-count').textContent = snapshot.matches.filter(m => m.status === 'finished').length;
  const m = snapshot.matches[0];
  $('recent-match').textContent = m ? `${m.date} · ${m.round} · 常规 ${m.score_a} : ${m.score_b}${m.overtime ? ` · SD ${m.overtime_score_a} : ${m.overtime_score_b}` : ''} · ${stateLabels[m.status]}　${m.name_a} VS ${m.name_b}${m.winner_name ? ` · 胜方 ${m.winner_name}` : ''}` : '暂无比赛记录';
  $('recent-match').title = $('recent-match').textContent;
}
function setConnection(online, message) {
  $('connection-dot').className = `dot ${online ? 'online' : 'offline'}`;
  $('connection').textContent = online ? '数据已连接' : '连接中断';
  $('update-status').classList.toggle('stale', !online);
  document.querySelector('.update-label').textContent = online ? '数据更新时间' : '最后成功更新';
  $('display-error').hidden = online;
  $('display-error').textContent = message || '';
  requestAnimationFrame(render);
}
async function poll() {
  if (pollBusy) return;
  pollBusy = true;
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 6000);
  try {
    const response = await fetch('/api/public', {cache:'no-store',signal:controller.signal,credentials:'omit'});
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const data = await response.json();
    if (!Array.isArray(data.standings) || !Array.isArray(data.matches)) throw new Error('数据格式异常');
    const signature = JSON.stringify([data.standings, data.matches]);
    snapshot = data;
    if (signature !== lastSignature) {
      lastSignature = signature;
      updateSummary();
      render();
    }
    const receivedAt = new Date();
    $('update-time').textContent = receivedAt.toLocaleTimeString('zh-CN',{hour12:false});
    $('update-time').dateTime = receivedAt.toISOString();
    $('update-time').title = `最近成功获取数据：${receivedAt.toLocaleString('zh-CN',{hour12:false})}`;
    setConnection(true);
  } catch (error) {
    setConnection(false, `无法连接数据服务（${error.name === 'AbortError' ? '请求超时' : error.message}）。${snapshot ? '保留上次结果，排名可能已过期。' : '尚无可用数据，请确认独立网页服务已启动。'}正在自动重试…`);
  } finally { clearTimeout(timeout); pollBusy = false; }
}
function changePage(delta) {
  page = (page + delta + pageCount()) % pageCount();
  pageStarted = performance.now();
  render();
}
function updatePauseLabel() {
  $('pause').textContent = paused ? '继续轮播' : '暂停轮播';
  $('pause').setAttribute('aria-pressed', String(paused));
}
$('previous').addEventListener('click', () => changePage(-1));
$('next').addEventListener('click', () => changePage(1));
$('pause').addEventListener('click', () => { paused = !paused; pageStarted = performance.now(); updatePauseLabel(); });
$('interval').addEventListener('change', () => { pageStarted = performance.now(); });
$('table-space').addEventListener('wheel', () => { paused = true; updatePauseLabel(); }, {passive:true});
document.querySelector('.page-controls').addEventListener('focusin', () => { focusPaused = true; });
document.querySelector('.page-controls').addEventListener('focusout', event => {
  if (!event.currentTarget.contains(event.relatedTarget)) { focusPaused = false; pageStarted = performance.now(); }
});
$('fullscreen').addEventListener('click', async () => {
  try {
    if (document.fullscreenElement) await document.exitFullscreen();
    else await document.documentElement.requestFullscreen();
  } catch { $('fullscreen').textContent = '请按 F11'; }
});
document.addEventListener('fullscreenchange', () => { $('fullscreen').textContent = document.fullscreenElement ? '退出全屏' : '全屏'; });
document.addEventListener('keydown', event => {
  if (['INPUT','SELECT','BUTTON'].includes(event.target.tagName)) return;
  if (event.key === 'ArrowLeft') changePage(-1);
  if (event.key === 'ArrowRight') changePage(1);
  if (event.code === 'Space') { event.preventDefault(); paused = !paused; pageStarted = performance.now(); updatePauseLabel(); }
});
document.addEventListener('visibilitychange', () => { pageStarted = performance.now(); if (!document.hidden) poll(); });
new ResizeObserver(() => render()).observe($('table-space'));
setInterval(() => {
  const now = performance.now();
  const duration = Number($('interval').value) * 1000;
  const stopped = paused || focusPaused || document.hidden || pageCount() === 1;
  if (stopped) pageStarted = now;
  if (!stopped && now - pageStarted >= duration) changePage(1);
  $('cycle-progress').style.width = `${stopped ? 0 : Math.min(100, (now - pageStarted) / duration * 100)}%`;
}, 250);
poll();
setInterval(poll, 3000);
