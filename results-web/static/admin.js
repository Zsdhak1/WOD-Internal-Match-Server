'use strict';
const $ = id => document.getElementById(id);
const teamForm = $('team-form');
const matchForm = $('match-form');
let csrf = '';
let data = {teams:[],matches:[],standings:[]};
let importToken = '';
let importRevision = 0;
let loading = false;
const statuses = {scheduled:'未开始',live:'进行中',overtime:'加时中',finished:'已完成',cancelled:'已取消'};
function node(tag, value, className) {
  const n = document.createElement(tag);
  if (value !== undefined) n.textContent = String(value);
  if (className) n.className = className;
  return n;
}
function notice(message, error=false) {
  $('notice').hidden = !message;
  $('notice').className = `notice${error ? ' error' : ''}`;
  $('notice').textContent = message;
}
function loggedIn(value) {
  $('login-panel').hidden = value;
  $('workspace').hidden = !value;
  $('logout').hidden = !value;
  $('refresh').hidden = !value;
  if (!value) { csrf = ''; invalidatePreview(); }
}
async function api(path, method='GET', body) {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 12000);
  try {
    const options = {method,credentials:'same-origin',cache:'no-store',signal:controller.signal,headers:{}};
    if (body !== undefined) {
      options.headers['Content-Type'] = 'application/json';
      if (csrf) options.headers['X-CSRF-Token'] = csrf;
      options.body = JSON.stringify(body);
    }
    const response = await fetch(path, options);
    const result = await response.json();
    if (!response.ok) {
      if (response.status === 401 && path !== '/api/login') loggedIn(false);
      throw new Error(result.error || `HTTP ${response.status}`);
    }
    return result;
  } catch (error) {
    if (error.name === 'AbortError') throw new Error('请求超时。请刷新核对数据后再操作，避免重复提交。');
    throw error;
  } finally { clearTimeout(timeout); }
}
async function action(button, fn) {
  const wasDisabled = button?.disabled;
  if (button) button.disabled = true;
  try { await fn(); } catch (error) { notice(error.message, true); }
  finally { if (button) button.disabled = button === $('commit-import') ? !importToken : Boolean(wasDisabled); }
}
function td(value) { return node('td', value); }
function button(label, fn, dangerous=false) {
  const b = node('button',label,dangerous ? 'danger' : '');
  b.type = 'button';
  b.addEventListener('click', () => action(b, fn));
  return b;
}
function selectOptions(select, teams, includeAutomatic=false) {
  const previous = select.value;
  const fragment = document.createDocumentFragment();
  if (includeAutomatic) {
    const empty = node('option','按合法比分校验确定'); empty.value = ''; fragment.append(empty);
  } else {
    const empty = node('option','请选择队伍'); empty.value = ''; fragment.append(empty);
  }
  for (const t of teams) { const option = node('option',t.name); option.value = t.id; fragment.append(option); }
  select.replaceChildren(fragment);
  if ([...select.options].some(o => o.value === previous)) select.value = previous;
}
function resetTeam() {
  teamForm.reset();
  teamForm.elements.id.value = '';
  teamForm.elements.version.value = '';
  $('team-form-title').textContent = '添加队伍';
}
function resetMatch() {
  matchForm.reset();
  matchForm.elements.id.value = '';
  matchForm.elements.version.value = '';
  matchForm.elements.date.value = new Date().toLocaleDateString('en-CA');
  // Explicit local YYYY-MM-DD: locale formatting differs on older browser engines.
  const today = new Date();
  matchForm.elements.date.value = `${today.getFullYear()}-${String(today.getMonth()+1).padStart(2,'0')}-${String(today.getDate()).padStart(2,'0')}`;
  $('match-form-title').textContent = '新增比赛 / 结果';
  updateWinner();
}
function editTeam(t) {
  for (const key of ['id','version','name','short_name','organization']) teamForm.elements[key].value = t[key];
  $('team-form-title').textContent = `编辑队伍 #${t.id}`;
  teamForm.elements.name.focus();
  notice('编辑后请明确保存；若有其他更新，将拒绝旧版本写入。');
}
function editMatch(m) {
  for (const key of ['id','version','team_a','team_b','date','round','score_a','score_b','status','overtime_score_a','overtime_score_b']) matchForm.elements[key].value = m[key];
  matchForm.elements.overtime.checked = m.overtime;
  updateWinner();
  matchForm.elements.winner_id.value = m.winner_id || '';
  $('match-form-title').textContent = `编辑比赛 #${m.id}`;
  updateVerdict();
  matchForm.scrollIntoView({block:'start',behavior:matchMedia('(prefers-reduced-motion: reduce)').matches ? 'auto' : 'smooth'});
}
function renderTeams() {
  const filter = $('team-search').value.trim().toLowerCase();
  const teams = data.teams.filter(t => `${t.name} ${t.organization} ${t.short_name}`.toLowerCase().includes(filter));
  const fragment = document.createDocumentFragment();
  for (const t of teams) {
    const row = node('tr');
    const name = td(t.name); name.append(node('small',`ID ${t.id}`));
    const org = td(t.organization || '—'); org.append(node('small',t.short_name || '无简称'));
    const actions = td();
    actions.append(button('编辑', () => editTeam(t)), button('删除', async () => {
      if (!confirm(`确定删除队伍「${t.name}」？有比赛的队伍将拒绝删除。`)) return;
      await api(`/api/teams/${t.id}`,'DELETE',{version:t.version});
      if (teamForm.elements.id.value === String(t.id)) resetTeam();
      await load(); notice('队伍已删除。');
    }, true));
    row.append(name,org,actions); fragment.append(row);
  }
  if (!teams.length) { const row=node('tr'); const cell=td(filter ? '没有符合搜索的队伍。' : '暂无队伍，可添加或导入名单。'); cell.colSpan=3; row.append(cell); fragment.append(row); }
  $('team-list').replaceChildren(fragment);
}
function renderMatches() {
  const filter = $('match-filter').value;
  const matches = data.matches.filter(m => filter === 'all' || m.status === filter);
  const fragment = document.createDocumentFragment();
  for (const m of matches) {
    const row = node('tr');
    const date = td(m.date); date.append(node('small',`#${m.id} · ${m.round}`));
    const teams = td(m.name_a); teams.append(node('small',`VS ${m.name_b}`));
    const score = td(`${m.score_a} : ${m.score_b}`); score.append(node('small',m.overtime ? `加时 ${m.overtime_score_a} : ${m.overtime_score_b}` : '无加时'));
    const status = td(); status.append(node('span',statuses[m.status],`badge ${m.status}`),node('small',m.winner_name ? `胜方：${m.winner_name}` : '未产生胜方'));
    const actions = td();
    actions.append(button('编辑', () => editMatch(m)),button('删除', async () => {
      if (!confirm(`确定删除比赛 #${m.id}（${m.name_a} vs ${m.name_b}）？排名会重新计算，此操作不可撤销。`)) return;
      await api(`/api/matches/${m.id}`,'DELETE',{version:m.version});
      if (matchForm.elements.id.value === String(m.id)) resetMatch();
      await load(); notice('比赛已删除，排名已重算。');
    },true));
    row.append(date,teams,score,status,actions); fragment.append(row);
  }
  if (!matches.length) { const row=node('tr'); const cell=td('暂无此状态的比赛记录。'); cell.colSpan=5; row.append(cell); fragment.append(row); }
  $('match-list').replaceChildren(fragment);
}
function updateWinner() {
  const ids = [Number(matchForm.elements.team_a.value),Number(matchForm.elements.team_b.value)];
  selectOptions(matchForm.elements.winner_id,data.teams.filter(t => ids.includes(t.id)),true);
  updateVerdict();
}
function updateVerdict() {
  const f = matchForm.elements;
  const a = Number(f.score_a.value), b = Number(f.score_b.value);
  const oa = Number(f.overtime_score_a.value), ob = Number(f.overtime_score_b.value);
  const overtime = f.overtime.checked;
  f.overtime_score_a.disabled = !overtime;
  f.overtime_score_b.disabled = !overtime;
  f.winner_id.disabled = f.status.value !== 'finished';
  let message = '比赛未完成，不记胜负和积分。';
  let invalid = false;
  if (f.team_a.value && f.team_a.value === f.team_b.value) { message='比赛双方不能相同。'; invalid=true; }
  else if (overtime && a !== b) { message='常规不同分，不允许加时。'; invalid=true; }
  else if (f.status.value === 'finished') {
    if (a === b && (!overtime || oa === ob)) { message='同分未决：必须进入SD加时；互杀同分继续，不能完成。'; invalid=true; }
    else if (overtime && Math.abs(oa - ob) !== 1) { message='SD加时最终分差必须恰好为1；支持互杀后 n : n+1。'; invalid=true; }
    else {
      const winnerId = overtime ? (oa > ob ? f.team_a.value : f.team_b.value) : (a > b ? f.team_a.value : f.team_b.value);
      const winner = data.teams.find(t => String(t.id) === winnerId);
      message = winner ? `合法胜方：${winner.name}；每次击杀 +1，无胜场奖励。` : '请选择双方队伍。';
      if (f.winner_id.value && f.winner_id.value !== winnerId) { message='所选胜方与比分冲突，无法保存。'; invalid=true; }
    }
  } else if (f.status.value === 'overtime') {
    message = overtime && a === b ? '常规同分，加时进行中；暂不计入排名。' : '加时中必须勾选加时且常规同分。';
    invalid = !overtime || a !== b;
  }
  $('verdict').textContent = message;
  $('verdict').className = `verdict${invalid ? ' invalid' : ''}`;
}
async function load() {
  if (loading) return;
  loading = true;
  try {
    data = await api('/api/admin');
    $('summary').textContent = `${data.teams.length} 支队伍 · ${data.matches.length} 场比赛 · ${data.matches.filter(m=>m.status==='finished').length} 场已完成`;
    renderTeams(); renderMatches();
    selectOptions(matchForm.elements.team_a,data.teams);
    selectOptions(matchForm.elements.team_b,data.teams);
    updateWinner();
  } finally { loading = false; }
}
for (const tab of document.querySelectorAll('[data-tab]')) {
  tab.addEventListener('click', () => {
    for (const other of document.querySelectorAll('[data-tab]')) {
      const selected = tab === other;
      other.classList.toggle('active', selected);
      other.setAttribute('aria-selected',String(selected));
      $(`tab-${other.dataset.tab}`).hidden = !selected;
    }
  });
}
$('login-form').addEventListener('submit', event => {
  event.preventDefault();
  const f = event.currentTarget;
  action(f.querySelector('button'), async () => {
    const result = await api('/api/login','POST',{username:f.elements.username.value,password:f.elements.password.value});
    csrf=result.csrf; f.elements.password.value=''; loggedIn(true); await load(); notice('已登录。展示页只读，无需分享密码。');
  });
});
$('logout').addEventListener('click', () => action($('logout'),async () => { await api('/api/logout','POST',{}); loggedIn(false); notice('已安全退出。'); }));
$('refresh').addEventListener('click', () => action($('refresh'),async () => { await load(); notice('列表已刷新。正在编辑的旧版本不会偷偷覆盖最新数据。'); }));
teamForm.addEventListener('submit', event => {
  event.preventDefault();
  const f=teamForm.elements;
  action(teamForm.querySelector('[type=submit]'),async () => {
    const body={name:f.name.value,short_name:f.short_name.value,organization:f.organization.value};
    if (f.id.value) body.version=Number(f.version.value);
    await api(f.id.value ? `/api/teams/${f.id.value}` : '/api/teams',f.id.value ? 'PUT':'POST',body);
    resetTeam(); invalidatePreview(); await load(); notice('队伍已保存。');
  });
});
matchForm.addEventListener('submit', event => {
  event.preventDefault();
  const f=matchForm.elements;
  action(matchForm.querySelector('[type=submit]'),async () => {
    const body={team_a:Number(f.team_a.value),team_b:Number(f.team_b.value),date:f.date.value,round:f.round.value,
      score_a:Number(f.score_a.value),score_b:Number(f.score_b.value),status:f.status.value,overtime:f.overtime.checked,
      overtime_score_a:f.overtime.checked ? Number(f.overtime_score_a.value) : 0,
      overtime_score_b:f.overtime.checked ? Number(f.overtime_score_b.value) : 0,
      winner_id:f.status.value === 'finished' && f.winner_id.value ? Number(f.winner_id.value) : null};
    if (f.id.value) body.version=Number(f.version.value);
    await api(f.id.value ? `/api/matches/${f.id.value}`:'/api/matches',f.id.value ? 'PUT':'POST',body);
    resetMatch(); await load(); notice('比赛已保存，积分排名已重新计算。');
  });
});
$('team-reset').addEventListener('click',resetTeam);
$('match-reset').addEventListener('click',resetMatch);
$('team-search').addEventListener('input',renderTeams);
$('match-filter').addEventListener('change',renderMatches);
matchForm.elements.team_a.addEventListener('change',updateWinner);
matchForm.elements.team_b.addEventListener('change',updateWinner);
matchForm.addEventListener('input',updateVerdict);
matchForm.addEventListener('change',updateVerdict);
function invalidatePreview() {
  importRevision++;
  importToken=''; $('commit-import').disabled=true; $('import-summary').textContent='内容已变更 / 尚未预览，请先校验。';
  $('import-preview').replaceChildren();
}
for (const id of ['import-content','import-format','duplicate-mode']) $(id).addEventListener('input',invalidatePreview);
$('import-file').addEventListener('change', () => action(null,async () => {
  invalidatePreview();
  const revision=importRevision;
  const file=$('import-file').files[0]; if (!file) return;
  if (file.size > 1024*1024) throw new Error('文件超过1MiB，请拆分名单后导入。');
  if (!/\.(csv|json)$/i.test(file.name)) throw new Error('只支持CSV或JSON；XLSX请另存为UTF-8 CSV。');
  const buffer=await file.arrayBuffer();
  let content;
  try { content=new TextDecoder('utf-8',{fatal:true}).decode(buffer); }
  catch { throw new Error('文件不是有效UTF-8编码，请在表格软件中另存为CSV UTF-8。'); }
  if (revision !== importRevision) return;
  $('import-content').value=content;
  $('import-format').value=/\.json$/i.test(file.name) ? 'json':'csv';
  invalidatePreview(); notice(`已读取 ${file.name}。请先预览校验。`);
}));
$('preview-import').addEventListener('click', () => action($('preview-import'),async () => {
  invalidatePreview();
  const revision=importRevision;
  const body={format:$('import-format').value,content:$('import-content').value,duplicate_mode:$('duplicate-mode').value};
  const result=await api('/api/import/preview','POST',body);
  if (revision !== importRevision || !csrf) return;
  importToken=result.can_commit ? result.token : '';
  $('import-summary').textContent=`新增 ${result.add_count} · 跳过 ${result.skip_count} · 错误 ${result.error_count} · 预览10分钟内有效`;
  $('commit-import').disabled=!result.can_commit;
  const fragment=document.createDocumentFragment();
  for (const row of result.rows) {
    const tr=node('tr'); const plan=td();
    plan.append(node('span',{add:'新增',skip:'跳过',error:'错误'}[row.action],`badge ${row.action}`));
    tr.append(td(row.row || '—'),td(row.name || '—'),plan,td(row.message || '校验通过，尚未写入')); fragment.append(tr);
  }
  $('import-preview').replaceChildren(fragment);
  notice(result.error_count ? '预览存在错误，没有任何数据写入。请修正或明确调整重复策略后重新预览。' : '预览完成，尚未写入。核对后点击“确认新增队伍”。',result.error_count > 0);
}));
$('commit-import').addEventListener('click', () => action($('commit-import'),async () => {
  if (!importToken) throw new Error('请先预览。');
  if (!confirm('确认按照当前预览新增队伍？重复项不会覆盖。')) return;
  const token=importToken; importToken='';
  const result=await api('/api/import/commit','POST',{token});
  invalidatePreview(); await load(); notice(`导入完成：新增 ${result.imported} 队，明确跳过 ${result.skipped} 队。`);
}));
$('password-form').addEventListener('submit', event => {
  event.preventDefault(); const f=event.currentTarget;
  action(f.querySelector('button'),async () => {
    if (f.elements.new_password.value !== f.elements.confirm_password.value) throw new Error('两次新密码不一致。');
    if (!confirm('修改密码将退出全部后台会话。确认继续？')) return;
    await api('/api/password','POST',{current_password:f.elements.current_password.value,new_password:f.elements.new_password.value});
    f.reset(); loggedIn(false); notice('密码已修改，全部会话已失效。请使用新密码登录。');
  });
});
resetMatch();
(async () => {
  try { const session=await api('/api/session'); csrf=session.csrf; loggedIn(true); await load(); }
  catch (error) { loggedIn(false); if (!error.message.includes('登录')) notice(error.message,true); }
})();
