(() => {
  'use strict';

  const state = {
    token: '',
    cameras: [],
    profiles: [],
    activeTab: 'cameras',
    frameUrl: '',
    analysisUrl: '',
    monitorTimer: 0
  };
  const $ = id => document.getElementById(id);
  const activeStatuses = new Set([
    'queued', 'starting', 'running', 'reconnecting', 'stopping'
  ]);
  const escapeHtml = value => String(value ?? '').replace(
    /[&<>"']/g,
    character => ({
      '&': '&amp;', '<': '&lt;', '>': '&gt;',
      '"': '&quot;', "'": '&#39;'
    })[character]
  );

  function notify(message, error = false) {
    const node = $('notice');
    node.textContent = message;
    node.className = `notice${error ? ' error' : ''}`;
    clearTimeout(notify.timer);
    notify.timer = setTimeout(
      () => node.classList.add('hidden'),
      7000
    );
  }

  function headers(json = false, version = null) {
    const result = {Authorization: `Bearer ${state.token}`};
    if (json) result['Content-Type'] = 'application/json';
    if (version !== null) result['If-Match'] = `"${version}"`;
    return result;
  }

  async function api(path, options = {}) {
    const response = await fetch(`/api/v1${path}`, options);
    if (response.status === 204) return null;
    const contentType = response.headers.get('content-type') || '';
    const body = contentType.includes('json')
      ? await response.json()
      : await response.text();
    if (!response.ok) {
      throw new Error(
        body.error || body.error_code ||
        `${response.status} ${response.statusText}`
      );
    }
    return body;
  }

  function badge(status) {
    const value = String(status || 'unknown');
    const kind = ['running', 'ready', 'ok', 'stopped'].includes(value)
      ? 'ok'
      : ['failed', 'error', 'dead_letter'].includes(value)
        ? 'bad' : 'warn';
    return `<span class="badge ${kind}">${escapeHtml(value)}</span>`;
  }

  function number(value, digits = 0) {
    const parsed = Number(value || 0);
    return Number.isFinite(parsed) ? parsed.toFixed(digits) : '0';
  }

  function time(value) {
    const timestamp = Number(value || 0);
    return timestamp > 0
      ? new Date(timestamp).toLocaleString()
      : '—';
  }

  function currentCamera(id) {
    return state.cameras.find(camera => camera.camera_id === id);
  }

  function syncCameraSelectors() {
    const options = state.cameras.map(camera =>
      `<option value="${escapeHtml(camera.camera_id)}">` +
      `${escapeHtml(camera.name)} (${escapeHtml(camera.camera_id)})` +
      '</option>'
    ).join('');
    for (const selector of [$('monitor-camera'), $('alert-camera')]) {
      const selected = selector.value;
      selector.innerHTML = options ||
        '<option value="">暂无摄像头</option>';
      if (state.cameras.some(camera => camera.camera_id === selected)) {
        selector.value = selected;
      }
    }
  }

  function showTab(id) {
    state.activeTab = id;
    document.querySelectorAll('.tab-panel').forEach(node =>
      node.classList.toggle('active', node.id === id)
    );
    document.querySelectorAll('.tabs button').forEach(node =>
      node.classList.toggle('active', node.dataset.tab === id)
    );
    scheduleMonitoring();
    refresh(id).catch(error => notify(error.message, true));
  }

  async function connect() {
    const token = $('token').value;
    if (!token) {
      notify('请输入管理员 Token。', true);
      return;
    }
    state.token = token;
    await Promise.all([loadProfiles(), loadCameras()]);
    await Promise.all([loadHubs(), loadMonitoring(), loadAlerts()]);
    $('connection').className = 'badge ok';
    $('connection').textContent = '已连接';
    notify('控制台已连接。Token 仅保存在当前页面内存。');
    scheduleMonitoring();
  }

  async function loadCameras() {
    const result = await api('/cameras', {headers: headers()});
    state.cameras = result.items || [];
    $('camera-rows').innerHTML = state.cameras.length
      ? state.cameras.map(camera => {
        const id = camera.camera_id;
        const status = camera.current_run?.status ||
          (camera.enabled ? 'idle' : 'stopped');
        const active = activeStatuses.has(status);
        const analysis = camera.analysis || {};
        return `<tr>
          <td><strong>${escapeHtml(camera.name)}</strong><br>
            <span class="muted mono">${escapeHtml(id)}</span></td>
          <td>${escapeHtml(camera.camera_profile)}</td>
          <td>${camera.frame_interval_ms} ms<br>
            <span class="muted">${escapeHtml(camera.output_mode)}</span></td>
          <td>${analysis.enabled ? '已启用' : '已停用'}<br>
            <span class="muted">${escapeHtml(
              (analysis.algorithms || []).join(', ') || '—'
            )}</span></td>
          <td>${badge(status)}</td>
          <td>${camera.version}</td>
          <td><div class="row-actions">
            <button class="small" data-camera-monitor="${escapeHtml(id)}">监控</button>
            <button class="small" data-camera-status="${escapeHtml(id)}">状态</button>
            <button class="small" data-camera-history="${escapeHtml(id)}">历史</button>
            <button class="small" data-camera-frame="${escapeHtml(id)}">原始帧</button>
            <button class="small" data-camera-edit="${escapeHtml(id)}">修改</button>
            <button class="small ${active ? '' : 'primary'}"
              data-camera-toggle="${escapeHtml(id)}"
              data-active="${active}">${active ? '停止' : '启动'}</button>
            <button class="small danger" data-camera-delete="${escapeHtml(id)}">删除</button>
          </div></td>
        </tr>`;
      }).join('')
      : '<tr><td colspan="7" class="empty-row">暂无摄像头实例</td></tr>';
    syncCameraSelectors();
  }

  async function loadProfiles() {
    const result = await api('/camera-profiles', {headers: headers()});
    state.profiles = result.items || [];
    $('profile-rows').innerHTML = state.profiles.length
      ? state.profiles.map(profile => `<tr>
          <td class="mono">${escapeHtml(profile.profile_id)}</td>
          <td>${escapeHtml(profile.display_name)}</td>
          <td class="mono">${escapeHtml(profile.url_env)}</td>
          <td>${escapeHtml(profile.transport)}</td>
          <td>${profile.enabled ? '是' : '否'}</td>
        </tr>`).join('')
      : '<tr><td colspan="5" class="empty-row">暂无可用 Profile</td></tr>';
    $('camera-profile').innerHTML = state.profiles
      .filter(profile => profile.enabled)
      .map(profile =>
        `<option value="${escapeHtml(profile.profile_id)}">` +
        `${escapeHtml(profile.display_name)} ` +
        `(${escapeHtml(profile.profile_id)})</option>`
      ).join('');
  }

  async function loadHubs() {
    const result = await api('/camera-hubs', {headers: headers()});
    $('hub-cards').innerHTML = result.items?.length
      ? result.items.map(hub => `<article class="card">
          <h3>${escapeHtml(hub.camera_profile)} ${badge(hub.state)}</h3>
          <div class="metric"><span>Hub ID</span><strong class="mono">${escapeHtml(hub.hub_instance_id)}</strong></div>
          <div class="metric"><span>打开次数</span><strong>${hub.open_count}</strong></div>
          <div class="metric"><span>订阅者</span><strong>${hub.subscriber_count}</strong></div>
          <div class="metric"><span>订阅类型</span><strong>${escapeHtml(JSON.stringify(hub.subscriber_types))}</strong></div>
          <div class="metric"><span>采集 FPS</span><strong>${number(hub.capture_fps, 2)}</strong></div>
          <div class="metric"><span>重连</span><strong>${hub.reconnect_count}</strong></div>
        </article>`).join('')
      : '<div class="detail empty">当前没有活跃 Hub。</div>';
  }

  function renderKpis(status) {
    const analysis = status.analysis || {};
    const metrics = [
      ['当前人数', analysis.occupancy ?? 0],
      ['初始人数', analysis.initial_occupancy ?? 0],
      ['累计进入', analysis.in_count ?? 0],
      ['累计离开', analysis.out_count ?? 0],
      ['画面人数', analysis.live_persons ?? 0],
      ['推理 FPS', number(analysis.infer_fps, 2)],
      ['Run 状态', status.status || 'unknown']
    ];
    $('monitor-kpis').innerHTML = metrics.map(([label, value]) =>
      `<article class="kpi"><span>${escapeHtml(label)}</span>` +
      `<strong>${escapeHtml(value)}</strong></article>`
    ).join('');
  }

  function renderPhases(status) {
    const stages = status.analysis?.security?.stages || {};
    $('phase-cards').innerHTML = ['phase1', 'phase2', 'phase3', 'phase4']
      .map((phase, index) => {
        const value = stages[phase] || {};
        const ready = Boolean(value.ready);
        const detail = Object.entries(value)
          .filter(([key]) => key !== 'ready')
          .slice(0, 4)
          .map(([key, item]) =>
            `${escapeHtml(key)}: ${escapeHtml(
              typeof item === 'object' ? JSON.stringify(item) : item
            )}`
          ).join('<br>') || '等待算法状态';
        return `<article class="phase-card ${ready ? 'ready' : ''}">
          <div><span>PHASE ${index + 1}</span>${badge(ready ? 'ready' : 'waiting')}</div>
          <h3>${escapeHtml(value.name || phase)}</h3>
          <p>${detail}</p>
        </article>`;
      }).join('');
  }

  async function loadAuthenticatedImage(path, element, placeholder, key) {
    const response = await fetch(`/api/v1${path}`, {headers: headers()});
    if (!response.ok) {
      element.removeAttribute('src');
      element.classList.add('hidden');
      placeholder.classList.remove('hidden');
      return;
    }
    if (state[key]) URL.revokeObjectURL(state[key]);
    state[key] = URL.createObjectURL(await response.blob());
    element.src = state[key];
    element.classList.remove('hidden');
    placeholder.classList.add('hidden');
  }

  async function loadMonitoring() {
    if (!state.token) return;
    const cameraId = $('monitor-camera').value;
    if (!cameraId) return;
    const status = await api(
      `/cameras/${encodeURIComponent(cameraId)}/status`,
      {headers: headers()}
    );
    renderKpis(status);
    renderPhases(status);
    const summary = {
      camera_id: status.camera_id,
      run_id: status.run_id,
      status: status.status,
      runtime_stale: status.runtime_stale,
      analysis_state: status.analysis?.state,
      analysis_runtime_stale: status.analysis?.runtime_stale,
      config_version: status.analysis?.config_version,
      reconnect_count: status.analysis?.reconnect_count,
      warmup_frames_remaining: status.analysis?.warmup_frames_remaining,
      capture: status.hub,
      pipeline: status.pipeline
    };
    $('monitor-detail').className = 'detail mono';
    $('monitor-detail').textContent = JSON.stringify(summary, null, 2);
    $('monitor-updated').textContent = `更新于 ${new Date().toLocaleTimeString()}`;
    await loadAuthenticatedImage(
      `/cameras/${encodeURIComponent(cameraId)}/analysis-snapshot`,
      $('analysis-snapshot'),
      $('snapshot-placeholder'),
      'analysisUrl'
    );
  }

  function scheduleMonitoring() {
    clearInterval(state.monitorTimer);
    state.monitorTimer = 0;
    if (state.activeTab === 'monitoring' &&
        $('monitor-auto').checked && state.token) {
      state.monitorTimer = setInterval(() => {
        loadMonitoring().catch(error => notify(error.message, true));
      }, 2000);
    }
  }

  async function loadAlerts() {
    if (!state.token) return;
    const cameraId = $('alert-camera').value;
    if (!cameraId) return;
    const query = new URLSearchParams({
      minimum_severity: $('alert-severity').value,
      limit: '100',
      offset: '0'
    });
    const eventType = $('alert-type').value.trim();
    if (eventType) query.set('event_type', eventType);
    const result = await api(
      `/cameras/${encodeURIComponent(cameraId)}/alerts?${query}`,
      {headers: headers()}
    );
    $('alert-rows').innerHTML = result.items?.length
      ? result.items.map(alert => `<tr>
          <td>${escapeHtml(time(alert.occurred_at_ms))}</td>
          <td><strong>${escapeHtml(alert.event_type)}</strong><br>
            <span class="muted">${escapeHtml(alert.category)}</span></td>
          <td><span class="severity severity-${Number(alert.severity || 1)}">${Number(alert.severity || 1)}</span></td>
          <td>${escapeHtml(alert.track_id ?? '—')}</td>
          <td>${escapeHtml(alert.algorithm?.profile || '—')}<br>
            <span class="muted">${escapeHtml(alert.algorithm?.config_version || '')}</span></td>
          <td>${badge(alert.delivery?.status || 'not_scheduled')}</td>
          <td class="mono">${escapeHtml(alert.run_id)}</td>
        </tr>`).join('')
      : '<tr><td colspan="7" class="empty-row">当前筛选条件下没有告警</td></tr>';
  }

  async function loadOperations() {
    const [health, ready, metrics] = await Promise.all([
      fetch('/api/v1/health').then(response => response.json()),
      fetch('/api/v1/ready').then(response => response.json()),
      state.token
        ? api('/operations/metrics', {headers: headers()}).catch(() => null)
        : Promise.resolve(null)
    ]);
    $('operations-content').innerHTML = `
      <article class="card"><h3>健康 ${badge(health.success ? 'ok' : 'error')}</h3>
        <pre class="mono">${escapeHtml(JSON.stringify(health, null, 2))}</pre></article>
      <article class="card"><h3>就绪 ${badge(ready.ready ? 'ready' : 'error')}</h3>
        <pre class="mono">${escapeHtml(JSON.stringify(ready, null, 2))}</pre></article>
      <article class="card"><h3>运维指标</h3>
        <pre class="mono">${escapeHtml(JSON.stringify(metrics || {}, null, 2))}</pre></article>`;
  }

  async function refresh(id) {
    if (!state.token && id !== 'operations') return;
    if (id === 'cameras') return loadCameras();
    if (id === 'monitoring') return loadMonitoring();
    if (id === 'alerts') return loadAlerts();
    if (id === 'profiles') return loadProfiles();
    if (id === 'hubs') return loadHubs();
    if (id === 'operations') return loadOperations();
  }

  function selectedAlgorithms() {
    return [...document.querySelectorAll(
      'input[name="algorithm"]:checked'
    )].map(input => input.value);
  }

  function openCamera(camera = null) {
    const analysis = camera?.analysis || {};
    $('camera-dialog-title').textContent = camera
      ? '修改摄像头（将替换现有 Run）' : '新增摄像头';
    $('camera-id').value = camera?.camera_id || '';
    $('camera-id').disabled = Boolean(camera);
    $('camera-version').value = camera?.version || '';
    $('camera-name').value = camera?.name || '';
    $('camera-profile').value = camera?.camera_profile ||
      state.profiles[0]?.profile_id || '';
    $('camera-interval').value = camera?.frame_interval_ms ?? 1000;
    $('camera-output').value = camera?.output_mode || 'latest';
    $('camera-quality').value = camera?.jpeg_quality ?? 90;
    $('camera-width').value = camera?.max_width ?? 0;
    $('camera-height').value = camera?.max_height ?? 0;
    $('camera-retention').value = camera?.retention_days ?? 7;
    $('camera-max-frames').value = camera?.max_saved_frames ?? 100000;
    $('camera-enabled').checked = camera?.enabled ?? true;
    $('analysis-enabled').checked = analysis.enabled ?? true;
    $('analysis-fps').value = analysis.target_infer_fps ?? 5;
    $('algorithm-profile').value =
      analysis.algorithm_profile || 'security_default';
    $('callback-profile').value = camera?.callback_profile || '';
    const algorithms = new Set(
      analysis.algorithms || ['people_flow', 'security']
    );
    document.querySelectorAll('input[name="algorithm"]').forEach(input => {
      input.checked = algorithms.has(input.value);
    });
    $('camera-dialog').showModal();
  }

  async function saveCamera(event) {
    event.preventDefault();
    const editing = $('camera-id').disabled;
    const id = $('camera-id').value;
    const algorithms = selectedAlgorithms();
    if ($('analysis-enabled').checked && !algorithms.length) {
      throw new Error('启用算法分析时至少选择一个算法。');
    }
    const payload = {
      name: $('camera-name').value,
      camera_profile: $('camera-profile').value,
      enabled: $('camera-enabled').checked,
      frame_interval_ms: Number($('camera-interval').value),
      output_mode: $('camera-output').value,
      jpeg_quality: Number($('camera-quality').value),
      max_width: Number($('camera-width').value),
      max_height: Number($('camera-height').value),
      retention_days: Number($('camera-retention').value),
      max_saved_frames: Number($('camera-max-frames').value),
      analysis: {
        enabled: $('analysis-enabled').checked,
        target_infer_fps: Number($('analysis-fps').value),
        algorithm_profile: $('algorithm-profile').value,
        algorithms
      },
      callback_profile: $('callback-profile').value
    };
    if (!editing) payload.camera_id = id;
    await api(
      editing ? `/cameras/${encodeURIComponent(id)}` : '/cameras',
      {
        method: editing ? 'PATCH' : 'POST',
        headers: headers(
          true,
          editing ? Number($('camera-version').value) : null
        ),
        body: JSON.stringify(payload)
      }
    );
    $('camera-dialog').close();
    await loadCameras();
    notify(editing
      ? '摄像头与算法配置已更新，替换 Run 命令已提交。'
      : '摄像头已创建，统一 Camera Run 启动命令已提交。'
    );
  }

  async function cameraAction(target) {
    const id = target.dataset.cameraMonitor ||
      target.dataset.cameraStatus ||
      target.dataset.cameraHistory ||
      target.dataset.cameraFrame ||
      target.dataset.cameraEdit ||
      target.dataset.cameraToggle ||
      target.dataset.cameraDelete;
    if (target.dataset.cameraMonitor) {
      $('monitor-camera').value = id;
      showTab('monitoring');
    } else if (target.dataset.cameraStatus) {
      const result = await api(
        `/cameras/${encodeURIComponent(id)}/status`,
        {headers: headers()}
      );
      $('camera-detail').className = 'detail mono';
      $('camera-detail').textContent = JSON.stringify(result, null, 2);
    } else if (target.dataset.cameraHistory) {
      const result = await api(
        `/cameras/${encodeURIComponent(id)}/runs?limit=20`,
        {headers: headers()}
      );
      $('camera-detail').className = 'detail mono';
      $('camera-detail').textContent = JSON.stringify(result, null, 2);
    } else if (target.dataset.cameraFrame) {
      await loadAuthenticatedImage(
        `/cameras/${encodeURIComponent(id)}/latest-frame`,
        $('latest-frame'),
        document.createElement('span'),
        'frameUrl'
      );
      $('frame-dialog').showModal();
    } else if (target.dataset.cameraEdit) {
      const result = await api(
        `/cameras/${encodeURIComponent(id)}`,
        {headers: headers()}
      );
      openCamera(result.camera);
    } else if (target.dataset.cameraToggle) {
      await api(
        `/cameras/${encodeURIComponent(id)}/` +
          (target.dataset.active === 'true' ? 'stop' : 'start'),
        {method: 'POST', headers: headers()}
      );
      await loadCameras();
      notify('Camera Pipeline 控制命令已提交。');
    } else if (target.dataset.cameraDelete) {
      if (!confirm(
        `确认删除摄像头 ${id}？当前 Run 将停止，历史数据保留。`
      )) return;
      const camera = currentCamera(id);
      await api(`/cameras/${encodeURIComponent(id)}`, {
        method: 'DELETE',
        headers: headers(false, camera.version)
      });
      await loadCameras();
      notify('摄像头删除和 Pipeline 停止命令已提交。');
    }
  }

  document.addEventListener('click', async event => {
    const target = event.target.closest('button');
    if (!target) return;
    try {
      if (target.dataset.tab) showTab(target.dataset.tab);
      else if (target.id === 'connect') await connect();
      else if (target.id === 'new-camera') openCamera();
      else if (target.dataset.close) $(target.dataset.close).close();
      else if (target.dataset.refresh) {
        await refresh(target.dataset.refresh);
      } else if (Object.keys(target.dataset).some(
        key => key.startsWith('camera')
      )) {
        await cameraAction(target);
      }
    } catch (error) {
      notify(error.message, true);
    }
  });

  $('camera-form').addEventListener('submit', event =>
    saveCamera(event).catch(error => notify(error.message, true))
  );
  $('monitor-camera').addEventListener('change', () =>
    loadMonitoring().catch(error => notify(error.message, true))
  );
  $('monitor-auto').addEventListener('change', scheduleMonitoring);
  $('alert-camera').addEventListener('change', () =>
    loadAlerts().catch(error => notify(error.message, true))
  );
})();
