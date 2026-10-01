const $ = id => document.getElementById(id);
const cards = Array.from({length: 3}, (_, index) => {
  const section = document.createElement('section');
  section.className = 'panel';
  section.innerHTML = `<div class="heading"><h2>센서 ${index + 1}</h2><span class="badge" role="status">연결 대기</span></div>
    <p class="detail">MCU 연결 대기 중</p><div class="grid stale" aria-label="센서 ${index + 1}의 64개 영역 거리"></div>
    <dl class="stats"><dt>I²C 주소</dt><dd>0x${(0x30 + index).toString(16).toUpperCase()}</dd>
      <dt>LPn 핀</dt><dd>D${index + 2}</dd><dt>수신 프레임</dt><dd class="seq">0</dd>
      <dt>유효 영역</dt><dd class="valid">—</dd><dt>최소 거리</dt><dd class="near">—</dd>
      <dt>최대 거리</dt><dd class="far">—</dd><dt>최근 수신</dt><dd class="age">—</dd></dl>
    <div class="actions"><button class="download" disabled>현재 프레임 CSV 저장</button></div>`;
  $('cards').append(section);
  const grid = section.querySelector('.grid');
  const cells = Array.from({length: 64}, () => {
    const cell = document.createElement('div');
    cell.className = 'cell'; cell.textContent = '—'; grid.append(cell);
    return cell;
  });
  return {section, grid, cells, badge: section.querySelector('.badge'),
    detail: section.querySelector('.detail'), download: section.querySelector('.download')};
});

let latest = null;
function renderSensor(data, card) {
  const live = data.live;
  const labels = {error: '통신 오류', initializing: '초기화 중', checking: 'I²C 확인 중',
    waiting: '연결 대기', measuring: '데이터 대기'};
  card.badge.textContent = live ? '● 실시간 측정' : (labels[data.phase] || data.phase);
  card.badge.className = 'badge' + (live ? ' live' : '');
  card.detail.textContent = data.detail + (data.error_code ? ` (오류 ${data.error_code})` : '');
  card.grid.classList.toggle('stale', !live);
  const values = data.distance_mm.filter(v => v !== null);
  const set = (selector, value) => card.section.querySelector(selector).textContent = value;
  set('.seq', data.seq);
  set('.valid', live ? `${values.length} / 64` : '—');
  set('.near', live && values.length ? `${Math.min(...values)} mm` : '—');
  set('.far', live && values.length ? `${Math.max(...values)} mm` : '—');
  set('.age', data.frame_age_s === null ? '—' : `${data.frame_age_s.toFixed(1)}초 전`);
  card.download.disabled = !live;
  const max = Number($('range').value);
  card.cells.forEach((cell, i) => {
    const zone = $('flip').checked ? Math.floor(i / 8) * 8 + 7 - i % 8 : i;
    const value = data.distance_mm[zone];
    cell.textContent = value === null ? '—' : value;
    cell.style.background = value === null ? '#273345' : `hsl(${Math.min(1, value / max) * 240} 75% 38%)`;
    cell.title = `영역 ${zone} · ${value === null ? '측정 없음' : value + ' mm'} · 상태 ${data.target_status[zone]}`;
  });
}

function render(data) {
  const max = Number($('range').value);
  $('scaleMax').textContent = `멀어짐 · ${max} mm`;
  data.sensors.forEach((sensor, index) => renderSensor(sensor, cards[index]));
  $('warning').textContent = data.sensors.every(sensor => sensor.mcu_age_s === null || sensor.mcu_age_s > 6)
    ? 'MCU 상태를 수신하지 못하고 있습니다.' : '';
}

async function poll() {
  try {
    const response = await fetch('/api/status', {cache: 'no-store', signal: AbortSignal.timeout(2500)});
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const data = await response.json();
    if (!Array.isArray(data.sensors) || data.sensors.length !== 3) throw new Error('Invalid sensor data');
    latest = data;
    render(data);
  } catch (error) {
    $('warning').textContent = '서버 연결이 끊겼습니다. 앱 실행 상태와 네트워크를 확인하세요.';
    cards.forEach(card => {
      card.badge.textContent = '서버 연결 끊김'; card.badge.className = 'badge';
      card.grid.classList.add('stale'); card.download.disabled = true;
    });
    if (latest) latest.sensors.forEach(sensor => { sensor.live = false; });
  } finally {
    setTimeout(poll, 200);
  }
}

$('range').onchange = $('flip').onchange = () => latest && render(latest);
cards.forEach((card, index) => {
  card.download.onclick = () => {
    const sensor = latest?.sensors[index];
    if (!sensor?.live) return;
    const rows = ['sensor,zone,row,column,distance_mm,target_status'];
    sensor.distance_mm.forEach((value, zone) => rows.push([
      index + 1, zone, Math.floor(zone / 8), zone % 8,
      value ?? '', sensor.target_status[zone]
    ].join(',')));
    const url = URL.createObjectURL(new Blob([rows.join('\n')], {type: 'text/csv;charset=utf-8'}));
    const link = document.createElement('a');
    link.href = url; link.download = `tof-${index + 1}-${sensor.seq}.csv`; link.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  };
});
poll();
