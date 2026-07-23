(() => {
  'use strict';
  const state = {token:'', cameras:[], profiles:[], frameUrl:''};
  const $ = id => document.getElementById(id);
  const escapeHtml = value => String(value ?? '').replace(/[&<>"']/g, ch => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[ch]));
  const activeStatuses = new Set(['queued','starting','running','reconnecting','stopping']);

  function notify(message, error=false) {
    const node=$('notice'); node.textContent=message; node.className=`notice${error?' error':''}`;
    clearTimeout(notify.timer); notify.timer=setTimeout(()=>node.classList.add('hidden'),7000);
  }
  function headers(json=false, version=null) {
    const result={Authorization:`Bearer ${state.token}`};
    if(json)result['Content-Type']='application/json'; if(version!==null)result['If-Match']=`"${version}"`;
    return result;
  }
  async function api(path, options={}) {
    const response=await fetch(`/api/v1${path}`,options); if(response.status===204)return null;
    const body=(response.headers.get('content-type')||'').includes('json')?await response.json():await response.text();
    if(!response.ok)throw new Error(body.error||body.error_code||`${response.status} ${response.statusText}`); return body;
  }
  function badge(status) {
    const kind=['running','ready','ok','stopped'].includes(status)?'ok':['failed','error'].includes(status)?'bad':'warn';
    return `<span class="badge ${kind}">${escapeHtml(status)}</span>`;
  }
  function showTab(id) {
    document.querySelectorAll('.tab-panel').forEach(n=>n.classList.toggle('active',n.id===id));
    document.querySelectorAll('.tabs button').forEach(n=>n.classList.toggle('active',n.dataset.tab===id));
    refresh(id).catch(error=>notify(error.message,true));
  }
  async function connect() {
    state.token=$('token').value; if(!state.token)return notify('请输入管理员 Token。',true);
    await loadProfiles(); await loadCameras(); await loadHubs();
    $('connection').className='badge ok'; $('connection').textContent='已连接'; notify('控制台已连接。Token 仅保存在页面内存。');
  }
  async function loadCameras() {
    const result=await api('/cameras',{headers:headers()}); state.cameras=result.items||[];
    $('camera-rows').innerHTML=state.cameras.length?state.cameras.map(camera=>{
      const id=camera.camera_id; const status=camera.current_run?.status||(camera.enabled?'idle':'stopped'); const active=activeStatuses.has(status);
      return `<tr><td><strong>${escapeHtml(camera.name)}</strong><br><span class="muted mono">${escapeHtml(id)}</span></td><td>${escapeHtml(camera.camera_profile)}</td><td>${camera.frame_interval_ms} ms</td><td>${escapeHtml(camera.output_mode)}</td><td>${badge(status)}</td><td>${camera.version}</td><td><div class="row-actions"><button class="small" data-camera-status="${id}">状态</button><button class="small" data-camera-history="${id}">运行历史</button><button class="small" data-camera-frame="${id}">预览</button><button class="small" data-camera-edit="${id}">修改</button><button class="small ${active?'':'primary'}" data-camera-toggle="${id}" data-active="${active}">${active?'停止':'启动'}</button><button class="small danger" data-camera-delete="${id}">删除</button></div></td></tr>`;
    }).join(''):'<tr><td colspan="7" class="empty-row">暂无摄像头实例</td></tr>';
  }
  async function loadProfiles() {
    const result=await api('/camera-profiles',{headers:headers()}); state.profiles=result.items||[];
    $('profile-rows').innerHTML=state.profiles.length?state.profiles.map(p=>`<tr><td class="mono">${escapeHtml(p.profile_id)}</td><td>${escapeHtml(p.display_name)}</td><td class="mono">${escapeHtml(p.url_env)}</td><td>${escapeHtml(p.transport)}</td><td>${p.enabled?'是':'否'}</td></tr>`).join(''):'<tr><td colspan="5" class="empty-row">暂无可用 Profile</td></tr>';
    $('camera-profile').innerHTML=state.profiles.filter(p=>p.enabled).map(p=>`<option value="${escapeHtml(p.profile_id)}">${escapeHtml(p.display_name)} (${escapeHtml(p.profile_id)})</option>`).join('');
  }
  async function loadHubs() {
    const result=await api('/camera-hubs',{headers:headers()});
    $('hub-cards').innerHTML=result.items?.length?result.items.map(h=>`<article class="card"><h3>${escapeHtml(h.camera_profile)} ${badge(h.state)}</h3><div class="metric"><span>Hub ID</span><strong class="mono">${escapeHtml(h.hub_instance_id)}</strong></div><div class="metric"><span>打开次数</span><strong>${h.open_count}</strong></div><div class="metric"><span>订阅者</span><strong>${h.subscriber_count}</strong></div><div class="metric"><span>订阅类型</span><strong>${escapeHtml(JSON.stringify(h.subscriber_types))}</strong></div><div class="metric"><span>采集 FPS</span><strong>${Number(h.capture_fps||0).toFixed(2)}</strong></div><div class="metric"><span>重连</span><strong>${h.reconnect_count}</strong></div></article>`).join(''):'<div class="detail empty">当前没有活跃 Hub。</div>';
  }
  async function loadOperations() {
    const [health,ready,metrics]=await Promise.all([fetch('/api/v1/health').then(r=>r.json()),fetch('/api/v1/ready').then(r=>r.json()),api('/operations/metrics',{headers:headers()}).catch(()=>null)]);
    $('operations-content').innerHTML=`<article class="card"><h3>健康 ${badge(health.success?'ok':'error')}</h3><pre class="mono">${escapeHtml(JSON.stringify(health,null,2))}</pre></article><article class="card"><h3>就绪 ${badge(ready.ready?'ready':'error')}</h3><pre class="mono">${escapeHtml(JSON.stringify(ready,null,2))}</pre></article><article class="card"><h3>运维指标</h3><pre class="mono">${escapeHtml(JSON.stringify(metrics||{},null,2))}</pre></article>`;
  }
  async function refresh(id) {if(!state.token&&id!=='operations')return;if(id==='cameras')return loadCameras();if(id==='profiles')return loadProfiles();if(id==='hubs')return loadHubs();if(id==='operations')return loadOperations();}

  function openCamera(camera=null) {
    $('camera-dialog-title').textContent=camera?'修改摄像头（将替换抽帧线程）':'新增摄像头';
    $('camera-id').value=camera?.camera_id||''; $('camera-id').disabled=Boolean(camera); $('camera-version').value=camera?.version||'';
    $('camera-name').value=camera?.name||''; $('camera-profile').value=camera?.camera_profile||state.profiles[0]?.profile_id||'';
    $('camera-interval').value=camera?.frame_interval_ms??1000; $('camera-output').value=camera?.output_mode||'latest'; $('camera-quality').value=camera?.jpeg_quality??90;
    $('camera-width').value=camera?.max_width??0; $('camera-height').value=camera?.max_height??0; $('camera-retention').value=camera?.retention_days??7; $('camera-max-frames').value=camera?.max_saved_frames??100000; $('camera-enabled').checked=camera?.enabled??true;
    $('camera-dialog').showModal();
  }
  async function saveCamera(event) {
    event.preventDefault(); const editing=$('camera-id').disabled; const id=$('camera-id').value;
    const payload={name:$('camera-name').value,camera_profile:$('camera-profile').value,enabled:$('camera-enabled').checked,frame_interval_ms:Number($('camera-interval').value),output_mode:$('camera-output').value,jpeg_quality:Number($('camera-quality').value),max_width:Number($('camera-width').value),max_height:Number($('camera-height').value),retention_days:Number($('camera-retention').value),max_saved_frames:Number($('camera-max-frames').value)};
    if(!editing)payload.camera_id=id;
    await api(editing?`/cameras/${id}`:'/cameras',{method:editing?'PATCH':'POST',headers:headers(true,editing?Number($('camera-version').value):null),body:JSON.stringify(payload)});
    $('camera-dialog').close(); await loadCameras(); notify(editing?'摄像头已更新，抽帧线程替换命令已提交。':'摄像头已创建，抽帧线程启动命令已提交。');
  }
  async function cameraAction(target) {
    const id=target.dataset.cameraStatus||target.dataset.cameraHistory||target.dataset.cameraFrame||target.dataset.cameraEdit||target.dataset.cameraToggle||target.dataset.cameraDelete;
    if(target.dataset.cameraStatus){const result=await api(`/cameras/${id}/status`,{headers:headers()});$('camera-detail').className='detail mono';$('camera-detail').textContent=JSON.stringify(result,null,2);}
    else if(target.dataset.cameraHistory){const result=await api(`/cameras/${id}/runs?limit=20`,{headers:headers()});$('camera-detail').className='detail mono';$('camera-detail').textContent=JSON.stringify(result,null,2);}
    else if(target.dataset.cameraFrame){const response=await fetch(`/api/v1/cameras/${id}/latest-frame`,{headers:headers()});if(!response.ok){const value=await response.json();throw new Error(value.error||value.error_code);}if(state.frameUrl)URL.revokeObjectURL(state.frameUrl);state.frameUrl=URL.createObjectURL(await response.blob());$('latest-frame').src=state.frameUrl;$('frame-dialog').showModal();}
    else if(target.dataset.cameraEdit){const result=await api(`/cameras/${id}`,{headers:headers()});openCamera(result.camera);}
    else if(target.dataset.cameraToggle){await api(`/cameras/${id}/${target.dataset.active==='true'?'stop':'start'}`,{method:'POST',headers:headers()});await loadCameras();notify('抽帧线程控制命令已提交。');}
    else if(target.dataset.cameraDelete){if(!confirm(`确认删除摄像头 ${id}？对应抽帧线程将停止并回收，Run 历史保留。`))return;const camera=state.cameras.find(c=>c.camera_id===id);await api(`/cameras/${id}`,{method:'DELETE',headers:headers(false,camera.version)});await loadCameras();notify('摄像头删除和线程停止命令已提交。');}
  }

  document.addEventListener('click',async event=>{const target=event.target.closest('button');if(!target)return;try{if(target.dataset.tab)showTab(target.dataset.tab);else if(target.id==='connect')await connect();else if(target.id==='new-camera')openCamera();else if(target.dataset.close)$(target.dataset.close).close();else if(target.dataset.refresh)await refresh(target.dataset.refresh);else if(Object.keys(target.dataset).some(key=>key.startsWith('camera')))await cameraAction(target);}catch(error){notify(error.message,true);}});
  $('camera-form').addEventListener('submit',event=>saveCamera(event).catch(error=>notify(error.message,true)));
})();
