// Lab web UI. Talks to lab/web/server.py over /ws. All test logic stays in Python.
"use strict";

const $ = (id) => document.getElementById(id);
const STATUS_KO = {
  Available: "사용 가능", Preparing: "준비 중", Charging: "충전 중", SuspendedEV: "차량 대기",
  SuspendedEVSE: "충전기 대기", Finishing: "충전 종료", Reserved: "예약됨",
  Unavailable: "사용 불가", Faulted: "고장",
};
const ERR_KO = {
  "ERR connector locked": "충전 중에는 케이블이 잠겨 있어 뽑을 수 없어요. 먼저 카드를 다시 대서 충전을 끝내세요.",
  "ERR a test is running": "시험이 실행 중이라 조작할 수 없어요.",
};

let ws, st = {}, fwOk = false, plugged = false, tests = [], results = {}, running = null;
const actionOfId = {};          // CALL id -> action (to label CALLRESULTs)

// ---------------- connection ----------------
function connect() {
  ws = new WebSocket(`${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/ws`);
  ws.onmessage = (m) => handle(JSON.parse(m.data));
  ws.onclose = () => { fwOk = false; st = {}; renderScene(); setBadge("b-csms", false); setTimeout(connect, 1000); };
}
function send(obj) { if (ws && ws.readyState === 1) ws.send(JSON.stringify(obj)); }
function cmd(c) { send({ t: "cmd", cmd: c }); }

function handle(m) {
  if (m.t === "hello") {
    tests = m.tests; results = m.results || {}; running = m.running || null;
    renderTests();
    $("log").innerHTML = "";
    m.events.forEach(addLog);
  } else if (m.t === "ocpp") {
    addLog(m.ev);
  } else if (m.t === "status") {
    fwOk = m.ok; st = m.st || {};
    setBadge("b-csms", m.csms_connected);
    renderScene();
    if (m.stats) {
      $("metric-count").textContent = m.stats.count;
      $("metric-mean").textContent = m.stats.mean == null ? "—" : m.stats.mean.toFixed(1) + "초";
      $("metric-max").textContent = m.stats.max == null ? "—" : m.stats.max.toFixed(1) + "초";
    }
  } else if (m.t === "reply") {
    const isErr = m.reply.startsWith("ERR");
    toast(isErr ? (ERR_KO[m.reply] || m.reply) : `${m.cmd}: ${m.reply}`, isErr);
    $("remote-reply").textContent = `${m.cmd}: ${m.reply}`;
  } else if (m.t === "test") {
    if (m.state === "running") { running = m.id; results[m.id] = { state: "running" }; }
    else { results[m.id] = m; if (running === m.id) running = null; showResult(m.id); }
    renderTests();
  }
}

// ---------------- scene ----------------
function setBadge(id, on, text) {
  const b = $(id);
  b.classList.toggle("on", !!on);
  b.classList.toggle("off", !on);
  if (text) b.lastChild.textContent = text;
}

function renderScene() {
  setBadge("b-fw", fwOk);
  setBadge("b-boot", st.boot === "Accepted", "부팅: " + (st.boot || "-"));
  const powerOn = fwOk && st.power === "on";
  // "Green only while healthy": LED needs a fresh answer from the firmware AND led=green.
  $("led").classList.toggle("green", fwOk && st.led === "green");
  $("pwr").classList.toggle("on", fwOk ? st.power === "on" : false);

  const screen = $("screen");
  const status = st["c1.status"];
  screen.className = "screen";
  if (!fwOk) {
    $("s-status").textContent = "응답 없음";
    $("s-energy").textContent = "";
    $("s-sub").textContent = "펌웨어가 실행 중인지 확인하세요";
  } else if (!powerOn) {
    screen.classList.add("dark");
  } else {
    if (status === "Charging") screen.classList.add("charging");
    if (status === "Faulted") screen.classList.add("fault");
    $("s-status").textContent = STATUS_KO[status] || status || "-";
    const kwh = (Number(st["c1.wh"] || 0) / 1000).toFixed(2);
    const kw = (Number(st["c1.power_w"] || 0) / 1000).toFixed(1);
    $("s-energy").textContent = `${kwh} kWh` + (status === "Charging" ? ` · ${kw} kW` : "");
    const parts = [];
    if (st["c1.tag"] && st["c1.tag"] !== "-") parts.push(`카드 ${st["c1.tag"]}`);
    if (st["c1.tx"] && st["c1.tx"] !== "-1") parts.push(`거래 #${st["c1.tx"]}`);
    if (st["c1.fault"] && st["c1.fault"] !== "none") parts.push(st["c1.fault"]);
    if (st["c1.noise"] === "on") parts.push(`노이즈 걸러냄 ${st["c1.glitches"] || 0}회`);
    if (st.ws !== "up") parts.push("서버 연결 끊김");
    $("s-sub").textContent = parts.join(" · ") || " ";
  }
  // energy bar: fill relative to 10 kWh, just to show it moving
  $("battery").style.width = Math.min(100, Number(st["c1.wh"] || 0) / 100) + "%";
  plugged = st["c1.cable"] === "1";
  if (!dragging) placePlug();
}

// ---------------- cable: click or drag the plug ----------------
const scene = $("scene"), plug = $("plug");
let dragging = false, moved = false, dragOff = { x: 0, y: 0 };

function rel(el) {
  const s = scene.getBoundingClientRect(), r = el.getBoundingClientRect();
  return { x: r.left - s.left, y: r.top - s.top, w: r.width, h: r.height };
}
function placePlug(x, y) {
  if (x === undefined) {
    const t = plugged ? rel($("port")) : rel($("holster"));
    x = t.x + t.w / 2 - 13; y = t.y + t.h / 2 - 13;
  }
  plug.style.left = x + "px"; plug.style.top = y + "px";
  drawCable(x + 13, y + 13);
}
function drawCable(px, py) {
  const h = rel($("holster"));
  const sx = h.x + 2, sy = h.y + h.h;                   // cable leaves the charger here
  const sag = Math.max(40, Math.abs(px - sx) / 3);
  $("cable-path").setAttribute("d",
    `M ${sx} ${sy} C ${sx} ${sy + sag}, ${px} ${py + sag}, ${px} ${py}`);
}
function near(el, x, y, d) {
  const r = rel(el);
  return Math.hypot(r.x + r.w / 2 - x, r.y + r.h / 2 - y) < d;
}

plug.addEventListener("pointerdown", (e) => {
  if (running || !fwOk) return;
  dragging = true; moved = false;
  plug.setPointerCapture(e.pointerId);
  plug.classList.add("dragging");
  const r = rel(plug), s = scene.getBoundingClientRect();
  dragOff = { x: e.clientX - s.left - r.x, y: e.clientY - s.top - r.y };
});
plug.addEventListener("pointermove", (e) => {
  if (!dragging) return;
  const s = scene.getBoundingClientRect();
  const x = e.clientX - s.left - dragOff.x, y = e.clientY - s.top - dragOff.y;
  if (Math.abs(x - parseFloat(plug.style.left)) + Math.abs(y - parseFloat(plug.style.top)) > 3) moved = true;
  placePlug(x, y);
  $("port").classList.toggle("hover", near($("port"), x + 13, y + 13, 50));
});
plug.addEventListener("pointercancel", () => { dragging = false; plug.classList.remove("dragging"); $("port").classList.remove("hover"); placePlug(); });
plug.addEventListener("keydown", (e) => { if ((e.key === "Enter" || e.key === " ") && fwOk && !running) { e.preventDefault(); cmd(plugged ? "unplug 1" : "plug 1"); } });
plug.addEventListener("pointerup", () => {
  if (!dragging) return;
  dragging = false;
  plug.classList.remove("dragging");
  $("port").classList.remove("hover");
  const x = parseFloat(plug.style.left) + 13, y = parseFloat(plug.style.top) + 13;
  if (!moved) cmd(plugged ? "unplug 1" : "plug 1");            // click = toggle
  else if (near($("port"), x, y, 50) && !plugged) cmd("plug 1");
  else if (!near($("port"), x, y, 50) && plugged) cmd("unplug 1");
  placePlug();                                                  // snap; status update confirms
});

// ---------------- cards: drag onto the reader ----------------
document.querySelectorAll(".rfid").forEach((card) => {
  let start = null;
  card.addEventListener("click", (e) => { if (e.detail === 0 && fwOk && !running) cmd(`card 1 ${card.dataset.tag}`); });
  card.addEventListener("pointerdown", (e) => {
    if (running || !fwOk) return;
    card.setPointerCapture(e.pointerId);
    start = { x: e.clientX, y: e.clientY };
    card.classList.add("dragging");
  });
  card.addEventListener("pointermove", (e) => {
    if (!start) return;
    card.style.transform = `translate(${e.clientX - start.x}px, ${e.clientY - start.y}px)`;
    $("reader").classList.toggle("hover", overReader(e));
  });
  const end = (e) => {
    if (!start) return;
    if (e.type !== "pointercancel" && overReader(e)) {
      cmd(`card 1 ${card.dataset.tag}`);
      const rd = $("reader");
      rd.classList.add("flash-ok");
      setTimeout(() => rd.classList.remove("flash-ok"), 400);
    }
    start = null;
    card.classList.remove("dragging");
    card.style.transform = "";
    $("reader").classList.remove("hover");
  };
  card.addEventListener("pointerup", end);
  card.addEventListener("pointercancel", end);
});
function overReader(e) {
  const r = $("reader").getBoundingClientRect();
  return e.clientX > r.left - 20 && e.clientX < r.right + 20 && e.clientY > r.top - 20 && e.clientY < r.bottom + 20;
}

// ---------------- buttons ----------------
document.querySelectorAll("[data-cmd]").forEach((b) => b.addEventListener("click", () => cmd(b.dataset.cmd)));
$("run-all").addEventListener("click", () => send({ t: "run_all" }));
$("clear-log").addEventListener("click", () => { $("log").innerHTML = ""; });
$("hide-hb").addEventListener("change", () => {
  document.querySelectorAll(".row.hb").forEach((r) => (r.hidden = $("hide-hb").checked));
});

let toastTimer;
function toast(text, isErr) {
  const t = $("toast");
  t.textContent = text; t.classList.toggle("err", !!isErr);
  clearTimeout(toastTimer);
  if (text) toastTimer = setTimeout(() => (t.textContent = ""), 6000);
}

// ---------------- tests ----------------
function renderTests() {
  const g = $("test-grid");
  g.innerHTML = "";
  tests.forEach((t) => {
    const r = results[t.id] || {};
    const b = document.createElement("button");
    b.className = "tbtn " + (r.state || "");
    b.title = t.name;
    const label = { PASS: "통과", FAIL: "실패", ERROR: "오류", "N/A": "해당 없음", running: "실행 중…" }[r.state] || t.name;
    b.innerHTML = `<b>${escapeHtml(t.id.replace(/_CS$/, ""))}</b><span>${escapeHtml(label)}</span>`;
    b.disabled = !!running;
    b.addEventListener("click", () => { if (r.state && r.state !== "running") showResult(t.id); send({ t: "run", id: t.id }); });
    g.appendChild(b);
  });
  const passed = Object.values(results).filter((r) => r.state === "PASS").length;
  $("test-summary").textContent = `${passed} / ${tests.length} 통과`;
  $("run-all").disabled = !!running;
  document.querySelectorAll("[data-cmd]").forEach((b) => (b.disabled = !!running));
  if (running) $("test-detail").innerHTML = `<b>${running}</b> 실행 중… (오른쪽 로그에서 메시지를 볼 수 있어요)`;
}
function showResult(id) {
  const r = results[id], t = tests.find((x) => x.id === id);
  if (!r || r.state === "running") return;
  const head = `<span class="${r.state === "PASS" ? "pass" : "fail"}">${escapeHtml({PASS:"통과",FAIL:"실패",ERROR:"오류","N/A":"해당 없음"}[r.state] || r.state)}</span>`;
  const reason = r.state === "PASS" ? (r.reason || "") : `이유: ${r.reason}`;
  $("test-detail").innerHTML = `<b>${id}</b> ${escapeHtml(t ? t.name : "")}\n${head} ${escapeHtml(reason)}  (${r.secs} 초)`;
}
function escapeHtml(s) { return String(s).replace(/[&<>]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;" }[c])); }

// ---------------- log ----------------
function addLog(ev) {
  if (ev.type === 2) actionOfId[ev.id] = ev.action;
  const action = ev.type === 2 ? ev.action : (actionOfId[ev.id] || "?");
  const kind = { 2: "요청", 3: "응답", 4: "오류" }[ev.type] || "?";
  const fromCp = ev.dir === "cp->cs";
  const row = document.createElement("div");
  row.className = `row ${fromCp ? "cp" : "cs"}` + (ev.type === 4 ? " err" : "") + (action === "Heartbeat" ? " hb" : "");
  row.innerHTML = `<div class="meta"><span class="t">${escapeHtml(ev.wall.slice(11, 19))}</span>` +
    `<span class="dir">${fromCp ? "충전기 → 서버" : "서버 → 충전기"}</span>` +
    `<span class="kind">${kind}</span><span class="act">${escapeHtml(action)}</span></div>` +
    `<div class="raw"></div>`;
  row.querySelector(".raw").textContent = ev.raw;
  row.addEventListener("click", () => row.classList.toggle("open"));
  if (action === "Heartbeat" && $("hide-hb").checked) row.hidden = true;
  const log = $("log");
  const atBottom = log.scrollTop + log.clientHeight >= log.scrollHeight - 30;
  log.appendChild(row);
  while (log.children.length > 600) log.removeChild(log.firstChild);
  if (atBottom) log.scrollTop = log.scrollHeight;
}

window.addEventListener("resize", () => placePlug());
placePlug();
connect();

function remote(action, payload) {
  if (running) return toast("시험이 실행 중입니다.", true);
  send({t: "ocpp", action, payload});
}
const tag = () => $("remote-tag").value.trim();
const reservationId = () => Number($("reservation-id").value);
$("remote-start").onclick = () => { if (tag()) remote("RemoteStartTransaction", {connectorId: 1, idTag: tag()}); };
$("remote-stop").onclick = () => {
  const tx = Number(st["c1.tx"]);
  if (!Number.isInteger(tx) || tx < 0) return toast("진행 중인 거래가 없습니다.", true);
  remote("RemoteStopTransaction", {transactionId: tx});
};
$("reserve").onclick = () => {
  const minutes = Number($("reservation-minutes").value), id = reservationId();
  if (!tag() || !Number.isInteger(id) || id < 0 || minutes < 1 || minutes > 1440) return toast("카드 ID, 예약 번호, 시간을 확인하세요.", true);
  remote("ReserveNow", {connectorId: 1, reservationId: id, idTag: tag(), expiryDate: new Date(Date.now() + minutes * 60000).toISOString()});
};
$("cancel-reserve").onclick = () => remote("CancelReservation", {reservationId: reservationId()});
$("trigger-meter").onclick = () => remote("TriggerMessage", {requestedMessage: "MeterValues", connectorId: 1});
$("list-version").onclick = () => remote("GetLocalListVersion", {});
$("install-list").onclick = () => remote("SendLocalList", {listVersion: 1, updateType: "Full", localAuthorizationList: ["TAG001", "TAG002"].map(idTag => ({idTag, idTagInfo: {status: "Accepted"}}))});
$("clear-cache").onclick = () => remote("ClearCache", {});
