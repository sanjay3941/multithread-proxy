'use strict';
const $ = id => document.getElementById(id);
const formatter = new Intl.NumberFormat();
let latest = null, busy = false, lastRules = '';
const text = (id, value) => { $(id).textContent = value; };
function bytes(value) {
  const units = ['B', 'KiB', 'MiB', 'GiB']; let i = 0;
  while (value >= 1024 && i < units.length - 1) { value /= 1024; i++; }
  return `${value.toFixed(i ? 1 : 0)} ${units[i]}`;
}
function duration(seconds) {
  if (seconds < 60) return `${seconds}s`;
  if (seconds < 3600) return `${Math.floor(seconds / 60)}m`;
  return `${Math.floor(seconds / 3600)}h ${Math.floor(seconds % 3600 / 60)}m`;
}
async function api(path, fields) {
  const controller = new AbortController(); const timer = setTimeout(() => controller.abort(), 4000);
  try {
    const headers = fields ? {'X-Proxy-Dashboard': '1'} : {};
    const response = await fetch(path, {method: fields ? 'POST' : 'GET', headers,
      body: fields ? new URLSearchParams(fields) : undefined, cache: 'no-store', signal: controller.signal});
    const result = await response.json();
    if (!response.ok) {
      throw new Error(result.error || 'Request failed.');
    }
    return result;
  } finally { clearTimeout(timer); }
}
function renderRules(force = false) {
  if (!latest) return;
  const p = latest.privacy, search = $('search').value.trim().toLowerCase();
  const identity = JSON.stringify([p.blocklist, p.allowlist, search]);
  if (!force && lastRules === identity) return;
  lastRules = identity; $('saved-rules').replaceChildren();
  text('rule-count', p.blocklist.length + p.allowlist.length);
  let shown = 0;
  for (const [kind, domains] of [['block', p.blocklist], ['allow', p.allowlist]]) {
    for (const domain of domains.filter(d => d.includes(search))) {
      shown++;
      const row = document.createElement('div'); row.className = 'rule';
      const label = document.createElement('span'); label.className = 'domain'; label.textContent = domain;
      const badge = document.createElement('span'); badge.className = `kind ${kind}`;
      badge.textContent = kind === 'block' ? 'BLOCK' : 'ALLOW';
      const remove = document.createElement('button'); remove.type = 'button'; remove.textContent = 'Remove';
      remove.setAttribute('aria-label', `Remove ${kind} rule for ${domain}`);
      remove.addEventListener('click', () => mutate('/api/rules', {action: 'remove', list: kind, domain}, `Removed ${domain}.`));
      row.append(label, badge, remove); $('saved-rules').append(row);
    }
  }
  if (!shown) {
    const empty = document.createElement('p'); empty.className = 'empty';
    empty.textContent = search ? 'No matching domains.' : 'No custom rules yet. Add your first domain above.';
    $('saved-rules').append(empty);
  }
}
function renderDevices(data) {
  const items = data.items || [];
  text('device-count', items.length);
  $('devices-empty').hidden = items.length > 0;
  text('devices-limit', data.untracked ? `${formatter.format(data.untracked)} finished connections were not split by IP because the 256-client session limit was reached.` : 'Up to 256 client IPs per session. Dashboard requests are excluded.');
  const rows = items.map(device => {
    const row = document.createElement('tr');
    const identity = document.createElement('td');
    const ip = document.createElement('strong'); ip.textContent = device.ip;
    const name = document.createElement('small');
    name.textContent = device.name ? `Network hostname: ${device.name}` : device.nameStatus === 'pending' ? 'Looking up hostname…' : 'Hostname unavailable';
    identity.append(ip, name); row.append(identity);
    for (const value of [device.active, device.total, device.successful, device.failed]) {
      const cell = document.createElement('td'); cell.textContent = formatter.format(value); row.append(cell);
    }
    const blocked = document.createElement('td'); blocked.textContent = formatter.format(device.blocked);
    const categories = document.createElement('small'); categories.textContent = `${device.ads} ads · ${device.trackers} trackers · ${device.custom} custom`;
    blocked.append(categories); row.append(blocked);
    const traffic = document.createElement('td'); traffic.textContent = bytes(device.received + device.sent); row.append(traffic);
    const activity = document.createElement('td'); activity.textContent = device.active ? 'In use' : `${duration(device.lastSeenSeconds)} ago`; row.append(activity);
    return row;
  });
  $('device-rows').replaceChildren(...rows);
}
function render(data) {
  latest = data; const p = data.privacy, c = data.connections;
  $('offline').hidden = true;
  text('connection', 'Proxy reachable'); $('connection').className = 'connection live';
  const blocked = p.adsBlocked + p.trackersBlocked + p.customBlocked;
  for (const [id, value] of Object.entries({'blocked-total': blocked, 'ads-count': p.adsBlocked,
    'trackers-count': p.trackersBlocked, 'custom-count': p.customBlocked, active: c.active,
    successful: c.successful, failed: c.failed, 'blocked-small': c.blocked})) text(id, formatter.format(value));
  text('uptime', duration(p.uptime));
  const protection = p.adsEnabled && p.trackersEnabled ? 'Ad and tracker protection enabled' :
    p.adsEnabled || p.trackersEnabled ? 'One built-in protection is paused' : 'Built-in protections paused';
  text('protection-state', protection);
  text('latency', p.connectMs === null ? '—' : `${Math.round(p.connectMs)} ms`);
  const completed = c.successful + c.failed;
  text('success-rate', completed ? `${Math.round(c.successful / completed * 100)}%` : '—');
  text('health-title', !completed && !p.originConnections ? 'Waiting for traffic' : c.failed ? 'Some connections failed' : 'Connections are completing');
  text('health-badge', completed ? `${formatter.format(completed)} completed attempts` : 'No completed attempts');
  text('health-description', c.failed ? 'Failures include timeouts, invalid requests and connection errors. Blocked attempts are counted separately.' :
    'Measured from traffic using this proxy. No background internet probes are sent.');
  const finished = completed + c.blocked;
  $('bar-success').style.width = `${finished ? c.successful / finished * 100 : 0}%`;
  $('bar-blocked').style.width = `${finished ? c.blocked / finished * 100 : 0}%`;
  $('bar-failed').style.width = `${finished ? c.failed / finished * 100 : 0}%`;
  text('cache-bytes', bytes(p.bytesSaved));
  text('cache-description', `${formatter.format(p.cacheHits)} cached responses served without another origin fetch`);
  text('logging-state', p.detailedLogs ? 'Debug logs enabled' : 'Off by default');
  text('logging-description', p.detailedLogs ? 'Detailed console and disk logs include client addresses, destinations and paths. Restart without PROXY_DETAILED_LOGS=1 to minimize logs.' :
    'Default logs omit client addresses, destinations and URL paths. No request log file is written; older logs are not deleted.');
  $('toggle-ads').checked = p.adsEnabled; $('toggle-trackers').checked = p.trackersEnabled;
  const addresses = data.bindAddresses || [data.bindAddress];
  const network = addresses.filter(address => address !== '127.0.0.1');
  text('config-local', addresses.includes('127.0.0.1') ? `Local proxy: 127.0.0.1 · Port ${data.proxyPort}` : 'Local proxy listener is not configured.');
  text('config-dashboard', data.dashboardPort ? `Direct dashboard: http://127.0.0.1:${data.dashboardPort}` : 'Direct dashboard listener is disabled.');
  text('config-network', network.length ? `Server IP: ${network.join(', ')} · Port ${data.proxyPort}` : 'No network listener. eth2 may be disconnected or a bind override is active.');
  text('proxy-address', data.bindAddress === '0.0.0.0' ?
    `Use the server's reachable IP address, port ${data.proxyPort}. Dashboard: http://proxy.internal` :
    `Proxy address: ${data.bindAddress}:${data.proxyPort}. Dashboard: http://proxy.internal`);
  if (!$('defaults').childElementCount) {
    for (const [category, domains] of [['Ads', p.defaultAds], ['Trackers', p.defaultTrackers]]) {
      for (const domain of domains) {
        const row = document.createElement('p'); row.textContent = domain;
        const label = document.createElement('span'); label.textContent = category; row.append(label); $('defaults').append(row);
      }
    }
  }
  renderRules();
  renderDevices(data.devices || {});
}
async function refresh() {
  try { render(await api('/api/status')); }
  catch (error) {
    text('connection', 'Dashboard disconnected'); $('connection').className = 'connection error';
    $('offline').hidden = false;
  }
}
async function mutate(path, fields, success) {
  if (busy) return;
  busy = true;
  document.querySelectorAll('#rules button, #rules input[type=checkbox]').forEach(node => { node.disabled = true; });
  text('message', 'Saving…'); $('message').className = 'message';
  try {
    await api(path, fields); text('message', success);
    if (fields.action === 'add') $('domain').value = '';
  } catch (error) { text('message', error.name === 'AbortError' ? 'Connection timed out. Refresh to check whether the change was saved.' : error.message); $('message').className = 'message error'; }
  finally {
    await refresh(); busy = false;
    document.querySelectorAll('#rules button, #rules input[type=checkbox]').forEach(node => { node.disabled = false; });
  }
}
$('rule-form').addEventListener('submit', event => {
  event.preventDefault(); mutate('/api/rules', {action: 'add', list: $('rule-kind').value, domain: $('domain').value.trim()}, 'Rule saved. New requests use it immediately.');
});
for (const category of ['ads', 'trackers']) $('toggle-' + category).addEventListener('change', event =>
  mutate('/api/settings', {category, enabled: event.target.checked ? '1' : '0'}, 'Protection setting saved.'));
$('search').addEventListener('input', () => renderRules());
refresh(); setInterval(() => { if (!busy && !document.hidden) refresh(); }, 2500);
